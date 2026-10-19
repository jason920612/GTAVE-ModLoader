// The suction fan ("multi-mode suction system"): fans under the car pull it onto the surface beneath its floor, at any
// speed, so the tyres press down much harder than the car's weight alone. A fan car holds about twice its weight
// standing still: enough grip to stay on a surface that curves up to vertical, or over the top into a ceiling.
//
// Only the pull is added; how the car gets onto a wall is left to the game's physics. The chassis climbs what its
// wheels can roll onto (a ramp or a curve whose slope rises gradually); a wall that rises straight from the ground is
// hit like any car would hit it.
//
// Each frame, with the fan on: a probe straight down the car's own up axis. When the surface under the floor is close,
// the car is pulled towards it (along the surface normal) at the fan's strength; the pull fades out as the floor gets
// farther from the surface, the way a fan's suction does, and is gone once the car is in the air.
#include "roadster.hpp"

#include <algorithm>
#include <cmath>

namespace roadster::fan
{
	namespace
	{
		struct V3
		{
			float x = 0, y = 0, z = 0;
		};
		V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
		V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
		float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		V3 Normalize(V3 a)
		{
			const float l = std::sqrt(Dot(a, a));
			return l > 1e-6f ? a * (1.0f / l) : V3{};
		}
		V3 From(const Vector3& v) { return {v.x, v.y, v.z}; }

		constexpr float kGravity = 9.81f;
		constexpr float kReach = 2.0f;    // probe length from the car's centre (m)
		constexpr float kFullGap = 0.25f; // full pull while the floor is this close to its resting height above the surface
		constexpr float kNoGap = 0.8f;    // no pull from this far above it

		struct State
		{
			Vehicle vehicle = 0;
			float ride = 0.55f; // height of the car's centre above the surface at rest (m), learnt while driving
			uint64_t lastLog = 0;
			Vector3 lastVel{}; // at the last log line
		} g;
	}

	void Reset()
	{
		g = {};
	}

	void Tick(Vehicle v, float strength, bool on, bool debug)
	{
		if (v != g.vehicle)
		{
			Reset();
			g.vehicle = v;
		}
		const float dt = std::min(MISC::GET_FRAME_TIME(), 0.05f);
		if (dt <= 0)
			return;
		Vector3 fwd3{}, right3{}, up3{}, pos3{};
		ENTITY::GET_ENTITY_MATRIX(v, &fwd3, &right3, &up3, &pos3);
		const V3 up = From(up3), pos = From(pos3);

		const int handle = SHAPETEST::START_EXPENSIVE_SYNCHRONOUS_SHAPE_TEST_LOS_PROBE(pos.x, pos.y, pos.z, pos.x - up.x * kReach,
		    pos.y - up.y * kReach, pos.z - up.z * kReach, 1 | 16, v, 7);
		BOOL hit = false;
		Vector3 end{}, normal3{};
		Entity entity = 0;
		SHAPETEST::GET_SHAPE_TEST_RESULT(handle, &hit, &end, &normal3, &entity);
		const V3 n = Normalize(From(normal3));
		float pull = 0, height = 0;
		if (hit && Dot(n, up) > 0.5f)
		{
			height = Dot(pos - From(end), n);
			if (Dot(n, up) > 0.98f && VEHICLE::IS_VEHICLE_ON_ALL_WHEELS(v))
				g.ride += (std::clamp(height, 0.2f, 1.2f) - g.ride) * 0.05f;
			const float gap = height - g.ride;
			pull = on ? strength * std::clamp((kNoGap - gap) / (kNoGap - kFullGap), 0.0f, 1.0f) : 0.0f;
			if (pull > 0)
			{
				const Vector3 vel = ENTITY::GET_ENTITY_VELOCITY(v);
				const float dv = pull * kGravity * dt;
				ENTITY::SET_ENTITY_VELOCITY(v, vel.x - n.x * dv, vel.y - n.y * dv, vel.z - n.z * dv);
			}
		}

		if (debug && ml::TickMs() - g.lastLog >= 100)
		{
			const float span = (ml::TickMs() - g.lastLog) / 1000.0f;
			const float speed = ENTITY::GET_ENTITY_SPEED(v);
			// Sideways acceleration (in g): the change of velocity since the last line, along the car's right axis.
			const Vector3 vel = ENTITY::GET_ENTITY_VELOCITY(v);
			const V3 accel = (From(vel) - From(g.lastVel)) * (span > 0 && span < 0.5f ? 1.0f / span : 0.0f);
			const float lateral = std::abs(Dot(accel, From(right3))) / kGravity;
			g.lastVel = vel;
			ml::Log("fan pos=({:.1f},{:.1f},{:.1f}) up=({:.2f},{:.2f},{:.2f}) speed={:.1f} hit={} height={:.2f} ride={:.2f} pull={:.2f} wheels={} lat={:.2f}g",
			    pos.x, pos.y, pos.z, up.x, up.y, up.z, speed, hit != 0, height, g.ride, pull, VEHICLE::IS_VEHICLE_ON_ALL_WHEELS(v), lateral);
			g.lastLog = ml::TickMs();
		}
	}
}
