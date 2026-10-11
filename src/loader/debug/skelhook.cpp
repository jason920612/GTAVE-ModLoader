#include "skelhook.hpp"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <filesystem>
#include <format>
#include <vector>
#include <fstream>
#include <cstring>
#include <mutex>

#include <MinHook.h>

#include "../log.hpp"
#include "../paths.hpp"
#include "../pattern.hpp"

namespace loader::debug::skelhook
{
	namespace
	{
		// Research (research/phase0.md §42). Skeleton: +0 skeleton data (+0x18 u16 bone count, +0x20 bone records of 0x50:
		// +0x32 u16 parent), +0x10 local matrices, +0x18 object matrices (0x40 each: x, y, z axes, translation).
		using UpdateFn = uint64_t (*)(void*, void*, void*, void*);
		UpdateFn g_orig = nullptr;
		std::atomic<uint8_t*> g_skeleton = nullptr;
		std::atomic_int g_bone = -1;
		std::atomic<float> g_angle = 0;
		std::atomic_int g_checks = 0;

		struct Mat
		{
			float m[16];
		};
		// Row-vector convention: a point p in the bone's space is p * local * parent object.
		Mat Mul(const Mat& a, const Mat& b)
		{
			Mat r{};
			for (int i = 0; i < 4; ++i)
				for (int j = 0; j < 4; ++j)
				{
					float s = 0;
					for (int k = 0; k < 4; ++k)
						s += a.m[i * 4 + k] * b.m[k * 4 + j];
					r.m[i * 4 + j] = s;
				}
			return r;
		}
		Mat Load(const float* p)
		{
			Mat r;
			std::memcpy(r.m, p, sizeof(r.m));
			r.m[3] = r.m[7] = r.m[11] = 0;
			r.m[15] = 1;
			return r;
		}

		uint64_t Detour(void* a, void* b, void* c, void* d)
		{
			const uint64_t result = g_orig(a, b, c, d);
			uint8_t* skeleton = g_skeleton;
			if (!skeleton)
				return result;
			uint8_t* data = *reinterpret_cast<uint8_t**>(skeleton);
			const int count = *reinterpret_cast<uint16_t*>(data + 0x18);
			uint8_t* bones = *reinterpret_cast<uint8_t**>(data + 0x20);
			float* local = *reinterpret_cast<float**>(skeleton + 0x10);
			float* object = *reinterpret_cast<float**>(skeleton + 0x18);
			const auto parent = [&](int i) { return static_cast<int>(*reinterpret_cast<uint16_t*>(bones + 0x50 * i + 0x32)); };
			const int bone = g_bone;
			if (bone < 0 || bone >= count)
				return result;
			// First calls: check that object = local * parent object reproduces the game's own result.
			if (g_checks < 3)
			{
				++g_checks;
				const int p = parent(bone);
				if (p < count)
				{
					const Mat expect = Mul(Load(local + 16 * bone), Load(object + 16 * p));
					float err = 0;
					for (int k = 0; k < 16; ++k)
						if (k % 4 != 3)
							err = std::max(err, std::fabs(expect.m[k] - object[16 * bone + k]));
					log::Info("skelhook: bone {} parent {} FK error {:.6f} (game t {:.3f},{:.3f},{:.3f})", bone, p, err, object[16 * bone + 12],
					    object[16 * bone + 13], object[16 * bone + 14]);
				}
			}
			// Bend: rotate the bone's local basis about its z, then redo the object matrices of it and its descendants.
			const float angle = g_angle, cs = std::cos(angle), sn = std::sin(angle);
			float* m = local + 16 * bone;
			for (int row = 0; row < 3; ++row)
			{
				const float x = m[row * 4], y = m[row * 4 + 1];
				m[row * 4] = cs * x - sn * y;
				m[row * 4 + 1] = sn * x + cs * y;
			}
			std::vector<bool> moved(count, false);
			moved[bone] = true;
			for (int i = bone; i < count; ++i)
			{
				const int p = i == bone ? parent(i) : parent(i);
				if (i != bone && (p >= count || !moved[p]))
					continue;
				moved[i] = true;
				const Mat r = Mul(Load(local + 16 * i), Load(object + 16 * p));
				for (int k = 0; k < 16; ++k)
					if (k % 4 != 3)
						object[16 * i + k] = r.m[k];
			}
			return result;
		}
	}

	void StartIfRequested()
	{
		std::ifstream in(paths::Get().root / L"debug_skelhook.txt");
		std::string rva;
		if (!in || !(in >> rva))
			return;
		const auto module = pattern::Module::Main();
		const uintptr_t at = module.base + std::stoull(rva, nullptr, 16);
		if (MH_CreateHook(reinterpret_cast<void*>(at), reinterpret_cast<void*>(&Detour), reinterpret_cast<void**>(&g_orig)) == MH_OK &&
		    MH_EnableHook(reinterpret_cast<void*>(at)) == MH_OK)
			log::Info("skelhook: hooked {}", rva);
	}

	void Poll()
	{
		static uint64_t next = 0;
		if (GetTickCount64() < next)
			return;
		next = GetTickCount64() + 500;
		std::ifstream in(paths::Get().root / L"debug_skelbend.txt");
		std::string skeleton;
		int bone = -1;
		float degrees = 0;
		if (!in || !(in >> skeleton >> bone >> degrees))
			return;
		in.close();
		std::error_code ec;
		std::filesystem::remove(paths::Get().root / L"debug_skelbend.txt", ec);
		g_checks = 0;
		g_angle = degrees * 3.14159265f / 180.0f;
		g_bone = bone;
		g_skeleton = bone < 0 ? nullptr : reinterpret_cast<uint8_t*>(std::stoull(skeleton, nullptr, 16));
		log::Info("skelhook: bend skeleton {} bone {} by {}", skeleton, bone, degrees);
	}
}
