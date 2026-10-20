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
//
// Driving: a driver notices a police car behind them with its lights on (stage 2+). Complying is pulling over (the
// indicator, then the roadside); stalling is driving on as if nothing happened; running is getting away by road:
// targets a few hundred metres off, away from the officer and on in the direction they are going (no U-turns through
// the officer), veterans preferring side streets and driving fast but well, novices driving faster and worse. Out of
// the officer's sight, at night they drive with the lights off; once the officer is lost, those who know better ditch
// the car somewhere quiet and walk away (the car would give them away). A wrecked or stuck car is left on foot. Those
// set on shooting stop and get out to do it.
//
// Changing what the officer is looking for: running on foot with a lead, those who dare take a car (the driver pulled
// out if there is one) and go on by road; hiding, those who know better change their look (a different top, the hat
// off). A suspect who has changed looks like anyone else from a distance: the officer has to come close to recognise
// them, and they keep walking calmly instead of running when the officer passes by.
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
			Cruise,  // driving, unaware
			PullOver, // complying in a vehicle: to the roadside
			Pulled,  // stopped at the roadside
			Evade,   // getting away by road
			Ditch,   // stopping to leave the vehicle
			Jack,    // taking a vehicle
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
			case State::Cruise: return "cruise";
			case State::PullOver: return "pull over";
			case State::Pulled: return "pulled";
			case State::Evade: return "evade";
			case State::Ditch: return "ditch";
			case State::Jack: return "jack";
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
			Vehicle car = 0;             // the vehicle they drive (0 = on foot)
			bool ambush = false;         // stopping to get out and shoot
			uint64_t stuckSince = 0;     // the vehicle not moving while it should
			uint64_t followedSince = 0;  // a police car with its lights on behind them since
			bool disguised = false;      // changed their look: not recognised from a distance
			uint64_t jackCheckAt = 0;    // next look for a vehicle to take
			Vector3 policeKnow{};        // where the police (officer or backup) last saw them
			uint64_t policeSawAt = 0;
			float policeDist = 1e9f;     // how far the police who saw them last were
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
			if (s.followedSince)
			{
				// Lights in the mirror that do not go away: driving on as if nothing happened stops being an option.
				const float followed = std::min(static_cast<float>(Now() - s.followedSince) / 10000.0f, 1.0f);
				comply += followed * 0.7f;
				stall -= followed * 0.6f;
			}
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

		bool Driving(const Suspect& s)
		{
			return s.car && ENTITY::DOES_ENTITY_EXIST(s.car) && PED::IS_PED_IN_VEHICLE(s.ped, s.car, false) &&
			       VEHICLE::GET_PED_IN_VEHICLE_SEAT(s.car, -1, false) == s.ped;
		}

		bool Night()
		{
			const int h = CLOCK::GET_CLOCK_HOURS();
			return h < 6 || h >= 20;
		}

		// Where to drive: a road node a few hundred metres off.
		bool PlanRoute(Suspect& s)
		{
			const Vector3 me = At(s);
			const Vector3 threat = s.senses.lastSeen;
			const Vector3 fwd = ENTITY::GET_ENTITY_FORWARD_VECTOR(s.car);
			const int tries = s.profile.skill == ai::Skill::Veteran ? 16 : s.profile.skill == ai::Skill::Average ? 10 : 5;
			const float noise = s.profile.skill == ai::Skill::Novice ? 0.6f : s.profile.skill == ai::Skill::Average ? 0.25f : 0.1f;
			float best = -1e9f;
			Vector3 bestAt{};
			for (int i = 0; i < tries; ++i)
			{
				// Ahead, give or take, mostly.
				const float spread = (Random() - 0.5f) * 3.6f;
				const float hx = fwd.x * std::cos(spread) - fwd.y * std::sin(spread), hy = fwd.x * std::sin(spread) + fwd.y * std::cos(spread);
				const float range = 150.0f + Random() * 200.0f;
				Vector3 node{};
				float heading = 0;
				int lanes = 0;
				if (!PATH::GET_NTH_CLOSEST_VEHICLE_NODE_WITH_HEADING(me.x + hx * range, me.y + hy * range, me.z, 1, &node, &heading, &lanes, 1, 3.0f, 0.0f))
					continue;
				const float gx = node.x - me.x, gy = node.y - me.y;
				const float gl = std::max(1.0f, std::sqrt(gx * gx + gy * gy));
				const float ahead = (fwd.x * gx + fwd.y * gy) / gl;                              // no U-turn
				const float away = (Distance(node, threat) - Distance(me, threat)) / gl;           // gains distance
				const float pass = std::clamp(SegmentDistance(threat, me, node) / 20.0f, 0.0f, 1.0f); // keeps clear of the officer
				int density = 0, flags = 0;
				PATH::GET_VEHICLE_NODE_PROPERTIES(node.x, node.y, node.z, &density, &flags);
				const float quiet = s.profile.skill == ai::Skill::Veteran ? (lanes <= 1 ? 0.5f : 0.0f) - static_cast<float>(density) * 0.03f : 0.0f;
				float score = ahead * 0.8f + away * 1.0f + pass * 0.8f + quiet;
				score += (Random() - 0.5f) * 2.0f * noise * 2.0f;
				if (score > best)
				{
					best = score;
					bestAt = node;
				}
			}
			if (best <= -1e8f)
				return false;
			s.goal = bestAt;
			return true;
		}

		void Drive(Suspect& s)
		{
			// Around traffic and obstacles, through red lights, against the traffic when needed; novices also take
			// the shortest way whatever is on it.
			const bool novice = s.profile.skill == ai::Skill::Novice;
			const int style = 4 | 8 | 16 | 32 | 512 | 262144 | (novice ? 0 : 1); // veterans and average stop for nothing but avoid cars
			const float speed = novice ? 45.0f : s.profile.skill == ai::Skill::Average ? 38.0f : 34.0f;
			PED::SET_DRIVER_ABILITY(s.ped, novice ? 0.3f : s.profile.skill == ai::Skill::Average ? 0.7f : 1.0f);
			PED::SET_DRIVER_AGGRESSIVENESS(s.ped, novice ? 1.0f : 0.6f);
			TASK::TASK_VEHICLE_DRIVE_TO_COORD_LONGRANGE(s.ped, s.car, s.goal.x, s.goal.y, s.goal.z, speed, style, 20.0f);
		}

		// The roadside a little ahead, on the right.
		void PullOver(Suspect& s)
		{
			const Vector3 at = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(s.car, 3.5f, 30.0f, 0.0f);
			VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(s.car, 0, true); // right
			TASK::TASK_VEHICLE_DRIVE_TO_COORD(s.ped, s.car, at.x, at.y, at.z, 6.0f, 0, ENTITY::GET_ENTITY_MODEL(s.car), 786603, 2.0f, 0.0f);
		}

		// Any of the police (the officer or the backup) has them in sight now.
		bool SeenByPolice(const Suspect& s)
		{
			return Now() - s.policeSawAt < 800;
		}
		// The nearest police (the officer, or backup officers within 120 m).
		float PoliceDistance(const Suspect& s)
		{
			return units::Near(ENTITY::GET_ENTITY_COORDS(s.ped, true), 120.0f) > 0 ? std::min(s.senses.distance, s.policeDist) : s.senses.distance;
		}

		// The officer recognises them: always unless they changed their look, then only close up.
		bool Recognised(const Suspect& s)
		{
			return s.senses.visible && (!s.disguised || s.senses.distance < 10.0f);
		}

		// A different top and no hat (what the model has to offer).
		void ChangeLook(Suspect& s)
		{
			for (const int slot : {3, 11}) // upper body (ambient models), top (freemode)
			{
				const int drawables = PED::GET_NUMBER_OF_PED_DRAWABLE_VARIATIONS(s.ped, slot);
				const int now = PED::GET_PED_DRAWABLE_VARIATION(s.ped, slot);
				if (drawables > 1)
				{
					int d = MISC::GET_RANDOM_INT_IN_RANGE(0, drawables - 1);
					if (d >= now)
						++d;
					PED::SET_PED_COMPONENT_VARIATION(s.ped, slot, d, 0, 0);
				}
				else if (const int textures = PED::GET_NUMBER_OF_PED_TEXTURE_VARIATIONS(s.ped, slot, now); textures > 1)
					PED::SET_PED_COMPONENT_VARIATION(s.ped, slot, now, (PED::GET_PED_TEXTURE_VARIATION(s.ped, slot) + 1) % textures, 0);
			}
			PED::CLEAR_ALL_PED_PROPS(s.ped, 0);
			s.disguised = true;
			ml::Log("suspect {}: changed their look", s.id);
		}

		// A vehicle within reach to take: on wheels, not the police's, closer to them than to the officer.
		Vehicle VehicleToTake(const Suspect& s)
		{
			int64_t buf[1 + 16] = {16};
			const int n = PED::GET_PED_NEARBY_VEHICLES(s.ped, reinterpret_cast<Any*>(buf));
			const Vector3 me = At(s);
			Vehicle best = 0;
			float bestD = 18.0f;
			for (int i = 0; i < n && i < 16; ++i)
			{
				const Vehicle v = static_cast<Vehicle>(buf[1 + i]);
				if (!v || !ENTITY::DOES_ENTITY_EXIST(v) || !VEHICLE::IS_VEHICLE_DRIVEABLE(v, false))
					continue;
				const Hash model = ENTITY::GET_ENTITY_MODEL(v);
				const int cls = VEHICLE::GET_VEHICLE_CLASS(v);
				if (cls == 18 || cls == 14 || cls == 15 || cls == 16 || cls == 21 || cls == 13 || !(VEHICLE::IS_THIS_MODEL_A_CAR(model) || VEHICLE::IS_THIS_MODEL_A_BIKE(model)))
					continue; // police, boats, helicopters, planes, trains, bicycles
				const Vector3 at = ENTITY::GET_ENTITY_COORDS(v, true);
				const float d = Distance(me, at);
				if (d < bestD && Distance(at, s.senses.playerAt) > d + 8.0f && v != s.car)
					best = v, bestD = d;
			}
			return best;
		}

		void OnFootFrom(Suspect& s, State st, const char* why)
		{
			if (s.car && ENTITY::DOES_ENTITY_EXIST(s.car))
				VEHICLE::SET_VEHICLE_LIGHTS(s.car, 0);
			TASK::TASK_LEAVE_VEHICLE(s.ped, s.car, 256); // leaves the door open, in a hurry
			s.planAt = 0;
			s.unseenSince = 0;
			Set(s, st, why);
		}

		void Begin(Suspect& s, State st)
		{
			const ::Ped player = PLAYER::PLAYER_PED_ID();
			if (Driving(s))
			{
				switch (st)
				{
				case State::Comply:
					PullOver(s);
					s.gaveUp = true;
					Set(s, State::PullOver, "decided");
					return;
				case State::Stall:
					if (s.state != State::Cruise && s.state != State::Stall)
						TASK::TASK_VEHICLE_DRIVE_WANDER(s.ped, s.car, 15.0f, 786603);
					Set(s, State::Stall, "drives on");
					return;
				case State::Flee:
				case State::Resist:
					if (!PlanRoute(s))
					{
						Begin(s, State::Comply);
						return;
					}
					Drive(s);
					s.planAt = Now() + 4000;
					s.unseenSince = 0;
					Set(s, State::Evade, Name(st));
					return;
				case State::Shoot:
					// Pulls over as if complying, then gets out to shoot.
					PullOver(s);
					s.ambush = true;
					Set(s, State::PullOver, "ambush");
					return;
				default:
					return;
				}
			}
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
			const bool seen = Recognised(s) && s.state != State::Gone;
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
			if (s.car && ENTITY::DOES_ENTITY_EXIST(s.car))
				ENTITY::SET_VEHICLE_AS_NO_LONGER_NEEDED(&s.car);
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
				if (Driving(s) && s.senses.siren && s.senses.visible)
				{
					if (!s.followedSince)
						s.followedSince = now;
				}
				else if (!s.senses.visible)
					s.followedSince = 0;
				if (s.senses.distance < 1.8f)
				{
					if (!s.heldSince)
						s.heldSince = now;
				}
				else
					s.heldSince = 0;
			}
			Breathe(s, now);
			if (Recognised(s))
			{
				s.policeKnow = At(s);
				s.policeSawAt = now;
				s.policeDist = s.senses.distance;
			}
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
				if (!se.visible && !SeenByPolice(s))
				{
					if (!s.unseenSince)
						s.unseenSince = now;
				}
				else
					s.unseenSince = 0;
				const uint64_t settle = s.profile.skill == ai::Skill::Novice ? 5000 : 1500;
				if (s.unseenSince && now - s.unseenSince > settle && PoliceDistance(s) > 20.0f)
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
				// A lead and a car within reach: take it (not novices, who do not think of it; not the drunk).
				if (now >= s.jackCheckAt && s.profile.skill != ai::Skill::Novice && s.profile.impairment != ai::Impairment::Drunk && se.distance > 15.0f)
				{
					s.jackCheckAt = now + 1500;
					const float dare = s.profile.nerve * (s.profile.skill == ai::Skill::Veteran ? 1.0f : 0.6f);
					if (Random() < dare)
						if (const Vehicle v = VehicleToTake(s))
						{
							s.car = v;
							ENTITY::SET_ENTITY_AS_MISSION_ENTITY(v, true, true);
							TASK::TASK_ENTER_VEHICLE(s.ped, v, 10000, -1, 2.0f, 8 /* takes it from whoever is in it */, nullptr);
							Set(s, State::Jack, "a vehicle within reach");
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
				// In cover a while: those who know better change their look.
				if (!s.disguised && now - s.since > 6000 && Distance(me, s.goal) < 1.5f && s.profile.skill != ai::Skill::Novice &&
				    Random() < (s.profile.skill == ai::Skill::Veteran ? 0.8f : 0.4f) * 0.05f) // checked every frame: within a few seconds
					ChangeLook(s);
				// Found: run again, or give up when the officer is right there (close by, a hedge or fence does not hide them).
				if ((se.visible && se.distance < (s.disguised ? 6.0f : 18.0f)) || se.distance < 5.0f ||
				    (SeenByPolice(s) && s.policeDist < (s.disguised ? 6.0f : 18.0f)))
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
				if (se.visible && se.distance < (s.disguised ? 5.0f : 15.0f))
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
			case State::Cruise:
				// A police car behind with its lights on.
				if (se.siren && se.visible && se.distance < 45.0f)
				{
					s.thinkAt = now + 700 + static_cast<uint64_t>(Random() * 800.0f); // a look in the mirror
					Set(s, State::Stall, "lights behind");
				}
				else if (!Driving(s))
					Set(s, State::Unaware, "out of the vehicle");
				break;
			case State::PullOver:
			{
				const float speed = ENTITY::GET_ENTITY_SPEED(s.car);
				if (now - s.since > 2500 && speed < 1.0f)
				{
					VEHICLE::BRING_VEHICLE_TO_HALT(s.car, 1.0f, 1, false);
					VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(s.car, 0, false);
					if (s.ambush)
					{
						s.ambush = false;
						s.next = State::Shoot;
						TASK::TASK_LEAVE_VEHICLE(s.ped, s.car, 0);
						Set(s, State::Tell, "out to shoot");
					}
					else
						Set(s, State::Pulled, "stopped");
				}
				else if (now - s.since > 15000)
				{
					VEHICLE::BRING_VEHICLE_TO_HALT(s.car, 3.0f, 1, false);
					Set(s, State::Pulled, "stopped where they were");
				}
				break;
			}
			case State::Pulled:
				// Sits there; may still change their mind (drive off) while the officer is not at the window.
				if (now >= s.thinkAt && now - s.since > 6000 && se.distance > 4.0f)
				{
					s.thinkAt = now + 2000;
					const State choice = Decide(s);
					if (choice == State::Flee || choice == State::Resist || choice == State::Shoot)
						Begin(s, choice);
				}
				break;
			case State::Evade:
			{
				if (!Driving(s))
				{
					Set(s, State::Flee, "out of the vehicle");
					break;
				}
				const Vector3 me = At(s);
				if (!se.visible && !SeenByPolice(s))
				{
					if (!s.unseenSince)
						s.unseenSince = now;
				}
				else
					s.unseenSince = 0;
				// Lights off at night once out of sight (not for novices, who do not think of it).
				if (Night() && s.profile.skill != ai::Skill::Novice)
					VEHICLE::SET_VEHICLE_LIGHTS(s.car, s.unseenSince ? 1 : 0);
				// Wrecked or stuck: on foot.
				const float speed = ENTITY::GET_ENTITY_SPEED(s.car);
				if (speed < 1.5f && now - s.since > 5000) // (pulling away takes a moment)
				{
					if (!s.stuckSince)
						s.stuckSince = now;
				}
				else
					s.stuckSince = 0;
				if ((s.stuckSince && now - s.stuckSince > 2500) || VEHICLE::GET_VEHICLE_ENGINE_HEALTH(s.car) < 100.0f ||
				    !VEHICLE::IS_VEHICLE_DRIVEABLE(s.car, false))
				{
					OnFootFrom(s, State::Flee, "vehicle stuck or wrecked");
					break;
				}
				// Lost the officer: ditch the car somewhere and walk (novices drive on).
				const uint64_t lostAfter = s.profile.skill == ai::Skill::Veteran ? 6000 : 12000;
				if (s.unseenSince && now - s.unseenSince > lostAfter && PoliceDistance(s) > 120.0f)
				{
					if (s.profile.skill == ai::Skill::Novice)
					{
						if (se.distance > 400.0f && now - s.unseenSince > 20000)
							Set(s, State::Gone, "got away by road");
					}
					else
					{
						VEHICLE::BRING_VEHICLE_TO_HALT(s.car, 8.0f, 1, false);
						Set(s, State::Ditch, "lost the officer");
						break;
					}
				}
				if (now >= s.planAt || Distance(me, s.goal) < 40.0f)
				{
					if (PlanRoute(s))
						Drive(s);
					s.planAt = now + 4000;
				}
				break;
			}
			case State::Jack:
				if (Driving(s))
				{
					// The officer was looking for another car.
					if (PlanRoute(s))
						Drive(s);
					s.planAt = now + 4000;
					s.unseenSince = 0;
					s.disguised = true; // a different vehicle: not recognised by it from a distance
					Set(s, State::Evade, "took a vehicle");
				}
				else if (now - s.since > 10000 || (se.distance < 6.0f && se.visible) || !ENTITY::DOES_ENTITY_EXIST(s.car))
				{
					s.car = 0;
					s.planAt = 0;
					Set(s, State::Flee, "could not take the vehicle");
				}
				break;
			case State::Ditch:
				if (ENTITY::GET_ENTITY_SPEED(s.car) < 1.0f || now - s.since > 6000)
				{
					// Out, the door shut, and away calmly as if it was not their car.
					TASK::TASK_LEAVE_VEHICLE(s.ped, s.car, 0);
					VEHICLE::SET_VEHICLE_LIGHTS(s.car, 0);
					const Vector3 me = At(s);
					const float dx = me.x - s.senses.lastSeen.x, dy = me.y - s.senses.lastSeen.y;
					const float l = std::max(1.0f, std::sqrt(dx * dx + dy * dy));
					s.goal = {me.x + dx / l * 250.0f, me.y + dy / l * 250.0f, me.z};
					TASK::TASK_FOLLOW_NAV_MESH_TO_COORD(s.ped, s.goal.x, s.goal.y, s.goal.z, 1.0f, -1, 5.0f, 0, 0.0f);
					Set(s, State::Blend, "ditched the car");
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

	int SpawnDriver(const Vector3& around, const ai::Profile& profile)
	{
		Vector3 node{};
		float heading = 0;
		int lanes = 0;
		if (!PATH::GET_NTH_CLOSEST_VEHICLE_NODE_WITH_HEADING(around.x, around.y, around.z, 1, &node, &heading, &lanes, 0, 3.0f, 0.0f))
			return 0;
		const char* const cars[] = {"sultan", "buffalo", "fugitive", "tailgater", "oracle", "felon", "baller", "dominator"};
		const Hash carModel = MISC::GET_HASH_KEY(cars[MISC::GET_RANDOM_INT_IN_RANGE(0, static_cast<int>(std::size(cars)))]);
		const Hash pedModel = MISC::GET_HASH_KEY("a_m_y_downtown_01");
		const auto c = ml::LoadModel(carModel);
		const auto m = ml::LoadModel(pedModel);
		if (!c || !m)
			return 0;
		Suspect s;
		s.car = VEHICLE::CREATE_VEHICLE(carModel, node.x, node.y, node.z + 0.5f, heading, false, false, false);
		if (!s.car)
			return 0;
		ENTITY::SET_ENTITY_AS_MISSION_ENTITY(s.car, true, true);
		VEHICLE::SET_VEHICLE_ON_GROUND_PROPERLY(s.car, 5.0f);
		s.ped = PED::CREATE_PED_INSIDE_VEHICLE(s.car, 4, pedModel, -1, false, false);
		if (!s.ped)
		{
			VEHICLE::DELETE_VEHICLE(&s.car);
			return 0;
		}
		PED::SET_PED_RANDOM_COMPONENT_VARIATION(s.ped, 0);
		ai::TakeControl(s.ped);
		VEHICLE::SET_VEHICLE_ENGINE_ON(s.car, true, true, false);
		TASK::TASK_VEHICLE_DRIVE_WANDER(s.ped, s.car, 15.0f, 786603);
		s.profile = profile;
		s.id = g_nextId++;
		s.since = Now();
		s.state = State::Cruise;
		g_suspects.push_back(s);
		ml::Log("suspect {}: driver spawned at ({:.0f}, {:.0f}), skill {} nerve {:.2f} hostility {:.2f} guilt {:.2f} armed {}", s.id, node.x, node.y,
		    static_cast<int>(profile.skill), profile.nerve, profile.hostility, profile.guilt, profile.armed);
		return s.id;
	}

	bool Pursued(Target& out)
	{
		for (Suspect& s : g_suspects)
		{
			if (s.state == State::Unaware || s.state == State::Cruise || s.state == State::Gone || !ENTITY::DOES_ENTITY_EXIST(s.ped))
				continue;
			out.ped = s.ped;
			out.car = Driving(s) ? s.car : 0;
			switch (s.state)
			{
			case State::Evade:
			case State::Jack:
			case State::Ditch:
				out.mode = Target::Mode::Vehicle;
				break;
			case State::Resist:
			case State::Shoot:
				out.mode = Target::Mode::Armed;
				break;
			case State::Comply:
			case State::PullOver:
			case State::Pulled:
				out.mode = Target::Mode::Stopped;
				break;
			case State::Stall:
				// Standing there arguing (or driving on): covered on foot, followed in a car.
				out.mode = out.car ? Target::Mode::Vehicle : Target::Mode::Stopped;
				break;
			default:
				out.mode = out.car ? Target::Mode::Vehicle : Target::Mode::Foot;
				break;
			}
			out.known = Now() - s.policeSawAt < 4000; // a moment out of view is not lost yet
			out.lastKnown = s.policeKnow;
			out.heading = s.goal;
			return true;
		}
		return false;
	}

	void Spotted(::Ped by)
	{
		for (Suspect& s : g_suspects)
			if (ENTITY::DOES_ENTITY_EXIST(s.ped))
			{
				const float d = ai::Distance(At(s), ENTITY::GET_ENTITY_COORDS(by, true));
				if (s.disguised && d >= 10.0f)
					continue;
				s.policeKnow = At(s);
				s.policeSawAt = Now();
				s.policeDist = d;
			}
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
			if (s.car && ENTITY::DOES_ENTITY_EXIST(s.car))
			{
				ENTITY::SET_ENTITY_AS_MISSION_ENTITY(s.car, true, true);
				VEHICLE::DELETE_VEHICLE(&s.car);
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
