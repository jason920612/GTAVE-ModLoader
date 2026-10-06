// Trainer: player, vehicle, weapon, teleport and world options in the loader window ("模組功能" tab).
// Also the reference mod for the menu API (pages, toggles, numbers, lists, actions, hotkeys, notifications).
#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include <modloader/natives.hpp>

ML_MOD_INFO("修改器", "0.1.0", "ModLoader", "玩家、載具、武器、傳送與世界選項")

namespace
{
	// ---- options read every frame ------------------------------------------------------------

	ml::Item g_god, g_wanted, g_stamina, g_superJump, g_fastRun, g_invisible, g_noRagdoll;
	ml::Item g_vehicleGod, g_autoRepair;
	ml::Item g_infiniteAmmo, g_noReload;
	ml::Item g_hour, g_freezeTime, g_weather, g_density;

	// Index 0 = leave the game's weather alone.
	constexpr std::array kWeathers = {"", "EXTRASUNNY", "CLEAR", "CLOUDS", "OVERCAST", "RAIN", "THUNDER", "CLEARING", "SMOG", "FOGGY", "XMAS",
	    "SNOWLIGHT", "BLIZZARD", "HALLOWEEN"};

	Ped Self() { return PLAYER::PLAYER_PED_ID(); }

	// The player's vehicle, or 0.
	Vehicle CurrentVehicle()
	{
		const Ped ped = Self();
		return PED::IS_PED_IN_ANY_VEHICLE(ped, false) ? PED::GET_VEHICLE_PED_IS_IN(ped, false) : 0;
	}

	// ---- teleport ------------------------------------------------------------------------------

	// Moves the player (with their vehicle) to x, y. `exact` = a known height (rooftops). Otherwise the ground is
	// searched while the collision there streams in: first just above `hint` (blips carry a height; this keeps
	// the player off roofs above the spot), then from the sky down.
	void TeleportTo(float x, float y, std::optional<float> exact = std::nullopt, std::optional<float> hint = std::nullopt)
	{
		const Vehicle vehicle = CurrentVehicle();
		const Entity entity = vehicle ? vehicle : Self();
		const auto place = [&](float z) {
			ENTITY::SET_ENTITY_COORDS_NO_OFFSET(entity, x, y, z, false, false, false);
			if (vehicle)
				VEHICLE::SET_VEHICLE_ON_GROUND_PROPERLY(vehicle, 5.0f);
		};
		if (exact)
		{
			STREAMING::REQUEST_COLLISION_AT_COORD(x, y, *exact);
			ENTITY::SET_ENTITY_COORDS_NO_OFFSET(entity, x, y, *exact, false, false, false);
			return;
		}
		if (hint)
		{
			for (int i = 0; i < 20; ++i) // up to ~1 s for the collision to stream in
			{
				STREAMING::REQUEST_COLLISION_AT_COORD(x, y, *hint);
				ENTITY::SET_ENTITY_COORDS_NO_OFFSET(entity, x, y, *hint + 1.0f, false, false, false);
				ml::Wait(50);
				float ground = 0;
				if (MISC::GET_GROUND_Z_FOR_3D_COORD(x, y, *hint + 2.0f, &ground, false, false) && ground > *hint - 5.0f)
				{
					place(ground + 1.0f);
					return;
				}
			}
		}
		for (float probe = 950.0f; probe >= -50.0f; probe -= 50.0f)
		{
			STREAMING::REQUEST_COLLISION_AT_COORD(x, y, probe);
			ENTITY::SET_ENTITY_COORDS_NO_OFFSET(entity, x, y, probe, false, false, false);
			ml::Wait(50);
			float ground = 0;
			if (MISC::GET_GROUND_Z_FOR_3D_COORD(x, y, probe, &ground, false, false))
			{
				place(ground + 1.0f);
				return;
			}
		}
		// No ground found (e.g. open water): stay at sea level.
		place(1.0f);
	}

	// Blips at map positions have height 0 or a real height; only the latter helps.
	std::optional<float> HeightOf(const Vector3& at)
	{
		return std::abs(at.z) > 1.0f ? std::optional<float>(at.z) : std::nullopt;
	}

	constexpr int kWaypointSprite = 8;
	constexpr int kMaxSprite = 900;

	// Mission objective: a blip with a GPS route first, then a yellow blip (the colour missions use for goals).
	std::optional<Vector3> FindObjective()
	{
		std::optional<Vector3> yellow;
		for (int sprite = 0; sprite < kMaxSprite; ++sprite)
		{
			if (sprite == kWaypointSprite)
				continue;
			for (Blip blip = HUD::GET_FIRST_BLIP_INFO_ID(sprite); HUD::DOES_BLIP_EXIST(blip); blip = HUD::GET_NEXT_BLIP_INFO_ID(sprite))
			{
				if (HUD::DOES_BLIP_HAVE_GPS_ROUTE(blip))
					return HUD::GET_BLIP_INFO_ID_COORD(blip);
				const int colour = HUD::GET_BLIP_COLOUR(blip);
				if (!yellow && (colour == 5 || colour == 66))
					yellow = HUD::GET_BLIP_INFO_ID_COORD(blip);
			}
		}
		return yellow;
	}

	void TeleportToWaypoint()
	{
		const Blip blip = HUD::GET_FIRST_BLIP_INFO_ID(kWaypointSprite);
		if (!HUD::DOES_BLIP_EXIST(blip))
		{
			ml::Notify("地圖上沒有標記點");
			return;
		}
		const Vector3 at = HUD::GET_BLIP_INFO_ID_COORD(blip);
		TeleportTo(at.x, at.y, std::nullopt, HeightOf(at));
		ml::Notify("已傳送到標記點");
	}

	void TeleportToObjective()
	{
		const auto at = FindObjective();
		if (!at)
		{
			ml::Notify("找不到任務目標，改傳送到地圖標記點");
			TeleportToWaypoint();
			return;
		}
		TeleportTo(at->x, at->y, std::nullopt, HeightOf(*at));
		ml::Notify("已傳送到任務目標");
	}

	struct Location
	{
		const char* name;
		float x, y;
		std::optional<float> z; // set for places above the ground (rooftops)
	};
	constexpr Location kLocations[] = {
	    {"洛聖都國際機場", -1336.0f, -3044.0f, std::nullopt},
	    {"麥克的家", -813.0f, 179.0f, std::nullopt},
	    {"富蘭克林的家（葡萄籽山）", 7.0f, 528.0f, std::nullopt},
	    {"崔佛的拖車", 1983.0f, 3820.0f, std::nullopt},
	    {"奇里亞德山山頂", 501.0f, 5604.0f, std::nullopt},
	    {"沙灘海岸機場", 1747.0f, 3273.0f, std::nullopt},
	    {"德爾佩羅碼頭", -1850.0f, -1231.0f, std::nullopt},
	    {"贊庫多堡軍事基地", -2047.0f, 3132.0f, std::nullopt},
	    {"美澤銀行大樓樓頂", -75.0f, -818.0f, 326.2f},
	};

	// ---- player -----------------------------------------------------------------------------------

	void Heal()
	{
		const Ped ped = Self();
		ENTITY::SET_ENTITY_HEALTH(ped, ENTITY::GET_ENTITY_MAX_HEALTH(ped), 0, 0);
		PED::SET_PED_ARMOUR(ped, PLAYER::GET_PLAYER_MAX_ARMOUR(PLAYER::PLAYER_ID()));
		ml::Notify("血量與護甲已補滿");
	}

	// ---- vehicle -----------------------------------------------------------------------------------

	Vehicle RequireVehicle()
	{
		const Vehicle vehicle = CurrentVehicle();
		if (!vehicle)
			ml::Notify("請先坐進一台載具");
		return vehicle;
	}

	void Repair(Vehicle vehicle)
	{
		VEHICLE::SET_VEHICLE_FIXED(vehicle);
		VEHICLE::SET_VEHICLE_DEFORMATION_FIXED(vehicle);
		VEHICLE::SET_VEHICLE_DIRT_LEVEL(vehicle, 0.0f);
	}

	void MaxMods(Vehicle vehicle)
	{
		VEHICLE::SET_VEHICLE_MOD_KIT(vehicle, 0);
		for (int type = 0; type < 50; ++type)
			if (const int count = VEHICLE::GET_NUM_VEHICLE_MODS(vehicle, type); count > 0)
				VEHICLE::SET_VEHICLE_MOD(vehicle, type, count - 1, false);
		for (const int toggle : {18, 22}) // turbo, xenon lights
			VEHICLE::TOGGLE_VEHICLE_MOD(vehicle, toggle, true);
	}

	struct Colour
	{
		const char* name;
		int r, g, b;
	};
	constexpr Colour kColours[] = {
	    {"黑色", 8, 8, 8}, {"白色", 240, 240, 240}, {"銀色", 160, 165, 170}, {"紅色", 180, 10, 10}, {"橘色", 230, 100, 10},
	    {"黃色", 240, 200, 10}, {"綠色", 20, 130, 40}, {"藍色", 15, 50, 160}, {"紫色", 90, 20, 140}, {"粉紅色", 230, 90, 160},
	};

	// ---- weapons ------------------------------------------------------------------------------------

	constexpr const char* kWeapons[] = {"WEAPON_KNIFE", "WEAPON_NIGHTSTICK", "WEAPON_HAMMER", "WEAPON_BAT", "WEAPON_CROWBAR", "WEAPON_GOLFCLUB",
	    "WEAPON_BOTTLE", "WEAPON_DAGGER", "WEAPON_HATCHET", "WEAPON_KNUCKLE", "WEAPON_MACHETE", "WEAPON_FLASHLIGHT", "WEAPON_SWITCHBLADE",
	    "WEAPON_POOLCUE", "WEAPON_WRENCH", "WEAPON_BATTLEAXE", "WEAPON_PISTOL", "WEAPON_COMBATPISTOL", "WEAPON_APPISTOL", "WEAPON_PISTOL50",
	    "WEAPON_SNSPISTOL", "WEAPON_HEAVYPISTOL", "WEAPON_VINTAGEPISTOL", "WEAPON_FLAREGUN", "WEAPON_MARKSMANPISTOL", "WEAPON_REVOLVER",
	    "WEAPON_STUNGUN", "WEAPON_MICROSMG", "WEAPON_SMG", "WEAPON_ASSAULTSMG", "WEAPON_COMBATPDW", "WEAPON_MACHINEPISTOL", "WEAPON_MINISMG",
	    "WEAPON_PUMPSHOTGUN", "WEAPON_SAWNOFFSHOTGUN", "WEAPON_ASSAULTSHOTGUN", "WEAPON_BULLPUPSHOTGUN", "WEAPON_MUSKET", "WEAPON_HEAVYSHOTGUN",
	    "WEAPON_DBSHOTGUN", "WEAPON_AUTOSHOTGUN", "WEAPON_ASSAULTRIFLE", "WEAPON_CARBINERIFLE", "WEAPON_ADVANCEDRIFLE", "WEAPON_SPECIALCARBINE",
	    "WEAPON_BULLPUPRIFLE", "WEAPON_COMPACTRIFLE", "WEAPON_MG", "WEAPON_COMBATMG", "WEAPON_GUSENBERG", "WEAPON_SNIPERRIFLE",
	    "WEAPON_HEAVYSNIPER", "WEAPON_MARKSMANRIFLE", "WEAPON_RPG", "WEAPON_GRENADELAUNCHER", "WEAPON_MINIGUN", "WEAPON_FIREWORK",
	    "WEAPON_RAILGUN", "WEAPON_HOMINGLAUNCHER", "WEAPON_COMPACTLAUNCHER", "WEAPON_GRENADE", "WEAPON_BZGAS", "WEAPON_SMOKEGRENADE",
	    "WEAPON_FLARE", "WEAPON_MOLOTOV", "WEAPON_STICKYBOMB", "WEAPON_PROXMINE", "WEAPON_PIPEBOMB", "WEAPON_SNOWBALL", "WEAPON_BALL",
	    "WEAPON_PETROLCAN", "WEAPON_FIREEXTINGUISHER", "GADGET_PARACHUTE"};

	void GiveAllWeapons()
	{
		const Ped ped = Self();
		int given = 0;
		for (const char* name : kWeapons)
		{
			const Hash weapon = MISC::GET_HASH_KEY(name);
			if (!WEAPON::IS_WEAPON_VALID(weapon))
				continue;
			WEAPON::GIVE_WEAPON_TO_PED(ped, weapon, 9999, false, false);
			++given;
		}
		ml::Notify("已給予 {} 種武器", given);
	}

	// ---- models: vehicle spawning and player model ------------------------------------------------

	ml::Page g_spawnPage, g_modelPage;
	ml::Item g_warpIn;
	Hash g_originalModel = 0;

	bool LoadModel(Hash model)
	{
		STREAMING::REQUEST_MODEL(model);
		for (int i = 0; i < 100 && !STREAMING::HAS_MODEL_LOADED(model); ++i) // up to 5 s
			ml::Wait(50);
		return STREAMING::HAS_MODEL_LOADED(model);
	}

	void SpawnVehicle(Hash model, const std::string& label)
	{
		if (!LoadModel(model))
		{
			ml::Notify("{} 載入失敗", label);
			return;
		}
		const Ped ped = Self();
		const float heading = ENTITY::GET_ENTITY_HEADING(ped);
		const Vector3 at = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(ped, 0.0f, g_warpIn ? 0.0f : 5.0f, 0.5f);
		const Vehicle vehicle = VEHICLE::CREATE_VEHICLE(model, at.x, at.y, at.z, heading, false, false, false);
		STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED(model);
		if (!vehicle)
		{
			ml::Notify("無法生成 {}", label);
			return;
		}
		VEHICLE::SET_VEHICLE_ON_GROUND_PROPERLY(vehicle, 5.0f);
		if (g_warpIn)
			PED::SET_PED_INTO_VEHICLE(ped, vehicle, -1);
		ml::Notify("已生成 {}", label);
	}

	void ChangeModel(Hash model, const std::string& label)
	{
		if (!LoadModel(model))
		{
			ml::Notify("{} 載入失敗", label);
			return;
		}
		if (!g_originalModel)
			g_originalModel = ENTITY::GET_ENTITY_MODEL(Self());
		PLAYER::SET_PLAYER_MODEL(PLAYER::PLAYER_ID(), model);
		PED::SET_PED_DEFAULT_COMPONENT_VARIATION(Self());
		STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED(model);
		ml::Notify("已換成 {}", label);
	}

	// Localised name of a vehicle ("NULL" or empty when the label is missing: add-on packs often have none).
	std::string VehicleLabel(const ml::Model& m)
	{
		const char* label = VEHICLE::GET_DISPLAY_NAME_FROM_VEHICLE_MODEL(m.hash);
		std::string text = label && HUD::DOES_TEXT_LABEL_EXIST(label) ? HUD::GET_FILENAME_FOR_AUDIO_CONVERSATION(label) : "";
		const std::string& model = m.name;
		if (text.empty() || text == "NULL")
			text = label && *label && std::string(label) != "CARNOTFOUND" ? label : model;
		// The game writes a narrow no-break space as "µ" in some names ("FMJµMKµV").
		for (size_t at; (at = text.find("\xC2\xB5")) != std::string::npos;)
			text.replace(at, 2, " ");
		std::string lower = text;
		std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return lower == model ? text : std::format("{}  ({})", text, model);
	}

	constexpr const char* kVehicleClasses[] = {"小型車", "轎車", "休旅車", "雙門跑車", "肌肉車", "經典跑車", "跑車", "超級跑車", "機車", "越野車",
	    "工業用車", "工具車", "廂型車", "腳踏車", "船", "直升機", "飛機", "公務車", "緊急車輛", "軍用車", "商用車", "火車", "開輪式賽車"};

	// Case-insensitive order for menu labels.
	void SortLabels(std::vector<std::pair<std::string, Hash>>& list)
	{
		const auto lower = [](std::string t) {
			std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return t;
		};
		std::sort(list.begin(), list.end(), [&](const auto& a, const auto& b) { return lower(a.first) < lower(b.first); });
	}

	void BuildVehicleList(const std::vector<ml::Model>& models)
	{
		std::map<std::string, std::vector<std::pair<std::string, Hash>>> byGroup; // group -> (label, hash)
		for (const auto& m : models)
		{
			const int cls = VEHICLE::GET_VEHICLE_CLASS_FROM_NAME(m.hash);
			std::string group = !m.pack.empty() ? "0 模組載具" : cls >= 0 && cls < static_cast<int>(std::size(kVehicleClasses))
			                                                          ? std::format("{:02} {}", cls + 1, kVehicleClasses[cls])
			                                                          : "99 其他";
			byGroup[group].emplace_back(VehicleLabel(m), m.hash);
		}
		g_spawnPage.Clear();
		for (auto& [group, list] : byGroup)
		{
			SortLabels(list);
			const ml::Page page = g_spawnPage.Sub(std::format("{}（{}）", group.substr(group.find(' ') + 1), list.size()).c_str());
			for (const auto& [label, hash] : list)
				page.Action(label.c_str(), [hash, label] { SpawnVehicle(hash, label); });
		}
	}

	// Ped groups by the game's naming scheme.
	std::string PedGroup(const ml::Model& m)
	{
		const auto& n = m.name;
		if (!m.pack.empty())
			return "0 模組角色";
		if (n.starts_with("a_c_"))
			return "2 動物";
		if (n.starts_with("a_"))
			return "1 路人";
		if (n.starts_with("cs_") || n.starts_with("csb_") || n.starts_with("ig_"))
			return "3 劇情角色";
		if (n.starts_with("g_"))
			return "4 幫派";
		if (n.starts_with("s_"))
			return "5 職業";
		if (n.starts_with("u_"))
			return "6 特殊";
		if (n.starts_with("player_") || n.starts_with("mp_"))
			return "7 主角與線上角色";
		return "8 其他";
	}

	void BuildPedList(const std::vector<ml::Model>& models)
	{
		std::map<std::string, std::vector<std::pair<std::string, Hash>>> byGroup;
		for (const auto& m : models)
			byGroup[PedGroup(m)].emplace_back(m.name, m.hash);
		g_modelPage.Clear();
		g_modelPage.Text("任務進行中更換角色，可能讓任務無法繼續。");
		g_modelPage.Action("恢復原本的角色", [] {
			if (g_originalModel)
				ChangeModel(g_originalModel, "原本的角色");
			else
				ml::Notify("目前就是原本的角色");
		});
		for (auto& [group, list] : byGroup)
		{
			SortLabels(list);
			const ml::Page page = g_modelPage.Sub(std::format("{}（{}）", group.substr(group.find(' ') + 1), list.size()).c_str());
			for (const auto& [label, hash] : list)
				page.Action(label.c_str(), [hash, label] { ChangeModel(hash, label); });
		}
	}

	// The loader reads the model names in the background after the game starts: false = not yet, try later.
	bool BuildModelLists()
	{
		bool ready = false;
		auto vehicles = ml::Models(ML_MODEL_VEHICLE, &ready);
		if (!ready)
			return false;
		auto peds = ml::Models(ML_MODEL_PED);
		// A model without a name has no model file in any archive: the game registers it but cannot load it.
		const auto loadable = [](std::vector<ml::Model>& models, const char* what) {
			const size_t all = models.size();
			std::erase_if(models, [](const ml::Model& m) { return m.name.empty() || !STREAMING::IS_MODEL_IN_CDIMAGE(m.hash); });
			ml::Log("{}: {} listed, {} without model files left out", what, models.size(), all - models.size());
		};
		loadable(vehicles, "vehicles");
		loadable(peds, "peds");
		BuildVehicleList(vehicles);
		BuildPedList(peds);
		return true;
	}

	// ---- menu ---------------------------------------------------------------------------------------

	void BuildMenu()
	{
		const ml::Page root = ml::Root();

		const ml::Page player = root.Sub("玩家");
		g_god = player.Toggle("god", "無敵");
		g_wanted = player.List("wanted", "通緝", {"不改變", "永不通緝", "固定 1 星", "固定 2 星", "固定 3 星", "固定 4 星", "固定 5 星"});
		g_stamina = player.Toggle("stamina", "無限體力");
		g_superJump = player.Toggle("superJump", "超級跳");
		g_fastRun = player.Toggle("fastRun", "快跑");
		g_invisible = player.Toggle("invisible", "隱形");
		g_noRagdoll = player.Toggle("noRagdoll", "不會跌倒");
		g_modelPage = player.Sub("更換角色");
		g_modelPage.Text("正在讀取角色清單…");
		player.Action("補滿血量與護甲", Heal);
		player.Action("清除通緝", [] { PLAYER::CLEAR_PLAYER_WANTED_LEVEL(PLAYER::PLAYER_ID()); });

		const ml::Page vehicle = root.Sub("載具");
		g_spawnPage = vehicle.Sub("生成載具");
		g_spawnPage.Text("正在讀取載具清單…");
		g_warpIn = vehicle.Toggle("warpIn", "生成後直接坐進去", true);
		g_vehicleGod = vehicle.Toggle("vehicleGod", "載具無敵");
		g_autoRepair = vehicle.Toggle("autoRepair", "自動修理");
		vehicle.Action("修理並清潔", [] {
			if (const Vehicle v = RequireVehicle())
			{
				Repair(v);
				ml::Notify("載具已修好");
			}
		});
		vehicle.Action("改裝全滿", [] {
			if (const Vehicle v = RequireVehicle())
			{
				MaxMods(v);
				ml::Notify("已套用所有最高等級改裝");
			}
		});
		const ml::Page colours = vehicle.Sub("車身顏色");
		for (const Colour& c : kColours)
			colours.Action(c.name, [c] {
				if (const Vehicle v = RequireVehicle())
				{
					VEHICLE::SET_VEHICLE_CUSTOM_PRIMARY_COLOUR(v, c.r, c.g, c.b);
					VEHICLE::SET_VEHICLE_CUSTOM_SECONDARY_COLOUR(v, c.r, c.g, c.b);
				}
			});

		const ml::Page weapons = root.Sub("武器");
		weapons.Action("給予全部武器", GiveAllWeapons);
		g_infiniteAmmo = weapons.Toggle("infiniteAmmo", "無限彈藥");
		g_noReload = weapons.Toggle("noReload", "不用換彈匣");

		const ml::Page teleport = root.Sub("傳送");
		teleport.Action("傳送到任務目標", TeleportToObjective);
		teleport.Action("傳送到地圖標記點", TeleportToWaypoint);
		teleport.Action("往前 5 公尺", [] {
			const Vehicle vehicle = CurrentVehicle();
			const Entity e = vehicle ? vehicle : Self();
			const Vector3 at = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(e, 0.0f, 5.0f, 0.0f);
			ENTITY::SET_ENTITY_COORDS_NO_OFFSET(e, at.x, at.y, at.z, false, false, false);
		});
		const ml::Page places = teleport.Sub("常用地點");
		for (const Location& l : kLocations)
			places.Action(l.name, [l] {
				TeleportTo(l.x, l.y, l.z);
				ml::Notify("已傳送到 {}", l.name);
			});

		const ml::Page world = root.Sub("世界");
		g_hour = world.Number(nullptr, "時間（時）", 0, 23, 1, 12); // shows the game clock (see MLMain)
		g_hour.OnChange([] { CLOCK::SET_CLOCK_TIME(g_hour.Int(), 0, 0); });
		g_freezeTime = world.Toggle("freezeTime", "凍結時間");
		g_freezeTime.OnChange([] { CLOCK::PAUSE_CLOCK(static_cast<bool>(g_freezeTime)); });
		g_weather = world.List("weather", "天氣", {"不改變", "大晴天", "晴天", "多雲", "陰天", "雨天", "雷雨", "雨後放晴", "霧霾", "大霧", "下雪", "小雪", "暴風雪", "萬聖節"});
		g_weather.OnChange([] {
			const int i = g_weather.Int();
			if (i > 0 && i < static_cast<int>(kWeathers.size()))
				MISC::SET_WEATHER_TYPE_NOW_PERSIST(kWeathers[i]);
			else
				MISC::CLEAR_WEATHER_TYPE_PERSIST();
		});
		g_density = world.Number("density", "路人與車流密度", 0.0f, 1.0f, 0.1f, 1.0f);

		ml::Hotkey("hkObjective", "傳送到任務目標", 0x75 /* F6 */, TeleportToObjective);
		ml::Hotkey("hkWaypoint", "傳送到地圖標記點", 0x76 /* F7 */, TeleportToWaypoint);
		ml::Hotkey("hkHeal", "補滿血量與護甲", 0, Heal);
		ml::Hotkey("hkRepair", "修理載具", 0, [] {
			if (const Vehicle v = RequireVehicle())
				Repair(v);
		});
	}

	// ---- per frame ----------------------------------------------------------------------------------

	struct Applied
	{
		Ped ped = 0;
		int god = -1, fastRun = -1, invisible = -1, noRagdoll = -1;
	};

	// Options that the game keeps once set: applied when they change, and again for a new player ped (character switch).
	template<class F>
	void Sync(int& last, bool now, bool force, F apply)
	{
		if (force || last != static_cast<int>(now))
		{
			last = now;
			apply(now);
		}
	}

	void Frame(Applied& a)
	{
		const Player player = PLAYER::PLAYER_ID();
		const Ped ped = Self();
		const bool newPed = ped != a.ped;
		a.ped = ped;

		Sync(a.god, static_cast<bool>(g_god), newPed, [&](bool on) { PLAYER::SET_PLAYER_INVINCIBLE(player, on); });
		Sync(a.fastRun, static_cast<bool>(g_fastRun), newPed, [&](bool on) { PLAYER::SET_RUN_SPRINT_MULTIPLIER_FOR_PLAYER(player, on ? 1.49f : 1.0f); });
		Sync(a.invisible, static_cast<bool>(g_invisible), newPed, [&](bool on) { ENTITY::SET_ENTITY_VISIBLE(ped, !on, false); });
		Sync(a.noRagdoll, static_cast<bool>(g_noRagdoll), newPed, [&](bool on) { PED::SET_PED_CAN_RAGDOLL(ped, !on); });

		if (const int wanted = g_wanted.Int(); wanted == 1)
			PLAYER::CLEAR_PLAYER_WANTED_LEVEL(player);
		else if (wanted >= 2 && PLAYER::GET_PLAYER_WANTED_LEVEL(player) != wanted - 1)
		{
			PLAYER::SET_PLAYER_WANTED_LEVEL(player, wanted - 1, false);
			PLAYER::SET_PLAYER_WANTED_LEVEL_NOW(player, false);
		}
		if (g_stamina)
			PLAYER::RESTORE_PLAYER_STAMINA(player, 1.0f);
		if (g_superJump)
			MISC::SET_SUPER_JUMP_THIS_FRAME(player);

		if (const Vehicle vehicle = CurrentVehicle())
		{
			if (g_vehicleGod)
			{
				ENTITY::SET_ENTITY_INVINCIBLE(vehicle, true, false);
				VEHICLE::SET_VEHICLE_CAN_BE_VISIBLY_DAMAGED(vehicle, false);
				VEHICLE::SET_VEHICLE_TYRES_CAN_BURST(vehicle, false);
			}
			if (g_autoRepair && VEHICLE::GET_VEHICLE_ENGINE_HEALTH(vehicle) < 1000.0f)
				Repair(vehicle);
		}

		if (g_infiniteAmmo)
			WEAPON::SET_PED_INFINITE_AMMO(ped, true, WEAPON::GET_SELECTED_PED_WEAPON(ped));
		if (g_noReload)
			WEAPON::SET_PED_INFINITE_AMMO_CLIP(ped, true);

		const float density = g_density.Value();
		if (density < 1.0f)
		{
			PED::SET_PED_DENSITY_MULTIPLIER_THIS_FRAME(density);
			PED::SET_SCENARIO_PED_DENSITY_MULTIPLIER_THIS_FRAME(density, density);
			VEHICLE::SET_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME(density);
			VEHICLE::SET_RANDOM_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME(density);
			VEHICLE::SET_PARKED_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME(density);
		}
	}
}

extern "C" __declspec(dllexport) int MLOnLoad(const MLApi* api, const MLContext* ctx)
{
	ml::Init(api, ctx);
	if (!ml::HasMenus() || !ml::HasModels())
	{
		ml::LogError("this loader has no menu support; update ModLoader");
		return 0;
	}
	BuildMenu();
	return 1;
}

extern "C" __declspec(dllexport) void MLMain()
{
	Applied applied;
	if (g_freezeTime)
		CLOCK::PAUSE_CLOCK(true);
	if (const int w = g_weather.Int(); w > 0 && w < static_cast<int>(kWeathers.size()))
		MISC::SET_WEATHER_TYPE_NOW_PERSIST(kWeathers[w]);
	bool listsBuilt = false;
	for (uint64_t nextClock = 0;;)
	{
		if (ml::TickMs() >= nextClock)
		{
			g_hour.Set(static_cast<float>(CLOCK::GET_CLOCK_HOURS()));
			if (!listsBuilt)
				listsBuilt = BuildModelLists();
			nextClock = ml::TickMs() + 1000;
		}
		Frame(applied);
		ml::Wait(0);
	}
}
