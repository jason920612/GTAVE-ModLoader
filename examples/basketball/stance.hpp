// The body's stance, built on the game's current pose: hips lowered with the feet kept where they are (two-bone IK on
// the legs, knees bending forward) and the upper body leaning forward from the hips. The arms are posed after it.
// Note: the game overwrites the spine's translation after our pose is applied (research §45), so a crouch lowers
// only the legs' side for now.
#pragma once
#include <vector>

#include <modloader/body.hpp>

namespace bball
{
	using ml::body::Pose;
	using ml::body::Quat;
	using ml::body::Skeleton;
	using ml::body::Vec3;

	struct Stance
	{
		float crouch = 0.0f; // how far the hips go down, m
		float lean = 0.0f;   // upper body forward from the hips, degrees
	};

	class Body
	{
	public:
		// False (and the bone list in the log) when the skeleton lacks the leg or spine bones.
		bool Build(const Skeleton& sk);
		// Adds the stance to the pose; fwd and up are the ped's directions.
		void Apply(Pose& pose, const Stance& stance, Vec3 fwd, Vec3 up) const;

	private:
		struct Leg
		{
			int thigh = -1, calf = -1, foot = -1;
		};
		const Skeleton* m_sk = nullptr;
		Leg m_legs[2];
		int m_head = -1;
		std::vector<int> m_hips; // children of the common ancestor of the legs and the head, on their paths (moved down together)
		int m_spine = -1;        // the one of them carrying the head (turned for the lean)
	};
}
