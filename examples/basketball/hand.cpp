#include "hand.hpp"

#include <cmath>
#include <format>
#include <string>

namespace bball
{
	namespace
	{
		constexpr float kSplit[3] = {0.45f, 0.35f, 0.20f}; // share of a finger's bend at its three joints

		// The rotation taking local axes a, b to world directions wa, wb (b made orthogonal to a).
		Quat Align(Vec3 a, Vec3 b, Vec3 wa, Vec3 wb)
		{
			const Quat first = Quat::FromTo(a, wa);
			const Vec3 bNow = first.Rotate(b);
			Vec3 target = wb - wa * wb.Dot(wa), cur = bNow - wa * bNow.Dot(wa);
			if (target.Length() < 1e-5f || cur.Length() < 1e-5f)
				return first;
			target = target.Normalized(), cur = cur.Normalized();
			float angle = std::acos(std::fmax(-1.0f, std::fmin(1.0f, cur.Dot(target))));
			if (cur.Cross(target).Dot(wa) < 0)
				angle = -angle;
			return Quat::AxisAngle(wa, angle) * first;
		}
	}

	bool Hand::Build(const Skeleton& sk, bool right)
	{
		const char side = right ? 'R' : 'L';
		m_hand = sk.Find(std::format("SKEL_{}_Hand", side));
		if (m_hand < 0)
			return false;
		for (int f = 0; f < 5; ++f)
		{
			Finger& finger = m_fingers[f];
			for (int j = 0; j < 3; ++j)
			{
				finger.joint[j] = sk.Find(std::format("SKEL_{}_Finger{}{}", side, f, j));
				if (finger.joint[j] < 0)
					return false;
				finger.rest[j] = sk.Rest(finger.joint[j]);
			}
			const Transform& last = finger.rest[2];
			const Vec3 segment = last.rot.Conjugate().Rotate(last.pos); // J1 -> J2, in J2's frame
			finger.tip = segment.Normalized() * (segment.Length() * 0.85f);
		}
		// Palm: knuckles of the four fingers and the wrist (hand space).
		Vec3 knuckles{};
		for (int f = 1; f < 5; ++f)
			knuckles = knuckles + m_fingers[f].rest[0].pos * 0.25f;
		m_finger = knuckles.Normalized();
		m_palm = knuckles * 0.65f;
		m_normal = (m_fingers[1].rest[0].pos.Cross(m_fingers[4].rest[0].pos)).Normalized();
		// Which side is the palm: the side the fingers curl towards in the rest pose.
		const Vec3 tipMiddle = Tip(m_fingers[2], 0) - m_fingers[2].rest[0].pos;
		if (tipMiddle.Dot(m_normal) < 0)
			m_normal = -m_normal;
		for (Finger& f : m_fingers)
		{
			const Vec3 dir = (f.rest[1].pos.Length() > 0 ? f.rest[0].rot.Rotate(f.rest[1].pos) : m_finger).Normalized();
			f.axis = dir.Cross(m_normal).Normalized();
		}
		return true;
	}

	Quat Hand::Orient(Vec3 normal, Vec3 fingers) const
	{
		return Align(m_normal, m_finger, normal.Normalized(), fingers.Normalized());
	}

	Vec3 Hand::Tip(const Finger& f, float angle, Quat* q) const
	{
		const Quat q0 = Quat::AxisAngle(f.axis, angle * kSplit[0]) * f.rest[0].rot;
		const Vec3 p1 = f.rest[0].pos + q0.Rotate(f.rest[1].pos);
		const Quat q1 = Quat::AxisAngle(f.axis, angle * kSplit[1]) * q0 * f.rest[1].rot;
		const Vec3 p2 = p1 + q1.Rotate(f.rest[2].pos);
		const Quat q2 = Quat::AxisAngle(f.axis, angle * kSplit[2]) * q1 * f.rest[2].rot;
		if (q)
			q[0] = q0, q[1] = q1, q[2] = q2;
		return p2 + q2.Rotate(f.tip);
	}

	void Hand::Set(Pose& pose, const Finger& f, float angle) const
	{
		Quat q[3];
		Tip(f, angle, q);
		pose.SetLocal(f.joint[0], q[0]);
		pose.SetLocal(f.joint[1], q[0].Conjugate() * q[1]);
		pose.SetLocal(f.joint[2], q[1].Conjugate() * q[2]);
	}

	void Hand::Wrap(Pose& pose, Vec3 ballHand, float radius) const
	{
		for (const Finger& f : m_fingers)
		{
			// Bend until the tip touches the ball (bisection; the tip comes closer as the finger bends).
			const auto gap = [&](float a) { return (Tip(f, a) - ballHand).Length() - (radius + kFingerPad); };
			float lo = -0.3f, hi = 1.8f;
			float angle = gap(lo) <= 0 ? lo : gap(hi) > 0 ? hi : 0.0f;
			if (gap(lo) > 0 && gap(hi) <= 0)
			{
				for (int i = 0; i < 24; ++i)
				{
					const float mid = (lo + hi) / 2;
					(gap(mid) > 0 ? lo : hi) = mid;
				}
				angle = (lo + hi) / 2;
			}
			Set(pose, f, angle);
		}
	}

	void Hand::Open(Pose& pose) const
	{
		for (const Finger& f : m_fingers)
			Set(pose, f, 0.1f);
	}

	Hand::Contact Hand::Measure(const Skeleton& sk, Vec3 ball, float radius) const
	{
		Contact c;
		const Transform hand = sk.Shown(m_hand);
		const Vec3 palm = hand.Apply(PalmSurface());
		c.palm = (palm - ball).Length() - radius;
		const Vec3 normal = hand.rot.Rotate(m_normal);
		c.angle = std::acos(std::fmax(-1.0f, std::fmin(1.0f, normal.Dot((ball - palm).Normalized())))) * 57.29578f;
		for (const Finger& f : m_fingers)
			c.fingertips += ((sk.Shown(f.joint[2]).Apply(f.tip) - ball).Length() - radius) / 5;
		return c;
	}
}
