// The shared AI layer (user's design): the mod decides what the peds it takes over do, from what they can perceive and
// who they are; the game only carries the decisions out (walking, driving, shooting). Taking a ped over blocks the
// game's own reactions to events (fleeing from gunfire, attacking whoever bumps into it), which is where the choices
// that do not fit the situation come from.
#include "police.hpp"

#include <algorithm>
#include <cmath>

namespace police::ai
{
	namespace
	{
		constexpr int kHeadBone = 31086; // SKEL_Head
		const Hash kUnarmed = 0xA2719263; // WEAPON_UNARMED
	}

	float Distance(const Vector3& a, const Vector3& b)
	{
		return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
	}

	float Random()
	{
		return MISC::GET_RANDOM_FLOAT_IN_RANGE(0.0f, 1.0f);
	}

	Vector3 Eye(::Ped ped)
	{
		return PED::GET_PED_BONE_COORDS(ped, kHeadBone, 0.0f, 0.0f, 0.05f);
	}

	bool Clear(const Vector3& from, const Vector3& to, Entity ignore)
	{
		// The map and objects (not peds or vehicles, which do not hide anyone for long).
		const int h = SHAPETEST::START_EXPENSIVE_SYNCHRONOUS_SHAPE_TEST_LOS_PROBE(from.x, from.y, from.z, to.x, to.y, to.z, 1 | 16, ignore, 7);
		BOOL hit = false;
		Vector3 end{}, normal{};
		Entity entity = 0;
		SHAPETEST::GET_SHAPE_TEST_RESULT(h, &hit, &end, &normal, &entity);
		return !hit;
	}

	bool OnFoot(float x, float y, float z, Vector3& out)
	{
		return PATH::GET_SAFE_COORD_FOR_PED(x, y, z, true, &out, 0) && Distance(out, {x, y, z}) < 25.0f;
	}

	void TakeControl(::Ped ped)
	{
		ENTITY::SET_ENTITY_AS_MISSION_ENTITY(ped, true, true);
		PED::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(ped, true);
		PED::SET_PED_KEEP_TASK(ped, true);
		PED::SET_PED_FLEE_ATTRIBUTES(ped, 0, false);
		PED::SET_PED_COMBAT_ATTRIBUTES(ped, 17, false); // does not flee on its own in combat
		PED::SET_PED_COMBAT_ATTRIBUTES(ped, 46, true);  // fights on when the decision is to fight
		TASK::SET_PED_PATH_CAN_USE_CLIMBOVERS(ped, true);
		TASK::SET_PED_PATH_CAN_USE_LADDERS(ped, true);
		TASK::SET_PED_PATH_CAN_DROP_FROM_HEIGHT(ped, true);
		TASK::SET_PED_PATH_PREFER_TO_AVOID_WATER(ped, true);
	}

	void Release(::Ped ped)
	{
		if (!ENTITY::DOES_ENTITY_EXIST(ped))
			return;
		PED::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(ped, false);
		PED::SET_PED_KEEP_TASK(ped, false);
	}

	void Sense(::Ped ped, Senses& s)
	{
		const ::Ped player = PLAYER::PLAYER_PED_ID();
		const Player id = PLAYER::PLAYER_ID();
		const Vector3 me = ENTITY::GET_ENTITY_COORDS(ped, true);
		const Vector3 them = ENTITY::GET_ENTITY_COORDS(player, true);
		s.distance = Distance(me, them);
		s.playerAt = them;
		// Seen: in front (or close enough to be heard) with nothing in between.
		const Vector3 fwd = ENTITY::GET_ENTITY_FORWARD_VECTOR(ped);
		const float dx = them.x - me.x, dy = them.y - me.y;
		const float len = std::max(0.01f, std::sqrt(dx * dx + dy * dy));
		const bool inFront = (fwd.x * dx + fwd.y * dy) / len > -0.2f; // a wide field of view (~200°)
		s.visible = s.distance < 120.0f && Clear(Eye(ped), Eye(player), ped);
		s.seesPlayer = s.visible && (inFront || s.distance < 8.0f);
		// Running away they hear and glance back: where the officer is stays known while in view.
		if (s.visible)
		{
			s.lastSeen = them;
			s.lastSeenAt = ml::TickMs();
		}
		Hash weapon = kUnarmed;
		weapon = WEAPON::GET_SELECTED_PED_WEAPON(player);
		s.gunDrawn = weapon != kUnarmed && WEAPON::IS_PED_ARMED(player, 4); // a firearm
		s.aimedAt = s.seesPlayer && (PLAYER::IS_PLAYER_FREE_AIMING_AT_ENTITY(id, ped) || PLAYER::IS_PLAYER_TARGETTING_ENTITY(id, ped));
		// The officer's side: the backup and the siren of a police car nearby.
		s.copsNear = static_cast<int>(units::Backup());
		s.siren = false;
		if (PED::IS_PED_IN_ANY_VEHICLE(player, false))
		{
			const Vehicle v = PED::GET_VEHICLE_PED_IS_IN(player, false);
			s.siren = lights::Stage(v) >= 2 && s.distance < 60.0f;
		}
		s.hurt = ENTITY::GET_ENTITY_HEALTH(ped) < ENTITY::GET_ENTITY_MAX_HEALTH(ped) * 3 / 4;
	}

	float Pressure(const Senses& s)
	{
		if (!s.seesPlayer && ml::TickMs() - s.lastSeenAt > 3000)
			return 0.0f;
		float p = 0.0f;
		if (s.aimedAt)
			p += 0.5f;
		else if (s.gunDrawn)
			p += 0.2f;
		p += std::clamp(1.0f - s.distance / 30.0f, 0.0f, 1.0f) * 0.3f; // close
		p += 0.15f * static_cast<float>(std::min(s.copsNear, 3));
		if (s.siren)
			p += 0.1f;
		if (s.hurt)
			p += 0.2f;
		return std::clamp(p, 0.0f, 1.5f);
	}
}
