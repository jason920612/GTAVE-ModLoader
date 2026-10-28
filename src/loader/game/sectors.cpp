#include "sectors.hpp"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "../config.hpp"
#include "../convert/archive.hpp"
#include "../convert/resource.hpp"
#include "../log.hpp"
#include "../paths.hpp"
#include "../pattern.hpp"
#include "../ui/notify.hpp"
#include "content.hpp"
#include "dlcpacks.hpp"
#include "landfiles.hpp"
#include "minimap.hpp"
#include "natives.hpp"
#include "text_override.hpp"

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
			bool registered = false; // (its files in the game's stores: there now, or put away and not given back yet)
			std::map<std::string, uint32_t> files; // (its pack's files by extension, once counted)
			// where its game coordinates are in the real world (land.json "projection": lat0, lon0, x0, y0, kx, ky - x = x0 +
			// (lon - lon0) kx, y = y0 + (lat - lat0) ky), its own box ("world") and the edges a neighbouring sector goes on
			// from ("open": n, s, e, w)
			bool projected = false;
			double proj[6]{};
			float world[4]{};
			std::string open;
			// its population zones (land.json "zones": [i, j, kind, name] for each 1 km block, tools/worldgen/zones.py)
			struct Zone
			{
				int i, j;
				char kind;
				std::string name;
			};
			std::vector<Zone> zones;

			void ToWorld(float x, float y, double& lat, double& lon) const
			{
				lon = proj[1] + (x - proj[2]) / proj[4];
				lat = proj[0] + (y - proj[3]) / proj[5];
			}
			void FromWorld(double lat, double lon, float& x, float& y) const
			{
				x = static_cast<float>(proj[2] + (lon - proj[1]) * proj[4]);
				y = static_cast<float>(proj[3] + (lat - proj[0]) * proj[5]);
			}
			bool Covers(double lat, double lon) const // (the real-world place its own box takes in)
			{
				double s, w, n, e;
				ToWorld(world[0], world[1], s, w);
				ToWorld(world[2], world[3], n, e);
				return projected && lat >= s && lat < n && lon >= w && lon < e;
			}
			void KeepFromEdges(float& x, float& y, float margin) const // (moved at least margin inside each of its open edges)
			{
				if (open.find('w') != std::string::npos)
					x = std::max(x, world[0] + margin);
				if (open.find('e') != std::string::npos)
					x = std::min(x, world[2] - margin);
				if (open.find('s') != std::string::npos)
					y = std::max(y, world[1] + margin);
				if (open.find('n') != std::string::npos)
					y = std::min(y, world[3] - margin);
			}
			bool AwayFromEdges(float x, float y, float margin) const // (at least margin inside each of its open edges)
			{
				const auto isOpen = [&](char side) { return open.find(side) != std::string::npos; };
				return (!isOpen('n') || y < world[3] - margin) && (!isOpen('s') || y > world[1] + margin) && (!isOpen('e') || x < world[2] - margin) &&
				       (!isOpen('w') || x > world[0] + margin);
			}
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
		                   kGetGroundZFor3dCoord = 0xC906A7DAB05C8D2B, kSetFocusPosAndVel = 0xBB7454BAFF08FE25, kClearFocus = 0x31B73D1EA9F01DA2,
		                   kIsPedInAnyVehicle = 0x997ABD671D25CA0B, kGetVehiclePedIsIn = 0x9A9112A0FE9A4713, kGetEntityModel = 0x9F47B058362C84B5,
		                   kGetVehicleColours = 0xA19435F193E081AC, kGetEntityHeading = 0xE83D4F9BA2A38914, kGetEntitySpeed = 0xD5037BA82E12416F,
		                   kGetEntityCoords = 0x3FEF770D40960D5A, kRequestModel = 0x963D27A58DF860AC, kHasModelLoaded = 0x98A4EB5D89A0C952,
		                   kGetClosestVehicleNodeWithHeading = 0xFF071FB798B803B0, kCreateVehicle = 0xAF35D0D2583051B0,
		                   kSetVehicleColours = 0x4F1D4BE3A7F24601, kSetPedIntoVehicle = 0xF75B0D629E1C063D, kSetVehicleOnGroundProperly = 0x49733E92263139D1,
		                   kSetVehicleForwardSpeed = 0xAB54A438726D25D5, kSetModelAsNoLongerNeeded = 0xE532F5D78798DAAB, kSetVehicleEngineOn = 0x2497C4717C8B881E;
		constexpr uint64_t kClearAreaOfVehicles = 0x01C7B9B38428AEB6, kClearAreaOfPeds = 0xBE31FD6CE464AC59, kGetRandomVehicleInSphere = 0x386F6CE5BAF6091C,
		                   kIsEntityAMissionEntity = 0x0A7B270912999B3C, kGetPedInVehicleSeat = 0xBB40DD2270B65366, kSetWaypointOff = 0xA7E4E2D361C2627F,
		                   kClearGpsPlayerWaypoint = 0xFF4FB7C8CDFA3DA7, kClearGpsMultiRoute = 0x67EEDEA1B9BAFD94, kClearGpsCustomRoute = 0xE6DE0561D9232A64;
		constexpr uint64_t kSetEntityHeading = 0x8E2530AA8ADA980E, kSetVehicleAsNoLongerNeeded = 0x629BFA74418D6239,
		                   kHasCollisionLoadedAroundEntity = 0xE9676F61BC0B3321, kRequestCollisionAtCoord = 0x07503F7948F491A7,
		                   kGetWaterHeight = 0xF6829842C06AE524;
		constexpr float kOldPlace = 3000; // (the old land's place cleared: this far round where the player was)
		// the edges: the switch when the player comes this near an open edge, and how far into the next land they arrive - no
		// further than needs be (a road found at least kEdge + 15 in): each crossing skips that much of the way, and one back
		// put them that much behind where they had turned
		constexpr float kEdge = 60, kArrive = 80;
		// where the player waits while a land is put away: Los Santos, away from any new land
		constexpr float kHold[3] = {200, -900, 31};

		// A switch under way (game thread, Tick), in two goes: the screen faded and the player held in Los Santos, the radar's
		// supertiles of the new land let go, the old land's maps put away (the map group's revert clears the game's world in
		// the call itself, as story mode <-> online: every entity gone), the player held again until the path server has
		// let the old land's navmeshes go (they are in its overlay, the start-up change set's: an archive's navmesh still
		// loaded when the archive is taken away stopped the game, 0xCE4CD0's int3), then the old land's overlay out, the
		// new one's in and its maps, the player put where they were going.
		enum class Step { None, Fade, Pictures, PutAway, SettleOld, Drain, Bring, SettleNew, Arrive };
		struct Switching
		{
			Step step = Step::None;
			std::string to;
			bool place = false;
			float at[2]{};
			ULONGLONG since = 0;
			std::vector<std::string> leaving; // (the lands put away)
			std::vector<bool> released;       // (their files given back to the stores)
			float from[3]{};                  // (where the player was)
			// the vehicle the player was in: the same one where they arrive - held in Los Santos meanwhile, a mission entity
			// of ours till then if it was not one (`own`) -, or one like it if it is gone (its model, colours, heading, speed)
			bool vehicle = false, modelAsked = false, own = false;
			int32_t handle = 0;
			uint32_t model = 0;
			int32_t colours[2]{};
			float heading = 0, speed = 0;
			// put down where they arrive: the player or their vehicle, held there until the ground round it is in
			int32_t landing = 0;
			ULONGLONG landingSince = 0;
			float landingZ = NAN; // (the ground's height there when it was chosen - a road's, or the ground found -, if known)
			// how long each step took (by Step), the one being timed and since when
			ULONGLONG took[9]{};
			Step timed = Step::None;
			ULONGLONG timedSince = 0;
			bool toldPictures = false;
		} g_switch;
		ULONGLONG g_lastWatch = 0, g_ended = 0;
		ULONGLONG g_lastAudit = 0; // (the stores looked over for objects loaded with none in them: landfiles::Audit)
		std::vector<std::string> g_noNeighbour; // (open edges with no land beyond, told once)

		// Los Santos's far views (research/phase0.md §63): the long-range and LOD maps of its areas - their names in
		// ModLoader\data\ls_far_maps.txt (worldgen ls-far: the game's own) - away while the player is on a new land (its
		// mountains stood over Florida's horizon), back once they are in Los Santos again (200 m out of every land's box); by
		// the camera's place.
		// Only those that were there are put away and brought back: a mission's swapped map stays as it was.
		constexpr uint64_t kRequestIpl = 0x41B4893843BBDB74, kRemoveIpl = 0xEE6C5AD3ECE0A82D, kIsIplActive = 0x88A741E44A2B3495,
		                   kGetFinalRenderedCamCoord = 0xA200EB1EE790F448;
		std::vector<std::string> g_farMaps, g_farAway; // (all; those put away)
		int g_far = -1;                                 // (-1 not known yet, 0 there, 1 away)
		ULONGLONG g_lastFar = 0;

		// Parked cars (research/phase0.md §63.3): the game's car generators on our lands made no car (their maps written -
		// CCarGen - and not one ever came), so the loader makes them: a land's parked.bin (worldgen build: each car's place on
		// a driveway, in a car park's stall, by a kerb - 'WBPK' version 1, the count, then x, y, z, heading, f32) read once
		// the camera is on it; the cars within 120 m of the camera made (a sedan, an SUV or a pickup, a common colour, by
		// the place) once the ground under them is in, those past 180 m taken away again; one the player gets into or moves
		// is the game's from then on - except in its first 8 s: one that fell or rolled then (made before the ground's
		// collision was in, after a teleport: nearly every car had gone that way) is made again later, up to three times.
		constexpr float kParkNear = 120, kParkFar = 180;
		constexpr uint64_t kDeleteVehicle = 0xEA386986E786A54F, kSetEntityAsMissionEntity = 0xAD738C3085FE7E11, kDoesEntityExist = 0x7239B21A38F536BA,
		                   kIsPedInVehicle = 0xA3EE4A07279BB9DB, kSetEntityAsNoLongerNeeded = 0xB736A491E64A32CF, kIsEntityAVehicle = 0x6AC7003FA6E5575E,
		                   kGetEntityUprightValue = 0x95EED5A694951F9F;
		struct ParkedCar
		{
			float x = 0, y = 0, z = 0, heading = 0;
			uint32_t model = 0;
			int32_t colour = 0;
			int32_t vehicle = 0; // (0 none, -1 taken: not made again)
			bool asked = false;
			ULONGLONG made = 0; // (when it was made)
			int tries = 0;
		};
		struct ParkedLand
		{
			bool read = false;
			std::vector<ParkedCar> cars;
			std::unordered_map<int64_t, std::vector<uint32_t>> cells; // (by 50 m cell)
		};
		std::map<std::string, ParkedLand> g_parked;
		std::vector<std::pair<std::string, uint32_t>> g_parkedLive; // (the cars made: their land, their place in its list)
		ULONGLONG g_lastPark = 0;

		// (the world cleared - a land put away, a save loaded -: the cars made are gone, and their numbers will be other
		// entities' - nothing done with them again)
		void ForgetParked()
		{
			for (auto& [land, k] : g_parkedLive)
				g_parked[land].cars[k].vehicle = 0;
			g_parkedLive.clear();
		}

		// (a land put away: the cars made taken away first - the loader's script made them, so the map group's revert left
		// them, and their road nodes were the old land's: the game read a node of the new land's road network in their place
		// and stopped, 0xF4A932, on the second switch)
		void DeleteParked()
		{
			const int32_t player = natives::Invoke<int32_t>(kPlayerPedId);
			for (auto& [land, k] : g_parkedLive)
			{
				ParkedCar& car = g_parked[land].cars[k];
				if (car.vehicle <= 0 || !natives::Invoke<int32_t>(kDoesEntityExist, car.vehicle) || !natives::Invoke<int32_t>(kIsEntityAVehicle, car.vehicle) ||
				    natives::Invoke<uint32_t>(kGetEntityModel, car.vehicle) != car.model)
					continue;
				// (the one the player is in: the game's - the switch makes it again where they arrive)
				if (natives::Invoke<int32_t>(kIsPedInVehicle, player, car.vehicle, 0))
					natives::Invoke<void>(kSetEntityAsNoLongerNeeded, &car.vehicle);
				else
				{
					natives::Invoke<void>(kSetEntityAsMissionEntity, car.vehicle, 1, 1);
					natives::Invoke<void>(kDeleteVehicle, &car.vehicle);
				}
			}
			ForgetParked();
		}

		int64_t ParkCell(float x, float y)
		{
			return static_cast<int64_t>(std::floor(x / 50)) << 32 | static_cast<uint32_t>(static_cast<int32_t>(std::floor(y / 50)));
		}

		void ReadParked(const Land& l, ParkedLand& pl)
		{
			pl.read = true;
			std::ifstream in(paths::Get().mods / l.name / L"parked.bin", std::ios::binary);
			char magic[4]{};
			uint32_t version = 0, count = 0;
			in.read(magic, 4);
			in.read(reinterpret_cast<char*>(&version), 4);
			in.read(reinterpret_cast<char*>(&count), 4);
			if (!in || std::memcmp(magic, "WBPK", 4) != 0 || version != 1 || count > 2000000)
				return;
			// (the Florida streets' cars: sedans, SUVs, pickups and minivans; common colours - white, black, greys, silver, a
			// few blue and red)
			static const char* const kModels[] = {"asea", "premier", "fugitive", "tailgater", "intruder", "primo", "stanier", "washington", "emperor",
			    "glendale", "baller", "granger", "cavalcade", "rebla", "landstalker", "huntley", "seminole", "gresley", "habanero", "fq2", "sadler", "bison",
			    "minivan", "serrano"};
			static const int32_t kColours[] = {111, 111, 111, 0, 0, 0, 4, 4, 5, 6, 7, 8, 13, 64, 62, 27, 112, 2};
			pl.cars.resize(count);
			for (uint32_t k = 0; k < count; ++k)
			{
				float rec[4];
				in.read(reinterpret_cast<char*>(rec), sizeof(rec));
				ParkedCar& c = pl.cars[k];
				c.x = rec[0], c.y = rec[1], c.z = rec[2], c.heading = rec[3];
				uint32_t h = static_cast<uint32_t>(std::lround(c.x * 10)) * 2654435761u ^ static_cast<uint32_t>(std::lround(c.y * 10)) * 40503u;
				h ^= h >> 15, h *= 0x2c1b3c6d, h ^= h >> 12;
				c.model = convert::Joaat(kModels[h % std::size(kModels)]);
				c.colour = kColours[(h >> 8) % std::size(kColours)];
				pl.cells[ParkCell(c.x, c.y)].push_back(k);
			}
			if (!in)
				pl.cars.clear(), pl.cells.clear();
			log::Info("sectors: {}: {} parked cars", l.name, pl.cars.size());
		}

		void WatchParked()
		{
			const ULONGLONG now = GetTickCount64();
			if (now - g_lastPark < 300)
				return;
			g_lastPark = now;
			const auto c = natives::Invoke<natives::ScrVector>(kGetFinalRenderedCamCoord);
			const Land* here = nullptr;
			for (const Land& l : g_lands)
				if (!l.dormant && c.x >= l.box[0] && c.x < l.box[2] && c.y >= l.box[1] && c.y < l.box[3])
				{
					here = &l;
					break;
				}
			// those made: taken away far off (or off the land), the game's once the player is in one or it was moved
			const int32_t player = natives::Invoke<int32_t>(kPlayerPedId);
			for (size_t k = 0; k < g_parkedLive.size();)
			{
				ParkedCar& car = g_parked[g_parkedLive[k].first].cars[g_parkedLive[k].second];
				// (still the car made there: a vehicle of its model - else its number is someone else's now)
				bool gone = !natives::Invoke<int32_t>(kDoesEntityExist, car.vehicle) || !natives::Invoke<int32_t>(kIsEntityAVehicle, car.vehicle) ||
				            natives::Invoke<uint32_t>(kGetEntityModel, car.vehicle) != car.model;
				if (gone)
					car.vehicle = 0;
				else
				{
					const auto p = natives::Invoke<natives::ScrVector>(kGetEntityCoords, car.vehicle, 1);
					const bool inside = natives::Invoke<int32_t>(kIsPedInVehicle, player, car.vehicle, 0) != 0;
					// (on its side or roof - made where something stood: taken away, the place not used again; fallen or rolled
					// in its first seconds - the ground's collision not in yet -: made again later)
					const bool settling = now - car.made < 8000 && car.tries < 3;
					const bool moved = std::hypot(p.x - car.x, p.y - car.y) > 3 || std::fabs(p.z - car.z) > 3;
					if (!inside && (natives::Invoke<float>(kGetEntityUprightValue, car.vehicle) < 0.5f || (settling && moved)))
					{
						natives::Invoke<void>(kSetEntityAsMissionEntity, car.vehicle, 1, 1);
						natives::Invoke<void>(kDeleteVehicle, &car.vehicle);
						car.vehicle = settling ? 0 : -1;
						gone = true;
					}
					else if (inside || moved)
					{
						natives::Invoke<void>(kSetEntityAsNoLongerNeeded, &car.vehicle);
						car.vehicle = -1;
						gone = true;
					}
					else if (!here || g_parkedLive[k].first != here->name || std::hypot(c.x - car.x, c.y - car.y) > kParkFar)
					{
						natives::Invoke<void>(kSetEntityAsMissionEntity, car.vehicle, 1, 1);
						natives::Invoke<void>(kDeleteVehicle, &car.vehicle);
						car.vehicle = 0;
						gone = true;
					}
				}
				if (gone)
				{
					g_parkedLive[k] = g_parkedLive.back();
					g_parkedLive.pop_back();
				}
				else
					++k;
			}
			if (!here)
				return;
			ParkedLand& pl = g_parked[here->name];
			if (!pl.read)
				ReadParked(*here, pl);
			// the near ones made, a few each time (each once its model is in)
			int made = 0;
			const int reach = static_cast<int>(std::ceil(kParkNear / 50));
			for (int dj = -reach; dj <= reach; ++dj)
				for (int di = -reach; di <= reach; ++di)
				{
					const auto it = pl.cells.find(ParkCell(c.x + di * 50.0f, c.y + dj * 50.0f));
					if (it == pl.cells.end())
						continue;
					for (const uint32_t k : it->second)
					{
						ParkedCar& car = pl.cars[k];
						if (car.vehicle != 0 || std::hypot(c.x - car.x, c.y - car.y) > kParkNear)
							continue;
						if (!car.asked)
						{
							natives::Invoke<void>(kRequestModel, car.model);
							car.asked = true;
							continue;
						}
						if (made >= 8 || !natives::Invoke<int32_t>(kHasModelLoaded, car.model))
							continue;
						// (the ground under it in: its collision found within 3 m of its place)
						float gz = 0;
						if (!natives::Invoke<int32_t>(kGetGroundZFor3dCoord, car.x, car.y, car.z + 3.0f, &gz, 0, 0) || std::fabs(gz - car.z) > 3)
							continue;
						const int32_t v = natives::Invoke<int32_t>(kCreateVehicle, car.model, car.x, car.y, car.z + 0.3f, car.heading, 0, 0, 0);
						if (!v)
							continue;
						car.made = now;
						++car.tries;
						natives::Invoke<void>(kSetVehicleOnGroundProperly, v, 5.0f);
						natives::Invoke<void>(kSetVehicleColours, v, car.colour, car.colour);
						car.vehicle = v;
						g_parkedLive.push_back({here->name, k});
						++made;
					}
				}
		}

		// The map types store (fwMapTypesStore: the streaming modules' tenth, research/phase0.md §63.4): its slot entries
		// (32 bytes) at +0x40, as many used as +0x58 says; an entry's +0xC its name's hash, +0x10 its flags. Our lands'
		// archetype files carry 0x10 (as the game's DLC ones do: 0xfa), and with it the store's Remove (vtable +0x48, 0x937770)
		// leaves the archetype factories' blocks of the slot in place (0xfe9e40 not called): the land's next coming loaded
		// the file again, the factories' Reserve found the old block full, and the archetypes ran past its end - garbage
		// for a model info, the crash at 0x92beab on going back to a land. A land's archetype files lose the bit before the
		// land is put away, so that their Remove cleans up as for any other file.
		constexpr uintptr_t kMapTypesStore = 0x3ED6620, kMapTypesStoreVtable = 0x2612380;
		void LetArchetypeFilesGo(const std::string& land)
		{
			const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
			auto* store = reinterpret_cast<uint8_t*>(base + kMapTypesStore);
			if (*reinterpret_cast<uintptr_t*>(store) != base + kMapTypesStoreVtable)
			{
				log::Warn("sectors: the map types store is not where this build has it; {}'s archetype files left as they are", land);
				return;
			}
			uint8_t* entries = *reinterpret_cast<uint8_t**>(store + 0x40);
			const uint32_t stride = *reinterpret_cast<uint32_t*>(store + 0x54), used = *reinterpret_cast<uint32_t*>(store + 0x58);
			if (!entries || stride < 0x14 || used > 100000)
				return;
			std::set<uint32_t> names{convert::Joaat(land)};
			for (int k = 1; k < 256; ++k)
				names.insert(convert::Joaat(std::format("{}_{}", land, k)));
			int changed = 0;
			for (uint32_t slot = 0; slot < used; ++slot)
			{
				uint8_t* e = entries + static_cast<size_t>(slot) * stride;
				if (names.contains(*reinterpret_cast<uint32_t*>(e + 0xC)))
				{
					auto& flags = *reinterpret_cast<uint16_t*>(e + 0x10);
					if (flags & 0x14)
						flags = static_cast<uint16_t>(flags & ~0x14), ++changed;
				}
			}
			log::Info("sectors: {}: {} archetype files to be let go in full", land, changed);
		}

		void WatchFar()
		{
			const ULONGLONG now = GetTickCount64();
			if (g_farMaps.empty() || now - g_lastFar < 1000)
				return;
			g_lastFar = now;
			// (where the picture is taken from: the camera's place - a scripted camera's too, the player left elsewhere)
			const auto p = natives::Invoke<natives::ScrVector>(kGetFinalRenderedCamCoord);
			bool on = false, away = true;
			for (const Land& l : g_lands)
			{
				if (l.dormant)
					continue;
				on = on || (p.x >= l.box[0] && p.x <= l.box[2] && p.y >= l.box[1] && p.y <= l.box[3]);
				away = away && (p.x < l.box[0] - 200 || p.x > l.box[2] + 200 || p.y < l.box[1] - 200 || p.y > l.box[3] + 200);
			}
			if (on && g_far != 1)
			{
				g_farAway.clear();
				for (const std::string& name : g_farMaps)
					if (natives::Invoke<int32_t>(kIsIplActive, name.c_str()))
					{
						natives::Invoke<void>(kRemoveIpl, name.c_str());
						g_farAway.push_back(name);
					}
				g_far = 1;
				log::Info("sectors: on a new land; {} of Los Santos's {} far maps put away", g_farAway.size(), g_farMaps.size());
			}
			else if (away && g_far != 0)
			{
				for (const std::string& name : g_farAway)
					natives::Invoke<void>(kRequestIpl, name.c_str());
				if (g_far == 1)
					log::Info("sectors: in Los Santos; its {} far maps back", g_farAway.size());
				g_farAway.clear();
				g_far = 0;
			}
		}
		std::vector<std::string> g_unmountable; // (lands whose pack could not be mounted: not switched to again)

		// The population zones every land shares (the mod tools/worldgen/zones.py --mod makes): each 1 km block of the box
		// (i 4..16, j -3..10) as three zones, W<ii><jj><kind> (i and j + 3 in two digits: the game keeps 7 characters of a
		// zone's name) - B beach, U town, R country, each bound like a game zone of its kind - named by the label W<ii><jj>.
		// Of each block's three the one the land there has is on, the others
		// off (SET_ZONE_ENABLED: the game's position lookup, 0xD8D7B0, skips a zone whose enabled bit is clear); the label
		// shows the block's place.
		constexpr int kZoneI0 = 4, kZoneI1 = 16, kZoneJ0 = -3, kZoneJ1 = 10;
		constexpr char kZoneKinds[3] = {'B', 'U', 'R'};
		constexpr uint64_t kGetZoneFromNameId = 0x98CD1D2934B76CC1, kSetZoneEnabled = 0xBA5ECEEA120E5611;
		std::vector<int32_t> g_zoneIds; // (per block and kind; -1: the game has no such zone)
		std::string g_zonesOf;          // (the land whose zones are on)

		// The land's zones on, the others off, its blocks' names shown. Game thread, story mode.
		void ApplyZones(const Land& land)
		{
			const int blocks = (kZoneI1 - kZoneI0 + 1) * (kZoneJ1 - kZoneJ0 + 1);
			if (g_zoneIds.empty())
			{
				g_zoneIds.resize(static_cast<size_t>(blocks) * 3);
				int found = 0;
				for (int b = 0; b < blocks; ++b)
					for (int k = 0; k < 3; ++k)
					{
						const std::string name = std::format("W{:02}{:02}{}", kZoneI0 + b % (kZoneI1 - kZoneI0 + 1), b / (kZoneI1 - kZoneI0 + 1), kZoneKinds[k]);
						const int32_t id = natives::Invoke<int32_t>(kGetZoneFromNameId, name.c_str());
						g_zoneIds[static_cast<size_t>(b) * 3 + k] = id;
						found += id >= 0;
					}
				log::Info("sectors: {} of the lands' {} population zones found", found, blocks * 3);
			}
			// (a land without zones of its own: its blocks a town's, named after the land)
			std::vector<char> kind(static_cast<size_t>(blocks), 0);
			std::vector<std::string> name(static_cast<size_t>(blocks));
			for (const Land::Zone& z : land.zones)
				if (z.i >= kZoneI0 && z.i <= kZoneI1 && z.j >= kZoneJ0 && z.j <= kZoneJ1)
				{
					const size_t b = static_cast<size_t>(z.j - kZoneJ0) * (kZoneI1 - kZoneI0 + 1) + (z.i - kZoneI0);
					kind[b] = z.kind == 'S' ? 'U' : z.kind, name[b] = z.name; // (suburbs: a town's, as the kinds once were four)
				}
			if (land.zones.empty())
				std::fill(kind.begin(), kind.end(), 'U');
			int on = 0;
			for (int b = 0; b < blocks; ++b)
			{
				for (int k = 0; k < 3; ++k)
					if (const int32_t id = g_zoneIds[static_cast<size_t>(b) * 3 + k]; id >= 0)
					{
						const bool enabled = kind[b] == kZoneKinds[k];
						natives::Invoke<void>(kSetZoneEnabled, id, enabled ? 1 : 0);
						on += enabled;
					}
				const std::string label = std::format("W{:02}{:02}", kZoneI0 + b % (kZoneI1 - kZoneI0 + 1), b / (kZoneI1 - kZoneI0 + 1));
				text_override::Set(convert::Joaat(label), name[b].empty() ? land.name : name[b]);
			}
			g_zonesOf = land.name;
			log::Info("sectors: {}'s population zones on ({} blocks)", land.name, on);
		}

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

		// The road junctions the traffic keeps (128 of 0x890 bytes at +0x43E18E0: +0x7D4 how many road nodes one is made of,
		// their addresses at +0x7D8 - the region in the low 16 bits, the node in the high): made round the player and kept a
		// while, their nodes those of the land they were on. With the old land's road network let go and the new one's read
		// (the same regions, other nodes), the game took a junction's old node from the new region - another node, or none -
		// and stopped (0xF4A932) a few seconds after the switch. Each is cleared with the game's own reset (+0xF443C0: the
		// nodes, its vehicles' references, its lights) before the networks change.
		constexpr uintptr_t kJunctions = 0x43E18E0, kClearJunction = 0xF443C0;
		constexpr int kJunctionCount = 128, kJunctionSize = 0x890;
		constexpr uint8_t kClearJunctionStart[] = {0x56, 0x57, 0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xCE, 0xC7, 0x81, 0xD4, 0x07, 0x00, 0x00};

		void ClearJunctions()
		{
			const auto base = pattern::Module::Main().base;
			if (std::memcmp(reinterpret_cast<const void*>(base + kClearJunction), kClearJunctionStart, sizeof(kClearJunctionStart)) != 0)
			{
				log::Warn("sectors: the junctions' reset is not where it was; the traffic's junctions are left as they are");
				return;
			}
			const auto clear = reinterpret_cast<void (*)(uint8_t*)>(base + kClearJunction);
			int cleared = 0;
			for (int k = 0; k < kJunctionCount; ++k)
			{
				auto* junction = reinterpret_cast<uint8_t*>(base + kJunctions + static_cast<uintptr_t>(k) * kJunctionSize);
				if (*reinterpret_cast<const int32_t*>(junction + 0x7D4) > 0)
					clear(junction), ++cleared;
			}
			if (cleared)
				log::Info("sectors: {} road junction(s) cleared", cleared);
		}

		// The old land's place emptied before the road networks change: the traffic the population makes there once the world
		// is cleared (a car with a driver came within 3 s of a land's map group put away, the player still there) holds the
		// old land's road nodes, and junctions are made from them again. What is left after is logged.
		void ClearOldPlace()
		{
			const float x = g_switch.from[0], y = g_switch.from[1], z = g_switch.from[2];
			natives::Invoke<void>(kClearAreaOfVehicles, x, y, z, kOldPlace, 0, 0, 0, 0, 0, 0, 0);
			natives::Invoke<void>(kClearAreaOfPeds, x, y, z, kOldPlace, 1);
			for (const int32_t flags : {0, 70})
				if (const int32_t v = natives::Invoke<int32_t>(kGetRandomVehicleInSphere, x, y, z, kOldPlace, 0, flags))
				{
					const auto p = natives::Invoke<natives::ScrVector>(kGetEntityCoords, v, 1);
					log::Info("sectors: a vehicle still on the old land's place, at {:.0f}, {:.0f} (a mission's: {}, driven: {})", p.x, p.y,
					    natives::Invoke<int32_t>(kIsEntityAMissionEntity, v) != 0, natives::Invoke<int32_t>(kGetPedInVehicleSeat, v, -1, 0) != 0);
					break;
				}
		}

		// The player's road node (the player info - the local player's ped, +0x434A958's +8, its +0x10A8 -: +0x290 the node,
		// +0x1050 the link from it, 0xFFFF none): not looked for again while they wait in Los Santos, then read with the new
		// land's road network - another region's node or none, and the game stopped (0xF02D58) a second after the switch.
		// Set to none (a region past 0x3FF) before the networks change, as the code reading it (0xF02CE3) checks.
		constexpr uintptr_t kWorldPlayer = 0x434A958, kReadsPlayerNode = 0xF02CE3;
		constexpr uint8_t kReadsPlayerNodeCode[] = {0x48, 0x8B, 0x87, 0xA8, 0x10, 0x00, 0x00, 0x44, 0x8B, 0xA0, 0x90, 0x02, 0x00, 0x00};

		void ForgetPlayerRoadNode()
		{
			const auto base = pattern::Module::Main().base;
			if (std::memcmp(reinterpret_cast<const void*>(base + kReadsPlayerNode), kReadsPlayerNodeCode, sizeof(kReadsPlayerNodeCode)) != 0)
			{
				log::Warn("sectors: the player's road node is not where it was; it is left as it is");
				return;
			}
			const auto* world = *reinterpret_cast<const uint8_t* const*>(base + kWorldPlayer);
			const auto* ped = world ? *reinterpret_cast<const uint8_t* const*>(world + 8) : nullptr;
			auto* info = ped ? *reinterpret_cast<uint8_t* const*>(ped + 0x10A8) : nullptr;
			if (!info)
				return;
			*reinterpret_cast<uint32_t*>(info + 0x290) = 0xFFFFFFFF;
			*reinterpret_cast<uint16_t*>(info + 0x1050) = 0xFFFF;
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

		// The stores a land's files take slots in, by extension (the store is its streaming module: found by its vtable;
		// its slot pool at +0x38 - +0x18 the size, +0x28 the slots used in the low 30 bits). A map group's revert leaves its
		// archives' files registered (research/phase0.md §63.12): a land put away gives them back (landfiles.cpp) - before
		// that the stores had filled up on the second big land (fwBasePool::New, +0x15A7C0, breaks into int3 with no slot
		// free).
		struct Store
		{
			const char* extension;
			uintptr_t vtable;
			const char* name;
		};
		constexpr Store kStores[] = {{"ydr", 0x2611D80, "DrawableStore"}, {"ytd", 0x2612BC0, "TxdStore"}, {"ybn", 0x2612820, "StaticBounds"},
		    {"ymap", 0x2612200, "MapDataStore"}, {"ydd", 0x2611F00, "DwdStore"}, {"ytyp", 0x2612380, "MapTypesStore"}, {"yft", 0x2612080, "FragmentStore"}};
		// (slots kept free for the game's own: script-made dictionaries and the like)
		constexpr uint32_t kStoreSpare = 500;

		bool StoreRoom(uintptr_t vtable, uint32_t& size, uint32_t& used)
		{
			const auto base = pattern::Module::Main().base;
			const auto* streaming = reinterpret_cast<const uint8_t*>(base + kStreaming);
			const auto* ranges = *reinterpret_cast<const uint8_t* const*>(streaming + 0x1A0);
			const uint16_t modules = *reinterpret_cast<const uint16_t*>(streaming + 0x1A8);
			for (uint16_t m = 0; ranges && m < modules; ++m)
			{
				const auto* module = *reinterpret_cast<const uint8_t* const*>(ranges + m * 16);
				if (module && *reinterpret_cast<const uintptr_t*>(module) == base + vtable)
				{
					size = *reinterpret_cast<const uint32_t*>(module + 0x38 + 0x18);
					used = *reinterpret_cast<const uint32_t*>(module + 0x38 + 0x28) & 0x3FFFFFFF;
					return true;
				}
			}
			return false;
		}

		void CountFiles(const convert::Archive& archive, std::map<std::string, uint32_t>& out)
		{
			const auto& nodes = archive.Nodes();
			for (uint32_t i = 0; i < nodes.size(); ++i)
			{
				const auto& n = nodes[i];
				if (n.directory)
					continue;
				if (!n.resource && !n.stored && n.name.ends_with(".rpf"))
				{
					convert::Archive inner;
					std::string error;
					if (archive.OpenNested(i, inner, error))
						CountFiles(inner, out);
					continue;
				}
				if (const size_t dot = n.name.rfind('.'); dot != std::string::npos)
					++out[n.name.substr(dot + 1)];
			}
		}

		// The stores that have no room for the land's files ("" when all have), its files counted once.
		std::string NoRoomFor(Land& land)
		{
			if (land.files.empty())
			{
				const auto path = paths::Get().mods / land.name / L"dlc.rpf";
				std::error_code ec;
				auto in = std::make_shared<std::ifstream>(path, std::ios::binary);
				convert::Archive archive;
				std::string error;
				if (!*in || !archive.Open(in, 0, std::filesystem::file_size(path, ec), "dlc.rpf", dlcpacks::Decryptor(), error))
				{
					log::Warn("sectors: {}'s pack not read ({}); its files not counted", land.name, error);
					return "";
				}
				CountFiles(archive, land.files);
			}
			std::string full;
			for (const Store& s : kStores)
			{
				const auto it = land.files.find(s.extension);
				uint32_t size = 0, used = 0;
				if (it == land.files.end() || !StoreRoom(s.vtable, size, used))
					continue;
				if (used + it->second + kStoreSpare > size)
					full += std::format("{}{} ({} used of {}, {} more wanted)", full.empty() ? "" : ", ", s.name, used, size, it->second);
			}
			return full;
		}

		void Hold(int32_t player)
		{
			natives::Invoke<void>(kClearPedTasksImmediately, player);
			natives::Invoke<void>(kSetEntityCoords, player, kHold[0], kHold[1], kHold[2], 0, 0, 0, 0);
			natives::Invoke<void>(kFreezeEntityPosition, player, 1);
			natives::Invoke<void>(kSetFocusPosAndVel, kHold[0], kHold[1], kHold[2], 0.0f, 0.0f, 0.0f);
		}

		// Whether the game has had half a second of frames since the maps changed (`spent` from the end of the change's own
		// long frame), with no loading screen (or one up for a minute). (A switch once waited 45 s here for a session
		// restart that does not come: it had been taken for one by that long frame.)
		bool Settled(ULONGLONG spent)
		{
			return spent > 500 && (!natives::Invoke<int32_t>(kIsLoadingScreenActive) || spent > 60000);
		}


		void Finish();

		// A switch given up once the old land's maps went: they come back (its files registered anew), and the player is put
		// where they were - kept from its open edges, or the edge would start the switch again - by the arrival's own steps.
		void BackToOldLand()
		{
			std::string back;
			for (const std::string& name : g_switch.leaving)
				if (Land* l = Find(name))
				{
					l->dormant = false;
					l->registered = true;
					content::Execute(l->hash, kGroupMapSp);
					landfiles::PutBoxesBack(l->name);
					back = l->name;
				}
			const Land* l = Find(back);
			if (!l)
			{
				Finish();
				return;
			}
			g_switch.to = back;
			g_switch.at[0] = g_switch.from[0], g_switch.at[1] = g_switch.from[1];
			l->KeepFromEdges(g_switch.at[0], g_switch.at[1], kEdge + kArrive);
			g_switch.place = true;
			g_switch.leaving.clear();
			g_switch.released.clear();
			g_switch.step = Step::SettleNew;
			g_switch.since = GetTickCount64();
		}

		void Finish()
		{
			const int32_t player = natives::Invoke<int32_t>(kPlayerPedId);
			natives::Invoke<void>(kFreezeEntityPosition, player, 0);
			natives::Invoke<void>(kClearFocus);
			natives::Invoke<void>(kDoScreenFadeIn, 1000);
			minimap::Resume();
			landfiles::Audit("the switch over");
			if (g_switch.timed != Step::None)
			{
				auto& took = g_switch.took;
				took[static_cast<size_t>(g_switch.timed)] += GetTickCount64() - g_switch.timedSince;
				ULONGLONG total = 0;
				for (const ULONGLONG t : took)
					total += t;
				log::Info("sectors: the switch took {:.1f} s (fade {:.1f}, pictures {:.1f}, maps away {:.1f}, settling {:.1f}, files back {:.1f}, the new "
				          "land in {:.1f}, settling {:.1f}, arriving {:.1f})",
				    total / 1000.0, took[1] / 1000.0, took[2] / 1000.0, took[3] / 1000.0, took[4] / 1000.0, took[5] / 1000.0, took[6] / 1000.0,
				    took[7] / 1000.0, took[8] / 1000.0);
			}
			g_switch = {};
			g_ended = GetTickCount64();
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
				if (j.contains("projection") && j.contains("world"))
				{
					for (int k = 0; k < 6; ++k)
						land.proj[k] = j["projection"].at(k).get<double>();
					for (int k = 0; k < 4; ++k)
						land.world[k] = j["world"].at(k).get<float>();
					land.projected = land.proj[4] != 0 && land.proj[5] != 0;
					land.open = j.value("open", "");
				}
				if (j.contains("zones"))
					for (const auto& z : j["zones"])
						if (z.size() >= 4 && z[2].is_string() && !z[2].get<std::string>().empty())
							land.zones.push_back({z[0].get<int>(), z[1].get<int>(), z[2].get<std::string>()[0], z[3].get<std::string>()});
				g_lands.push_back(std::move(land));
			}
			catch (const std::exception& e)
			{
				log::Warn("sectors: {}\\land.json: {}", name, e.what());
			}
		}
		std::sort(g_lands.begin(), g_lands.end(), [](const Land& a, const Land& b) { return a.name < b.name; });
		Settle(config::Get().worldSector);
		if (!g_lands.empty())
		{
			std::ifstream list(paths::Get().root / L"data" / L"ls_far_maps.txt");
			for (std::string line; std::getline(list, line);)
			{
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				if (!line.empty())
					g_farMaps.push_back(line);
			}
			if (g_farMaps.empty())
				log::Warn("sectors: no ModLoader\\data\\ls_far_maps.txt (worldgen ls-far): Los Santos's mountains stay on the lands' horizon");
		}
		for (Land& l : g_lands)
		{
			l.registered = !l.dormant;
			if (l.dormant)
				log::Info("sectors: {} waits (it shares its place with the land there)", l.name);
		}
		// (a restart file of an earlier loader: gone)
		std::filesystem::remove(paths::Get().root / L"data" / L"sector_next.json", ec);
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
		Land* land = Find(to);
		if (!land)
			return "no land " + to;
		if (!land->dormant)
			return to + " is there already";
		if (g_switch.step != Step::None)
			return "a switch is under way";
		const int32_t player = natives::Invoke<int32_t>(kPlayerPedId);
		// (no place given: where the player is now - a save loaded in the other land's place)
		const auto here = natives::Invoke<natives::ScrVector>(kGetEntityCoords, player, 1);
		const float where[2] = {at ? at[0] : here.x, at ? at[1] : here.y};
		g_switch = {Step::Fade, to, true, {where[0], where[1]}, GetTickCount64()};
		g_switch.from[0] = here.x, g_switch.from[1] = here.y, g_switch.from[2] = here.z;
		if (natives::Invoke<int32_t>(kIsPedInAnyVehicle, player, 0))
		{
			const int32_t v = natives::Invoke<int32_t>(kGetVehiclePedIsIn, player, 0);
			g_switch.vehicle = true;
			g_switch.handle = v;
			g_switch.model = natives::Invoke<uint32_t>(kGetEntityModel, v);
			natives::Invoke<void>(kGetVehicleColours, v, &g_switch.colours[0], &g_switch.colours[1]);
			g_switch.heading = natives::Invoke<float>(kGetEntityHeading, v);
			g_switch.speed = natives::Invoke<float>(kGetEntitySpeed, v);
		}
		natives::Invoke<void>(kDoScreenFadeOut, 800);
		DeleteParked();
		log::Info("sectors: switching to {}", to);
		landfiles::Audit("before the switch to " + to);
		return "";
	}

	namespace
	{
		// The player at an open edge of the land there (a neighbouring sector's side: land.json "open"): the sector whose
		// own box takes in the real world's place just past it, there (kArrive into it), in the vehicle they are in.
		void WatchEdges()
		{
			const ULONGLONG now = GetTickCount64();
			if (now - g_lastWatch < 250)
				return;
			g_lastWatch = now;
			const int32_t player = natives::Invoke<int32_t>(kPlayerPedId);
			const int32_t vehicle = natives::Invoke<int32_t>(kIsPedInAnyVehicle, player, 0) ? natives::Invoke<int32_t>(kGetVehiclePedIsIn, player, 0) : 0;
			const auto p = natives::Invoke<natives::ScrVector>(kGetEntityCoords, vehicle ? vehicle : player, 1);
			for (const Land& l : g_lands)
			{
				if (l.dormant || !l.projected || l.open.empty() || p.x < l.world[0] - 500 || p.x > l.world[2] + 500 || p.y < l.world[1] - 500 ||
				    p.y > l.world[3] + 500)
					continue;
				struct Edge
				{
					char side;
					bool close;
					float beyond[2], inward[2];
				};
				const Edge edges[] = {{'n', p.y > l.world[3] - kEdge, {p.x, l.world[3] + 1}, {0, 1}},
				    {'s', p.y < l.world[1] + kEdge, {p.x, l.world[1] - 1}, {0, -1}}, {'e', p.x > l.world[2] - kEdge, {l.world[2] + 1, p.y}, {1, 0}},
				    {'w', p.x < l.world[0] + kEdge, {l.world[0] - 1, p.y}, {-1, 0}}};
				for (const Edge& e : edges)
				{
					if (!e.close || l.open.find(e.side) == std::string::npos)
						continue;
					double lat, lon;
					l.ToWorld(e.beyond[0], e.beyond[1], lat, lon);
					const auto next = std::find_if(g_lands.begin(), g_lands.end(), [&](const Land& o) {
						return &o != &l && o.Covers(lat, lon) && std::find(g_unmountable.begin(), g_unmountable.end(), o.name) == g_unmountable.end();
					});
					if (next == g_lands.end())
					{
						const std::string key = l.name + e.side;
						if (std::find(g_noNeighbour.begin(), g_noNeighbour.end(), key) == g_noNeighbour.end())
						{
							g_noNeighbour.push_back(key);
							log::Warn("sectors: {}'s {} edge is open, but no land takes in the place past it ({:.5f}, {:.5f})", l.name, e.side, lat, lon);
						}
						continue;
					}
					float at[2];
					next->FromWorld(lat, lon, at[0], at[1]);
					at[0] += e.inward[0] * kArrive, at[1] += e.inward[1] * kArrive;
					log::Info("sectors: the player at {}'s {} edge ({:.0f}, {:.0f}): on to {} at ({:.0f}, {:.0f})", l.name, e.side, p.x, p.y, next->name, at[0], at[1]);
					Switch(next->name, at);
					return;
				}
			}
		}

	}

	void Tick()
	{
		if (g_switch.step == Step::None)
		{
			// (the zones of the land there, once the game has its zones: story mode is running)
			if (g_zonesOf.empty() && text_override::Ready())
				for (const Land& l : g_lands)
					if (!l.dormant && l.projected)
					{
						ApplyZones(l);
						break;
					}
			WatchFar();
			WatchEdges();
			WatchParked();
			if (const ULONGLONG now = GetTickCount64(); now - g_lastAudit > 5000)
			{
				g_lastAudit = now;
				landfiles::Audit("now");
			}
			return;
		}
		const ULONGLONG now = GetTickCount64(), spent = now - g_switch.since;
		if (g_switch.step != g_switch.timed)
		{
			if (g_switch.timed != Step::None)
				g_switch.took[static_cast<size_t>(g_switch.timed)] += now - g_switch.timedSince;
			g_switch.timed = g_switch.step;
			g_switch.timedSince = now;
		}
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
				// (a land held back at the start: its pack mounted now; one that cannot be is not gone to again)
				if (const std::string error = dlcpacks::MountLater(to->name); !error.empty())
				{
					log::Warn("sectors: {} could not be mounted ({}); the switch is given up", to->name, error);
					g_unmountable.push_back(to->name);
					Finish();
					break;
				}
				Hold(player);
				// (their vehicle held beside them, out of the way of the old place's clearing - and not left where nothing is
				// under it once the old land's maps go)
				if (g_switch.handle && natives::Invoke<int32_t>(kDoesEntityExist, g_switch.handle))
				{
					if (!natives::Invoke<int32_t>(kIsEntityAMissionEntity, g_switch.handle))
					{
						natives::Invoke<void>(kSetEntityAsMissionEntity, g_switch.handle, 1, 1);
						g_switch.own = true;
					}
					natives::Invoke<void>(kSetEntityCoords, g_switch.handle, kHold[0] + 25.0f, kHold[1], kHold[2], 0, 0, 0, 0);
					natives::Invoke<void>(kFreezeEntityPosition, g_switch.handle, 1);
				}
				// the lands put away: those there in the new one's place - their pause map pictures let go first, while their
				// archives are still there
				for (const Land& l : g_lands)
					if (&l != to && !l.dormant && Overlap(l, *to))
						g_switch.leaving.push_back(l.name), g_switch.released.push_back(false);
				minimap::Going(g_switch.leaving);
				g_switch.step = Step::Pictures;
				g_switch.since = now;
			}
			break;
		case Step::Pictures:
		{
			bool done = true;
			std::string held;
			const bool tell = spent > 2000 && !g_switch.toldPictures;
			for (const std::string& name : g_switch.leaving)
				done = landfiles::LetPicturesGo(name, paths::Get().mods / name / L"dlc.rpf", tell ? &held : nullptr) && done;
			if (tell && !done)
			{
				g_switch.toldPictures = true;
				log::Info("sectors: the pause map's pictures still held after 2 s: {}", held);
			}
			if (done || spent > 10000)
			{
				if (!done)
					log::Warn("sectors: the pause map's pictures of the land put away still held after 10 s; its maps go all the same");
				g_switch.step = Step::PutAway;
				g_switch.since = now;
			}
			break;
		}
		case Step::PutAway:
			// the old land's maps away (the sea and the map's pictures off it at once; the radar's supertiles let go of the
			// old land's dictionaries first)
			natives::Invoke<void>(kLoadAllObjectsNow);
			minimap::Suspend();
			// (the map's waypoint and the GPS routes: their road nodes the old land's, their places another land's in the new
			// one)
			natives::Invoke<void>(kSetWaypointOff);
			natives::Invoke<void>(kClearGpsPlayerWaypoint);
			natives::Invoke<void>(kClearGpsMultiRoute);
			natives::Invoke<void>(kClearGpsCustomRoute);
			for (const std::string& name : g_switch.leaving)
				if (Land* l = Find(name))
				{
					LetArchetypeFilesGo(l->name);
					content::Revert(l->hash, kGroupMapSp);
					ClearOldPlace();
					l->dormant = true;
					log::Info("sectors: {}'s maps put away", l->name);
				}
			landfiles::Audit("the old land's maps put away");
			g_switch.step = Step::SettleOld;
			g_switch.since = GetTickCount64();
			break;
		case Step::SettleOld:
			if (Settled(spent))
			{
				Hold(player); // (as the world was cleared)
				landfiles::Audit("the old land's maps settled");
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
			// (and the old land's files given back to the stores, once the streaming engine has unloaded them all - after
			// 30 s what is still loaded keeps its slot: the files of every land gone to had stayed registered, the stores full
			// on the second big land)
			bool released = true;
			for (size_t i = 0; i < g_switch.leaving.size(); ++i)
				if (!g_switch.released[i])
					if (Land* l = Find(g_switch.leaving[i]))
					{
						if (landfiles::Release(l->name, paths::Get().mods / l->name / L"dlc.rpf", spent > 30000).done)
							g_switch.released[i] = true, l->registered = false;
						else
							released = false;
					}
			if (busy <= 0 && released)
			{
				if (busy < 0)
					log::Warn("sectors: the navmeshes' streaming module was not found; switching without waiting for them");
				landfiles::Audit("the old land's files given back");
				g_switch.step = Step::Bring;
			}
			else if (spent > 60000)
			{
				log::Warn("sectors: {} navmeshes of the old land still in use after 60 s; the switch to {} is given up", busy, to->name);
				BackToOldLand();
			}
			break;
		}
		case Step::Bring:
			// (room in the stores for the new land's files - with the old land's given back there always is: else, rather than
			// a full pool's int3, the switch given up and the old land's maps back)
			if (!to->registered)
				if (const std::string full = NoRoomFor(*to); !full.empty())
				{
					log::Error("sectors: no room for {}'s files after the old land's went back ({}); the switch is given up", to->name, full);
					ui::Notify("地圖", std::format("切換到 {} 失敗：遊戲的資源表沒有空位。", to->name));
					BackToOldLand();
					break;
				}
			// the old land's overlay (road network, navmeshes) out, the new one's in, the road network's regions dropped (they
			// load again from the new files), then its maps
			natives::Invoke<void>(kLoadAllObjectsNow);
			ClearOldPlace();
			ClearJunctions();
			ForgetPlayerRoadNode();
			for (const std::string& name : g_switch.leaving)
				if (const Land* l = Find(name))
					content::Revert(l->hash, kGroupStartup);
			to->dormant = false;
			to->registered = true;
			content::Execute(to->hash, kGroupStartup);
			{
				auto* paths = reinterpret_cast<uint8_t*>(pattern::Module::Main().base + kThePaths);
				paths[0x4BE7] = paths[0x4BE6] ^ 1;
			}
			content::Execute(to->hash, kGroupMapSp);
			landfiles::PutBoxesBack(to->name);
			config::Get().worldSector = to->name;
			config::Save();
			log::Info("sectors: {} is there now", to->name);
			landfiles::Audit("the new land's maps in");
			if (!g_zonesOf.empty())
				ApplyZones(*to);
			g_switch.step = Step::SettleNew;
			g_switch.since = GetTickCount64();
			break;
		case Step::SettleNew:
			if (Settled(spent))
			{
				landfiles::PutBoxesBack(to->name);
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
			// (put down: held until the ground round them is in - a car put on a road 200 m from where the collision had come
			// in fell through the world on four of ten edge crossings -, then on the ground and on their way)
			if (g_switch.landing)
			{
				// (the collision round them in, and the ground under them where it was when the place was chosen: a road just
				// given its collision back had only the sea floor under it for a moment, and the car went down there)
				const auto p = natives::Invoke<natives::ScrVector>(kGetEntityCoords, g_switch.landing, 1);
				float gz = 0;
				const bool below = natives::Invoke<int32_t>(kGetGroundZFor3dCoord, p.x, p.y, p.z + 5.0f, &gz, 0, 0) != 0;
				const bool there = std::isnan(g_switch.landingZ) || (below && std::fabs(gz - g_switch.landingZ) < 10);
				const bool ground = natives::Invoke<int32_t>(kHasCollisionLoadedAroundEntity, g_switch.landing) != 0 && there;
				if (!ground && now - g_switch.landingSince < 15000)
					break;
				if (!ground)
					log::Warn("sectors: the ground round the player not in after 15 s; let go as they are");
				const bool inVehicle = g_switch.landing != player;
				if (ground && below)
					natives::Invoke<void>(kSetEntityCoords, g_switch.landing, p.x, p.y, gz + (inVehicle ? 0.5f : 1.0f), 0, 0, 0, 0);
				if (inVehicle)
				{
					natives::Invoke<void>(kSetVehicleOnGroundProperly, g_switch.landing, 5.0f);
					natives::Invoke<void>(kFreezeEntityPosition, g_switch.landing, 0);
					natives::Invoke<void>(kSetVehicleEngineOn, g_switch.landing, 1, 1, 0);
					natives::Invoke<void>(kSetVehicleForwardSpeed, g_switch.landing, g_switch.speed);
					if (g_switch.own)
					{
						int32_t v = g_switch.landing;
						natives::Invoke<void>(kSetVehicleAsNoLongerNeeded, &v);
					}
				}
				Finish();
				break;
			}
			// on the ground there once its collision is in (or after 20 s, as it is); in the vehicle they came in - the same
			// one, or one like it if it is gone -, on the nearest road, going their way
			const bool carried = g_switch.handle && natives::Invoke<int32_t>(kDoesEntityExist, g_switch.handle);
			if (g_switch.vehicle && !carried && !g_switch.modelAsked)
			{
				natives::Invoke<void>(kRequestModel, g_switch.model);
				g_switch.modelAsked = true;
			}
			float z = 0, water = 0;
			const bool ground = natives::Invoke<int32_t>(kGetGroundZFor3dCoord, g_switch.at[0], g_switch.at[1], 500.0f, &z, 0, 0) != 0;
			// (open water there - the sea, a bay, a swamp -: nothing to wait for, they come up on its surface)
			const bool wet = !ground && natives::Invoke<int32_t>(kGetWaterHeight, g_switch.at[0], g_switch.at[1], 30.0f, &water) != 0;
			const bool model = !g_switch.vehicle || carried || natives::Invoke<int32_t>(kHasModelLoaded, g_switch.model) != 0;
			if (!((ground || wet) && model) && spent < 20000)
				break;
			if (wet)
				z = water;
			float x = g_switch.at[0], y = g_switch.at[1];
			g_switch.landingZ = ground ? z : NAN;
			if (g_switch.vehicle && model)
			{
				float h = g_switch.heading, nodeHeading = 0;
				natives::ScrVector node{};
				// (not a road along the edge they came over: the edge watch would send them back at once, and on again)
				if (natives::Invoke<int32_t>(kGetClosestVehicleNodeWithHeading, x, y, z, &node, &nodeHeading, 1, 3.0f, 0) &&
				    std::hypot(node.x - x, node.y - y) < 200 && to->AwayFromEdges(node.x, node.y, kEdge + 15))
				{
					x = node.x, y = node.y, z = node.z;
					h = std::fabs(std::remainder(nodeHeading - g_switch.heading, 360.0f)) <= 90 ? nodeHeading : nodeHeading + 180;
					g_switch.landingZ = node.z;
				}
				int32_t v = g_switch.handle;
				if (carried)
				{
					natives::Invoke<void>(kSetEntityCoords, v, x, y, z + 0.5f, 0, 0, 0, 0);
					natives::Invoke<void>(kSetEntityHeading, v, h);
				}
				else
				{
					v = natives::Invoke<int32_t>(kCreateVehicle, g_switch.model, x, y, z + 0.5f, h, 0, 0, 0);
					natives::Invoke<void>(kSetVehicleColours, v, g_switch.colours[0], g_switch.colours[1]);
					natives::Invoke<void>(kSetModelAsNoLongerNeeded, g_switch.model);
					g_switch.own = true;
				}
				natives::Invoke<void>(kFreezeEntityPosition, v, 1);
				natives::Invoke<void>(kFreezeEntityPosition, player, 0);
				natives::Invoke<void>(kSetPedIntoVehicle, player, v, -1);
				g_switch.landing = v;
			}
			else
			{
				natives::Invoke<void>(kSetEntityCoords, player, x, y, z + 1.0f, 0, 0, 0, 0);
				g_switch.landing = player; // (still held from the switch's start)
			}
			natives::Invoke<void>(kSetFocusPosAndVel, x, y, z, 0.0f, 0.0f, 0.0f);
			natives::Invoke<void>(kRequestCollisionAtCoord, x, y, z);
			g_switch.landingSince = now;
			log::Info("sectors: the player at {:.0f}, {:.0f} in {}{}", x, y, to->name,
			    !g_switch.vehicle ? "" : carried ? ", in their vehicle" : ", in a vehicle like theirs");
			break;
		}
		case Step::None:
			break;
		}
	}

	bool Switching()
	{
		return g_switch.step != Step::None || (g_ended && GetTickCount64() - g_ended < 15000);
	}

	namespace
	{
		// The lands there at each save (its number: mods.cpp's ML_SAVE_NONCE): ModLoader\data\sector_saves.json, {"<save>":
		// "<land> <land>..."}.
		std::filesystem::path SavesFile()
		{
			return paths::Get().root / L"data" / L"sector_saves.json";
		}

		nlohmann::json ReadSaves()
		{
			std::ifstream in(SavesFile());
			auto j = in ? nlohmann::json::parse(in, nullptr, false) : nlohmann::json::object();
			return j.is_object() ? j : nlohmann::json::object();
		}
	}

	void Saved(int32_t save)
	{
		if (!save || g_lands.empty())
			return;
		auto j = ReadSaves();
		j[std::format("{:08x}", static_cast<uint32_t>(save))] = Active();
		std::error_code ec;
		std::filesystem::create_directories(SavesFile().parent_path(), ec);
		std::ofstream(SavesFile(), std::ios::trunc) << j.dump(1) << "\n";
	}

	void Loaded(int32_t save)
	{
		ForgetParked();
		if (!save || g_lands.empty())
			return;
		const auto j = ReadSaves();
		const auto it = j.find(std::format("{:08x}", static_cast<uint32_t>(save)));
		if (it == j.end() || !it->is_string())
			return;
		// (the first land of the save's that waits now: switched to, the player kept where the save put them)
		const std::string lands = it->get<std::string>();
		for (size_t a = 0; a < lands.size();)
		{
			const size_t b = std::min(lands.find(' ', a), lands.size());
			const std::string name = lands.substr(a, b - a);
			a = b + 1;
			if (const Land* land = Find(name); land && land->dormant)
			{
				log::Info("sectors: the save loaded was made in {}", name);
				if (const std::string error = Switch(name, nullptr); !error.empty())
					log::Warn("sectors: {}: {}", name, error);
				return;
			}
		}
	}
}
