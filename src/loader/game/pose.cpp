#include "pose.hpp"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <MinHook.h>

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
		GetEntityFn g_getEntity = nullptr;
		PostAnimationFn g_postAnimation = nullptr;

		struct Override
		{
			std::vector<MLBonePose> bones;
			uint64_t setAt = 0;
			std::vector<float> shownLocal, shownObject; // the last frame as shown (after the overrides)
		};
		std::unordered_map<uint8_t*, Override> g_overrides; // by entity (game thread only)

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
			o.shownLocal.assign(s.local, s.local + 16 * s.count);
			o.shownObject.assign(s.object, s.object + 16 * s.count);
		}

		uint64_t PostAnimation(void* a, void* b, void* c, void* d)
		{
			const uint64_t result = g_postAnimation(a, b, c, d);
			if (g_overrides.empty() || !Readable(a, kComponentEntity + 8))
				return result;
			uint8_t* entity = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(a) + kComponentEntity);
			const auto it = g_overrides.find(entity);
			if (it == g_overrides.end())
				return result;
			if (GetTickCount64() - it->second.setAt > 2000)
			{
				g_overrides.erase(it);
				return result;
			}
			Apply(entity, it->second);
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
		if (const auto it = g_overrides.find(entity); shownSpace && it != g_overrides.end() && it->second.shownObject.size() == 16u * s.count)
		{
			local = it->second.shownLocal.data();
			object = it->second.shownObject.data();
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
		if (count == 0)
		{
			g_overrides.erase(entity);
			return 1;
		}
		Override& o = g_overrides[entity];
		o.bones.assign(bones, bones + count);
		o.setAt = GetTickCount64();
		return 1;
	}
}
