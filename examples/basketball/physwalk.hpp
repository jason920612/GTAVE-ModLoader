// Drives the player's skeleton from the physically simulated body (physbody.hpp) (research/phase0.md §50).
//
// Every skeleton bone belongs to one simulated body (the segments of tools/fit_capsules.py: roll bones to their limb,
// fingers to the rigid hand). The simulation's frame is the skeleton's object frame at its bind pose (the body model was
// built from it), with the floor at the ground. A bone's target = its body's transform x the bone's rest placement
// in that body. Bones where the owning body changes (and each body's own bone) get rotation and translation; the
// others keep the game's animation (fingers, face).
#pragma once
#include <memory>
#include <string>
#include <vector>

#include <modloader/body.hpp>

#include "physbody.hpp"

namespace bball
{
	class PhysWalk
	{
	public:
		// dataDir: the exported policy (body.mjb, policy.bin, reference.bin).
		bool Start(int32_t ped, const std::wstring& dataDir, std::string& error);
		// Advances the simulation by the frame time (100 Hz control steps) and poses the skeleton. False when done.
		bool Update(float frameTime);
		void Stop();
		const PhysBody* Body() const { return m_body.get(); }
		// The clip's overall walking direction in the world (unit, horizontal).
		ml::body::Vec3 WorldDirection() const;

	private:
		struct BoneLink
		{
			int bone = -1;
			int body = -1;               // MuJoCo body id
			ml::body::Quat restRot;      // bone rotation at the bind pose (object frame)
			ml::body::Vec3 offset;       // bone position - body origin at the bind pose (object frame)
		};
		void Pose();

		int32_t m_ped = 0;
		std::unique_ptr<PhysBody> m_body;
		std::unique_ptr<ml::body::Skeleton> m_sk;
		std::vector<BoneLink> m_links; // in skeleton order (parents first)
		ml::body::Transform m_entity;  // the ped's entity transform at the start
		float m_floor = 0;             // sim z of the object frame's origin (ground below the entity)
		float m_bindLift = 0;          // the body model's bind pose sits this high above its object frame
		double m_time = 0;
	};
}
