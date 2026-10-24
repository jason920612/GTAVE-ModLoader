#include "gameobjects.hpp"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "../log.hpp"
#include "../pattern.hpp"

namespace loader::graphics::gameobjects
{
	namespace
	{
		// The encrypted pool pointer: the key, the pointer as stored and the flag byte (bit 0: there is a pool).
		const uint64_t* g_key = nullptr;
		const uint64_t* g_stored = nullptr;
		const uint8_t* g_flag = nullptr;
		uint32_t g_stride = 0; // (the object's size, from the set-up: the pool's own must agree)
		bool g_tried = false;

		constexpr size_t kModelInfo = 0x20, kType = 0x28, kFlags = 0x2C, kMatrix = 0x60, kDummy = 0x3D8;
		constexpr size_t kModelHash = 0x18;
		constexpr uint8_t kVisible = 1; // (the flags' lowest bit: SET_ENTITY_VISIBLE clears it, measured)
		constexpr uint8_t kObjectType = 5, kDummyType = 6;

		uint64_t Rol(uint64_t v, unsigned n)
		{
			n &= 63;
			return n ? (v << n) | (v >> (64 - n)) : v;
		}

		// As the set-up encodes it, undone: k = rol(key, 30); p = ~rol(rol(stored ^ k, 32), (k & 31) + 3).
		uintptr_t Pool()
		{
			if (!g_flag || !(*g_flag & 1))
				return 0;
			const uint64_t k = Rol(*g_key, 0x1E);
			const uint64_t p = Rol(Rol(*g_stored ^ k, 0x20), static_cast<unsigned>((k & 0x1F) + 3));
			return static_cast<uintptr_t>(~p);
		}

		// (memory of another subsystem's: read under SEH, as a pool slot can be in flux)
		bool Copy(void* to, uintptr_t from, size_t size)
		{
			__try
			{
				std::memcpy(to, reinterpret_cast<const void*>(from), size);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		struct Matrix
		{
			float right[4], forward[4], up[4], position[4];
		};

		float Distance2(const float* a, const float* b)
		{
			const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
			return x * x + y * y + z * z;
		}

		float Dot(const float* a, const float* b)
		{
			return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
		}
	}

	bool Install()
	{
		if (g_tried)
			return g_flag != nullptr;
		g_tried = true;
		// CObject's pool set-up: push rsi/rdi/rbx; sub rsp,30h; mov edi,edx; mov ecx,30h; call (allocate); mov rsi,rax;
		// mov rcx,[rip+config]; mov edx,39958261h ("Object"); mov r8d,1; call (its size); ...; mov dword [rsp+20h],<size>
		// (at +60h); ... and at +0A2h the encoding: mov rdx,[rip+key]; rol rdx,1Eh; not rsi; mov ecx,edx; and cl,1Fh;
		// add cl,3; ror rsi,cl; mov [rip+x],eax; rol rsi,20h; xor rsi,rdx; mov [rip+stored],rsi; mov byte [rip+flag],1
		const auto module = pattern::Module::Main();
		const auto at = pattern::Find(module.text,
		    pattern::Pattern::Parse("56 57 53 48 83 EC 30 89 D7 B9 30 00 00 00 E8 ? ? ? ? 48 89 C6 48 8B 0D ? ? ? ? BA 61 82 95 39 41 B8 01 00 00 00 E8"));
		if (!at)
		{
			log::Warn("ue5 objects: the game's object pool not found; the renderer draws the map's objects as placed");
			return false;
		}
		const auto* code = reinterpret_cast<const uint8_t*>(*at);
		static constexpr uint8_t kEncode[] = {0x48, 0xC1, 0xC2, 0x1E, 0x48, 0xF7, 0xD6, 0x89, 0xD1, 0x80, 0xE1, 0x1F, 0x80, 0xC1, 0x03, 0x48, 0xD3, 0xCE};
		if (code[0x60] != 0xC7 || code[0x61] != 0x44 || code[0xA2] != 0x48 || code[0xA3] != 0x8B || code[0xA4] != 0x15 ||
		    std::memcmp(code + 0xA9, kEncode, sizeof(kEncode)) != 0 || code[0xC8] != 0x48 || code[0xC9] != 0x89 || code[0xCA] != 0x35 ||
		    code[0xCF] != 0xC6 || code[0xD0] != 0x05)
		{
			log::Warn("ue5 objects: the game's object pool code is not as known; the renderer draws the map's objects as placed");
			return false;
		}
		std::memcpy(&g_stride, code + 0x64, sizeof(g_stride));
		g_key = reinterpret_cast<const uint64_t*>(pattern::Rip(*at + 0xA5));
		g_stored = reinterpret_cast<const uint64_t*>(pattern::Rip(*at + 0xCB));
		// (mov byte [rip+disp], imm8: the instruction ends a byte after the displacement)
		g_flag = reinterpret_cast<const uint8_t*>(pattern::Rip(*at + 0xD1) + 1);
		log::Info("ue5 objects: the object pool's key at +{:#x} (objects {:#x} bytes)", reinterpret_cast<uintptr_t>(g_key) - module.base, g_stride);
		return true;
	}

	size_t Near(const float point[3], float radius, GTAVBridge::GameObject* out, size_t max)
	{
		const uintptr_t pool = Pool();
		if (!pool)
			return 0;
		struct
		{
			uintptr_t storage, flags;
			int32_t count, stride;
		} p{};
		if (!Copy(&p.storage, pool + 8, 16) || !Copy(&p.count, pool + 0x18, 8) || p.count <= 0 || p.count > 0x10000 ||
		    static_cast<uint32_t>(p.stride) != g_stride)
			return 0;
		std::vector<uint8_t> flags(static_cast<size_t>(p.count));
		if (!Copy(flags.data(), p.flags, flags.size()))
			return 0;
		struct Found
		{
			float distance;
			GTAVBridge::GameObject object;
		};
		static std::vector<Found> found;
		found.clear();
		const float radius2 = radius * radius;
		for (int32_t i = 0; i < p.count; ++i)
		{
			if (flags[i] & 0x80)
				continue;
			const uintptr_t o = p.storage + static_cast<uintptr_t>(i) * static_cast<uint32_t>(p.stride);
			uint8_t type = 0, entityFlags = 0;
			Matrix m{};
			uintptr_t info = 0, dummy = 0;
			if (!Copy(&type, o + kType, 1) || type != kObjectType || !Copy(&entityFlags, o + kFlags, 1) || !Copy(&m, o + kMatrix, sizeof(m)) ||
			    !Copy(&info, o + kModelInfo, 8) || !info || !Copy(&dummy, o + kDummy, 8))
				continue;
			const bool visible = (entityFlags & kVisible) != 0;
			const float d2 = Distance2(m.position, point);
			if (!(d2 <= radius2))
				continue;
			GTAVBridge::GameObject g{};
			if (!Copy(&g.model, info + kModelHash, 4) || !g.model)
				continue;
			// (as the map placed it and shown, it is the renderer's already: only one moved, turned or hidden; one not the
			// map's only shown)
			const float nan = std::numeric_limits<float>::quiet_NaN();
			g.placed[0] = g.placed[1] = g.placed[2] = nan;
			uint8_t dummyType = 0;
			Matrix placed{};
			if (dummy && Copy(&dummyType, dummy + kType, 1) && dummyType == kDummyType && Copy(&placed, dummy + kMatrix, sizeof(placed)))
			{
				const bool moved = Distance2(m.position, placed.position) > 0.02f * 0.02f || Dot(m.forward, placed.forward) < 0.9995f * std::sqrt(Dot(m.forward, m.forward) * Dot(placed.forward, placed.forward)) ||
				                   Dot(m.up, placed.up) < 0.9995f * std::sqrt(Dot(m.up, m.up) * Dot(placed.up, placed.up));
				if (!moved && visible)
					continue;
				std::memcpy(g.placed, placed.position, sizeof(g.placed));
			}
			else if (!visible)
				continue;
			g.id = static_cast<uint32_t>(i) | static_cast<uint32_t>(flags[i] & 0x7F) << 16;
			std::memcpy(g.position, m.position, sizeof(g.position));
			std::memcpy(g.right, m.right, sizeof(g.right));
			std::memcpy(g.forward, m.forward, sizeof(g.forward));
			std::memcpy(g.up, m.up, sizeof(g.up));
			g.flags = visible ? GTAVBridge::kObjectVisible : 0;
			found.push_back({d2, g});
		}
		std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.distance < b.distance; });
		const size_t n = std::min(found.size(), max);
		for (size_t k = 0; k < n; ++k)
			out[k] = found[k].object;
		return n;
	}
}
