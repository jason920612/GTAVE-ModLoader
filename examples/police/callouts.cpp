// The other callouts (the user's list, after the stolen car of dispatch.cpp): a store robbery, a traffic accident, a
// fight, shots fired, a suspicious person. Dispatch offers them as it does the stolen car (F9, in the beat when it can
// be); this file makes each scene when the officer gets near and tells dispatch when it is over.
// - Store robbery: a 24/7, LTD or Rob's Liquor; the robber still inside holding up the clerk, or seen running off
//   (then where they were last seen, like the stolen car's reports).
// - Traffic accident: two cars crashed at the roadside (damaged, hazard lights), their drivers by them (one of them
//   drunk, sometimes), sometimes someone hurt on the ground: the officer secures it, checks who they are, calls the
//   ambulance and the tow truck (F9); over once the hurt are taken and the cars towed.
// - Fight: two people fighting in the street (drunk, sometimes a knife); they stop, run or turn on the officer.
// - Shots fired: an armed person somewhere around where it was heard (an area to search), sometimes someone hit; a
//   backup unit goes too.
// - Suspicious person: someone hanging about cars or houses; innocent or not (carrying controlled substances,
//   burglary tools): check them, let them go or arrest them.
// The people are the suspect AI's (suspect.cpp: those with nothing to hide comply, the action menu works on all).
#include "police.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <vector>

namespace police::callouts
{
	namespace
	{
		using ai::Distance;

		struct Store
		{
			const char* name;
			float x, y, z; // the cashier
		};
		const Store kStores[] = {
		    {"草莓區 24/7", 24.5f, -1347.3f, 29.5f},       {"葛洛夫街 LTD", -46.7f, -1758.1f, 29.4f},       {"小首爾 LTD", -706.2f, -914.5f, 19.2f},
		    {"威斯普奇 Rob's Liquor", -1222.0f, -908.3f, 12.3f}, {"好麥塢 24/7", 372.6f, 326.6f, 103.6f},  {"鏡子公園 LTD", 1164.6f, -323.6f, 69.2f},
		    {"埃爾蘭喬 Rob's Liquor", 1134.3f, -982.3f, 46.4f}, {"晨木 Rob's Liquor", -1486.3f, -377.6f, 40.2f}, {"沙灘鎮 24/7", 1960.1f, 3740.0f, 32.3f},
		    {"哈莫尼 24/7", 549.3f, 2671.3f, 42.2f},        {"葡萄籽 LTD", 1697.5f, 4923.0f, 42.1f},          {"佩立托灣 24/7", 1728.6f, 6417.0f, 35.0f},
		};

		struct Scene
		{
			Vector3 at{};
			Kind kind = Kind::Robbery;
			float heading = 0;
			std::string place, title;
			bool made = false, arrived = false;
			std::vector<int> suspects;     // everyone involved (suspect AI)
			std::vector<::Ped> extras;     // the clerk, the hurt
			std::vector<Vehicle> cars;     // the crashed cars
			Blip area = 0;
			uint64_t since = 0, deadline = 0, nextReport = 0;
			bool fled = false;     // the robber ran off before the officer came
			bool ambulance = false;
		};
		Scene g;
		bool g_active = false;

		std::string Text(const char* label)
		{
			const char* t = label ? HUD::GET_FILENAME_FOR_AUDIO_CONVERSATION(label) : nullptr;
			return t && *t && std::string(t) != "NULL" ? t : "";
		}
		std::string Zone(const Vector3& at)
		{
			const std::string z = Text(ZONE::GET_NAME_OF_ZONE(at.x, at.y, at.z));
			return z.empty() ? "市區" : z;
		}

		bool Exists(Entity e)
		{
			return e && ENTITY::DOES_ENTITY_EXIST(e);
		}

		// A road for a scene, 450-850 m from the officer (in the beat when it can be), not a motorway or a junction.
		bool RoadPlace(Vector3& at, float& heading)
		{
			const Vector3 me = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), true);
			bool found = false;
			for (int i = 0; i < 12; ++i)
			{
				const float a = ai::Random() * 6.283f, r = 450.0f + ai::Random() * 400.0f;
				Vector3 node{};
				float h = 0;
				int lanes = 0;
				if (!PATH::GET_NTH_CLOSEST_VEHICLE_NODE_WITH_HEADING(me.x + std::cos(a) * r, me.y + std::sin(a) * r, me.z, 1, &node, &h, &lanes, 0, 3.0f, 0.0f))
					continue;
				int density = 0, flags = 0;
				PATH::GET_VEHICLE_NODE_PROPERTIES(node.x, node.y, node.z, &density, &flags);
				if ((flags & 0xC0) != 0 || Distance(node, me) < 300.0f)
					continue;
				at = node, heading = h, found = true;
				if (shift::InBeat(node))
					break;
			}
			return found;
		}

		// The kerb on the right of the way the road goes.
		Vector3 Kerb(const Vector3& node, float heading, float in)
		{
			const float h = heading * 0.0174533f;
			const Vector3 right{std::cos(h), std::sin(h), 0.0f};
			for (const float way : {heading, heading + 180.0f})
				if (Vector3 edge{}; PATH::GET_ROAD_BOUNDARY_USING_HEADING(node.x, node.y, node.z, way, &edge) && Distance(edge, node) < 15.0f &&
				                    (edge.x - node.x) * right.x + (edge.y - node.y) * right.y > 0.5f)
					return {edge.x - right.x * in, edge.y - right.y * in, edge.z};
			return node;
		}

		// A place on foot near a point, within `most` metres (else the point itself).
		Vector3 Foot(const Vector3& p, float most = 3.0f)
		{
			Vector3 out{};
			return ai::OnFoot(p.x, p.y, p.z, out) && Distance(out, p) < most ? out : p;
		}

		int Person(const Vector3& at, float heading, const ai::Profile& p)
		{
			const int id = suspect::Spawn(at, heading, p);
			if (id)
				g.suspects.push_back(id);
			return id;
		}

		ai::Profile Innocent()
		{
			ai::Profile p = suspect::RandomProfile(ai::Skill::Novice);
			p.guilt = 0.05f + ai::Random() * 0.15f;
			p.nerve = 0.1f + ai::Random() * 0.2f;
			p.hostility = 0.05f + ai::Random() * 0.15f;
			p.armed = 0;
			p.impairment = ai::Impairment::Sober;
			p.substanceGrade = 0;
			return p;
		}

		void Area(const Vector3& c, float radius)
		{
			if (g.area && HUD::DOES_BLIP_EXIST(g.area))
				HUD::REMOVE_BLIP(&g.area);
			g.area = HUD::ADD_BLIP_FOR_RADIUS(c.x, c.y, c.z, radius);
			HUD::SET_BLIP_COLOUR(g.area, 5);
			HUD::SET_BLIP_ALPHA(g.area, 110);
			HUD::SET_BLIP_ROUTE(g.area, true);
			HUD::SET_BLIP_ROUTE_COLOUR(g.area, 5);
		}

		::Ped Extra(const char* model, const Vector3& at, float heading)
		{
			const Hash hash = MISC::GET_HASH_KEY(model);
			const auto loaded = ml::LoadModel(hash);
			if (!loaded)
				return 0;
			const ::Ped p = PED::CREATE_PED(4, hash, at.x, at.y, at.z, heading, false, false);
			if (p)
			{
				ENTITY::SET_ENTITY_AS_MISSION_ENTITY(p, true, true);
				PED::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(p, true);
				g.extras.push_back(p);
			}
			return p;
		}

		// Someone hurt on the ground (for the ambulance).
		::Ped Hurt(const Vector3& at)
		{
			const ::Ped p = Extra(ai::Random() < 0.5f ? "a_m_y_business_02" : "a_f_y_tourist_01", at, 0.0f);
			if (p)
			{
				// On the ground in pain (an animation: the game's writhing task bleeds them out and blames its target).
				STREAMING::REQUEST_ANIM_DICT("combat@damage@writheidle_a");
				for (const uint64_t end = ml::TickMs() + 2000; ml::TickMs() < end && !STREAMING::HAS_ANIM_DICT_LOADED("combat@damage@writheidle_a");)
					ml::Wait(0);
				TASK::TASK_PLAY_ANIM(p, "combat@damage@writheidle_a", "writhe_idle_a", 8.0f, -8.0f, -1, 1, 0.0f, false, false, false);
				ENTITY::SET_ENTITY_INVINCIBLE(p, true, false);
				records::Note("現場有人受傷");
			}
			return p;
		}

		// ---- making each scene (when the officer is near) -------------------------------------------------------
		void MakeRobbery()
		{
			// The store's own shopkeeper (the game's), or one of ours when there is none.
			const Vector3 cashier{g.at.x, g.at.y, g.at.z};
			::Ped clerk = 0;
			int64_t peds[1 + 24] = {24};
			const int n = PED::GET_PED_NEARBY_PEDS(PLAYER::PLAYER_PED_ID(), reinterpret_cast<Any*>(peds), -1);
			for (int i = 0; i < n && i < 24 && !clerk; ++i)
				if (const ::Ped p = static_cast<::Ped>(peds[1 + i]);
				    p && ENTITY::GET_ENTITY_MODEL(p) == MISC::GET_HASH_KEY("mp_m_shopkeep_01") && Distance(ENTITY::GET_ENTITY_COORDS(p, true), cashier) < 6.0f)
					clerk = p;
			if (clerk)
			{
				ENTITY::SET_ENTITY_AS_MISSION_ENTITY(clerk, true, true);
				PED::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(clerk, true);
				g.extras.push_back(clerk);
			}
			else
				clerk = Extra("mp_m_shopkeep_01", cashier, 0.0f);
			if (clerk)
				TASK::TASK_HANDS_UP(clerk, -1, 0, -1, 0);
			ai::Profile p = suspect::RandomProfile(ai::Random() < 0.4f ? ai::Skill::Veteran : ai::Skill::Average);
			p.armed = 2;
			p.guilt = 0.95f;
			p.hostility = 0.5f + ai::Random() * 0.4f;
			p.nerve = 0.5f + ai::Random() * 0.4f;
			if (!g.fled)
			{
				// Inside, across the counter from the clerk (the way the clerk faces), the gun on them.
				const Vector3 c = clerk ? ENTITY::GET_ENTITY_COORDS(clerk, true) : cashier;
				const Vector3 across = clerk ? ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(clerk, 0.0f, 2.4f, 0.0f) : Vector3{c.x + 2.0f, c.y, c.z};
				const Vector3 at = Foot(across, 1.5f);
				if (const int id = Person(at, clerk ? ENTITY::GET_ENTITY_HEADING(clerk) + 180.0f : 0.0f, p); id && clerk)
					TASK::TASK_AIM_GUN_AT_ENTITY(suspect::StatusOf(id).ped, clerk, -1, false);
			}
			else
			{
				// Gone off on foot a block or two: seen there by someone.
				const float a = ai::Random() * 6.283f;
				const Vector3 away = Foot({cashier.x + std::cos(a) * 120.0f, cashier.y + std::sin(a) * 120.0f, cashier.z}, 40.0f);
				Person(away, 0.0f, p);
			}
			records::Evidence("店內監視器畫面");
		}

		void MakeAccident()
		{
			const float h = g.heading * 0.0174533f;
			const Vector3 forward{-std::sin(h), std::cos(h), 0.0f};
			const Vector3 a = Kerb(g.at, g.heading, 1.3f);
			const Vector3 b{a.x + forward.x * 5.5f, a.y + forward.y * 5.5f, a.z};
			const char* const models[] = {"asea", "premier", "ingot", "stanier", "washington", "emperor", "primo", "intruder", "fugitive", "granger"};
			for (int i = 0; i < 2; ++i)
			{
				const Hash model = MISC::GET_HASH_KEY(models[static_cast<size_t>(ai::Random() * std::size(models)) % std::size(models)]);
				const auto loaded = ml::LoadModel(model);
				const Vector3& spot = i == 0 ? a : b;
				const Vehicle v = loaded ? VEHICLE::CREATE_VEHICLE(model, spot.x, spot.y, spot.z + 0.4f, g.heading + (i == 0 ? 8.0f : -25.0f), false, false, false) : 0;
				if (!v)
					continue;
				ENTITY::SET_ENTITY_AS_MISSION_ENTITY(v, true, true);
				VEHICLE::SET_VEHICLE_ON_GROUND_PROPERLY(v, 5.0f);
				// The first hit the second from behind.
				VEHICLE::SET_VEHICLE_DAMAGE(v, 0.0f, i == 0 ? 2.0f : -2.0f, 0.2f, 900.0f, 250.0f, true);
				VEHICLE::SET_VEHICLE_ENGINE_HEALTH(v, 250.0f);
				VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(v, 0, true);
				VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(v, 1, true);
				VEHICLE::SET_VEHICLE_DOOR_OPEN(v, 0, false, false);
				g.cars.push_back(v);
				// Its driver beside it (one of them drunk, sometimes: the one who hit the other).
				ai::Profile p = Innocent();
				if (i == 0 && ai::Random() < 0.35f)
				{
					p.guilt = 0.75f;
					p.impairment = ai::Impairment::Drunk;
					p.nerve = 0.3f + ai::Random() * 0.5f;
				}
				const Vector3 by = Foot(ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(v, 1.8f, 0.0f, 0.0f));
				Person(by, g.heading + 90.0f, p);
				records::VehicleOf(v); // (the plates for the computer)
			}
			if (ai::Random() < 0.5f)
				Hurt(Foot(ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(g.cars.empty() ? 0 : g.cars.back(), 2.5f, 2.0f, 0.0f)));
		}

		void MakeFight()
		{
			const Vector3 a = Foot(Kerb(g.at, g.heading, -3.0f), 8.0f);
			for (int i = 0; i < 2; ++i)
			{
				ai::Profile p = suspect::RandomProfile(ai::Skill::Average);
				p.hostility = 0.6f + ai::Random() * 0.3f;
				p.nerve = 0.4f + ai::Random() * 0.4f;
				p.guilt = 0.6f + ai::Random() * 0.2f;
				p.armed = i == 0 && ai::Random() < 0.25f ? 1 : 0;
				if (ai::Random() < 0.5f)
					p.impairment = ai::Impairment::Drunk;
				Person(Foot({a.x + (i ? 1.2f : -1.2f), a.y, a.z}), i ? 270.0f : 90.0f, p);
			}
			if (g.suspects.size() == 2)
			{
				// At it until the police come (unhurt: the game's fists would beat one of them to death meanwhile).
				const ::Ped x = suspect::StatusOf(g.suspects[0]).ped, y = suspect::StatusOf(g.suspects[1]).ped;
				ENTITY::SET_ENTITY_INVINCIBLE(x, true, false);
				ENTITY::SET_ENTITY_INVINCIBLE(y, true, false);
				TASK::TASK_COMBAT_PED(x, y, 0, 16);
				TASK::TASK_COMBAT_PED(y, x, 0, 16);
			}
			records::Charge("傷害");
		}

		void MakeShots()
		{
			// The shooter somewhere around where it was heard (the area dispatch gives).
			const float a = ai::Random() * 6.283f;
			const Vector3 at = Foot({g.at.x + std::cos(a) * 60.0f, g.at.y + std::sin(a) * 60.0f, g.at.z}, 40.0f);
			ai::Profile p = suspect::RandomProfile(ai::Skill::Veteran);
			p.armed = 2;
			p.hostility = 0.8f + ai::Random() * 0.15f;
			p.nerve = 0.6f + ai::Random() * 0.3f;
			p.guilt = 0.95f;
			Person(at, 0.0f, p);
			if (ai::Random() < 0.5f)
				Hurt(Foot(g.at, 10.0f));
			records::Charge("非法持有武器");
		}

		void MakeSuspicious()
		{
			const Vector3 at = Foot(Kerb(g.at, g.heading, -2.5f), 8.0f);
			ai::Profile p = Innocent();
			if (ai::Random() < 0.55f)
			{
				// Something to hide.
				p.guilt = 0.6f + ai::Random() * 0.35f;
				p.nerve = 0.3f + ai::Random() * 0.6f;
				p.hostility = 0.2f + ai::Random() * 0.4f;
				if (ai::Random() < 0.5f)
					p.substanceGrade = 2 + static_cast<int>(ai::Random() * 2.0f);
				else
					p.armed = 1;
			}
			Person(at, g.heading, p);
		}

		void Make()
		{
			g.made = true;
			switch (g.kind)
			{
			case Kind::Robbery: MakeRobbery(); break;
			case Kind::Accident: MakeAccident(); break;
			case Kind::Fight: MakeFight(); break;
			case Kind::Shots: MakeShots(); break;
			case Kind::Suspicious: MakeSuspicious(); break;
			}
			ml::Log("callouts: the scene is there ({} people, {} cars)", g.suspects.size(), g.cars.size());
		}

		// Everyone involved dealt with (booked, let go, gone, dead): the outcome; "" = not yet.
		std::string Resolved()
		{
			int arrested = 0, released = 0, escaped = 0, dead = 0;
			for (const int id : g.suspects)
			{
				const suspect::Status st = suspect::StatusOf(id);
				if (custody::Booked(id))
					++arrested;
				else if (st.dead)
					++dead;
				else if (st.released)
					++released;
				else if (st.gone)
					++escaped;
				else
					return "";
			}
			std::vector<std::string> parts;
			if (arrested)
				parts.push_back(std::format("逮捕 {} 人", arrested));
			if (released)
				parts.push_back(std::format("盤查後放行 {} 人", released));
			if (escaped)
				parts.push_back(std::format("{} 人逃逸", escaped));
			if (dead)
				parts.push_back(std::format("{} 人死亡", dead));
			std::string s;
			for (const std::string& p : parts)
				s += (s.empty() ? "" : "、") + p;
			return s.empty() ? "處理完畢" : s;
		}

		void Clean()
		{
			if (g.area && HUD::DOES_BLIP_EXIST(g.area))
				HUD::REMOVE_BLIP(&g.area);
			for (const int id : g.suspects)
			{
				const suspect::Status st = suspect::StatusOf(id);
				if (!st.gone && !st.cuffed && !st.custody)
					suspect::Release(id);
			}
			for (::Ped& p : g.extras)
				if (Exists(p))
				{
					PED::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(p, false);
					TASK::CLEAR_PED_TASKS(p);
					ENTITY::SET_PED_AS_NO_LONGER_NEEDED(&p);
				}
			for (Vehicle& v : g.cars)
				if (Exists(v) && !services::Towing(v))
					ENTITY::SET_VEHICLE_AS_NO_LONGER_NEEDED(&v);
			g = Scene{};
			g_active = false;
		}
	}

	bool Plan(Kind kind, Offer& out)
	{
		Vector3 at{};
		float heading = 0;
		out = {};
		out.kind = kind;
		if (kind == Kind::Robbery)
		{
			// The nearest stores (500 m to 3 km off), one in the beat when it can be.
			const Vector3 me = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), true);
			std::vector<const Store*> stores;
			for (const Store& s : kStores)
				if (const float d = Distance({s.x, s.y, s.z}, me); d > 300.0f && d < 3000.0f)
					stores.push_back(&s);
			if (stores.empty())
				return false;
			std::sort(stores.begin(), stores.end(), [](const Store* a, const Store* b) { return shift::InBeat({a->x, a->y, a->z}) > shift::InBeat({b->x, b->y, b->z}); });
			const size_t pick = std::min<size_t>(stores.size(), 3);
			const Store* s = stores[static_cast<size_t>(ai::Random() * static_cast<float>(pick)) % pick];
			out.at = {s->x, s->y, s->z};
			out.place = s->name;
			out.fled = ai::Random() < 0.4f;
			out.title = std::string("商店搶案（") + s->name + "）";
			out.radio = out.fled ? std::string(s->name) + "剛被持槍搶劫，嫌犯已經徒步逃離" : std::string(s->name) + "有人持槍搶劫，嫌犯可能還在店內";
			return true;
		}
		if (!RoadPlace(at, heading))
			return false;
		out.at = at;
		out.heading = heading;
		out.place = Zone(at);
		switch (kind)
		{
		case Kind::Accident:
			out.title = "交通事故（" + out.place + "）";
			out.radio = out.place + "有兩車相撞，可能有人受傷";
			break;
		case Kind::Fight:
			out.title = "打架滋事（" + out.place + "）";
			out.radio = out.place + "有人在路邊打架";
			break;
		case Kind::Shots:
			out.title = "開槍（" + out.place + "）";
			out.radio = out.place + "有民眾報案聽到槍聲，可能有人中彈";
			break;
		case Kind::Suspicious:
			out.title = "可疑人物（" + out.place + "）";
			out.radio = out.place + "有民眾反映有人在車旁和住家附近徘徊";
			break;
		default:
			break;
		}
		return true;
	}

	void Begin(const Offer& offer)
	{
		Clean();
		g.kind = offer.kind;
		g.at = offer.at;
		g.heading = offer.heading;
		g.place = offer.place;
		g.title = offer.title;
		g.fled = offer.fled;
		g.since = ml::TickMs();
		g.deadline = g.since + (offer.kind == Kind::Accident ? 20 : 15) * 60000ull;
		g_active = true;
		Area(g.at, offer.kind == Kind::Shots ? 140.0f : offer.kind == Kind::Robbery && offer.fled ? 200.0f : 60.0f);
		records::BeginCase(offer.title, offer.place);
		records::Note("調度：" + offer.radio);
		if (offer.kind == Kind::Robbery)
			records::Charge("強盜");
		if (offer.kind == Kind::Shots)
			units::CallBackup(); // shots fired: a unit goes too
		ml::Log("callouts: {} accepted", offer.title);
	}

	bool Active()
	{
		return g_active;
	}

	std::string Tick()
	{
		if (!g_active)
			return "";
		const uint64_t now = ml::TickMs();
		const Vector3 me = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), true);
		const float d = Distance(me, g.at);
		// (a robbery still inside: when the store's own people are there, closer)
		if (!g.made && d < (g.kind == Kind::Robbery && !g.fled ? 70.0f : 200.0f))
			Make();
		if (!g.made)
		{
			if (now > g.deadline)
			{
				Clean();
				return "一直沒有到場，案件轉給其他單位";
			}
			return "";
		}
		if (!g.arrived && d < 40.0f)
		{
			g.arrived = true;
			records::Note("抵達現場");
			if (g.kind == Kind::Fight)
				for (const int id : g.suspects)
					if (const ::Ped p = suspect::StatusOf(id).ped)
						ENTITY::SET_ENTITY_INVINCIBLE(p, false, false);
			if (g.kind == Kind::Robbery && !g.fled)
				ml::Notify("店員：他還在裡面！他有槍！");
			else if (g.kind == Kind::Robbery)
				ml::Notify("店員：他往外跑了，剛走沒多久！");
			else if (g.kind == Kind::Accident)
				ml::Notify("現場：處理完後用 F9 叫救護車和拖吊車");
		}
		// The robber who ran: where they were last seen, now and then.
		if (g.kind == Kind::Robbery && g.fled && now >= g.nextReport && !g.suspects.empty())
		{
			g.nextReport = now + 30000;
			const suspect::Status st = suspect::StatusOf(g.suspects[0]);
			suspect::Target t;
			if (!st.gone && st.ped && !(suspect::Pursued(t) && t.known))
			{
				const Vector3 p = ENTITY::GET_ENTITY_COORDS(st.ped, true);
				const float off = 40.0f;
				Area({p.x + (ai::Random() - 0.5f) * off, p.y + (ai::Random() - 0.5f) * off, p.z}, 90.0f);
			}
			else if (g.area && HUD::DOES_BLIP_EXIST(g.area) && suspect::Pursued(t) && t.known)
				HUD::REMOVE_BLIP(&g.area);
		}
		else if (g.arrived && g.area && HUD::DOES_BLIP_EXIST(g.area) && g.kind != Kind::Shots)
			HUD::REMOVE_BLIP(&g.area);

		// Over: everyone dealt with (and, for an accident, the hurt taken and the cars towed).
		bool sceneClear = true;
		if (g.kind == Kind::Accident)
		{
			for (const Vehicle v : g.cars)
				if (Exists(v) && !services::Towing(v))
					sceneClear = false;
			for (const ::Ped p : g.extras)
				if (Exists(p))
					sceneClear = false;
			// Cleared: the drivers with nothing to answer for go on their way (the officer had their details).
			if (sceneClear)
				for (const int id : g.suspects)
					if (const suspect::Status st = suspect::StatusOf(id); !st.gone && !st.cuffed && !st.custody && st.ped)
						if (ai::Profile pr; suspect::ProfileOf(st.ped, pr) && pr.guilt < 0.5f)
							suspect::Release(id);
		}
		std::string people = Resolved();
		if (!people.empty() && sceneClear)
		{
			const std::string outcome = g.kind == Kind::Accident ? "交通事故處理完畢（" + people + "）" : people;
			Clean();
			return outcome;
		}
		if (now > g.deadline && !std::any_of(g.suspects.begin(), g.suspects.end(), [](int id) { const auto st = suspect::StatusOf(id); return st.cuffed || st.custody; }))
		{
			Clean();
			return "時間過久，案件轉給其他單位";
		}
		if (d > 900.0f && g.arrived)
		{
			Clean();
			return "員警離開現場";
		}
		return "";
	}

	void Abandon()
	{
		Clean();
	}

	menu::Item Actions()
	{
		if (!g_active || !g.made)
			return {};
		std::vector<::Ped> hurt;
		for (const ::Ped p : g.extras)
			if (Exists(p) && ENTITY::GET_ENTITY_MODEL(p) != MISC::GET_HASH_KEY("mp_m_shopkeep_01"))
				hurt.push_back(p);
		if (hurt.empty() || g.ambulance)
			return {};
		menu::Item i{"叫救護車", std::format("{} 人受傷", hurt.size()), "救護車開過來，把傷者送醫"};
		i.action = [hurt] {
			if (services::Ambulance(g.at, hurt))
			{
				g.ambulance = true;
				records::Note("呼叫救護車");
				ml::Notify("救護車在路上了");
			}
			else
				ml::Notify("附近沒有路可以讓救護車過來");
		};
		return i;
	}

	void Forget()
	{
		g = Scene{};
		g_active = false;
	}
}
