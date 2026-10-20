// Suspects (user's design): who they are (nerve, hostility, what they have to lose, armed, sober or not, how
// experienced) and what the officer does (gun drawn and aimed, how close, backup, siren) decide, once a second, whether
// they comply, stall, run, fight or shoot. A decision to run or fight shows first (they glance at their way out, step
// back) so the officer has a moment to react.
//
// Running on foot: they pick where to go, not just away. Points around them on the navmesh are scored for leading
// away from the officer, not passing them, and getting out of their sight (a wall or a building between); experienced
// suspects look at more points and choose better, novices panic and choose with more noise. Once out of sight they
// hide (crouched, where the officer's last view of them cannot reach) and wait; when the officer has not found them,
// they walk away calmly, as anyone would, and are gone.
#include "police.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace police::suspect
{
	namespace
	{
		using ai::Distance;
		using ai::Random;

		enum class State
		{
			Unaware, // going about their business
			Tell,    // about to run or fight: glances at the way out
			Comply,  // hands up
			Stall,   // stays, does not do as told
			Flee,    // running on foot
			Hide,    // out of sight, crouched
			Blend,   // walking away calmly
			Resist,  // fists
			Shoot,
			Gone,    // escaped or dead: to be removed
		};
		const char* Name(State s)
		{
			switch (s)
			{
			case State::Unaware: return "unaware";
			case State::Tell: return "tell";
			case State::Comply: return "comply";
			case State::Stall: return "stall";
			case State::Flee: return "flee";
			case State::Hide: return "hide";
			case State::Blend: return "blend";
			case State::Resist: return "resist";
			case State::Shoot: return "shoot";
			case State::Gone: return "gone";
			}
			return "?";
		}

		struct Suspect
		{
			::Ped ped = 0;
			ai::Profile profile;
			ai::Senses senses;
			State state = State::Unaware;
			State next = State::Unaware; // what the tell leads to
			uint64_t since = 0;          // state began
			uint64_t thinkAt = 0;        // next decision
			uint64_t senseAt = 0;
			uint64_t planAt = 0;         // next route (re)plan
			Vector3 goal{};              // where they are going
			uint64_t unseenSince = 0;    // the officer out of sight since
			float stamina = 1.0f;        // 1 fresh, 0 spent: sprinting uses it up, walking and hiding bring it back
			uint64_t tickAt = 0;         // last frame (for the stamina)
			uint64_t heldSince = 0;      // the officer within arm's reach since
			bool gaveUp = false;         // has put their hands up once: rarely bolts again
			Blip blip = 0;
			Blip area = 0;               // last known area while not seen
			int id = 0;
		};
		std::vector<Suspect> g_suspects;
		int g_nextId = 1;

		uint64_t Now()
		{
			return ml::TickMs();
		}

		void Set(Suspect& s, State st, const char* why)
		{
			if (s.state == st)
				return;
			ml::Log("suspect {}: {} -> {} ({})", s.id, Name(s.state), Name(st), why);
			s.state = st;
			s.since = Now();
		}

		Vector3 At(const Suspect& s)
		{
			return ENTITY::GET_ENTITY_COORDS(s.ped, true);
		}

		// Distance from p to the segment a-b (on the ground plane).
		float SegmentDistance(const Vector3& p, const Vector3& a, const Vector3& b)
		{
			const float vx = b.x - a.x, vy = b.y - a.y;
			const float l2 = vx * vx + vy * vy;
			float t = l2 > 0.0f ? ((p.x - a.x) * vx + (p.y - a.y) * vy) / l2 : 0.0f;
			t = std::clamp(t, 0.0f, 1.0f);
			const float x = a.x + t * vx - p.x, y = a.y + t * vy - p.y;
			return std::sqrt(x * x + y * y);
		}

		// Where to run: the best of points around them on the navmesh.
		bool PlanEscape(Suspect& s)
		{
			const Vector3 me = At(s);
			const Vector3 threat = s.senses.lastSeen;
			const Vector3 eye = {threat.x, threat.y, threat.z + 0.7f};
			const int dirs = s.profile.skill == ai::Skill::Veteran ? 20 : s.profile.skill == ai::Skill::Average ? 12 : 6;
			const float noise = s.profile.skill == ai::Skill::Novice ? 0.6f : s.profile.skill == ai::Skill::Average ? 0.25f : 0.1f;
			const float base = Random() * 6.283f;
			float best = -1e9f;
			Vector3 bestAt{};
			for (int i = 0; i < dirs; ++i)
				for (const float range : {22.0f, 40.0f, 65.0f})
				{
					const float a = base + 6.283f * static_cast<float>(i) / static_cast<float>(dirs);
					Vector3 p{};
					if (!ai::OnFoot(me.x + std::cos(a) * range, me.y + std::sin(a) * range, me.z, p))
						continue;
					if (std::abs(p.z - me.z) > 12.0f)
						continue;
					const float away = (Distance(p, threat) - Distance(me, threat)) / range;          // gains distance
					const float pass = std::clamp(SegmentDistance(threat, me, p) / 10.0f, 0.0f, 1.0f); // keeps clear of the officer
					const bool hidden = !ai::Clear(eye, {p.x, p.y, p.z + 0.8f}, 0);                    // out of their sight there
					float score = away * 1.0f + pass * 1.2f + (hidden ? 1.0f : 0.0f) - range / 200.0f;
					// Running already: on in about the same direction (turning back costs the lead).
					const Vector3 v = ENTITY::GET_ENTITY_VELOCITY(s.ped);
					const float vl = std::sqrt(v.x * v.x + v.y * v.y);
					if (vl > 1.5f)
					{
						const float gx = p.x - me.x, gy = p.y - me.y;
						const float gl = std::max(0.1f, std::sqrt(gx * gx + gy * gy));
						score += 0.6f * (v.x * gx + v.y * gy) / (vl * gl);
					}
					if (pass < 0.3f)
						score -= 2.0f; // straight past the officer
					score += (Random() - 0.5f) * 2.0f * noise * 2.0f;
					if (score > best)
					{
						best = score;
						bestAt = p;
					}
				}
			if (best <= -1e8f)
				return false;
			s.goal = bestAt;
			return true;
		}

		// A nearby place the officer's last view of them does not reach.
		bool PlanHide(Suspect& s)
		{
			const Vector3 me = At(s);
			const Vector3 eye = {s.senses.lastSeen.x, s.senses.lastSeen.y, s.senses.lastSeen.z + 0.7f};
			float best = -1e9f;
			Vector3 bestAt{};
			for (int i = 0; i < 16; ++i)
			{
				const float a = 6.283f * static_cast<float>(i) / 16.0f;
				const float r = 6.0f + Random() * 10.0f;
				Vector3 p{};
				if (!ai::OnFoot(me.x + std::cos(a) * r, me.y + std::sin(a) * r, me.z, p) || std::abs(p.z - me.z) > 4.0f)
					continue;
				if (ai::Clear(eye, {p.x, p.y, p.z + 0.5f}, 0))
					continue; // seen from there
				const float score = Distance(p, s.senses.lastSeen) / 30.0f - r / 20.0f + Random() * 0.3f;
				if (score > best)
				{
					best = score;
					bestAt = p;
				}
			}
			if (best <= -1e8f)
				return false;
			s.goal = bestAt;
			return true;
		}

		void Run(Suspect& s, float speed)
		{
			TASK::TASK_FOLLOW_NAV_MESH_TO_COORD(s.ped, s.goal.x, s.goal.y, s.goal.z, speed, -1, 1.5f, 0, 0.0f);
		}

		float RunSpeed(const Suspect& s)
		{
			if (s.profile.impairment == ai::Impairment::Drunk)
				return 2.2f; // a jog at best
			return s.stamina > 0.15f ? 3.0f : 2.0f; // spent: a run, no more sprinting
		}

		// Sprinting uses up stamina (veterans and the sober last longer), anything slower brings it back.
		void Breathe(Suspect& s, uint64_t now)
		{
			const float dt = s.tickAt ? static_cast<float>(now - s.tickAt) / 1000.0f : 0.0f;
			s.tickAt = now;
			const float speed = ENTITY::GET_ENTITY_SPEED(s.ped);
			const float lasts = (s.profile.skill == ai::Skill::Veteran ? 40.0f : s.profile.skill == ai::Skill::Average ? 30.0f : 22.0f) *
			                    (s.profile.impairment == ai::Impairment::Sober ? 1.0f : 0.6f); // seconds of sprint
			if (speed > 5.5f)
				s.stamina -= dt / lasts;
			else if (speed > 2.5f)
				s.stamina += dt / 60.0f;
			else
				s.stamina += dt / 20.0f;
			s.stamina = std::clamp(s.stamina, 0.0f, 1.0f);
		}

		// The choice from who they are and what the officer does (the higher score wins).
		State Decide(Suspect& s)
		{
			const ai::Profile& p = s.profile;
			const float pressure = ai::Pressure(s.senses);
			const float armed = p.armed >= 2 ? 1.0f : p.armed == 1 ? 0.5f : 0.0f;
			const bool outgunned = s.senses.aimedAt && armed < 1.0f;
			float comply = (1.0f - p.nerve) * 0.6f + pressure * 0.8f - p.guilt * 0.5f - p.hostility * 0.3f;
			float stall = 0.35f + p.hostility * 0.2f - pressure * 0.2f;
			float flee = p.nerve * 0.4f + p.guilt * 0.8f - pressure * 0.35f;
			float fight = p.hostility * 0.8f + p.nerve * 0.3f + armed * 0.15f - pressure * 0.6f - (outgunned ? 0.6f : 0.0f) - 0.3f;
			float shoot = p.armed >= 2 ? p.hostility * 0.9f + p.guilt * 0.3f - pressure * (p.skill == ai::Skill::Veteran ? 0.7f : 0.4f) - 0.2f : -9.0f;
			if (s.senses.distance < 3.0f)
				flee -= 0.3f; // the officer is right there
			if (s.senses.hurt)
				flee -= 0.2f, fight -= 0.3f;
			flee -= (1.0f - s.stamina) * 0.6f; // out of breath
			if (s.heldSince && Now() - s.heldSince > 1200)
			{
				// The officer has caught up and is on them: a breakaway takes nerve and breath.
				comply += 0.5f;
				flee -= 0.5f;
				fight += p.hostility * 0.3f;
			}
			// Sobriety and inexperience make for less sensible choices.
			const float noise = (p.impairment != ai::Impairment::Sober ? 0.35f : 0.1f) + (p.skill == ai::Skill::Novice ? 0.2f : 0.0f);
			comply += (Random() - 0.5f) * noise;
			stall += (Random() - 0.5f) * noise;
			flee += (Random() - 0.5f) * noise;
			fight += (Random() - 0.5f) * noise;
			shoot += (Random() - 0.5f) * noise;
			// Sticking with what they do unless something else is clearly better.
			const float keep = 0.25f;
			if (s.gaveUp)
				flee -= 0.3f;
			switch (s.state)
			{
			case State::Comply: comply += keep; break;
			case State::Stall: stall += keep; break;
			case State::Flee: case State::Hide: case State::Blend: flee += keep; break;
			case State::Resist: fight += keep; break;
			case State::Shoot: shoot += keep; break;
			default: break;
			}
			State choice = State::Comply;
			float best = comply;
			for (const auto& [st, v] : {std::pair{State::Stall, stall}, std::pair{State::Flee, flee}, std::pair{State::Resist, fight}, std::pair{State::Shoot, shoot}})
				if (v > best)
					best = v, choice = st;
			ml::Log("suspect {}: pressure {:.2f} d {:.0f} seen {} aimed {} | comply {:.2f} stall {:.2f} flee {:.2f} fight {:.2f} shoot {:.2f} -> {}", s.id, pressure,
			    s.senses.distance, s.senses.seesPlayer, s.senses.aimedAt, comply, stall, flee, fight, shoot, Name(choice));
			return choice;
		}

		void Begin(Suspect& s, State st)
		{
			const ::Ped player = PLAYER::PLAYER_PED_ID();
			switch (st)
			{
			case State::Comply:
				TASK::CLEAR_PED_TASKS(s.ped);
				TASK::TASK_HANDS_UP(s.ped, -1, player, 1500, 0);
				s.gaveUp = true;
				Set(s, st, "decided");
				break;
			case State::Stall:
				TASK::CLEAR_PED_TASKS(s.ped);
				TASK::TASK_TURN_PED_TO_FACE_ENTITY(s.ped, player, -1);
				Set(s, st, "decided");
				break;
			case State::Flee:
			case State::Resist:
			case State::Shoot:
				// First the tell: a look at the way out (or at the officer), a step back.
				if (st == State::Flee && !PlanEscape(s))
				{
					Begin(s, State::Comply);
					return;
				}
				TASK::CLEAR_PED_TASKS(s.ped);
				if (st == State::Flee)
					TASK::TASK_LOOK_AT_COORD(s.ped, s.goal.x, s.goal.y, s.goal.z + 1.0f, 1500, 0, 2);
				TASK::TASK_TURN_PED_TO_FACE_ENTITY(s.ped, player, 1200);
				s.next = st;
				Set(s, State::Tell, Name(st));
				break;
			default:
				break;
			}
		}

		void Act(Suspect& s)
		{
			const ::Ped player = PLAYER::PLAYER_PED_ID();
			const uint64_t now = Now();
			switch (s.next)
			{
			case State::Flee:
				PlanEscape(s);
				Run(s, RunSpeed(s));
				s.planAt = now + 2500;
				s.unseenSince = 0;
				Set(s, State::Flee, "after the tell");
				break;
			case State::Resist:
				WEAPON::SET_CURRENT_PED_WEAPON(s.ped, 0xA2719263, true);
				TASK::TASK_COMBAT_PED(s.ped, player, 0, 16);
				Set(s, State::Resist, "after the tell");
				break;
			case State::Shoot:
				WEAPON::GIVE_WEAPON_TO_PED(s.ped, MISC::GET_HASH_KEY(s.profile.armed >= 3 ? "WEAPON_MICROSMG" : "WEAPON_PISTOL"), 60, false, true);
				TASK::TASK_COMBAT_PED(s.ped, player, 0, 16);
				Set(s, State::Shoot, "after the tell");
				break;
			default:
				break;
			}
		}

		uint64_t HideTime(const Suspect& s)
		{
			return s.profile.skill == ai::Skill::Veteran ? 60000 : s.profile.skill == ai::Skill::Average ? 40000 : 20000;
		}

		void UpdateBlips(Suspect& s)
		{
			const bool seen = s.senses.visible && s.state != State::Gone;
			if (seen && !s.blip)
			{
				s.blip = HUD::ADD_BLIP_FOR_ENTITY(s.ped);
				HUD::SET_BLIP_COLOUR(s.blip, 1);
				HUD::BEGIN_TEXT_COMMAND_SET_BLIP_NAME("STRING");
				HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME("嫌犯");
				HUD::END_TEXT_COMMAND_SET_BLIP_NAME(s.blip);
			}
			else if (!seen && s.blip)
				HUD::REMOVE_BLIP(&s.blip);
			// Where they were last seen, while they are not.
			const bool lost = !seen && s.state != State::Gone && s.state != State::Unaware && s.senses.lastSeenAt;
			if (lost && !s.area)
			{
				const Vector3 at = At(s);
				s.area = HUD::ADD_BLIP_FOR_RADIUS(at.x, at.y, at.z, 60.0f);
				HUD::SET_BLIP_COLOUR(s.area, 1);
				HUD::SET_BLIP_ALPHA(s.area, 90);
			}
			else if (!lost && s.area)
				HUD::REMOVE_BLIP(&s.area);
		}

		void Remove(Suspect& s)
		{
			if (s.blip)
				HUD::REMOVE_BLIP(&s.blip);
			if (s.area)
				HUD::REMOVE_BLIP(&s.area);
			if (ENTITY::DOES_ENTITY_EXIST(s.ped))
			{
				ai::Release(s.ped);
				ENTITY::SET_PED_AS_NO_LONGER_NEEDED(&s.ped);
			}
		}

		void Tick(Suspect& s)
		{
			const uint64_t now = Now();
			if (!ENTITY::DOES_ENTITY_EXIST(s.ped) || ENTITY::IS_ENTITY_DEAD(s.ped, false))
			{
				Set(s, State::Gone, "dead or gone");
				return;
			}
			if (now >= s.senseAt)
			{
				ai::Sense(s.ped, s.senses);
				s.senseAt = now + 200;
				if (s.senses.distance < 1.8f)
				{
					if (!s.heldSince)
						s.heldSince = now;
				}
				else
					s.heldSince = 0;
			}
			Breathe(s, now);
			UpdateBlips(s);
			const ai::Senses& se = s.senses;
			switch (s.state)
			{
			case State::Unaware:
				// The officer comes up to them, points a gun or sounds the siren close by.
				if ((se.seesPlayer && se.distance < 15.0f) || se.aimedAt || (se.siren && se.distance < 30.0f))
				{
					s.thinkAt = now + 400 + static_cast<uint64_t>(Random() * 600.0f); // a moment to take it in
					Set(s, State::Stall, "noticed the officer");
					TASK::TASK_TURN_PED_TO_FACE_ENTITY(s.ped, PLAYER::PLAYER_PED_ID(), -1);
				}
				break;
			case State::Comply:
			case State::Stall:
				// A decision holds for a few seconds (no flip-flopping).
				if (now >= s.thinkAt && now - s.since > (s.state == State::Comply ? 5000u : 2000u))
				{
					s.thinkAt = now + 1000;
					const State choice = Decide(s);
					if (choice != s.state)
						Begin(s, choice);
				}
				break;
			case State::Tell:
			{
				const uint64_t tell = s.profile.skill == ai::Skill::Veteran ? 800 : s.profile.skill == ai::Skill::Average ? 1200 : 1600;
				if (now - s.since >= tell)
					Act(s);
				break;
			}
			case State::Flee:
			{
				const Vector3 me = At(s);
				// Out of the officer's sight for a while and far enough: hide (novices keep running a while longer).
				if (!se.visible)
				{
					if (!s.unseenSince)
						s.unseenSince = now;
				}
				else
					s.unseenSince = 0;
				const uint64_t settle = s.profile.skill == ai::Skill::Novice ? 5000 : 1500;
				if (s.unseenSince && now - s.unseenSince > settle && se.distance > 20.0f)
				{
					if (PlanHide(s))
					{
						TASK::TASK_FOLLOW_NAV_MESH_TO_COORD(s.ped, s.goal.x, s.goal.y, s.goal.z, 2.0f, -1, 0.8f, 0, 0.0f);
						Set(s, State::Hide, "out of sight");
						break;
					}
				}
				// Cornered or caught up with, or out of breath: maybe give up, depending on who they are.
				if ((se.distance < 4.0f || s.stamina < 0.2f) && now >= s.thinkAt)
				{
					s.thinkAt = now + 1000;
					const State choice = Decide(s);
					if (choice != State::Flee)
					{
						Begin(s, choice);
						break;
					}
				}
				if (now >= s.planAt || Distance(me, s.goal) < 3.0f)
				{
					if (PlanEscape(s))
						Run(s, RunSpeed(s));
					s.planAt = now + (s.profile.skill == ai::Skill::Novice ? 4000 : 3500);
				}
				break;
			}
			case State::Hide:
			{
				const Vector3 me = At(s);
				if (Distance(me, s.goal) < 1.2f && !PED::GET_PED_STEALTH_MOVEMENT(s.ped))
				{
					TASK::CLEAR_PED_TASKS(s.ped);
					PED::SET_PED_STEALTH_MOVEMENT(s.ped, true, "DEFAULT_ACTION");
					TASK::TASK_STAND_STILL(s.ped, -1);
				}
				// Found: run again, or give up when the officer is right there (close by, a hedge or fence does not hide them).
				if ((se.visible && se.distance < 18.0f) || se.distance < 5.0f)
				{
					PED::SET_PED_STEALTH_MOVEMENT(s.ped, false, "DEFAULT_ACTION");
					const State choice = se.distance < 6.0f ? Decide(s) : State::Flee;
					Begin(s, choice == State::Flee ? State::Flee : choice);
					break;
				}
				if (now - s.since > HideTime(s) && ml::TickMs() - se.lastSeenAt > HideTime(s) / 2)
				{
					// Walk off calmly, away from where the officer was.
					PED::SET_PED_STEALTH_MOVEMENT(s.ped, false, "DEFAULT_ACTION");
					const float dx = me.x - se.lastSeen.x, dy = me.y - se.lastSeen.y;
					const float l = std::max(1.0f, std::sqrt(dx * dx + dy * dy));
					s.goal = {me.x + dx / l * 250.0f, me.y + dy / l * 250.0f, me.z};
					TASK::TASK_WANDER_IN_AREA(s.ped, s.goal.x, s.goal.y, s.goal.z, 60.0f, 0.0f, 0.0f);
					TASK::TASK_FOLLOW_NAV_MESH_TO_COORD(s.ped, s.goal.x, s.goal.y, s.goal.z, 1.0f, -1, 5.0f, 0, 0.0f);
					Set(s, State::Blend, "not found");
				}
				break;
			}
			case State::Blend:
				if (se.visible && se.distance < 15.0f)
				{
					Begin(s, Decide(s));
					break;
				}
				if (se.distance > 200.0f && !se.visible)
					Set(s, State::Gone, "escaped");
				break;
			case State::Resist:
			case State::Shoot:
				// Giving up when hurt or outmatched.
				if (now >= s.thinkAt)
				{
					s.thinkAt = now + 1500;
					if (se.hurt || ai::Pressure(se) > 0.9f)
					{
						const State choice = Decide(s);
						if (choice == State::Comply || choice == State::Flee)
						{
							WEAPON::REMOVE_ALL_PED_WEAPONS(s.ped, true);
							Begin(s, choice);
						}
					}
				}
				break;
			case State::Gone:
				break;
			}
		}
	}

	ai::Profile RandomProfile(ai::Skill skill)
	{
		ai::Profile p;
		p.skill = skill;
		p.nerve = 0.2f + Random() * 0.7f;
		p.hostility = Random() * 0.7f;
		p.guilt = Random();
		const float r = Random();
		p.armed = r < 0.6f ? 0 : r < 0.8f ? 1 : r < 0.95f ? 2 : 3;
		const float q = Random();
		p.impairment = q < 0.7f ? ai::Impairment::Sober : q < 0.85f ? ai::Impairment::Drunk : ai::Impairment::Substance;
		p.substanceGrade = p.impairment == ai::Impairment::Substance ? 1 + MISC::GET_RANDOM_INT_IN_RANGE(0, 4) : 0;
		return p;
	}

	int Spawn(const Vector3& at, float heading, const ai::Profile& profile)
	{
		const char* const models[] = {"a_m_y_downtown_01", "a_m_y_stbla_01", "a_m_m_skater_01", "a_m_y_genstreet_01", "a_f_y_genhot_01", "a_m_y_hipster_01"};
		const Hash model = MISC::GET_HASH_KEY(models[MISC::GET_RANDOM_INT_IN_RANGE(0, static_cast<int>(std::size(models)))]);
		const auto loaded = ml::LoadModel(model);
		if (!loaded)
			return 0;
		Suspect s;
		s.ped = PED::CREATE_PED(4, model, at.x, at.y, at.z, heading, false, false);
		if (!s.ped)
			return 0;
		PED::SET_PED_RANDOM_COMPONENT_VARIATION(s.ped, 0);
		ai::TakeControl(s.ped);
		s.profile = profile;
		s.id = g_nextId++;
		s.since = Now();
		TASK::TASK_START_SCENARIO_IN_PLACE(s.ped, "WORLD_HUMAN_STAND_MOBILE", 0, true);
		g_suspects.push_back(s);
		ml::Log("suspect {}: spawned, skill {} nerve {:.2f} hostility {:.2f} guilt {:.2f} armed {} impairment {} (grade {})", s.id,
		    static_cast<int>(profile.skill), profile.nerve, profile.hostility, profile.guilt, profile.armed, static_cast<int>(profile.impairment),
		    profile.substanceGrade);
		return s.id;
	}

	size_t Count()
	{
		return g_suspects.size();
	}

	void Clear()
	{
		for (Suspect& s : g_suspects)
		{
			if (s.blip)
				HUD::REMOVE_BLIP(&s.blip);
			if (s.area)
				HUD::REMOVE_BLIP(&s.area);
			if (ENTITY::DOES_ENTITY_EXIST(s.ped))
			{
				ENTITY::SET_ENTITY_AS_MISSION_ENTITY(s.ped, true, true);
				PED::DELETE_PED(&s.ped);
			}
		}
		g_suspects.clear();
	}

	void Forget()
	{
		g_suspects.clear();
	}

	void Tick()
	{
		for (size_t i = 0; i < g_suspects.size();)
		{
			Tick(g_suspects[i]);
			if (g_suspects[i].state == State::Gone)
			{
				Remove(g_suspects[i]);
				g_suspects.erase(g_suspects.begin() + static_cast<std::ptrdiff_t>(i));
			}
			else
				++i;
		}
	}
}
