#include "playarea.hpp"

#include <Windows.h>

#include <cstring>

#include "../config.hpp"
#include "../log.hpp"
#include "../pattern.hpp"

namespace loader::game::playarea
{
	namespace
	{
		// The box's one-time build: cmp byte [flag],0; jne <built>; movups xmm0,[tunables: max x, max y, min x, min y]
		constexpr uintptr_t kBuild = 0xD50D31, kMin = 0x43524A0, kMax = 0x43524B0, kFlag = 0x43524C0, kTunables = 0x434A2EC;
		constexpr float kWorld = 16000.0f; // (the engine's world box, research §61.4)
		bool g_on = false;
	}

	bool Install()
	{
		if (!config::Get().worldPathGrid || !config::Get().worldPlayArea)
			return false;
		const auto module = pattern::Module::Main();
		const auto* code = reinterpret_cast<const uint8_t*>(module.base + kBuild);
		const auto target = [&](size_t at, size_t end) { // a rip-relative operand at `at`, the instruction ending at `end`
			int32_t disp;
			std::memcpy(&disp, code + at, 4);
			return kBuild + end + disp;
		};
		// cmp byte [rip+d],0 (7 bytes); jne short; movups xmm0,[rip+d] (7 bytes)
		if (code[0] != 0x80 || code[1] != 0x3D || code[6] != 0x00 || target(2, 7) != kFlag || code[7] != 0x75 || code[9] != 0x0F || code[10] != 0x10 ||
		    code[11] != 0x05 || target(12, 16) != kTunables)
		{
			log::Warn("play area: the game's player box differs from the build this was made for; the player cannot drive on new land "
			          "beyond (-8000, -8000)..(8000, 9000)");
			return false;
		}
		g_on = true;
		return true;
	}

	void Tick()
	{
		if (!g_on)
			return;
		// The tunables come with the game's data (after the loader is in), the box is built from them on first use, and
		// the game's start zeroes the box (0xDAE890, the flag left as it was): each frame, whatever of them holds the
		// game's values gets the world's.
		const auto base = pattern::Module::Main().base;
		auto* tunables = reinterpret_cast<float*>(base + kTunables);
		if (tunables[0] == 8000.0f && tunables[1] == 9000.0f && tunables[2] == -8000.0f && tunables[3] == -8000.0f)
		{
			const float wide[4] = {kWorld, kWorld, -kWorld, -kWorld};
			std::memcpy(tunables, wide, sizeof(wide));
			log::Info("play area: the player's box is the world's, (-{0:.0f}, -{0:.0f})..({0:.0f}, {0:.0f}) (was (-8000, -8000)..(8000, 9000))", kWorld);
		}
		auto* min = reinterpret_cast<float*>(base + kMin);
		auto* max = reinterpret_cast<float*>(base + kMax);
		if (*reinterpret_cast<const volatile uint8_t*>(base + kFlag) && min[0] == -8000.0f && min[1] == -8000.0f && max[0] == 8000.0f && max[1] == 9000.0f)
		{
			min[0] = min[1] = min[3] = -kWorld;
			max[0] = max[1] = max[3] = kWorld;
			log::Info("play area: the built box widened to the world's");
		}
	}
}
