// The officer's conduct and Internal Affairs (police life part 4, user's design 2026-10-22, research/phase0.md §56).
// What is looked into (while the officer is played):
// - every use of a firearm on duty (a use-of-force review): justified when a suspect was fighting or shooting, or the
//   officer was being hurt, at the time; a bystander hit makes it worse;
// - bystanders hurt or killed by the officer (shot, run over), suspects hit while cuffed, a cuffed suspect left behind,
//   police officers attacked, the officer wanted off duty, driving into other people's cars without lights on, absence
//   from a shift (shift.cpp);
// On duty the game's police leave the officer alone (their own shots, a borrowed patrol car: no wanted level), unless
// the officer attacks police: then the wanted level stays.
// - civilian complaints: a gun pointed at someone who has done nothing, a scraped car (by chance).
// Each becomes an Internal Affairs case, decided two or three game days later by how serious it is and the officer's
// recent record: unfounded or justified, a written warning, a suspension (shifts without pay, no going on duty), a
// demotion, or dismissal. Dismissal (the user did not say; decided here): the uniform and equipment go back, no duty,
// and after seven game days the officer may apply again at the station and starts over (Officer I, probation).
// The results count in the evaluation (career.cpp). Kept in the save: ml::save "officer_discipline".
#include "police.hpp"

#include <algorithm>
#include <format>
#include <set>
#include <string>
#include <vector>

namespace police::discipline
{
	namespace
	{
		using ai::Distance;

		struct Case
		{
			int id = 0;
			std::string title, detail;
			int points = 0;      // how serious
			bool review = false; // a use-of-force review
			bool justified = false;
			int64_t opened = 0, due = 0; // game days
			bool closed = false;
			std::string result;
			int sustained = 0; // points that stand (0 = unfounded / justified)
		};
		struct State
		{
			std::vector<Case> cases;
			int next = 1;
		};
		State g;
		bool g_loaded = false;

		// A firing under way: shots within 10 s are one use of force.
		struct Firing
		{
			uint64_t last = 0;
			bool justified = false;
			bool hitBystander = false;
			int shots = 0;
		} g_firing;
		float g_lastHealth = 0;
		uint64_t g_hurtAt = 0;
		std::set<::Ped> g_handled;    // peds already counted (hurt, killed)
		std::set<int> g_abandoned;    // suspects left behind already counted
		bool g_wantedCase = false;    // the wanted level now already counted
		uint64_t g_copAttackAt = 0;   // the officer last hurt a police officer
		int g_multiplier = -1;        // the wanted-level multiplier set (0 on duty, 1 otherwise)
		uint64_t g_nextScan = 0, g_nextScrape = 0;
		::Ped g_aimedAt = 0;
		uint64_t g_aimedSince = 0;
		std::set<::Ped> g_complained;

		ml::Json ToJson(const Case& c)
		{
			return {{"id", c.id}, {"title", c.title}, {"detail", c.detail}, {"points", c.points}, {"review", c.review}, {"justified", c.justified},
			    {"opened", c.opened}, {"due", c.due}, {"closed", c.closed}, {"result", c.result}, {"sustained", c.sustained}};
		}

		void Store()
		{
			ml::Json list = ml::Json::Array();
			for (const Case& c : g.cases)
				list.Push(ToJson(c));
			ml::save::Set("officer_discipline", ml::Json{{"cases", list}, {"next", g.next}});
		}

		void Load()
		{
			if (g_loaded)
				return;
			g_loaded = true;
			g = State{};
			const ml::Json j = ml::save::Get("officer_discipline");
			g.next = j["next"].Int(1);
			for (const ml::Json& c : j["cases"].Items())
				g.cases.push_back({c["id"].Int(), c["title"].Str(), c["detail"].Str(), c["points"].Int(), c["review"].Bool(), c["justified"].Bool(),
				    c["opened"].Int64(), c["due"].Int64(), c["closed"].Bool(), c["result"].Str(), c["sustained"].Int()});
		}

		void Open(const std::string& title, const std::string& detail, int points, bool review = false, bool justified = false)
		{
			Load();
			Case c;
			c.id = g.next++;
			c.title = title;
			c.detail = detail;
			c.points = points;
			c.review = review;
			c.justified = justified;
			c.opened = shift::Day();
			c.due = c.opened + 2 + (ai::Random() < 0.5f ? 1 : 0);
			g.cases.push_back(c);
			if (g.cases.size() > 40)
				g.cases.erase(g.cases.begin());
			Store();
			if (review)
				ml::Notify("內部調查室：開槍案已列入使用武力審查（案號 IA-{}）", c.id);
			else
				ml::Notify("內部調查室：就「{}」立案調查（案號 IA-{}），結果幾天後通知", title, c.id);
			ml::Log("discipline: case {} opened: {} ({} points{})", c.id, title, points, review ? (justified ? ", review, justified" : ", review") : "");
		}

		// The officer's record lately: points that stood in the last 30 days.
		int Record(int64_t today)
		{
			int sum = 0;
			for (const Case& c : g.cases)
				if (c.closed && c.sustained > 0 && today - c.due <= 30)
					sum += c.sustained;
			return sum;
		}

		void Decide(Case& c)
		{
			c.closed = true;
			const int64_t today = shift::Day();
			if (c.review && c.justified)
			{
				c.result = "合理使用武力，不予處分";
				c.sustained = 0;
			}
			else if (c.points <= 1 && ai::Random() < 0.4f)
			{
				c.result = "查無實據，不予處分";
				c.sustained = 0;
			}
			else
			{
				c.sustained = c.points;
				const int total = c.points + Record(today) / 2;
				if (total <= 2)
				{
					c.result = "書面警告";
					career::AddPenalty(5, "書面警告");
				}
				else if (total <= 5)
				{
					c.result = "停職 1 班（不支薪）";
					career::AddPenalty(10, "停職");
					shift::Suspend(1);
				}
				else if (total <= 8)
				{
					c.result = "停職 3 班（不支薪）";
					career::AddPenalty(10, "停職");
					shift::Suspend(3);
				}
				else if (total <= 12)
				{
					c.result = "降級，並停職 2 班";
					career::Demote();
					shift::Suspend(2);
				}
				else
				{
					c.result = "免職";
					Dismiss();
				}
			}
			Store();
			ml::Notify("內部調查室：IA-{}「{}」調查結果：{}", c.id, c.title, c.result);
			ml::Log("discipline: case {} decided: {}", c.id, c.result);
		}

		// The colleague riding along, backup, other police: not bystanders.
		bool Police(::Ped p)
		{
			return p == partner::Ped() || PED::GET_PED_RELATIONSHIP_GROUP_HASH(p) == MISC::GET_HASH_KEY("COP") || PED::GET_PED_TYPE(p) == 6 ||
			       PED::GET_PED_TYPE(p) == 27;
		}

		bool Suspect(::Ped p)
		{
			ai::Profile profile;
			return suspect::ProfileOf(p, profile);
		}

		void Watch()
		{
			const uint64_t now = ml::TickMs();
			const ::Ped pl = PLAYER::PLAYER_PED_ID();
			const bool onDuty = duty::OnDuty();
			suspect::Target t;
			const bool threat = suspect::Pursued(t) && t.mode == suspect::Target::Mode::Armed;

			// Being hurt: a reason to shoot.
			const float health = static_cast<float>(ENTITY::GET_ENTITY_HEALTH(pl));
			if (health < g_lastHealth - 1.0f)
				g_hurtAt = now;
			g_lastHealth = health;

			// Shots on duty (or dealing with a crime off duty): one use of force per firing.
			if ((onDuty || offduty::Responding()) && PED::IS_PED_SHOOTING(pl))
			{
				if (!g_firing.last)
					g_firing = {};
				g_firing.last = now;
				++g_firing.shots;
				if (threat || now - g_hurtAt < 10000)
					g_firing.justified = true;
			}
			if (g_firing.last && now - g_firing.last > 10000)
			{
				const bool justified = g_firing.justified && !g_firing.hitBystander;
				Open("使用武力審查", g_firing.hitBystander ? "開槍時傷及旁人" : justified ? "嫌犯持械反抗或員警遭攻擊" : "開槍時沒有立即的威脅",
				    g_firing.hitBystander ? 6 : 3, true, justified);
				g_firing = {};
			}

			// Wanted by the police: on duty only for attacking police (the game's own wanted level is kept off), off duty
			// as anyone.
			const bool copAttacked = g_copAttackAt && now - g_copAttackAt < 60000;
			if (const int want = onDuty && !copAttacked ? 0 : 1; want != g_multiplier)
			{
				PLAYER::SET_WANTED_LEVEL_MULTIPLIER(static_cast<float>(want));
				g_multiplier = want;
			}
			const int stars = PLAYER::GET_PLAYER_WANTED_LEVEL(PLAYER::PLAYER_ID());
			if (stars > 0 && onDuty && !copAttacked)
			{
				PLAYER::CLEAR_PLAYER_WANTED_LEVEL(PLAYER::PLAYER_ID());
				ml::Log("discipline: a wanted level on duty cleared");
			}
			else if (stars == 0)
				g_wantedCase = false;
			else if (!g_wantedCase && !onDuty)
			{
				g_wantedCase = true;
				Open("下班觸法", std::format("通緝等級 {}", stars), 2 + stars);
			}

			if (now < g_nextScan)
				return;
			g_nextScan = now + 400;
			const Vector3 me = ENTITY::GET_ENTITY_COORDS(pl, true);
			const Vehicle myCar = PED::IS_PED_IN_ANY_VEHICLE(pl, false) ? PED::GET_VEHICLE_PED_IS_IN(pl, false) : 0;

			// Bystanders hurt by the officer (shot, run over); suspects hit while cuffed.
			int64_t peds[1 + 32] = {32};
			const int n = PED::GET_PED_NEARBY_PEDS(pl, reinterpret_cast<Any*>(peds), -1);
			for (int i = 0; i < n && i < 32; ++i)
			{
				const ::Ped p = static_cast<::Ped>(peds[1 + i]);
				if (!p || !ENTITY::DOES_ENTITY_EXIST(p) || g_handled.contains(p))
					continue;
				const bool byMe = ENTITY::HAS_ENTITY_BEEN_DAMAGED_BY_ENTITY(p, pl, true) || (myCar && ENTITY::HAS_ENTITY_BEEN_DAMAGED_BY_ENTITY(p, myCar, true));
				if (!byMe)
					continue;
				ENTITY::CLEAR_ENTITY_LAST_DAMAGE_ENTITY(p);
				if (Police(p))
				{
					if (p != partner::Ped() || ENTITY::IS_ENTITY_DEAD(p, false))
					{
						g_handled.insert(p);
						g_copAttackAt = now;
						Open(ENTITY::IS_ENTITY_DEAD(p, false) ? "員警死亡" : "攻擊員警", "員警遭本人攻擊或誤傷", 10);
					}
					continue;
				}
				if (Suspect(p))
				{
					suspect::Status st{};
					for (const int id : suspect::Ids())
						if (suspect::StatusOf(id).ped == p)
							st = suspect::StatusOf(id);
					if (st.cuffed || st.custody)
					{
						g_handled.insert(p);
						Open("對已上銬者施暴", "嫌犯上銬後遭員警攻擊", 6);
					}
					continue;
				}
				g_handled.insert(p);
				const bool dead = ENTITY::IS_ENTITY_DEAD(p, false);
				if (g_firing.last)
					g_firing.hitBystander = true;
				if (!onDuty && !dead)
					continue; // (off duty a scuffle is the game's: the wanted level tells)
				Open(dead ? "民眾死亡" : "傷及民眾", myCar && ENTITY::HAS_ENTITY_BEEN_DAMAGED_BY_ENTITY(p, myCar, true) ? "遭員警車輛撞擊" : "遭員警攻擊或流彈波及",
				    dead ? 10 : 6);
			}

			// A cuffed suspect left behind.
			for (const int id : suspect::Ids())
			{
				const suspect::Status st = suspect::StatusOf(id);
				if (st.cuffed && !st.custody && st.ped && !g_abandoned.contains(id) && Distance(ENTITY::GET_ENTITY_COORDS(st.ped, true), me) > 300.0f)
				{
					g_abandoned.insert(id);
					Open("棄置已上銬嫌犯", "上銬的嫌犯被留在現場", 3);
				}
			}

			// Driving into other people's cars without lights on (by chance, a complaint).
			if (onDuty && myCar && now >= g_nextScrape && lights::Stage(myCar) < 2)
			{
				int64_t cars[1 + 16] = {16};
				const int m = PED::GET_PED_NEARBY_VEHICLES(pl, reinterpret_cast<Any*>(cars));
				for (int i = 0; i < m && i < 16; ++i)
				{
					const Vehicle v = static_cast<Vehicle>(cars[1 + i]);
					if (!v || v == myCar || !ENTITY::HAS_ENTITY_BEEN_DAMAGED_BY_ENTITY(v, myCar, true))
						continue;
					ENTITY::CLEAR_ENTITY_LAST_DAMAGE_ENTITY(v);
					if (VEHICLE::GET_VEHICLE_CLASS(v) == 18 || suspect::OwnerOf(v))
						continue;
					g_nextScrape = now + 120000;
					if (ai::Random() < 0.35f)
						Open("民眾投訴：危險駕駛", "巡邏車沒開警燈擦撞民眾車輛", 1);
					break;
				}
			}

			// A gun pointed at someone who has done nothing (by chance, a complaint).
			Entity aimed = 0;
			if (onDuty && PLAYER::GET_ENTITY_PLAYER_IS_FREE_AIMING_AT(PLAYER::PLAYER_ID(), &aimed) && aimed && ENTITY::IS_ENTITY_A_PED(aimed) &&
			    !Police(aimed) && !Suspect(aimed) && !ENTITY::IS_ENTITY_DEAD(aimed, false))
			{
				if (aimed != g_aimedAt)
					g_aimedAt = aimed, g_aimedSince = now;
				else if (now - g_aimedSince > 3000 && !g_complained.contains(aimed))
				{
					g_complained.insert(aimed);
					if (ai::Random() < 0.35f)
						Open("民眾投訴：持槍指著民眾", "員警無故以槍指著民眾", 1);
				}
			}
			else
				g_aimedAt = 0;
		}
	}

	void Absent()
	{
		Open("缺勤", "排班時間沒有到勤", 1);
	}

	void Dismiss()
	{
		career::Fire();
		shift::Fire(7);
		if (officer::IsPlayer() && officer::InUniform())
		{
			officer::Dress(false);
			ml::Notify("你已被免職：交回制服與裝備。七天後可以到警局重新申請。");
		}
	}

	ml::Json Json()
	{
		Load();
		ml::Json list = ml::Json::Array();
		for (auto it = g.cases.rbegin(); it != g.cases.rend(); ++it)
			list.Push(ToJson(*it));
		return list;
	}

	void Loading()
	{
		g_loaded = false;
		g_firing = {};
		g_handled.clear();
		g_abandoned.clear();
		g_complained.clear();
		g_wantedCase = false;
	}

	void Tick()
	{
		if (!persist::Settled())
			return;
		Load();
		// Decisions come when due (whoever is played).
		static uint64_t nextDecision = 0;
		if (ml::TickMs() >= nextDecision)
		{
			nextDecision = ml::TickMs() + 2000;
			const int64_t today = shift::Day();
			for (Case& c : g.cases)
				if (!c.closed && today >= c.due)
				{
					Decide(c);
					break; // one at a time
				}
		}
		if (!officer::IsPlayer())
		{
			g_firing = {};
			g_lastHealth = 0;
			if (g_multiplier != 1 && g_multiplier != -1)
				PLAYER::SET_WANTED_LEVEL_MULTIPLIER(1.0f); // the protagonists: the game's own
			g_multiplier = -1;
			return;
		}
		Watch();
	}
}
