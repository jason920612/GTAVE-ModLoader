// Ball physics (pure maths, no game calls): a basketball as a rigid sphere with spin, against static shapes —
// the floor (plane), the backboard (box) and the rim (torus) — plus contact with hands (handled by the callers).
#pragma once
#include <vector>

#include <modloader/body.hpp>

namespace bball
{
	using ml::body::Quat;
	using ml::body::Vec3;

	constexpr float kGravity = 9.81f;
	constexpr float kBallRadius = 0.12f;  // size 7 ball: 0.749 m around
	constexpr float kBallMass = 0.62f;

	struct Ball
	{
		Vec3 pos, vel;
		Vec3 spin;      // angular velocity, rad/s (world)
		Quat rot;       // orientation (for drawing)
		bool held = false; // a hand has it: no free flight
	};

	// Static colliders.
	struct Plane
	{
		Vec3 point, normal;   // floor: normal up
		float restitution = 0.82f, friction = 0.55f;
	};
	struct Box
	{
		Vec3 center, half;    // axis-aligned in its own frame
		Quat rot;
		float restitution = 0.6f, friction = 0.3f;
	};
	struct Torus
	{
		Vec3 center, axis;    // rim: axis up
		float major = 0.2286f, minor = 0.008f; // 18 in rim, 16 mm tube
		float restitution = 0.6f, friction = 0.3f;
	};

	struct World
	{
		std::vector<Plane> planes;
		std::vector<Box> boxes;
		std::vector<Torus> tori;
	};

	struct Contact
	{
		bool hit = false;
		Vec3 normal;   // from the shape to the ball centre
		float depth = 0;
		float restitution = 0, friction = 0;
		int kind = 0;  // 1 plane, 2 box, 3 torus
	};

	// Advances the ball by dt (fixed substeps inside). Returns the strongest contact of the step (for sounds, logs).
	Contact Step(Ball& b, const World& w, float dt);
}
