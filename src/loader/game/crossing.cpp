#include "crossing.hpp"

#include <Windows.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <vector>

#include "../log.hpp"
#include "../pattern.hpp"
#include "natives.hpp"

namespace loader::game::crossing
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// The vehicles: a pool of their own (+0x435F238, made by 0xECF8E0: +8 the vehicles - pointers -, +0x10 how many
		// places, +0x38 a bit a place - set: one there -, +0x68 how many there). The people: the game's usual pool (fwBasePool)
		// behind an encoded pointer (+0x434A928, its key +0x434A930, +0x434A920 bit 0 set once made: k = rol(key, 30), the
		// pool = ~rol(stored ^ k, 32 + (k & 31) + 2), as the set-up 0xD4E7A0 encodes it): +8 the people, +0x10 a byte a place
		// (bit 7 set: free), +0x18 how many places, +0x1C each one's size. An entity's place: its matrix's last row (+0x90).
		// Its script handle: 0x149200 (the one the natives give back, made on first asking).
		constexpr uintptr_t kVehiclePool = 0x435F238, kPedPoolStored = 0x434A928, kPedPoolKey = 0x434A930, kPedPoolMade = 0x434A920;
		constexpr uintptr_t kGuidOf = 0x149200;
		// (the code that makes them, as this build has it: the vehicles' pool stored, the people's pool's encoding, the handle's
		// start)
		struct Known
		{
			uintptr_t at;
			std::vector<uint8_t> bytes;
		};
		// The collision round a place: the collision store's box streamer (+0x3ED8E30: vtable +0x18 the files whose boxes
		// meet a box - its index list -, +0x20 the store - vtable +0x70 a file's contents if in -, +0x38 a u16 of flags a file
		// - 4 off, (& 0x38) == 8 not wanted, >> 6 its kind -, +0x61 and +0x66 the kinds wanted, +0x68 the reach the game holds
		// it to). Each frame the game asks for the files round the camera (0x92D4A0) and then checks those within the reach
		// of it (0x92D650: 10 m): one not in yet, and it asks for it and loads everything asked for there and then (0xCF03F0)
		// - a long frame after a jump to where the collision was not in.
		constexpr uintptr_t kBoundsStreamer = 0x3ED8E30, kAskAround = 0x92D4A0, kFree = 0x18D0;
		const Known kKnown[] = {{0xECF910, {0x48, 0x89, 0x1D, 0x21, 0xF9, 0x48, 0x03}},
		    {0xD4E842, {0x48, 0x8B, 0x15, 0xE7, 0xC0, 0x5F, 0x03, 0x48, 0xC1, 0xC2, 0x1E}},
		    {0xD4E85C, {0x80, 0xE1, 0x1F, 0x80, 0xC1, 0x02, 0x48, 0xD3, 0xCE, 0x48, 0x31, 0xD6, 0x48, 0x89, 0x35, 0xB9, 0xC0, 0x5F, 0x03}},
		    {kGuidOf, {0x56, 0x57, 0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xCF, 0x48, 0x8D, 0x71, 0x10}},
		    {kAskAround, {0x56, 0x48, 0x83, 0xEC, 0x70, 0x48, 0x89, 0xCE, 0x80, 0xB9, 0xB4, 0x00, 0x00, 0x00, 0x01}},
		    // (0x92D650's look-up: the reach, the box, the query through the vtable)
		    {0x92D65B, {0x44, 0x0F, 0xB6, 0x41, 0x61, 0x45, 0x84, 0xC0}}, {0x92D66C, {0x0F, 0x10, 0x41, 0x68}}, {0x92D6B3, {0xFF, 0x50, 0x18}},
		    {0x92D6E5, {0x48, 0x8B, 0x46, 0x38, 0x0F, 0xB7, 0x0C, 0x58, 0xF6, 0xC1, 0x04}}, {0x92D710, {0x48, 0x8B, 0x4E, 0x20, 0x48, 0x8B, 0x01, 0x89, 0xDA, 0xFF, 0x50, 0x70}}};

		constexpr uint64_t kPlayerPedId = 0xD80958FC74E988A6, kIsPedInAnyVehicle = 0x997ABD671D25CA0B, kGetVehiclePedIsIn = 0x9A9112A0FE9A4713,
		                   kGetEntityCoords = 0x3FEF770D40960D5A, kGetEntityVelocity = 0x4805D2B1D8CF94A9, kSetEntityCoordsNoOffset = 0x239A3351AC1DA385,
		                   kSetEntityVelocity = 0x1C99BB7B6E96D16F, kGetCamRelativeHeading = 0x743607648ADD4587, kSetCamRelativeHeading = 0xB4EC2312F4E5B1F1,
		                   kGetCamRelativePitch = 0x3A6867B4845BEDA2, kSetCamRelativePitch = 0x6D0858B8EDFD2B7D, kGetPedInVehicleSeat = 0xBB40DD2270B65366,
		                   kTaskVehicleDriveWander = 0x480142959D337D00, kTaskWanderStandard = 0xBB9CE077274F6A1B, kIsPedAPlayer = 0x12534C348C6CB68B,
		                   kIsEntityDead = 0x5F9532F3B5CC2551, kGetEntitySpeed = 0xD5037BA82E12416F, kIsEntityAMissionEntity = 0x0A7B270912999B3C;
		constexpr int32_t kDrivingStyle = 786603; // (the traffic's own: stops for lights and cars, keeps to its lane)

		uint64_t Rol(uint64_t v, unsigned n)
		{
			n &= 63;
			return n ? (v << n) | (v >> (64 - n)) : v;
		}

		bool CodeKnown()
		{
			static const bool known = [] {
				const auto base = pattern::Module::Main().base;
				for (const Known& k : kKnown)
					if (std::memcmp(reinterpret_cast<const void*>(base + k.at), k.bytes.data(), k.bytes.size()) != 0)
					{
						log::Warn("crossing: the game's code at {:#x} is not as this build had it; the traffic is not carried over", k.at);
						return false;
					}
				return true;
			}();
			return known;
		}

		// (memory of another subsystem's: read under SEH, as a pool's place can be in flux)
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

		// The vehicles and the people within `radius` (across) of (x, y): their entities.
		void Near(float x, float y, float radius, std::vector<uintptr_t>& vehicles, std::vector<uintptr_t>& peds)
		{
			const auto base = pattern::Module::Main().base;
			const float r2 = radius * radius;
			const auto close = [&](uintptr_t entity) {
				float p[3]{};
				return entity && Copy(p, entity + 0x90, sizeof(p)) && (p[0] - x) * (p[0] - x) + (p[1] - y) * (p[1] - y) <= r2;
			};
			if (const uintptr_t pool = *reinterpret_cast<const uintptr_t*>(base + kVehiclePool))
			{
				uintptr_t list = 0, bits = 0;
				uint32_t size = 0;
				if (Copy(&list, pool + 8, 8) && Copy(&size, pool + 0x10, 4) && Copy(&bits, pool + 0x38, 8) && list && bits && size <= 0x10000)
					for (uint32_t i = 0; i < size; ++i)
					{
						uint32_t word = 0;
						uintptr_t vehicle = 0;
						if (Copy(&word, bits + (i >> 5) * 4, 4) && (word >> (i & 31) & 1) && Copy(&vehicle, list + i * 8ull, 8) && close(vehicle))
							vehicles.push_back(vehicle);
					}
			}
			if (*reinterpret_cast<const uint8_t*>(base + kPedPoolMade) & 1)
			{
				const uint64_t k = Rol(*reinterpret_cast<const uint64_t*>(base + kPedPoolKey), 0x1E);
				const uintptr_t pool = ~Rol(*reinterpret_cast<const uint64_t*>(base + kPedPoolStored) ^ k, 32 + static_cast<unsigned>(k & 31) + 2);
				uintptr_t storage = 0, flags = 0;
				int32_t count = 0, stride = 0;
				if (Copy(&storage, pool + 8, 8) && Copy(&flags, pool + 0x10, 8) && Copy(&count, pool + 0x18, 4) && Copy(&stride, pool + 0x1C, 4) && storage &&
				    flags && count > 0 && count <= 0x10000 && stride > 0)
				{
					std::vector<uint8_t> free(static_cast<size_t>(count));
					if (Copy(free.data(), flags, free.size()))
						for (int32_t i = 0; i < count; ++i)
							if (!(free[i] & 0x80) && close(storage + static_cast<uintptr_t>(i) * stride))
								peds.push_back(storage + static_cast<uintptr_t>(i) * stride);
				}
			}
		}

		// The collision round a place: asked for as the game asks for the camera's (`ask`), and how many of the files within
		// `radius` of it are not in yet - the game's own check (0x92D650) made for the place, wider.
		size_t CollisionMissing(const float* at, float radius, bool ask)
		{
			const auto base = pattern::Module::Main().base;
			auto* streamer = reinterpret_cast<uint8_t*>(base + kBoundsStreamer);
			alignas(16) float p[4] = {at[0], at[1], at[2], 0};
			if (ask)
				reinterpret_cast<void (*)(uint8_t*, const float*)>(base + kAskAround)(streamer, p);
			alignas(16) float box[8] = {at[0] - radius, at[1] - radius, at[2] - radius, 0, at[0] + radius, at[1] + radius, at[2] + radius, 0};
			struct
			{
				uint32_t* items = nullptr;
				uint16_t count = 0, capacity = 0;
			} list;
			const uint8_t kinds = streamer[0x61] & streamer[0x66];
			using QueryFn = void (*)(uint8_t*, const float*, uint32_t, void*, uint8_t);
			(*reinterpret_cast<QueryFn*>(*reinterpret_cast<uintptr_t*>(streamer) + 0x18))(streamer, box, streamer[0x61], &list, 0);
			const auto* flags = *reinterpret_cast<const uint16_t* const*>(streamer + 0x38);
			auto* store = *reinterpret_cast<uint8_t**>(streamer + 0x20);
			using GetFn = void* (*)(uint8_t*, uint32_t);
			const auto get = *reinterpret_cast<GetFn*>(*reinterpret_cast<uintptr_t*>(store) + 0x70);
			size_t missing = 0;
			for (uint16_t i = 0; i < list.count; ++i)
			{
				const uint32_t file = list.items[i];
				const uint16_t f = flags[file];
				if ((f & 4) || (f & 0x38) == 8 || !(kinds & (1u << (f >> 6))))
					continue;
				missing += get(store, file) == nullptr;
			}
			if (list.capacity && list.items)
				reinterpret_cast<void (*)(void*)>(base + kFree)(list.items);
			return missing;
		}

		// A place made ready ahead of the shift: everything round it asked for (the game's load scene, collision included)
		// and its collision asked for every frame.
		constexpr uint64_t kNewLoadSceneStartSphere = 0xACCFB4ACF53551B0, kIsNewLoadSceneLoaded = 0x01B8247A7A8B9AD1, kNewLoadSceneStop = 0xC197616D221FF4A4,
		                   kRequestCollisionAtCoord = 0x07503F7948F491A7;
		constexpr int32_t kLoadSceneCollision = 1; // (NEWLOADSCENE_FLAG_REQUIRE_COLLISION)
		constexpr float kCollisionReach = 30;     // (the game holds the camera to 10 m)
		struct Preload
		{
			bool on = false;
			float at[3]{};
			size_t missing = 0;
			Clock::time_point since;
		} g_preload;
	}

	void Prepare(const float* at, float radius)
	{
		using natives::Invoke;
		if (!CodeKnown())
			return;
		if (g_preload.on)
			Invoke<void>(kNewLoadSceneStop);
		g_preload = {true, {at[0], at[1], at[2]}, 0, Clock::now()};
		Invoke<int32_t>(kNewLoadSceneStartSphere, at[0], at[1], at[2], radius, kLoadSceneCollision);
	}

	void Release()
	{
		if (g_preload.on)
			natives::Invoke<void>(kNewLoadSceneStop);
		g_preload = {};
	}

	bool Ready(std::string* why)
	{
		if (!g_preload.on)
			return false;
		const bool scene = natives::Invoke<int32_t>(kIsNewLoadSceneLoaded) != 0;
		if (why)
			*why = std::format("scene {}, {} collision file(s) within {:.0f} m not in, {:.1f} s", scene ? "in" : "loading", g_preload.missing, kCollisionReach,
			    std::chrono::duration<double>(Clock::now() - g_preload.since).count());
		return scene && g_preload.missing == 0;
	}

	void Tick()
	{
		if (!g_preload.on)
			return;
		natives::Invoke<void>(kRequestCollisionAtCoord, g_preload.at[0], g_preload.at[1], g_preload.at[2]);
		g_preload.missing = CollisionMissing(g_preload.at, kCollisionReach, true);
	}

	std::string Shift(float dx, float dy, float dz, float radius)
	{
		using natives::Invoke;
		using natives::ScrVector;
		if (!CodeKnown())
			return "the game's code is not as this build had it (see the log)";
		const auto t = Clock::now();
		const int32_t player = Invoke<int32_t>(kPlayerPedId);
		const int32_t own = Invoke<int32_t>(kIsPedInAnyVehicle, player, 0) ? Invoke<int32_t>(kGetVehiclePedIsIn, player, 0) : 0;
		const auto at = Invoke<ScrVector>(kGetEntityCoords, own ? own : player, 1);
		const float heading = Invoke<float>(kGetCamRelativeHeading), pitch = Invoke<float>(kGetCamRelativePitch);
		std::vector<uintptr_t> vehicleEntities, pedEntities;
		Near(at.x, at.y, radius, vehicleEntities, pedEntities);
		const auto guidOf = reinterpret_cast<int32_t (*)(uintptr_t)>(pattern::Module::Main().base + kGuidOf);
		struct Mover
		{
			int32_t handle;
			bool vehicle;
			ScrVector velocity;
		};
		std::vector<Mover> movers;
		for (const uintptr_t v : vehicleEntities)
			if (const int32_t h = guidOf(v))
				movers.push_back({h, true});
		const size_t vehicles = movers.size();
		size_t riding = 0;
		for (const uintptr_t p : pedEntities)
			if (const int32_t h = guidOf(p))
			{
				// (one in a vehicle goes with it)
				if (Invoke<int32_t>(kIsPedInAnyVehicle, h, 0))
					++riding;
				else
					movers.push_back({h, false});
			}
		const auto found = Clock::now();
		for (Mover& m : movers)
		{
			m.velocity = Invoke<ScrVector>(kGetEntityVelocity, m.handle);
			const auto p = Invoke<ScrVector>(kGetEntityCoords, m.handle, 0);
			Invoke<void>(kSetEntityCoordsNoOffset, m.handle, p.x + dx, p.y + dy, p.z + dz, 0, 0, 0);
			Invoke<void>(kSetEntityVelocity, m.handle, m.velocity.x, m.velocity.y, m.velocity.z);
		}
		Invoke<void>(kSetCamRelativeHeading, heading);
		Invoke<void>(kSetCamRelativePitch, pitch, 1.0f);
		const auto moved = Clock::now();
		// on their way again from here: the drivers wandering on at their speed, the people walking on (a mission's own left
		// to the mission)
		size_t drivers = 0, walkers = 0;
		for (const Mover& m : movers)
		{
			if (Invoke<int32_t>(kIsEntityAMissionEntity, m.handle) || Invoke<int32_t>(kIsEntityDead, m.handle, 0))
				continue;
			if (m.vehicle)
			{
				const int32_t driver = Invoke<int32_t>(kGetPedInVehicleSeat, m.handle, -1, 0);
				if (driver && !Invoke<int32_t>(kIsPedAPlayer, driver) && !Invoke<int32_t>(kIsEntityAMissionEntity, driver))
				{
					Invoke<void>(kTaskVehicleDriveWander, driver, m.handle, std::max(Invoke<float>(kGetEntitySpeed, m.handle), 8.0f), kDrivingStyle);
					++drivers;
				}
			}
			else if (!Invoke<int32_t>(kIsPedAPlayer, m.handle))
			{
				Invoke<void>(kTaskWanderStandard, m.handle, 10.0f, 10);
				++walkers;
			}
		}
		const auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
		const auto done = Clock::now();
		return std::format("{} vehicles, {} people on foot ({} riding) moved by ({:.0f}, {:.0f}); {} drivers and {} walkers set on again; found {:.2f} ms, moved "
		                   "{:.2f} ms, tasks {:.2f} ms (all {:.2f} ms)",
		    vehicles, movers.size() - vehicles, riding, dx, dy, drivers, walkers, ms(t, found), ms(found, moved), ms(moved, done), ms(t, done));
	}
}
