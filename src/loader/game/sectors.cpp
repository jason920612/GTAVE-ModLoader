#include "sectors.hpp"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "../config.hpp"
#include "../convert/resource.hpp"
#include "../log.hpp"
#include "../paths.hpp"
#include "../pattern.hpp"
#include "content.hpp"
#include "natives.hpp"

namespace loader::game::sectors
{
	namespace
	{
		struct Land
		{
			std::string name;  // the pack's folder (and its name in the game)
			uint32_t hash = 0; // the name's hash, as the content manager has it
			float box[4]{};    // min x, min y, max x, max y
			bool dormant = false;
		};
		std::vector<Land> g_lands;
		constexpr uint32_t kGroupStartup = 0x5BF6578F, kGroupMapSp = 0x578F99E2; // GROUP_STARTUP, GROUP_MAP_SP
		// ThePaths: +0x4BE6 the set of road network regions wanted (Cayo's island set or the main one), +0x4BE7 the one
		// they were loaded for; when the two differ the streaming update (0xF62560) drops all the regions of both sets and
		// loads those it wants again
		constexpr uintptr_t kThePaths = 0x4426E10;
		// The streaming objects (0x42F9E48): +0 their states (8 bytes each, the second dword's low bits: 0 not there, 1
		// loaded, 2 asked for, 3 loading), +0x1A0 the modules by range (16 bytes: the module, the end of its range), +0x1A8 how
		// many; a module: +8 its first object, +0x10 how many. The navmeshes' module (its vtable 0x262DBA0): one object a
		// square of the navmesh grid (150 m from (-6000, -6000), 147 a row: game/navgrid.hpp).
		constexpr uintptr_t kStreaming = 0x42F9E48, kNavMeshesVtable = 0x262DBA0;
		constexpr float kNavOrigin = -6000, kNavSquare = 150;
		constexpr int kNavWidth = 147, kNavHeight = 110;

		constexpr uint64_t kPlayerPedId = 0xD80958FC74E988A6, kSetEntityCoords = 0x06843DA7060A026B, kFreezeEntityPosition = 0x428CA6DBD1094446,
		                   kClearPedTasksImmediately = 0xAAA34F8A7CB32098, kDoScreenFadeOut = 0x891B5B39AC6302AF, kDoScreenFadeIn = 0xD4E8E24955024033,
		                   kIsScreenFadedOut = 0xB16FCE9DDC7BA182, kIsLoadingScreenActive = 0x10D0A8F259E93EC9, kLoadAllObjectsNow = 0xBD6E84632DD4CB3F,
		                   kGetGroundZFor3dCoord = 0xC906A7DAB05C8D2B, kSetFocusPosAndVel = 0xBB7454BAFF08FE25, kClearFocus = 0x31B73D1EA9F01DA2;
		// where the player waits while a land is put away: Los Santos, away from any new land
		constexpr float kHold[3] = {200, -900, 31};

		// A switch under way (game thread, Tick), in two goes: the screen faded and the player held in Los Santos, the old
		// land's maps put away (the game restarts the session for it: every entity gone, the player at the save's place),
		// the player held again until the path server has let the old land's navmeshes go (they are in its overlay, the
		// start-up change set's: an archive's navmesh still loaded when the archive is taken away stopped the game,
		// 0xCE4CD0's int3), then the old land's overlay out, the new one's in and its maps (a second restart), the player
		// put where they were going.
		enum class Step { None, Fade, PutAway, FirstRestart, Drain, Bring, SecondRestart, Arrive };
		struct Switching
		{
			Step step = Step::None;
			std::string to;
			bool place = false;
			float at[2]{};
			ULONGLONG since = 0;
			bool loadingSeen = false;
			std::vector<std::string> leaving; // (the lands put away)
		} g_switch;
		ULONGLONG g_lastTick = 0; // (a gap in the ticks: the session was restarting, story mode's scripts not running)

		bool Overlap(const Land& a, const Land& b)
		{
			return a.box[0] < b.box[2] && b.box[0] < a.box[2] && a.box[1] < b.box[3] && b.box[1] < a.box[3];
		}

		// Of the lands that share a place, all but one dormant: the wanted one, else the first by name.
		void Settle(const std::string& wanted)
		{
			std::vector<Land*> order;
			for (Land& l : g_lands)
			{
				l.dormant = false;
				order.push_back(&l);
			}
			std::stable_sort(order.begin(), order.end(), [&](const Land* a, const Land* b) { return (a->name == wanted) > (b->name == wanted); });
			for (size_t i = 0; i < order.size(); ++i)
				if (!order[i]->dormant)
					for (size_t k = i + 1; k < order.size(); ++k)
						if (!order[k]->dormant && Overlap(*order[i], *order[k]))
							order[k]->dormant = true;
		}

		// How many navmeshes in the box are loaded, asked for or loading (-1: the module not found).
		int NavMeshesIn(const float* box)
		{
			const auto base = pattern::Module::Main().base;
			const auto* streaming = reinterpret_cast<const uint8_t*>(base + kStreaming);
			const auto* states = *reinterpret_cast<const uint32_t* const*>(streaming);
			const auto* ranges = *reinterpret_cast<const uint8_t* const*>(streaming + 0x1A0);
			const uint16_t modules = *reinterpret_cast<const uint16_t*>(streaming + 0x1A8);
			const uint8_t* navmeshes = nullptr;
			for (uint16_t m = 0; m < modules && !navmeshes; ++m)
			{
				const auto* module = *reinterpret_cast<const uint8_t* const*>(ranges + m * 16);
				if (module && *reinterpret_cast<const uintptr_t*>(module) == base + kNavMeshesVtable)
					navmeshes = module;
			}
			if (!navmeshes || !states)
				return -1;
			const uint32_t first = *reinterpret_cast<const uint32_t*>(navmeshes + 8);
			const int x0 = std::max(0, static_cast<int>(std::floor((box[0] - kNavOrigin) / kNavSquare))),
			          x1 = std::min(kNavWidth - 1, static_cast<int>(std::floor((box[2] - kNavOrigin) / kNavSquare)));
			const int y0 = std::max(0, static_cast<int>(std::floor((box[1] - kNavOrigin) / kNavSquare))),
			          y1 = std::min(kNavHeight - 1, static_cast<int>(std::floor((box[3] - kNavOrigin) / kNavSquare)));
			int busy = 0;
			for (int y = y0; y <= y1; ++y)
				for (int x = x0; x <= x1; ++x)
					busy += (states[2 * (first + static_cast<uint32_t>(y * kNavWidth + x)) + 1] & 3) != 0;
			return busy;
		}

		Land* Find(const std::string& name)
		{
			const auto it = std::find_if(g_lands.begin(), g_lands.end(), [&](const Land& l) { return l.name == name; });
			return it == g_lands.end() ? nullptr : &*it;
		}

		void Hold(int32_t player)
		{
			natives::Invoke<void>(kClearPedTasksImmediately, player);
			natives::Invoke<void>(kSetEntityCoords, player, kHold[0], kHold[1], kHold[2], 0, 0, 0, 0);
			natives::Invoke<void>(kFreezeEntityPosition, player, 1);
			natives::Invoke<void>(kSetFocusPosAndVel, kHold[0], kHold[1], kHold[2], 0.0f, 0.0f, 0.0f);
		}

		// Whether a restart's loading screen (or a gap in the ticks: story mode's scripts not running) came and went.
		bool Restarted(ULONGLONG spent, ULONGLONG gap)
		{
			if (natives::Invoke<int32_t>(kIsLoadingScreenActive) || gap > 1000)
				g_switch.loadingSeen = true;
			return (g_switch.loadingSeen && spent > 1000) || spent > 45000;
		}


		void Finish()
		{
			const int32_t player = natives::Invoke<int32_t>(kPlayerPedId);
			natives::Invoke<void>(kFreezeEntityPosition, player, 0);
			natives::Invoke<void>(kClearFocus);
			natives::Invoke<void>(kDoScreenFadeIn, 1000);
			g_switch = {};
		}
	}

	void Install()
	{
		const auto& disabled = config::Get().disabledAssets;
		std::error_code ec;
		for (const auto& entry : std::filesystem::directory_iterator(paths::Get().mods, ec))
		{
			if (!entry.is_directory())
				continue;
			const std::string name = entry.path().filename().string();
			std::ifstream in(entry.path() / L"land.json");
			if (!in || disabled.contains(name))
				continue;
			try
			{
				const auto j = nlohmann::json::parse(in);
				Land land{name, convert::Joaat(name)};
				for (int k = 0; k < 4; ++k)
					land.box[k] = j.at("box").at(k).get<float>();
				g_lands.push_back(std::move(land));
			}
			catch (const std::exception& e)
			{
				log::Warn("sectors: {}\\land.json: {}", name, e.what());
			}
		}
		std::sort(g_lands.begin(), g_lands.end(), [](const Land& a, const Land& b) { return a.name < b.name; });
		Settle(config::Get().worldSector);
		for (const Land& l : g_lands)
			if (l.dormant)
				log::Info("sectors: {} waits (it shares its place with the land there)", l.name);
	}

	bool Dormant(const std::string& pack)
	{
		return std::any_of(g_lands.begin(), g_lands.end(), [&](const Land& l) { return l.dormant && l.name == pack; });
	}

	bool DormantHash(uint32_t packHash)
	{
		return std::any_of(g_lands.begin(), g_lands.end(), [&](const Land& l) { return l.dormant && l.hash == packHash; });
	}

	std::string Active()
	{
		std::string out;
		for (const Land& l : g_lands)
			if (!l.dormant)
				out += (out.empty() ? "" : " ") + l.name;
		return out;
	}

	std::vector<std::string> Lands()
	{
		std::vector<std::string> out;
		for (const Land& l : g_lands)
			out.push_back(l.name);
		return out;
	}

	std::string Switch(const std::string& to, const float* at)
	{
		const Land* land = Find(to);
		if (!land)
			return "no land " + to;
		if (!land->dormant)
			return to + " is there already";
		if (g_switch.step != Step::None)
			return "a switch is under way";
		g_switch = {Step::Fade, to, at != nullptr, {at ? at[0] : 0, at ? at[1] : 0}, GetTickCount64()};
		natives::Invoke<void>(kDoScreenFadeOut, 800);
		log::Info("sectors: switching to {}", to);
		return "";
	}

	void Tick()
	{
		if (g_switch.step == Step::None)
			return;
		const ULONGLONG now = GetTickCount64(), spent = now - g_switch.since, gap = g_lastTick ? now - g_lastTick : 0;
		g_lastTick = now;
		const int32_t player = natives::Invoke<int32_t>(kPlayerPedId);
		Land* to = Find(g_switch.to);
		if (!to)
		{
			Finish();
			return;
		}
		switch (g_switch.step)
		{
		case Step::Fade:
			if (natives::Invoke<int32_t>(kIsScreenFadedOut) || spent > 3000)
			{
				Hold(player);
				g_switch.step = Step::PutAway;
				g_switch.since = now;
			}
			break;
		case Step::PutAway:
			// the old land's maps away (the sea and the map's pictures off it at once)
			natives::Invoke<void>(kLoadAllObjectsNow);
			for (Land& l : g_lands)
				if (&l != to && !l.dormant && Overlap(l, *to))
				{
					content::Revert(l.hash, kGroupMapSp);
					l.dormant = true;
					g_switch.leaving.push_back(l.name);
					log::Info("sectors: {}'s maps put away", l.name);
				}
			g_switch.step = Step::FirstRestart;
			g_switch.since = now;
			g_switch.loadingSeen = false;
			break;
		case Step::FirstRestart:
			if (Restarted(spent, gap))
			{
				Hold(player); // (the restart put the player at the save's place)
				g_switch.step = Step::Drain;
				g_switch.since = now;
			}
			break;
		case Step::Drain:
		{
			int busy = 0;
			for (const std::string& name : g_switch.leaving)
				if (const Land* l = Find(name))
					busy = std::max(busy, NavMeshesIn(l->box));
			if (busy <= 0)
			{
				if (busy < 0)
					log::Warn("sectors: the navmeshes' streaming module was not found; switching without waiting for them");
				g_switch.step = Step::Bring;
			}
			else if (spent > 60000)
			{
				// (given up: the old land's maps back)
				log::Warn("sectors: {} navmeshes of the old land still in use after 60 s; the switch to {} is given up", busy, to->name);
				for (const std::string& name : g_switch.leaving)
					if (Land* l = Find(name))
					{
						l->dormant = false;
						content::Execute(l->hash, kGroupMapSp);
					}
				Finish();
			}
			break;
		}
		case Step::Bring:
			// the old land's overlay (road network, navmeshes) out, the new one's in, the road network's regions dropped (they
			// load again from the new files), then its maps (the second restart)
			natives::Invoke<void>(kLoadAllObjectsNow);
			for (const std::string& name : g_switch.leaving)
				if (const Land* l = Find(name))
					content::Revert(l->hash, kGroupStartup);
			to->dormant = false;
			content::Execute(to->hash, kGroupStartup);
			{
				auto* paths = reinterpret_cast<uint8_t*>(pattern::Module::Main().base + kThePaths);
				paths[0x4BE7] = paths[0x4BE6] ^ 1;
			}
			content::Execute(to->hash, kGroupMapSp);
			config::Get().worldSector = to->name;
			config::Save();
			log::Info("sectors: {} is there now", to->name);
			g_switch.step = Step::SecondRestart;
			g_switch.since = now;
			g_switch.loadingSeen = false;
			break;
		case Step::SecondRestart:
			if (Restarted(spent, gap))
			{
				g_switch.step = Step::Arrive;
				g_switch.since = now;
				if (g_switch.place)
				{
					natives::Invoke<void>(kClearPedTasksImmediately, player);
					natives::Invoke<void>(kSetEntityCoords, player, g_switch.at[0], g_switch.at[1], 60.0f, 0, 0, 0, 0);
					natives::Invoke<void>(kFreezeEntityPosition, player, 1);
					natives::Invoke<void>(kSetFocusPosAndVel, g_switch.at[0], g_switch.at[1], 30.0f, 0.0f, 0.0f, 0.0f);
				}
			}
			break;
		case Step::Arrive:
		{
			if (!g_switch.place)
			{
				Finish();
				break;
			}
			// on the ground there, once its collision is in (or after 20 s, as it is)
			float z = 0;
			if (natives::Invoke<int32_t>(kGetGroundZFor3dCoord, g_switch.at[0], g_switch.at[1], 500.0f, &z, 0, 0) || spent > 20000)
			{
				natives::Invoke<void>(kSetEntityCoords, player, g_switch.at[0], g_switch.at[1], z + 1.0f, 0, 0, 0, 0);
				log::Info("sectors: the player at {:.0f}, {:.0f} in {}", g_switch.at[0], g_switch.at[1], to->name);
				Finish();
			}
			break;
		}
		case Step::None:
			break;
		}
	}
}
