// On and off duty (user's design): the officer goes on duty at their department's station, in the locker room, where
// they change into the uniform and take their equipment, and picks a patrol car in the station's lot. Off duty again
// at the locker. Being in uniform is being on duty: the switch scene at the station puts the officer there on duty too.
//
// Mission Row (LSPD): the story-mode interior (30978) holds the lobby and the rooms behind it. The locker room is the
// bare briefing room: from the lobby through the double door marked CELLS / BRIEFING, down the corridor, the blue door
// on the right; the mod stands lockers in it. The corridor ends at a locked door marked GARAGE, which the mod makes a
// way (with a fade) to the lot behind the station, by the parking ramp; the game parks its own patrol cars there.
//
// The other departments' stations have no usable interior in story mode (the sheriff's offices in Sandy Shores and
// Paleto Bay stand as bare, untextured shells), so the officer changes "inside" through the front door: E at the door,
// a fade, and out again changed. Sheriff (BCSO): Sandy Shores. Highway patrol (SAHP, no station in the game): the La
// Mesa police station by the freeways. NOOSE: the Los Santos Government Facility.
//
// Patrol cars: the game's own (police, sheriff, fbi ...) and the newer shared police cars (polbuffalo, polgreenwood ...)
// whose livery (vehicle mod 48) is named per department: POLSHARED_LIV1 LSPD, LIV4 highway patrol, LIV5 sheriff
// (the index differs from car to car, so it is looked up by name).
#include "police.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <vector>

namespace police::duty
{
	namespace
	{
		struct Car
		{
			const char* model;
			const char* livery; // the mod-48 livery's label, nullptr = the car's own
			int rank = 0;       // the rank that may take it (career.cpp)
		};
		struct Station
		{
			const char* name;
			const char* cop;          // the department's police ped
			bool door;                // changing happens behind the front door (no locker room)
			Place idle;               // where they stand about when switched to on duty
			Place locker;             // where the officer stands to change (the locker, or before the door)
			Place lot;                // where they stand to pick a car
			Place bay;                // where the picked car is put
			std::vector<Car> cars;
			// A locker room only:
			Place doorInside{};         // the way out to the lot ...
			Place doorOutside{};        // ... and back in
			std::vector<Place> lockers; // locker props the mod puts in the (bare) room
			size_t own = 0;             // the officer's (open) one among them
		};
		// In the order of look::Department(): LSPD, BCSO, SAHP, NOOSE.
		const Station kStations[] = {
		    {
		        "任務街警局",
		        "s_m_y_cop_01",
		        false,
		        {436.5f, -981.3f, 30.7f, 270.0f}, // in the lobby, facing into it
		        {438.4f, -995.6f, 30.69f, 180.0f},
		        {446.0f, -1015.5f, 28.49f, 180.0f},
		        {443.0f, -1019.0f, 28.5f, 270.0f},
		        {{"police", nullptr}, {"police2", nullptr}, {"police3", nullptr, 1}, {"police4", nullptr, 2}, {"policeb", nullptr, 1},
		            {"polbuffalo", "POLSHARED_LIV1", 1}, {"polgreenwood", "POLSHARED_LIV1", 2}, {"polcaracara", "POLSHARED_LIV1", 2},
		            {"polimpaler5", "POLSHARED_LIV1", 3}},
		        {445.5f, -996.0f, 30.69f, 0.0f},    // before the GARAGE door, arriving facing up the corridor
		        {450.0f, -1012.6f, 28.49f, 180.0f}, // before the parking ramp's shutters, arriving facing the lot
		        // A row along the room's south wall (x 435.9-442.8, y -996.8 to -989.7), the officer's in front of the marker.
		        {{436.54f, -996.3f, 29.69f, 180.0f}, {437.16f, -996.3f, 29.69f, 180.0f}, {437.78f, -996.3f, 29.69f, 180.0f},
		            {438.40f, -996.3f, 29.69f, 180.0f}, {439.02f, -996.3f, 29.69f, 180.0f}, {439.64f, -996.3f, 29.69f, 180.0f},
		            {440.26f, -996.3f, 29.69f, 180.0f}},
		        3,
		    },
		    {
		        "沙灘鎮郡警局",
		        "s_m_y_sheriff_01",
		        true,
		        {1853.5f, 3680.6f, 34.3f, 210.0f}, // on the porch
		        {1855.6f, 3682.0f, 34.3f, 210.0f}, // the porch before the front door; out again facing the yard
		        {1850.0f, 3672.0f, 33.7f, 300.0f},
		        {1855.8f, 3671.9f, 33.5f, 120.0f},
		        {{"sheriff", nullptr}, {"sheriff2", nullptr}, {"polbuffalo", "POLSHARED_LIV5", 1}, {"polgreenwood", "POLSHARED_LIV5", 2},
		            {"polcaracara", "POLSHARED_LIV5", 2}, {"polimpaler5", "POLSHARED_LIV5", 3}, {"policeb2", "POLSHARED_LIV5", 1}},
		    },
		    {
		        "拉梅薩警局（公路巡警）",
		        "s_m_y_hwaycop_01",
		        true,
		        {823.0f, -1288.5f, 29.3f, 90.0f}, // on the landing, facing the street
		        {823.6f, -1290.0f, 29.3f, 90.0f}, // the landing before the doors at the top of the steps
		        {826.0f, -1274.5f, 26.3f, 0.0f},
		        {832.0f, -1270.5f, 26.3f, 90.0f},
		        {{"policeb2", "POLSHARED_LIV4"}, {"polbuffalo", "POLSHARED_LIV4"}, {"polgauntlet", "POLSHARED_LIV4", 1},
		            {"polgreenwood", "POLSHARED_LIV4", 1}, {"polcoquette4", "POLSHARED_LIV4", 2}, {"poldominator10", "POLSHARED_LIV4", 2},
		            {"polimpaler6", "POLIMP6_LIV6", 3}},
		    },
		    {
		        "NOOSE 總部",
		        "s_m_y_swat_01",
		        true,
		        {2482.8f, -384.0f, 93.9f, 270.0f}, // on the steps, facing the square
		        {2476.4f, -384.0f, 94.4f, 270.0f}, // the landing before the main doors
		        {2522.0f, -395.0f, 93.0f, 0.0f},
		        {2528.0f, -389.0f, 93.0f, 0.0f},
		        {{"fbi", nullptr}, {"fbi2", nullptr}, {"riot", nullptr}, {"policet", nullptr}},
		    },
		};

		const Station& Current()
		{
			return kStations[std::clamp(look::Department(), 0, static_cast<int>(std::size(kStations)) - 1)];
		}

		// The equipment (user's list): a pistol, the taser, the baton and a flashlight; long guns by rank (career.cpp).
		const char* const kEquipment[] = {"WEAPON_COMBATPISTOL", "WEAPON_STUNGUN", "WEAPON_NIGHTSTICK", "WEAPON_FLASHLIGHT"};
		const char* const kShotgun = "WEAPON_PUMPSHOTGUN";
		const char* const kCarbine = "WEAPON_CARBINERIFLE";

		std::vector<Object> g_props; // the station's lockers while the officer is near
		const Station* g_propsOf = nullptr;
		Vehicle g_car = 0;     // the patrol car picked in the lot
		Blip g_carBlip = 0;
		// Picking a car: the choice stands in the bay while the player looks through them.
		bool g_picking = false;
		size_t g_choice = 0;
		Vehicle g_preview = 0;

		float Distance(const Vector3& a, const Place& b)
		{
			return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
		}
		void Marker(const Place& p)
		{
			GRAPHICS::DRAW_MARKER(1, p.x, p.y, p.z - 1.0f, 0, 0, 0, 0, 0, 0, 1.0f, 1.0f, 0.6f, 93, 182, 229, 120, false, false, 2, false, nullptr, nullptr,
			    false);
		}
		// A text component holds about 99 bytes: a second line (after ~n~) goes in a second one.
		void Help(const std::string& text)
		{
			const size_t split = text.find("~n~");
			HUD::BEGIN_TEXT_COMMAND_DISPLAY_HELP(split == std::string::npos ? "STRING" : "TWOSTRINGS");
			HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME(text.substr(0, split).c_str());
			if (split != std::string::npos)
				HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME(text.substr(split).c_str());
			HUD::END_TEXT_COMMAND_DISPLAY_HELP(0, false, false, -1);
		}
		bool Pressed(int control)
		{
			return PAD::IS_CONTROL_JUST_PRESSED(0, control);
		}
		constexpr int kContext = 51;   // INPUT_CONTEXT (E)
		constexpr int kDetonate = 47;  // INPUT_DETONATE (G)
		constexpr int kLeft = 174;     // INPUT_CELLPHONE_LEFT (←)
		constexpr int kRight = 175;    // INPUT_CELLPHONE_RIGHT (→)
		constexpr int kCancel = 177;   // INPUT_CELLPHONE_CANCEL (Backspace)

		void Fade(bool out)
		{
			if (out)
			{
				CAMERA::DO_SCREEN_FADE_OUT(400);
				for (const uint64_t end = ml::TickMs() + 3000; ml::TickMs() < end && !CAMERA::IS_SCREEN_FADED_OUT();)
					ml::Wait(0);
			}
			else
				CAMERA::DO_SCREEN_FADE_IN(400);
		}

		bool Exists(Entity e)
		{
			return e && ENTITY::DOES_ENTITY_EXIST(e);
		}
		void Delete(Vehicle& v)
		{
			if (Exists(v))
			{
				ENTITY::SET_ENTITY_AS_MISSION_ENTITY(v, true, true);
				VEHICLE::DELETE_VEHICLE(&v);
			}
			v = 0;
		}

		// The game's name of a car model, in the game's language.
		std::string CarName(const Car& car)
		{
			const char* model = car.model;
			const char* label = VEHICLE::GET_DISPLAY_NAME_FROM_VEHICLE_MODEL(MISC::GET_HASH_KEY(model));
			const char* text = label ? HUD::GET_FILENAME_FOR_AUDIO_CONVERSATION(label) : nullptr;
			return text && *text && std::string(text) != "NULL" ? text : model;
		}

		// The car's mod-48 livery with this label (-1 when it has none).
		int Livery(Vehicle v, const char* label)
		{
			for (int i = 0, n = VEHICLE::GET_NUM_VEHICLE_MODS(v, 48); i < n; ++i)
				if (const char* l = VEHICLE::GET_MOD_TEXT_LABEL(v, 48, i); l && std::string(l) == label)
					return i;
			return -1;
		}

		Vehicle Spawn(const Car& car, const Place& at)
		{
			const Hash hash = MISC::GET_HASH_KEY(car.model);
			const auto loaded = ml::LoadModel(hash);
			if (!loaded)
				return 0;
			// The bay cleared, the game's own parked patrol cars too (CLEAR_AREA_OF_VEHICLES leaves those).
			MISC::CLEAR_AREA(at.x, at.y, at.z, 4.0f, true, false, true, false);
			const Vehicle v = VEHICLE::CREATE_VEHICLE(hash, at.x, at.y, at.z + 0.3f, at.heading, false, false, false);
			if (v)
			{
				ENTITY::SET_ENTITY_AS_MISSION_ENTITY(v, true, true);
				VEHICLE::SET_VEHICLE_ON_GROUND_PROPERLY(v, 5.0f);
				VEHICLE::SET_VEHICLE_DOORS_LOCKED(v, 1);
				if (car.livery)
				{
					VEHICLE::SET_VEHICLE_MOD_KIT(v, 0);
					if (const int livery = Livery(v, car.livery); livery >= 0)
						VEHICLE::SET_VEHICLE_MOD(v, 48, livery, false);
					else
						ml::LogError("duty: {} has no livery {}", car.model, car.livery);
				}
			}
			return v;
		}

		void ShowPreview()
		{
			Delete(g_preview);
			g_preview = Spawn(Current().cars[g_choice], Current().bay);
		}

		// The car already picked stands aside (out of sight, out of the way) while another is looked at.
		void Aside(bool aside)
		{
			if (!Exists(g_car))
				return;
			ENTITY::SET_ENTITY_VISIBLE(g_car, !aside, false);
			ENTITY::SET_ENTITY_COLLISION(g_car, !aside, true);
			ENTITY::FREEZE_ENTITY_POSITION(g_car, aside);
		}

		void StartPicking()
		{
			g_picking = true;
			if (g_choice >= Current().cars.size())
				g_choice = 0;
			Aside(true);
			ShowPreview();
		}

		// Ends picking: the car looked at becomes the patrol car (`keep`), or the old one comes back.
		void StopPicking(bool keep)
		{
			g_picking = false;
			if (keep && Exists(g_preview))
			{
				Delete(g_car);
				g_car = g_preview;
				g_preview = 0;
				ml::Log("duty: patrol car {}", Current().cars[g_choice].model);
				return;
			}
			Delete(g_preview);
			Aside(false);
		}

		void Pick(const std::vector<Car>& cars)
		{
			if (Pressed(kLeft) || Pressed(kRight))
			{
				g_choice = (g_choice + (Pressed(kRight) ? 1 : cars.size() - 1)) % cars.size();
				ShowPreview();
			}
			const bool locked = cars[g_choice].rank > career::Rank();
			if (locked)
				Help("~INPUT_CELLPHONE_LEFT~~INPUT_CELLPHONE_RIGHT~ " + CarName(cars[g_choice]) + "（" + career::Title(cars[g_choice].rank) +
				     "解鎖）~n~~INPUT_CELLPHONE_CANCEL~ 取消");
			else
				Help("~INPUT_CELLPHONE_LEFT~~INPUT_CELLPHONE_RIGHT~ " + CarName(cars[g_choice]) + "~n~~INPUT_CONTEXT~ 確定 ~INPUT_CELLPHONE_CANCEL~ 取消");
			if (Pressed(kContext) && Exists(g_preview) && !locked)
				StopPicking(true);
			else if (Pressed(kCancel))
				StopPicking(false);
		}

		// The lockers stand while the officer is played near the station.
		void Props(bool show)
		{
			if ((show ? &Current() : nullptr) == g_propsOf)
				return;
			for (Object& o : g_props)
				if (Exists(o))
				{
					ENTITY::SET_ENTITY_AS_MISSION_ENTITY(o, true, true);
					OBJECT::DELETE_OBJECT(&o);
				}
			g_props.clear();
			g_propsOf = nullptr;
			if (!show)
				return;
			g_propsOf = &Current();
			const Hash closed = MISC::GET_HASH_KEY("p_cs_locker_01_s"), open = MISC::GET_HASH_KEY("p_cs_locker_01");
			const auto loadedClosed = ml::LoadModel(closed);
			const auto loadedOpen = ml::LoadModel(open);
			if (!loadedClosed || !loadedOpen)
				return;
			const Station& station = Current();
			for (size_t i = 0; i < station.lockers.size(); ++i)
				if (const Place& at = station.lockers[i]; const Object o = OBJECT::CREATE_OBJECT_NO_OFFSET(i == station.own ? open : closed, at.x, at.y, at.z, false, false, false, 0))
				{
					ENTITY::SET_ENTITY_HEADING(o, at.heading);
					ENTITY::FREEZE_ENTITY_POSITION(o, true);
					g_props.push_back(o);
				}
		}

		// The station's own business (G at the locker): the station computer (promotion, the exam, duty and pay on the
		// in-car computer's pages), asking for another shift, today's duty.
		void StationMenu()
		{
			menu::List m{Current().name, {}};
			if (shift::MayRejoin())
			{
				menu::Item rejoin{"重新申請入職", "", "從警員一級的試用期重新開始"};
				rejoin.action = [] { shift::Rejoin(); };
				m.items.push_back(rejoin);
			}
			menu::Item computer{"警局電腦", "", "升遷與考試、勤務、薪資、案件報告"};
			computer.action = [] {
				if (!ml::web::Available())
					ml::Notify("沒有遊戲內瀏覽器，無法使用警局電腦");
				else
					ml::web::Open("https://mdt.lspd/#career");
			};
			m.items.push_back(computer);
			menu::Item request{"申請換班", "", "試用期滿後可以申請日班、小夜班或大夜班，從下週起生效"};
			request.action = [] { shift::RequestShift(); };
			request.keepOpen = true;
			m.items.push_back(request);
			m.items.push_back(shift::MenuItem());
			if (partner::MayTrain())
			{
				const bool on = partner::RookieWanted();
				menu::Item rookie{"帶新人", on ? "今天這班" : "", on ? "不帶了：新人回去" : "這班帶一個新人出勤（新人會問問題，帶滿一班考評 +2）"};
				rookie.action = [on] { partner::WantRookie(!on); };
				m.items.push_back(rookie);
			}
			menu::Open(m);
		}

		void CarBlip(bool show)
		{
			if (show == (g_carBlip != 0) && (!show || HUD::DOES_BLIP_EXIST(g_carBlip)))
				return;
			if (g_carBlip)
				HUD::REMOVE_BLIP(&g_carBlip);
			g_carBlip = 0;
			if (show)
			{
				g_carBlip = HUD::ADD_BLIP_FOR_ENTITY(g_car);
				HUD::SET_BLIP_SPRITE(g_carBlip, 56); // police car
				HUD::SET_BLIP_COLOUR(g_carBlip, 3);
				HUD::BEGIN_TEXT_COMMAND_SET_BLIP_NAME("STRING");
				HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME("巡邏車");
				HUD::END_TEXT_COMMAND_SET_BLIP_NAME(g_carBlip);
			}
		}
	}

	void Equip(::Ped ped, bool on)
	{
		// What the department issued goes back off duty, not the officer's own guns (kept in the save: "officer_issued").
		ml::Json issued = ml::save::Get("officer_issued");
		const auto wasIssued = [&](const std::string& w) {
			for (const ml::Json& i : issued.Items())
				if (i.Str() == w)
					return true;
			return false;
		};
		if (on)
		{
			std::vector<const char*> weapons(std::begin(kEquipment), std::end(kEquipment));
			if (career::May(career::Unlock::Shotgun))
				weapons.push_back(kShotgun);
			if (career::May(career::Unlock::Carbine))
				weapons.push_back(kCarbine);
			for (const char* weapon : weapons)
			{
				const Hash hash = MISC::GET_HASH_KEY(weapon);
				if (!WEAPON::HAS_PED_GOT_WEAPON(ped, hash, false) && !wasIssued(weapon))
					issued.Push(weapon);
				WEAPON::GIVE_WEAPON_TO_PED(ped, hash, 120, true, false);
			}
		}
		else
		{
			for (const ml::Json& w : issued.Items())
				WEAPON::REMOVE_WEAPON_FROM_PED(ped, MISC::GET_HASH_KEY(w.Str().c_str()));
			issued = ml::Json::Array();
		}
		ml::save::Set("officer_issued", issued.IsArray() ? issued : ml::Json::Array());
	}

	bool AtStation(const Vector3& at)
	{
		return Distance(at, Current().locker) < 60.0f || Distance(at, Current().lot) < 40.0f;
	}

	Place StationPlace()
	{
		return Current().locker;
	}

	bool NearLocker(const Vector3& at)
	{
		return Distance(at, Current().locker) < 1.6f;
	}

	const char* CopModel()
	{
		return Current().cop;
	}

	Vehicle UnitCar(const Place& at)
	{
		return Spawn(Current().cars.front(), at);
	}

	Vehicle PatrolCar()
	{
		return Exists(g_car) ? g_car : 0;
	}

	Place Idle()
	{
		return Current().idle;
	}

	bool OnDuty()
	{
		return officer::InUniform();
	}

	void Forget()
	{
		g_car = g_preview = 0;
		g_carBlip = 0;
		g_picking = false;
		g_props.clear();
		g_propsOf = nullptr;
	}

	void Tick()
	{
		const bool player = officer::IsPlayer();
		// The department's station on the map while the officer is played.
		static Blip stationBlip = 0;
		static const Station* blipOf = nullptr;
		if ((player ? &Current() : nullptr) != blipOf)
		{
			if (stationBlip)
				HUD::REMOVE_BLIP(&stationBlip);
			stationBlip = 0;
			blipOf = player ? &Current() : nullptr;
			if (blipOf)
			{
				const Place& at = blipOf->locker;
				stationBlip = HUD::ADD_BLIP_FOR_COORD(at.x, at.y, at.z);
				HUD::SET_BLIP_SPRITE(stationBlip, 60); // police station
				HUD::SET_BLIP_AS_SHORT_RANGE(stationBlip, true);
				HUD::BEGIN_TEXT_COMMAND_SET_BLIP_NAME("STRING");
				HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME(blipOf->name);
				HUD::END_TEXT_COMMAND_SET_BLIP_NAME(stationBlip);
			}
		}
		CarBlip(player && OnDuty() && Exists(g_car) && !ENTITY::IS_ENTITY_DEAD(g_car, false) &&
		        !PED::IS_PED_IN_VEHICLE(PLAYER::PLAYER_PED_ID(), g_car, false));
		if (!player)
		{
			Props(false);
			if (g_picking)
				StopPicking(false);
			return;
		}
		const ::Ped ped = PLAYER::PLAYER_PED_ID();
		// Nobody drags an officer on duty out of their car (passers-by jacked the patrol car the officer sat in).
		PED::SET_PED_CAN_BE_DRAGGED_OUT(ped, !OnDuty());
		const Station& station = Current();
		const Vector3 at = ENTITY::GET_ENTITY_COORDS(ped, true);
		Props(!station.lockers.empty() && Distance(at, station.locker) < 80.0f);
		if (PED::IS_PED_IN_ANY_VEHICLE(ped, false))
			return;

		// The locker: on duty or off.
		if (Distance(at, station.locker) < 25.0f)
		{
			Marker(station.locker);
			if (Distance(at, station.locker) < 1.2f)
			{
				const bool on = OnDuty();
				static std::string warning;
				static uint64_t warningUntil = 0;
				const bool menuOpen = menu::IsOpen(); // (the station's menu: its keys are its own)
				if (!menuOpen && ml::TickMs() < warningUntil)
					Help(warning);
				else if (!menuOpen && station.door)
					Help(on ? "按 ~INPUT_CONTEXT~ 進去換回便服下班~n~按 ~INPUT_DETONATE~ 警局事務" : "按 ~INPUT_CONTEXT~ 進去換上制服、領裝備上班~n~按 ~INPUT_DETONATE~ 警局事務");
				else if (!menuOpen)
					Help(on ? "按 ~INPUT_CONTEXT~ 換回便服下班~n~按 ~INPUT_DETONATE~ 警局事務" : "按 ~INPUT_CONTEXT~ 換上制服、領裝備上班~n~按 ~INPUT_DETONATE~ 警局事務");
				if (Pressed(kDetonate) && !menuOpen)
					StationMenu();
				bool go = Pressed(kContext) && !menuOpen;
				if (go && on)
					if (const std::string w = shift::BeforeClockOut(); !w.empty())
					{
						// Reports not submitted: said once, the next press goes.
						warning = w;
						warningUntil = ml::TickMs() + 8000;
						go = false;
					}
				if (go && !on)
					if (std::string why; !shift::MayWork(why))
					{
						warning = why;
						warningUntil = ml::TickMs() + 6000;
						go = false;
					}
				if (go)
				{
					warningUntil = 0;
					Fade(true);
					officer::Dress(!on);
					if (station.door)
					{
						// Out of the door again, facing away from it.
						ENTITY::SET_ENTITY_COORDS(ped, station.locker.x, station.locker.y, station.locker.z, false, false, false, false);
						ENTITY::SET_ENTITY_HEADING(ped, station.locker.heading);
						CAMERA::SET_GAMEPLAY_CAM_RELATIVE_HEADING(0.0f);
						ml::Wait(1200);
					}
					ml::Wait(300);
					Fade(false);
					if (on)
					{
						ml::Notify("已下班");
						shift::ClockOut();
					}
					else
					{
						ml::Notify("已上班：{}。到停車場選巡邏車", station.name);
						shift::ClockIn();
					}
					ml::Log("duty: {}", on ? "off" : "on");
				}
			}
		}

		// The way between the corridor and the lot (a locker room only).
		for (const auto& [from, to, text] : {std::tuple{station.doorInside, station.doorOutside, "按 ~INPUT_CONTEXT~ 去停車場"},
		         std::tuple{station.doorOutside, station.doorInside, "按 ~INPUT_CONTEXT~ 進警局"}})
		{
			// (not while booking someone at Mission Row's back door: the same spot)
			if (station.door || Distance(at, from) >= 25.0f || custody::Busy())
				continue;
			Marker(from);
			if (Distance(at, from) < 1.2f)
			{
				Help(text);
				if (Pressed(kContext))
				{
					Fade(true);
					life::Load(to);
					ENTITY::SET_ENTITY_COORDS(ped, to.x, to.y, to.z, false, false, false, false);
					ENTITY::SET_ENTITY_HEADING(ped, to.heading);
					CAMERA::SET_GAMEPLAY_CAM_RELATIVE_HEADING(0.0f);
					ml::Wait(300);
					Fade(false);
				}
			}
		}

		// The lot: a patrol car while on duty.
		if (OnDuty() && Distance(at, station.lot) < 30.0f)
		{
			if (g_picking)
				Pick(station.cars);
			else
			{
				Marker(station.lot);
				if (Distance(at, station.lot) < 1.2f)
				{
					Help("按 ~INPUT_CONTEXT~ 選一台巡邏車");
					if (Pressed(kContext))
						StartPicking();
				}
			}
		}
		else if (g_picking)
			StopPicking(false);
	}
}
