#include "stance.hpp"

#include <algorithm>
#include <format>
#include <string>

namespace bball
{
	namespace
	{
		std::vector<int> Path(const Skeleton& sk, int bone) // the bone and its ancestors, up to the root
		{
			std::vector<int> path;
			for (int b = bone; b >= 0; b = sk.Parent(b))
				path.push_back(b);
			return path;
		}
	}

	bool Body::Build(const Skeleton& sk)
	{
		m_sk = &sk;
		for (int s = 0; s < 2; ++s)
		{
			const char side = s == 0 ? 'L' : 'R';
			m_legs[s] = {sk.Find(std::format("SKEL_{}_Thigh", side)), sk.Find(std::format("SKEL_{}_Calf", side)), sk.Find(std::format("SKEL_{}_Foot", side))};
		}
		m_head = sk.Find("SKEL_Head");
		bool ok = m_head >= 0;
		for (const Leg& l : m_legs)
			ok = ok && l.thigh >= 0 && l.calf >= 0 && l.foot >= 0 && sk.Parent(l.foot) == l.calf && sk.Parent(l.calf) == l.thigh;
		if (!ok)
		{
			std::string list;
			for (int i = 0; i < sk.Count(); ++i)
				list += std::format("{}#{}<{} ", sk.Name(i), i, sk.Parent(i));
			ml::LogError("stance: missing leg or head bones; skeleton: {}", list);
			return false;
		}
		// The lowest common ancestor of the thighs and the head; its children on those paths carry the hips and the spine.
		const std::vector<int> head = Path(sk, m_head);
		int common = -1;
		for (const int b : Path(sk, m_legs[0].thigh))
			if (std::find(head.begin(), head.end(), b) != head.end())
			{
				common = b;
				break;
			}
		m_hips.clear();
		for (const int end : {m_legs[0].thigh, m_legs[1].thigh, m_head})
		{
			const std::vector<int> path = Path(sk, end);
			const auto at = std::find(path.begin(), path.end(), common);
			if (at == path.begin() || at == path.end())
				continue;
			const int child = *(at - 1);
			if (std::find(m_hips.begin(), m_hips.end(), child) == m_hips.end())
				m_hips.push_back(child);
			if (end == m_head)
				m_spine = child;
		}
		ml::Log("stance: hips move at {} bone(s) under {}, lean at {}", m_hips.size(), sk.Name(common), sk.Name(m_spine));
		return !m_hips.empty() && m_spine >= 0;
	}

	void Body::Apply(Pose& pose, const Stance& stance, Vec3 fwd, Vec3 up) const
	{
		const Skeleton& sk = *m_sk;
		// Feet stay where the game's pose has them.
		ml::body::Transform feet[2];
		for (int s = 0; s < 2; ++s)
			feet[s] = sk.World(m_legs[s].foot);
		// Hips down.
		for (const int b : m_hips)
		{
			const Quat parent = pose.WorldOf(sk.Parent(b)).rot;
			pose.SetLocalTranslation(b, sk.Local(b).pos + parent.Conjugate().Rotate(up * -stance.crouch));
		}
		// Lean: the spine turns forward about the axis across the body.
		if (stance.lean != 0)
			pose.SetWorld(m_spine, Quat::AxisAngle(up.Cross(fwd), stance.lean / 57.29578f) * pose.WorldOf(m_spine).rot);
		// Legs: knees forward, feet back on their spots with their rotation.
		for (int s = 0; s < 2; ++s)
		{
			const Leg& l = m_legs[s];
			pose.ReachTwoBone(l.thigh, l.calf, l.foot, feet[s].pos, pose.WorldOf(l.calf).pos + fwd * 0.5f);
			pose.SetWorld(l.foot, feet[s].rot);
		}
	}
}
