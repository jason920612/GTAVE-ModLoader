// A hand built from the ped's skeleton: the palm (centre, normal, finger direction), the fingers' chains, and how to
// put the palm on a ball and wrap the fingers around it. Contact is measured as in Liu & Hodgins 2018 (proxy points
// on the fingertips: mean distance to the ball centre minus its radius).
#pragma once
#include <modloader/body.hpp>

namespace bball
{
	using ml::body::Pose;
	using ml::body::Quat;
	using ml::body::Skeleton;
	using ml::body::Transform;
	using ml::body::Vec3;

	class Hand
	{
	public:
		// right: SKEL_R_* bones, else SKEL_L_*. False when the skeleton lacks them.
		bool Build(const Skeleton& sk, bool right);

		int Bone() const { return m_hand; }
		// Hand space (the hand bone's frame).
		Vec3 PalmSurface() const { return m_palm + m_normal * kPalmPad; }
		Vec3 Normal() const { return m_normal; }
		Vec3 FingerDir() const { return m_finger; }

		// The hand's world rotation with the palm normal along `normal` and the fingers towards `fingers`.
		Quat Orient(Vec3 normal, Vec3 fingers) const;
		// Wrist (hand bone) position that puts the palm surface at `palmWorld` with rotation `rot`.
		Vec3 WristFor(Vec3 palmWorld, const Quat& rot) const { return palmWorld - rot.Rotate(PalmSurface()); }

		// Bends every finger so its tip rests on the ball (centre in hand space). Adds the finger joints to the pose.
		void Wrap(Pose& pose, Vec3 ballHand, float radius) const;
		// Flat open hand (fingers straight).
		void Open(Pose& pose) const;

		struct Contact
		{
			float fingertips = 0; // mean fingertip distance to the ball surface (Liu & Hodgins eq. 7), m
			float palm = 0;       // palm surface distance to the ball surface, m
			float angle = 0;      // between the palm normal and the direction to the ball centre, degrees
		};
		// Measured on the pose as shown last frame.
		Contact Measure(const Skeleton& sk, Vec3 ballWorld, float radius) const;

		static constexpr float kPalmPad = 0.012f; // palm surface in front of the bones
		static constexpr float kFingerPad = 0.008f;

	private:
		struct Finger
		{
			int joint[3] = {-1, -1, -1};
			Transform rest[3];
			Vec3 tip;   // tip offset in the last joint's frame
			Vec3 axis;  // flex axis, hand space (bending moves the finger towards the palm normal)
		};
		// Hand-space tip of a finger flexed by a total angle (split over its three joints).
		Vec3 Tip(const Finger& f, float angle, Quat* q = nullptr) const;
		void Set(Pose& pose, const Finger& f, float angle) const;

		int m_hand = -1;
		Finger m_fingers[5]; // thumb, index, middle, ring, pinky
		Vec3 m_palm, m_normal, m_finger;
	};
}
