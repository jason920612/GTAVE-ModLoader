#include "gamelights.hpp"

#include <Windows.h>

#include <atomic>
#include <cstring>

#include <MinHook.h>

#include "../log.hpp"
#include "../pattern.hpp"
#include "GTAVBridgeProtocol.h"

namespace loader::graphics::gamelights
{
	namespace
	{
		// A light in the game's list (0x1c0 bytes): the parts read, as DRAW_LIGHT_WITH_RANGE's and DRAW_SPOT_LIGHT's code
		// fills them (type 16 is no light: an occluder with no colour).
		constexpr size_t kStride = 0x1C0;
		constexpr size_t kPosition = 0x00, kColor = 0x10, kDirection = 0x20, kTangent = 0x30, kType = 0x60, kFlags = 0x64, kIntensity = 0x68,
		                 kRadius = 0x98, kFalloff = 0x9C, kLength = 0xB0, kInner = 0xB8, kOuter = 0xBC;
		constexpr int32_t kMost = 1024; // (the game's list holds as many)

		using StartFn = void (*)();
		StartFn g_original = nullptr;
		const int32_t* g_count = nullptr;
		uint8_t* const* g_array = nullptr;
		std::atomic<GTAVBridge::Shared*> g_target = nullptr;
		uint32_t g_sequence = 0; // (the game's main thread only)

		float Float(const uint8_t* e, size_t at)
		{
			float v;
			std::memcpy(&v, e + at, sizeof(v));
			return v;
		}

		void Publish()
		{
			GTAVBridge::Shared* s = g_target.load(std::memory_order_acquire);
			const int32_t count = *g_count;
			const uint8_t* array = *g_array;
			if (!s || !array || count < 0 || count > kMost)
				return;
			if (++g_sequence == 0)
				++g_sequence;
			GTAVBridge::LightList& list = s->lightLists[g_sequence % GTAVBridge::kLightLists];
			list.sequence = 0;
			MemoryBarrier();
			uint32_t n = 0;
			for (int32_t i = 0; i < count && n < GTAVBridge::kMaxGameLights; ++i)
			{
				const uint8_t* e = array + kStride * i;
				uint32_t type;
				std::memcpy(&type, e + kType, sizeof(type));
				if (type != 1 && type != 2 && type != 4)
					continue;
				GTAVBridge::GameLight& l = list.lights[n++];
				std::memcpy(l.position, e + kPosition, sizeof(l.position));
				std::memcpy(l.direction, e + kDirection, sizeof(l.direction));
				std::memcpy(l.tangent, e + kTangent, sizeof(l.tangent));
				std::memcpy(l.color, e + kColor, sizeof(l.color));
				std::memcpy(&l.flags, e + kFlags, sizeof(l.flags));
				l.type = type;
				l.intensity = Float(e, kIntensity);
				l.radius = Float(e, kRadius);
				l.falloffExponent = Float(e, kFalloff);
				l.coneInner = type == 2 ? Float(e, kInner) : 0.0f;
				l.coneOuter = type == 2 ? Float(e, kOuter) : 0.0f;
				l.length = type == 4 ? Float(e, kLength) : 0.0f;
			}
			list.count = n;
			MemoryBarrier();
			list.sequence = g_sequence;
			s->lightSequence = g_sequence;
		}

		// The game's main thread, as a frame starts its list anew: the last frame's is complete.
		void HookStart()
		{
			Publish();
			g_original();
		}
	}

	bool Install()
	{
		const auto module = pattern::Module::Main();
		// The new frame's lights: saves the old list's lights that stay (an interior's), swaps the list buffers, count 0.
		// ...; mov eax,[rip+index]; inc eax; xor ebx,ebx; cmp eax,2; cmove eax,ebx; ...; cmp dword [rip+count],0; jle ...;
		// xor edi,edi; mov r13d,120h; ...; mov rbp,[rip+array] (at +85h)
		const auto start = pattern::Find(module.text,
		    pattern::Pattern::Parse("41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 28 C7 05 ? ? ? ? 00 00 00 00 8B 05 ? ? ? ? FF C0 31 DB 83 F8 02 0F 44 C3 "
		                            "89 05 ? ? ? ? 48 98 48 8D 0D ? ? ? ? C6 04 08 00 83 3D ? ? ? ? 00 0F 8E ? ? ? ? 31 FF 41 BD 20 01 00 00"));
		if (!start)
		{
			log::Warn("ue5 bridge: the game's light list not found; the renderer lights by the map's data alone");
			return false;
		}
		const auto* code = reinterpret_cast<const uint8_t*>(*start);
		if (code[0x85] != 0x48 || code[0x86] != 0x8B || code[0x87] != 0x2D)
		{
			log::Warn("ue5 bridge: the game's light list code is not as known; the renderer lights by the map's data alone");
			return false;
		}
		// (cmp dword [rip+disp32], imm8: the displacement at +3Fh, the instruction ends 5 bytes after it)
		g_count = reinterpret_cast<const int32_t*>(pattern::Rip(*start + 0x3F) + 1);
		g_array = reinterpret_cast<uint8_t* const*>(pattern::Rip(*start + 0x88));
		if (MH_CreateHook(reinterpret_cast<void*>(*start), reinterpret_cast<void*>(&HookStart), reinterpret_cast<void**>(&g_original)) != MH_OK ||
		    MH_EnableHook(reinterpret_cast<void*>(*start)) != MH_OK)
		{
			log::Warn("ue5 bridge: could not hook the game's light list");
			return false;
		}
		log::Info("ue5 bridge: the game's lights (+{:#x}; list +{:#x}, count +{:#x})", *start - module.base,
		    reinterpret_cast<uintptr_t>(g_array) - module.base, reinterpret_cast<uintptr_t>(g_count) - module.base);
		return true;
	}

	void SetTarget(GTAVBridge::Shared* shared)
	{
		g_target.store(shared, std::memory_order_release);
	}
}
