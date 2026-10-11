#include "pose.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>

#include <MinHook.h>

#include "../config.hpp"
#include "../debug/watch.hpp"
#include "../log.hpp"
#include "../pattern.hpp"

namespace loader::game::pose
{
	namespace
	{
		// research/phase0.md §42.
		// Script handle -> entity (the game's own lookup, used by every entity native).
		constexpr const char* kGetEntity = "85 C9 0F 88 ? ? ? ? 41 89 C9 41 89 C8 49 C1 E8 08 0F B6 05";
		// Per-ped update after the animation pass (main thread): (ped component a, ...); *(a+0x100) = entity.
		constexpr const char* kPostAnimation =
		    "56 57 48 83 EC 58 44 0F 29 44 24 40 0F 29 7C 24 30 0F 29 74 24 20 41 80 B8 ? ? 00 00 00 74 ? F3 41 0F 10 80 ? ? 00 00 F3 41 0F 10 98";
		// Animation job (worker threads) that writes an animation frame into a skeleton: resets the local matrices to the
		// defaults, writes the frame's tracks and recomputes the object matrices (research §45). It runs after the
		// post-animation update, and frames only carry translations for some bones, so overrides are applied again after it.
		// Job: +0x20 local matrices. The pattern is 0x6d bytes into the function.
		constexpr const char* kFrameToSkeleton = "4C 8B 31 48 8B 59 08 4C 8B 61 10 48 8B 71 20 44 8B 51 28 44 8B 69 30 44 0F B6 59 38";
		constexpr ptrdiff_t kFrameToSkeletonOffset = 0x6d;
		constexpr size_t kEntitySkeleton = 0xff8; // entity -> skeleton
		constexpr size_t kComponentEntity = 0x100; // post-animation argument -> entity
		constexpr size_t kEntityMatrix = 0x60;

		// Skeleton: +0 data, +0x10 local matrices, +0x18 object matrices (0x40 each).
		// Data: +0x18 u16 bone count, +0x20 bone records (0x50: +0 rest rotation, +0x10 rest translation, +0x32 u16 parent,
		// +0x38 name, +0x44 u16 tag).
		struct Skeleton
		{
			uint8_t* base;
			uint8_t* data;
			int count;
			uint8_t* bones;
			float* local;
			float* object;
			int Parent(int i) const
			{
				const uint16_t p = *reinterpret_cast<uint16_t*>(bones + 0x50 * i + 0x32);
				return p >= count ? -1 : p;
			}
		};

		using GetEntityFn = uint8_t* (*)(int32_t);
		using PostAnimationFn = uint64_t (*)(void*, void*, void*, void*);
		using FrameToSkeletonFn = uint64_t (*)(void*);
		FrameToSkeletonFn g_frameToSkeleton = nullptr;
		GetEntityFn g_getEntity = nullptr;
		PostAnimationFn g_postAnimation = nullptr;

		struct Override
		{
			std::vector<MLBonePose> bones;
			uint64_t setAt = 0;
			std::vector<float> shownLocal, shownObject; // the last frame as shown (after the overrides)
			// This frame's animation before the overrides: what scripts build on. Bones the animation does not write (no
			// track in the clip) start each frame from the previous frame's final pose, which holds our override; for those
			// the value from before the override is used (and put back when the override ends).
			std::vector<float> animLocal, animObject;
			struct Base
			{
				float local[16];   // the animation's value
				float applied[16]; // what we left in the skeleton last
				bool hasApplied = false;
			};
			std::unordered_map<int, Base> base; // by bone
			bool ending = false;                // cleared or expired: restore the bases next frame, then drop
			const float* local = nullptr;       // the skeleton's local matrices (to recognise animation jobs)
		};
		// By entity. Used by the game thread (scripts, post-animation) and the animation workers: under g_mutex.
		std::unordered_map<uint8_t*, Override> g_overrides;
		std::mutex g_mutex;
		std::atomic_int g_overrideCount = 0;

		bool Readable(const void* p, size_t n)
		{
			return p && !IsBadReadPtr(p, n);
		}

		bool SkeletonOf(uint8_t* entity, Skeleton& out)
		{
			if (!Readable(entity, kEntitySkeleton + 8))
				return false;
			uint8_t* s = *reinterpret_cast<uint8_t**>(entity + kEntitySkeleton);
			if (!Readable(s, 0x28))
				return false;
			uint8_t* data = *reinterpret_cast<uint8_t**>(s);
			if (!Readable(data, 0x28))
				return false;
			out.base = s;
			out.data = data;
			out.count = *reinterpret_cast<uint16_t*>(data + 0x18);
			out.bones = *reinterpret_cast<uint8_t**>(data + 0x20);
			out.local = *reinterpret_cast<float**>(s + 0x10);
			out.object = *reinterpret_cast<float**>(s + 0x18);
			if (out.count <= 0 || out.count > 1024 || !Readable(out.bones, 0x50) || !out.local || !out.object)
				return false;
			const char* name = *reinterpret_cast<const char**>(out.bones + 0x38);
			return Readable(name, 4) && std::memcmp(name, "SKEL", 4) == 0; // the root bone
		}

		uint8_t* Entity(int32_t ped)
		{
			return g_getEntity && ped > 0 ? g_getEntity(ped) : nullptr;
		}

		// ---- maths (row-vector 4x4: x, y, z axis rows, translation row) ------------------------------

		void QuatToRows(const float q[4], float* m)
		{
			const float x = q[0], y = q[1], z = q[2], w = q[3];
			// Rows are the images of the local axes: the columns of the usual column-vector rotation matrix.
			m[0] = 1 - 2 * (y * y + z * z), m[1] = 2 * (x * y + z * w), m[2] = 2 * (x * z - y * w);
			m[4] = 2 * (x * y - z * w), m[5] = 1 - 2 * (x * x + z * z), m[6] = 2 * (y * z + x * w);
			m[8] = 2 * (x * z + y * w), m[9] = 2 * (y * z - x * w), m[10] = 1 - 2 * (x * x + y * y);
		}
		void RowsToQuat(const float* m, float q[4])
		{
			const float tr = m[0] + m[5] + m[10];
			if (tr > 0)
			{
				const float s = std::sqrt(tr + 1.0f) * 2;
				q[3] = 0.25f * s, q[0] = (m[6] - m[9]) / s, q[1] = (m[8] - m[2]) / s, q[2] = (m[1] - m[4]) / s;
			}
			else if (m[0] > m[5] && m[0] > m[10])
			{
				const float s = std::sqrt(1.0f + m[0] - m[5] - m[10]) * 2;
				q[3] = (m[6] - m[9]) / s, q[0] = 0.25f * s, q[1] = (m[4] + m[1]) / s, q[2] = (m[8] + m[2]) / s;
			}
			else if (m[5] > m[10])
			{
				const float s = std::sqrt(1.0f + m[5] - m[0] - m[10]) * 2;
				q[3] = (m[8] - m[2]) / s, q[0] = (m[4] + m[1]) / s, q[1] = 0.25f * s, q[2] = (m[9] + m[6]) / s;
			}
			else
			{
				const float s = std::sqrt(1.0f + m[10] - m[0] - m[5]) * 2;
				q[3] = (m[1] - m[4]) / s, q[0] = (m[8] + m[2]) / s, q[1] = (m[9] + m[6]) / s, q[2] = 0.25f * s;
			}
		}
		void Slerp(const float a[4], float b[4], float t, float out[4])
		{
			float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
			float bb[4] = {b[0], b[1], b[2], b[3]};
			if (d < 0)
			{
				d = -d;
				for (float& v : bb)
					v = -v;
			}
			float wa = 1 - t, wb = t;
			if (d < 0.9995f)
			{
				const float th = std::acos(d), s = std::sin(th);
				wa = std::sin((1 - t) * th) / s;
				wb = std::sin(t * th) / s;
			}
			float n = 0;
			for (int i = 0; i < 4; ++i)
			{
				out[i] = wa * a[i] + wb * bb[i];
				n += out[i] * out[i];
			}
			n = 1 / std::sqrt(n);
			for (int i = 0; i < 4; ++i)
				out[i] *= n;
		}
		// out = a * b (affine rows; translation row last).
		void MulAffine(const float* a, const float* b, float* out)
		{
			float r[16];
			for (int i = 0; i < 4; ++i)
				for (int j = 0; j < 3; ++j)
					r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + (i == 3 ? b[12 + j] : 0);
			for (int i = 0; i < 4; ++i)
			{
				for (int j = 0; j < 3; ++j)
					out[i * 4 + j] = r[i * 4 + j];
			}
		}

		void Apply(uint8_t* entity, Override& o)
		{
			Skeleton s;
			if (!SkeletonOf(entity, s))
				return;
			std::vector<uint8_t> dirty(static_cast<size_t>(s.count), 0);
			int first = s.count;
			for (const MLBonePose& p : o.bones)
			{
				if (p.bone < 0 || p.bone >= s.count || p.weight <= 0)
					continue;
				float* m = s.local + 16 * p.bone;
				const float w = std::min(p.weight, 1.0f);
				if (p.flags & ML_POSE_ROTATION)
				{
					float cur[4], target[4] = {p.rotation[0], p.rotation[1], p.rotation[2], p.rotation[3]}, q[4];
					RowsToQuat(m, cur);
					Slerp(cur, target, w, q);
					QuatToRows(q, m);
				}
				if (p.flags & ML_POSE_TRANSLATION)
					for (int k = 0; k < 3; ++k)
						m[12 + k] += (p.translation[k] - m[12 + k]) * w;
				dirty[static_cast<size_t>(p.bone)] = 1;
				first = std::min(first, p.bone);
				Override::Base& b = o.base[p.bone];
				std::memcpy(b.applied, m, sizeof(b.applied));
				b.hasApplied = true;
			}
			// Object matrices: the bone, then everything below it (parents come before their children). The root keeps
			// the offset the post-animation update added (object root - local root).
			for (int i = first; i < s.count; ++i)
			{
				const int parent = s.Parent(i);
				if (!dirty[static_cast<size_t>(i)] && (parent < 0 || !dirty[static_cast<size_t>(parent)]))
					continue;
				dirty[static_cast<size_t>(i)] = 1;
				float* obj = s.object + 16 * i;
				const float* loc = s.local + 16 * i;
				if (parent < 0)
				{
					const float off[3] = {obj[12] - loc[12], obj[13] - loc[13], obj[14] - loc[14]};
					for (int k = 0; k < 12; ++k)
						if (k % 4 != 3)
							obj[k] = loc[k];
					for (int k = 0; k < 3; ++k)
						obj[12 + k] = loc[12 + k] + off[k];
				}
				else
					MulAffine(loc, s.object + 16 * parent, obj);
			}
			// Research (§45): who writes a translated spine bone after us. Watches the object and local translation z of the
			// first translated bone whose name has "Spine", and the object z of its first child.
			static bool watched = false;
			if (!watched && config::Get().debugPoseWatch)
				for (const MLBonePose& p : o.bones)
				{
					const char* name = *reinterpret_cast<const char* const*>(s.bones + 0x50 * p.bone + 0x38);
					if (!(p.flags & ML_POSE_TRANSLATION) || !Readable(name, 4) || !std::strstr(name, "Spine"))
						continue;
					int child = -1;
					for (int i = p.bone + 1; i < s.count && child < 0; ++i)
						if (s.Parent(i) == p.bone)
							child = i;
					// object z translation, object x-axis y (turns with the lean), local z translation, local x-axis y.
					std::vector<debug::Watch> w{{reinterpret_cast<uintptr_t>(s.object + 16 * p.bone + 14), 4, false, true},
					    {reinterpret_cast<uintptr_t>(s.object + 16 * p.bone + 1), 4, false, true},
					    {reinterpret_cast<uintptr_t>(s.local + 16 * p.bone + 14), 4, false, true},
					    {reinterpret_cast<uintptr_t>(s.local + 16 * p.bone + 1), 4, false, true}};
					log::Info("pose: watching bone {} {} (object {}, local {}), child {}", p.bone, name, static_cast<void*>(s.object + 16 * p.bone),
					    static_cast<void*>(s.local + 16 * p.bone), child);
					std::thread([w] { debug::ArmWriteWatches(w); }).detach();
					watched = true;
					break;
				}
			o.local = s.local;
			o.shownLocal.assign(s.local, s.local + 16 * s.count);
			o.shownObject.assign(s.object, s.object + 16 * s.count);
		}

		bool Near(const float* a, const float* b)
		{
			for (int k = 0; k < 16; ++k)
				if (k % 4 != 3 && std::fabs(a[k] - b[k]) > 1e-3f)
					return false;
			return true;
		}

		// Object matrices of the changed bones and everything below them, from the local ones. Other bones keep the
		// game's object matrices (helper bones are not plain FK).
		void Forward(const Skeleton& s, const float* local, float* object, std::vector<uint8_t>& changed)
		{
			for (int i = 0; i < s.count; ++i)
			{
				const int parent = s.Parent(i);
				if (parent < 0 || !(changed[static_cast<size_t>(i)] || changed[static_cast<size_t>(parent)]))
					continue;
				changed[static_cast<size_t>(i)] = 1;
				MulAffine(local + 16 * i, object + 16 * parent, object + 16 * i);
			}
		}

		// The animation as scripts should see it (see Override::base).
		void CaptureAnimation(const Skeleton& s, Override& o)
		{
			o.animLocal.assign(s.local, s.local + 16 * s.count);
			o.animObject.assign(s.object, s.object + 16 * s.count);
			for (const MLBonePose& p : o.bones)
				if (p.bone >= 0 && p.bone < s.count && !o.base.contains(p.bone))
					std::memcpy(o.base[p.bone].local, s.local + 16 * p.bone, 64);
			std::vector<uint8_t> carried(static_cast<size_t>(s.count), 0);
			for (auto& [bone, b] : o.base)
			{
				const float* cur = s.local + 16 * bone;
				if (b.hasApplied && Near(cur, b.applied))
				{
					std::memcpy(&o.animLocal[16 * bone], b.local, 64); // carried over from our last frame
					carried[static_cast<size_t>(bone)] = 1;
				}
				else
					std::memcpy(b.local, cur, 64); // the animation wrote it
			}
			Forward(s, o.animLocal.data(), o.animObject.data(), carried);
			// Bones the mod no longer poses go back to the animation.
			std::vector<uint8_t> restored(static_cast<size_t>(s.count), 0);
			bool any = false;
			for (auto it = o.base.begin(); it != o.base.end();)
			{
				const int bone = it->first;
				if (std::any_of(o.bones.begin(), o.bones.end(), [bone](const MLBonePose& p) { return p.bone == bone; }))
				{
					++it;
					continue;
				}
				std::memcpy(s.local + 16 * bone, it->second.local, 64);
				restored[static_cast<size_t>(bone)] = 1, any = true;
				it = o.base.erase(it);
			}
			if (any)
				Forward(s, s.local, s.object, restored);
		}

		// Puts back what the animation would have (bones it does not write keep our last values otherwise).
		void Restore(const Skeleton& s, Override& o)
		{
			std::vector<uint8_t> restored(static_cast<size_t>(s.count), 0);
			for (auto& [bone, b] : o.base)
				if (b.hasApplied && Near(s.local + 16 * bone, b.applied))
				{
					std::memcpy(s.local + 16 * bone, b.local, 64);
					restored[static_cast<size_t>(bone)] = 1;
				}
			Forward(s, s.local, s.object, restored);
		}

		uint64_t PostAnimation(void* a, void* b, void* c, void* d)
		{
			const uint64_t result = g_postAnimation(a, b, c, d);
			if (g_overrideCount == 0 || !Readable(a, kComponentEntity + 8))
				return result;
			std::lock_guard lock(g_mutex);
			uint8_t* entity = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(a) + kComponentEntity);
			const auto it = g_overrides.find(entity);
			if (it == g_overrides.end())
				return result;
			if (it->second.ending || GetTickCount64() - it->second.setAt > 2000)
			{
				if (Skeleton s; SkeletonOf(entity, s))
					Restore(s, it->second);
				g_overrides.erase(it);
				g_overrideCount = static_cast<int>(g_overrides.size());
				return result;
			}
			if (Skeleton s; SkeletonOf(entity, s))
				CaptureAnimation(s, it->second);
			Apply(entity, it->second);
			return result;
		}

		uint64_t FrameToSkeleton(void* job)
		{
			const uint64_t result = g_frameToSkeleton(job);
			if (g_overrideCount == 0)
				return result;
			const float* local = *reinterpret_cast<float* const*>(static_cast<uint8_t*>(job) + 0x20);
			std::lock_guard lock(g_mutex);
			for (auto& [entity, o] : g_overrides)
				if (o.local == local)
				{
					Apply(entity, o);
					break;
				}
			return result;
		}
	}

	bool Install()
	{
		const auto module = pattern::Module::Main();
		const auto get = pattern::Find(module.text, pattern::Pattern::Parse(kGetEntity));
		const auto post = pattern::Find(module.text, pattern::Pattern::Parse(kPostAnimation));
		if (!get || !post)
		{
			log::Warn("pose: game code not found ({} {}); the ped pose API is off", get.has_value(), post.has_value());
			return false;
		}
		g_getEntity = reinterpret_cast<GetEntityFn>(*get);
		if (MH_CreateHook(reinterpret_cast<void*>(*post), reinterpret_cast<void*>(&PostAnimation), reinterpret_cast<void**>(&g_postAnimation)) != MH_OK ||
		    MH_EnableHook(reinterpret_cast<void*>(*post)) != MH_OK)
		{
			log::Warn("pose: could not hook the post-animation update; the ped pose API is off");
			g_getEntity = nullptr;
			return false;
		}
		// Without the animation job hook, overrides still show except translations of bones whose frames lack them.
		if (const auto job = pattern::Find(module.text, pattern::Pattern::Parse(kFrameToSkeleton)); !job)
			log::Warn("pose: animation job not found; translating the hips or spine will not show");
		else if (void* fn = reinterpret_cast<uint8_t*>(*job) - kFrameToSkeletonOffset;
		         MH_CreateHook(fn, reinterpret_cast<void*>(&FrameToSkeleton), reinterpret_cast<void**>(&g_frameToSkeleton)) != MH_OK ||
		         MH_EnableHook(fn) != MH_OK)
			log::Warn("pose: could not hook the animation job; translating the hips or spine will not show");
		log::Info("pose: hooks installed");
		return true;
	}

	int32_t BoneCount(int32_t ped)
	{
		Skeleton s;
		return SkeletonOf(Entity(ped), s) ? s.count : 0;
	}

	int32_t FindBone(int32_t ped, uint32_t tag)
	{
		Skeleton s;
		if (!SkeletonOf(Entity(ped), s))
			return -1;
		for (int i = 0; i < s.count; ++i)
			if (*reinterpret_cast<uint16_t*>(s.bones + 0x50 * i + 0x44) == tag)
				return i;
		return -1;
	}

	int32_t BoneInfo(int32_t ped, int32_t bone, MLBoneInfo* out)
	{
		Skeleton s;
		if (!SkeletonOf(Entity(ped), s) || bone < 0 || bone >= s.count)
			return 0;
		const uint8_t* r = s.bones + 0x50 * bone;
		*out = {};
		out->tag = *reinterpret_cast<const uint16_t*>(r + 0x44);
		out->parent = static_cast<int16_t>(s.Parent(bone));
		if (const char* name = *reinterpret_cast<const char* const*>(r + 0x38); Readable(name, 1))
			strncpy_s(out->name, name, _TRUNCATE);
		std::memcpy(out->restRotation, r, 16);
		std::memcpy(out->restTranslation, r + 0x10, 12);
		return 1;
	}

	int32_t GetBone(int32_t ped, int32_t bone, int32_t space, float* matrix16)
	{
		uint8_t* entity = Entity(ped);
		Skeleton s;
		if (!SkeletonOf(entity, s) || bone < 0 || bone >= s.count)
			return 0;
		const bool shownSpace = (space & ML_SPACE_SHOWN) != 0;
		space &= ~ML_SPACE_SHOWN;
		const float* local = s.local;
		const float* object = s.object;
		std::lock_guard lock(g_mutex);
		if (const auto it = g_overrides.find(entity); it != g_overrides.end())
		{
			const Override& o = it->second;
			const auto& l = shownSpace ? o.shownLocal : o.animLocal;
			const auto& m = shownSpace ? o.shownObject : o.animObject;
			if (m.size() == 16u * s.count && l.size() == m.size())
				local = l.data(), object = m.data();
		}
		const float* src = (space == ML_SPACE_LOCAL ? local : object) + 16 * bone;
		std::memcpy(matrix16, src, 64);
		if (space == ML_SPACE_WORLD)
			MulAffine(src, reinterpret_cast<const float*>(entity + kEntityMatrix), matrix16);
		matrix16[3] = matrix16[7] = matrix16[11] = 0;
		matrix16[15] = 1;
		return 1;
	}

	int32_t SetPose(int32_t ped, const MLBonePose* bones, int32_t count)
	{
		uint8_t* entity = Entity(ped);
		if (!entity)
			return 0;
		std::lock_guard lock(g_mutex);
		if (count == 0)
		{
			if (const auto it = g_overrides.find(entity); it != g_overrides.end())
				it->second.ending = true, it->second.bones.clear(); // restored at the next post-animation update
			return 1;
		}
		Override& o = g_overrides[entity];
		o.ending = false;
		o.bones.assign(bones, bones + count);
		o.setAt = GetTickCount64();
		g_overrideCount = static_cast<int>(g_overrides.size());
		return 1;
	}
}
