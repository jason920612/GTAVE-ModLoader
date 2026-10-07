// Research aids of the property mod, each started by a file in ModLoader (research/phase0.md §28..§30).
#define NOMINMAX
#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "property.hpp"

namespace property::research
{
	void OnLoad()
	{
		// Research: with ModLoader\scaleform_log.txt present at start, the game browser's scaleform calls (method names
		// and parameters) are written to data\scaleform.log, to read the original websites' catalogues.
		if (std::filesystem::exists("ModLoader/scaleform_log.txt"))
		{
			static std::ofstream log(std::filesystem::path(ml::Context().dataDir) / "scaleform.log");
			const auto str = [](ml::scripts::NativeCall& call, const char* what) {
				call.CallOriginal();
				const char* s = call.Arg<const char*>(0);
				log << what << ' ' << (s ? s : "(null)") << '\n';
			};
			ml::scripts::OverrideNative("appinternet", 0xF6E48914C7A8694EULL, [](ml::scripts::NativeCall& call) {
				call.CallOriginal();
				const char* s = call.Arg<const char*>(1);
				log << "\nMETHOD " << (s ? s : "(null)") << '\n';
			});
			ml::scripts::OverrideNative("appinternet", 0xC3D0841A0CC546A6ULL, [](ml::scripts::NativeCall& call) {
				call.CallOriginal();
				log << "int " << call.Arg<int>(0) << '\n';
			});
			ml::scripts::OverrideNative("appinternet", 0xD69736AAE04DB51AULL, [](ml::scripts::NativeCall& call) {
				call.CallOriginal();
				log << "float " << call.Arg<float>(0) << '\n';
			});
			ml::scripts::OverrideNative("appinternet", 0x80338406F3475E55ULL, [str](ml::scripts::NativeCall& call) { str(call, "text"); });
			ml::scripts::OverrideNative("appinternet", 0x77FE3402004CD1B0ULL, [str](ml::scripts::NativeCall& call) { str(call, "literal"); });
			ml::scripts::OverrideNative("appinternet", 0xBA7148484BD90365ULL, [str](ml::scripts::NativeCall& call) { str(call, "texture"); });
			ml::scripts::OverrideNative("appinternet", 0xE83A3E3557A56640ULL, [str](ml::scripts::NativeCall& call) { str(call, "player"); });
			ml::scripts::OverrideNative("appinternet", 0xC63CD5D2920ACBE7ULL, [str](ml::scripts::NativeCall& call) { str(call, "label"); });
			ml::scripts::OverrideNative("appinternet", 0x03B504CF259931BCULL, [](ml::scripts::NativeCall& call) {
				call.CallOriginal();
				log << "number " << call.Arg<int>(0) << '\n';
			});
		}
	}

	void Tick()
	{
		// Research: ModLoader\water_sample.txt samples water on a world grid into data\water.txt ("x y water" lines), to
		// calibrate the website's map against the game world.
		if (std::error_code ec; std::filesystem::remove("ModLoader/water_sample.txt", ec))
		{
			std::ofstream out(std::filesystem::path(ml::Context().dataDir) / "water.txt");
			int n = 0;
			for (int y = -4500; y <= 8500; y += 50)
			{
				for (int x = -4500; x <= 5000; x += 50)
				{
					float h = 0;
					const bool water = WATER::GET_WATER_HEIGHT_NO_WAVES(static_cast<float>(x), static_cast<float>(y), 0.0f, &h);
					out << x << ' ' << y << ' ' << (water ? 1 : 0) << '\n';
					++n;
				}
				ml::Wait(0);
			}
			ml::Log("water samples: {}", n);
		}
		// Research: ModLoader\global_dump.txt ("<first> <count> <name>") writes those script globals to data\<name>.bin.
		if (std::ifstream in("ModLoader/global_dump.txt"); in)
		{
			uint32_t first = 0, count = 0;
			std::string name;
			in >> first >> count >> name;
			in.close();
			std::error_code ec;
			std::filesystem::remove("ModLoader/global_dump.txt", ec);
			std::ofstream out(std::filesystem::path(ml::Context().dataDir) / (name + ".bin"), std::ios::binary);
			for (uint32_t i = 0; i < count; ++i)
			{
				const int64_t* g = ml::scripts::Global(first + i);
				// Global blocks are smaller than their index range: only read committed, readable memory.
				static uintptr_t readableFrom = 0, readableTo = 0;
				const auto at = reinterpret_cast<uintptr_t>(g);
				if (g && (at < readableFrom || at + 8 > readableTo))
				{
					MEMORY_BASIC_INFORMATION mbi{};
					const bool ok = VirtualQuery(g, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
					                (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READWRITE)) && !(mbi.Protect & PAGE_GUARD);
					readableFrom = ok ? reinterpret_cast<uintptr_t>(mbi.BaseAddress) : 0;
					readableTo = ok ? readableFrom + mbi.RegionSize : 0;
					if (!ok)
						g = nullptr;
				}
				const int64_t v = g ? *g : 0x7FFFFFFFFFFFFFFF;
				out.write(reinterpret_cast<const char*>(&v), 8);
			}
			ml::Log("dumped globals {}..{} to {}.bin", first, first + count, name);
		}
		// Research: ModLoader\model_names.txt (hashes, one per line) logs each model's display name and class.
		if (std::ifstream in("ModLoader/model_names.txt"); in)
		{
			std::vector<int64_t> hashes;
			for (int64_t h; in >> h;)
				hashes.push_back(h);
			in.close();
			std::error_code ec;
			std::filesystem::remove("ModLoader/model_names.txt", ec);
			std::ofstream out(std::filesystem::path(ml::Context().dataDir) / "model_names.txt");
			for (const int64_t h : hashes)
			{
				const Hash model = static_cast<Hash>(h);
				const char* label = VEHICLE::GET_DISPLAY_NAME_FROM_VEHICLE_MODEL(model);
				out << h << '\t' << label << '\t' << Text(label) << '\t' << VEHICLE::GET_VEHICLE_CLASS_FROM_NAME(model) << '\t'
				    << static_cast<int>(STREAMING::IS_MODEL_VALID(model)) << '\n';
			}
			ml::Log("model names: {}", hashes.size());
		}
		// Research: ModLoader\mp_map.txt ("on" / "off") switches the game's multiplayer map data and logs which Online
		// garage interiors exist afterwards.
		if (std::ifstream in("ModLoader/mp_map.txt"); in)
		{
			std::string mode;
			in >> mode;
			in.close();
			std::error_code ec;
			std::filesystem::remove("ModLoader/mp_map.txt", ec);
			if (mode == "on")
				DLC::ON_ENTER_MP();
			else if (mode == "off")
				DLC::ON_ENTER_SP();
			ml::Wait(2000);
			for (const auto& [x, y] : {std::pair{173.14f, -1008.1f}, {206.19f, -1006.42f}, {229.22f, -1005.1f}})
			{
				const Interior interior = INTERIOR::GET_INTERIOR_AT_COORDS(x, y, -99.5f);
				ml::Log("mp map {}: interior at {} {} = {} disabled {} capped {} ready {}", mode, x, y, interior,
				    static_cast<int>(INTERIOR::IS_INTERIOR_DISABLED(interior)), static_cast<int>(INTERIOR::IS_INTERIOR_CAPPED(interior)),
				    static_cast<int>(INTERIOR::IS_INTERIOR_READY(interior)));
			}
		}
		// Research: ModLoader\spawn_vehicle.txt ("<model> [x y z heading]") spawns a vehicle, gives it a few modifications
		// and puts the player in the driver's seat.
		if (std::ifstream in("ModLoader/spawn_vehicle.txt"); in)
		{
			std::string name;
			in >> name;
			const Ped ped = PLAYER::PLAYER_PED_ID();
			Vector3 at = ENTITY::GET_ENTITY_COORDS(ped, true);
			float heading = ENTITY::GET_ENTITY_HEADING(ped);
			in >> at.x >> at.y >> at.z >> heading;
			in.close();
			std::error_code ec;
			std::filesystem::remove("ModLoader/spawn_vehicle.txt", ec);
			const Hash model = MISC::GET_HASH_KEY(name.c_str());
			STREAMING::REQUEST_MODEL(model);
			for (int i = 0; i < 200 && !STREAMING::HAS_MODEL_LOADED(model); ++i)
				ml::Wait(10);
			const Vehicle v = VEHICLE::CREATE_VEHICLE(model, at.x, at.y, at.z, heading, false, false, false);
			STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED(model);
			VEHICLE::SET_VEHICLE_MOD_KIT(v, 0);
			VEHICLE::SET_VEHICLE_COLOURS(v, 28, 0); // red / black
			VEHICLE::SET_VEHICLE_MOD(v, 0, 1, false); // spoiler
			VEHICLE::SET_VEHICLE_MOD(v, 23, 3, false); // wheels
			VEHICLE::SET_VEHICLE_NUMBER_PLATE_TEXT(v, "ML TEST");
			PED::SET_PED_INTO_VEHICLE(ped, v, -1);
			ml::Log("spawned {} as {}", name, v);
		}
		// Research: ModLoader\teleport.txt ("x y z [heading]") moves the player there.
		if (std::ifstream in("ModLoader/teleport.txt"); in)
		{
			float x = 0, y = 0, z = 0, h = 0;
			in >> x >> y >> z >> h;
			in.close();
			std::error_code ec;
			std::filesystem::remove("ModLoader/teleport.txt", ec);
			const Ped ped = PLAYER::PLAYER_PED_ID();
			STREAMING::REQUEST_COLLISION_AT_COORD(x, y, z);
			ENTITY::SET_ENTITY_COORDS(ped, x, y, z, false, false, false, false);
			ENTITY::SET_ENTITY_HEADING(ped, h);
			ml::Wait(500);
			ml::Log("teleported to {} {} {} (interior {})", x, y, z, INTERIOR::GET_INTERIOR_FROM_ENTITY(ped));
		}
	}
}
