// Procedural animation: maths, ped skeletons and joint overrides (research/phase0.md §42).
//
//   ml::body::Skeleton sk(ped);                       // bones, parents, rest pose
//   const int hand = sk.Find(0xDEAD), fore = sk.Parent(hand), upper = sk.Parent(fore);
//   ml::body::Pose pose(sk);
//   pose.ReachTwoBone(upper, fore, hand, target, pole); // arm IK towards a world point
//   pose.Commit();                                      // every frame while you drive the ped
//
// Conventions: a bone's local transform maps a point from bone space to its parent's space: p' = rot * p + pos.
// World transforms map bone space to world space. Quaternions are x, y, z, w (Hamilton; a * b applies b first).
#pragma once
#include <cmath>
#include <string>
#include <vector>

#include "modloader.hpp"

namespace ml::body
{
	struct Vec3
	{
		float x = 0, y = 0, z = 0;
		Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
		Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
		Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
		Vec3 operator-() const { return {-x, -y, -z}; }
		float Dot(Vec3 o) const { return x * o.x + y * o.y + z * o.z; }
		Vec3 Cross(Vec3 o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
		float Length() const { return std::sqrt(Dot(*this)); }
		Vec3 Normalized() const
		{
			const float l = Length();
			return l > 1e-8f ? *this * (1.0f / l) : Vec3{};
		}
	};

	struct Quat
	{
		float x = 0, y = 0, z = 0, w = 1;
		static Quat AxisAngle(Vec3 axis, float radians)
		{
			const Vec3 a = axis.Normalized();
			const float s = std::sin(radians / 2);
			return {a.x * s, a.y * s, a.z * s, std::cos(radians / 2)};
		}
		// The shortest rotation taking direction a to direction b.
		static Quat FromTo(Vec3 a, Vec3 b)
		{
			a = a.Normalized(), b = b.Normalized();
			const float d = a.Dot(b);
			if (d < -0.999999f)
			{
				Vec3 axis = Vec3{1, 0, 0}.Cross(a);
				if (axis.Length() < 1e-4f)
					axis = Vec3{0, 1, 0}.Cross(a);
				return AxisAngle(axis, 3.14159265f);
			}
			const Vec3 c = a.Cross(b);
			return Quat{c.x, c.y, c.z, 1 + d}.Normalized();
		}
		Quat operator*(Quat o) const
		{
			return {w * o.x + x * o.w + y * o.z - z * o.y, w * o.y - x * o.z + y * o.w + z * o.x, w * o.z + x * o.y - y * o.x + z * o.w,
			    w * o.w - x * o.x - y * o.y - z * o.z};
		}
		Quat Conjugate() const { return {-x, -y, -z, w}; }
		// The angle of the rotation between this and o, degrees.
		float AngleTo(Quat o) const
		{
			const float d = std::fabs(x * o.x + y * o.y + z * o.z + w * o.w);
			return 2 * std::acos(d < 1 ? d : 1.0f) * 57.29578f;
		}
		Quat Normalized() const
		{
			const float l = std::sqrt(x * x + y * y + z * z + w * w);
			return l > 0 ? Quat{x / l, y / l, z / l, w / l} : Quat{};
		}
		Vec3 Rotate(Vec3 v) const
		{
			const Vec3 u{x, y, z};
			const Vec3 t = u.Cross(v) * 2.0f;
			return v + t * w + u.Cross(t);
		}
		static Quat Slerp(Quat a, Quat b, float t)
		{
			float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
			if (d < 0)
				b = {-b.x, -b.y, -b.z, -b.w}, d = -d;
			float wa = 1 - t, wb = t;
			if (d < 0.9995f)
			{
				const float th = std::acos(d), s = std::sin(th);
				wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
			}
			return Quat{a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb}.Normalized();
		}
		// From a row-vector rotation (rows = images of the x, y, z axes).
		static Quat FromRows(const float* m)
		{
			Quat q;
			const float tr = m[0] + m[5] + m[10];
			if (tr > 0)
			{
				const float s = std::sqrt(tr + 1.0f) * 2;
				q = {(m[6] - m[9]) / s, (m[8] - m[2]) / s, (m[1] - m[4]) / s, 0.25f * s};
			}
			else if (m[0] > m[5] && m[0] > m[10])
			{
				const float s = std::sqrt(1.0f + m[0] - m[5] - m[10]) * 2;
				q = {0.25f * s, (m[4] + m[1]) / s, (m[8] + m[2]) / s, (m[6] - m[9]) / s};
			}
			else if (m[5] > m[10])
			{
				const float s = std::sqrt(1.0f + m[5] - m[0] - m[10]) * 2;
				q = {(m[4] + m[1]) / s, 0.25f * s, (m[9] + m[6]) / s, (m[8] - m[2]) / s};
			}
			else
			{
				const float s = std::sqrt(1.0f + m[10] - m[0] - m[5]) * 2;
				q = {(m[8] + m[2]) / s, (m[9] + m[6]) / s, 0.25f * s, (m[1] - m[4]) / s};
			}
			return q.Normalized();
		}
	};

	struct Transform
	{
		Quat rot;
		Vec3 pos;
		Vec3 Apply(Vec3 p) const { return rot.Rotate(p) + pos; }
		Transform operator*(const Transform& child) const { return {rot * child.rot, Apply(child.pos)}; } // parent * child
		Transform Inverse() const
		{
			const Quat r = rot.Conjugate();
			return {r, -r.Rotate(pos)};
		}
		static Transform FromMatrix(const float* m) { return {Quat::FromRows(m), {m[12], m[13], m[14]}}; }
	};

	inline bool Available() { return detail::Has(&MLApi::PoseSet); }

	// A ped's skeleton: bones, parents, names and rest pose; current transforms are read from the game each call.
	class Skeleton
	{
	public:
		explicit Skeleton(int32_t ped) : m_ped(ped)
		{
			if (!Available())
				return;
			const int n = Api().PoseBoneCount(ped);
			for (int i = 0; i < n; ++i)
			{
				MLBoneInfo info{};
				Api().PoseBoneInfo(ped, i, &info);
				m_bones.push_back(info);
			}
		}
		int32_t Ped() const { return m_ped; }
		int Count() const { return static_cast<int>(m_bones.size()); }
		bool Valid() const { return !m_bones.empty(); }
		int Find(uint16_t tag) const
		{
			for (int i = 0; i < Count(); ++i)
				if (m_bones[i].tag == tag)
					return i;
			return -1;
		}
		int Find(const std::string& name) const
		{
			for (int i = 0; i < Count(); ++i)
				if (name == m_bones[i].name)
					return i;
			return -1;
		}
		int Parent(int bone) const { return bone >= 0 && bone < Count() ? m_bones[bone].parent : -1; }
		const char* Name(int bone) const { return bone >= 0 && bone < Count() ? m_bones[bone].name : ""; }
		Transform Rest(int bone) const
		{
			const MLBoneInfo& b = m_bones[bone];
			return {Quat{b.restRotation[0], b.restRotation[1], b.restRotation[2], b.restRotation[3]}.Normalized(),
			    {b.restTranslation[0], b.restTranslation[1], b.restTranslation[2]}};
		}
		// This frame's transforms as the game animated them (before overrides: what a Pose builds on).
		Transform Local(int bone) const { return Get(bone, ML_SPACE_LOCAL); }
		Transform World(int bone) const { return Get(bone, ML_SPACE_WORLD); }
		// As shown on screen last frame (overrides applied): to check what a Pose did.
		Transform Shown(int bone) const { return Get(bone, ML_SPACE_WORLD | ML_SPACE_SHOWN); }

	private:
		Transform Get(int bone, int space) const
		{
			float m[16]{};
			return Api().PoseGetBone(m_ped, bone, space, m) ? Transform::FromMatrix(m) : Transform{};
		}
		int32_t m_ped;
		std::vector<MLBoneInfo> m_bones;
	};

	// Joint overrides for one frame, built against the ped's current pose.
	class Pose
	{
	public:
		// The game moves the root after the script runs (the post-animation update shifts it, by about 1.6 cm for a
		// standing ped); the shift is taken from the pose shown last frame so that targets land where they are shown.
		explicit Pose(const Skeleton& sk) : m_sk(sk)
		{
			for (int i = 0; i < sk.Count(); ++i)
				if (sk.Parent(i) < 0)
				{
					m_rootShift = sk.Shown(i).pos - sk.World(i).pos;
					break;
				}
		}

		// Local rotation (parent space) of a bone.
		void SetLocal(int bone, Quat rot, float weight = 1.0f)
		{
			Entry(bone).rot = rot;
			Entry(bone).flags |= ML_POSE_ROTATION;
			Entry(bone).weight = weight;
		}
		void SetLocalTranslation(int bone, Vec3 pos, float weight = 1.0f)
		{
			Entry(bone).pos = pos;
			Entry(bone).flags |= ML_POSE_TRANSLATION;
			Entry(bone).weight = weight;
		}
		// World rotation of a bone (its parent's world rotation as this pose leaves it).
		void SetWorld(int bone, Quat worldRot, float weight = 1.0f)
		{
			const int p = m_sk.Parent(bone);
			const Quat parent = p < 0 ? Quat{} : WorldOf(p).rot;
			SetLocal(bone, parent.Conjugate() * worldRot, weight);
		}
		// World transform of a bone under this pose (current game pose with this pose's overrides applied above it).
		Transform WorldOf(int bone) const
		{
			const int p = m_sk.Parent(bone);
			if (p < 0)
			{
				// The root's parent is the entity: apply this pose's root override in the entity's frame.
				Transform root = m_sk.World(bone);
				const Transform local = m_sk.Local(bone);
				const Quat entity = root.rot * local.rot.Conjugate();
				for (const Item& e : m_items)
					if (e.bone == bone)
					{
						if (e.flags & ML_POSE_TRANSLATION)
							root.pos = root.pos + entity.Rotate(e.pos - local.pos);
						if (e.flags & ML_POSE_ROTATION)
							root.rot = entity * e.rot;
					}
				root.pos = root.pos + m_rootShift;
				return root;
			}
			Transform local = m_sk.Local(bone);
			for (const Item& e : m_items)
				if (e.bone == bone)
				{
					if (e.flags & ML_POSE_ROTATION)
						local.rot = e.rot;
					if (e.flags & ML_POSE_TRANSLATION)
						local.pos = e.pos;
				}
			return WorldOf(p) * local;
		}

		// Two-bone IK: turns `upper` and `mid` so that `end` reaches `target` (world), bending towards `pole`.
		// Returns false when the target is out of reach (the chain is then stretched towards it).
		bool ReachTwoBone(int upper, int mid, int end, Vec3 target, Vec3 pole, float weight = 1.0f)
		{
			const Transform wa = WorldOf(upper), wb = WorldOf(mid), wc = WorldOf(end);
			const Vec3 a = wa.pos, b = wb.pos, c = wc.pos;
			const float lab = (b - a).Length(), lbc = (c - b).Length();
			Vec3 toTarget = target - a;
			float d = toTarget.Length();
			const bool reach = d <= lab + lbc - 1e-4f;
			d = std::fmax(std::fabs(lab - lbc) + 1e-4f, std::fmin(d, lab + lbc - 1e-4f));
			const Vec3 u = toTarget.Normalized();
			Vec3 v = (pole - a) - u * (pole - a).Dot(u);
			if (v.Length() < 1e-5f)
				v = (b - a) - u * (b - a).Dot(u);
			v = v.Normalized();
			const float cosA = std::fmax(-1.0f, std::fmin(1.0f, (lab * lab + d * d - lbc * lbc) / (2 * lab * d)));
			const Vec3 elbow = a + (u * cosA + v * std::sqrt(std::fmax(0.0f, 1 - cosA * cosA))) * lab;
			const Vec3 hand = a + u * d;
			// Upper bone: point (b - a) at (elbow - a). Mid bone: point (c - b), as rotated, at (hand - elbow).
			const Quat qa = Quat::FromTo(b - a, elbow - a);
			const Quat upperWorld = qa * wa.rot;
			const Vec3 c1 = a + qa.Rotate(c - a), b1 = a + qa.Rotate(b - a);
			const Quat qb = Quat::FromTo(c1 - b1, hand - elbow);
			const Quat midWorld = qb * qa * wb.rot;
			SetWorld(upper, upperWorld, weight);
			SetWorld(mid, midWorld, weight);
			return reach;
		}

		bool Commit() const
		{
			std::vector<MLBonePose> out;
			for (const Item& e : m_items)
				out.push_back({e.bone, e.flags, {e.rot.x, e.rot.y, e.rot.z, e.rot.w}, {e.pos.x, e.pos.y, e.pos.z}, e.weight});
			return Available() && Api().PoseSet(m_sk.Ped(), out.data(), static_cast<int32_t>(out.size())) != 0;
		}
		// Gives the ped back to the game's animation.
		static void Clear(int32_t ped)
		{
			if (Available())
				Api().PoseSet(ped, nullptr, 0);
		}

	private:
		struct Item
		{
			int bone;
			uint32_t flags = 0;
			Quat rot;
			Vec3 pos;
			float weight = 1;
		};
		Item& Entry(int bone)
		{
			for (Item& e : m_items)
				if (e.bone == bone)
					return e;
			m_items.push_back({bone});
			return m_items.back();
		}
		const Skeleton& m_sk;
		std::vector<Item> m_items;
		Vec3 m_rootShift;
	};
}
