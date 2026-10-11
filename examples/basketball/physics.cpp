#include "physics.hpp"

#include <algorithm>
#include <cmath>

namespace bball
{
	namespace
	{
		constexpr float kSubstep = 1.0f / 240.0f;
		constexpr float kAirDrag = 0.012f; // linear drag per second (a basketball barely slows over a pass)

		Contact Against(const Ball& b, const Plane& p)
		{
			Contact c;
			const float d = (b.pos - p.point).Dot(p.normal) - kBallRadius;
			if (d < 0)
				c = {true, p.normal, -d, p.restitution, p.friction, 1};
			return c;
		}

		Contact Against(const Ball& b, const Box& box)
		{
			// Closest point of the box (in its frame) to the centre.
			const Quat inv = box.rot.Conjugate();
			const Vec3 local = inv.Rotate(b.pos - box.center);
			const Vec3 clamped{std::clamp(local.x, -box.half.x, box.half.x), std::clamp(local.y, -box.half.y, box.half.y),
			    std::clamp(local.z, -box.half.z, box.half.z)};
			const Vec3 delta = local - clamped;
			const float dist = delta.Length();
			Contact c;
			if (dist < kBallRadius && dist > 1e-6f)
				c = {true, box.rot.Rotate(delta * (1.0f / dist)), kBallRadius - dist, box.restitution, box.friction, 2};
			return c;
		}

		Contact Against(const Ball& b, const Torus& t)
		{
			// Closest point on the tube's centre circle, then a sphere of the tube's radius.
			const Vec3 axis = t.axis.Normalized();
			const Vec3 rel = b.pos - t.center;
			Vec3 radial = rel - axis * rel.Dot(axis);
			if (radial.Length() < 1e-6f)
				radial = Vec3{1, 0, 0} - axis * axis.x;
			const Vec3 ring = t.center + radial.Normalized() * t.major;
			const Vec3 delta = b.pos - ring;
			const float dist = delta.Length(), reach = kBallRadius + t.minor;
			Contact c;
			if (dist < reach && dist > 1e-6f)
				c = {true, delta * (1.0f / dist), reach - dist, t.restitution, t.friction, 3};
			return c;
		}

		// Impulse response: restitution on the normal speed, Coulomb friction on the contact point's slip, which also
		// changes the spin (solid-ish shell: I = 2/3 m r^2).
		void Resolve(Ball& b, const Contact& c)
		{
			b.pos = b.pos + c.normal * c.depth;
			const float vn = b.vel.Dot(c.normal);
			if (vn >= 0)
				return;
			const Vec3 r = c.normal * -kBallRadius; // centre -> contact point
			const Vec3 vContact = b.vel + b.spin.Cross(r);
			const Vec3 vt = vContact - c.normal * vContact.Dot(c.normal);
			const float jn = -(1 + c.restitution) * vn; // per unit mass
			b.vel = b.vel + c.normal * jn;
			const float slip = vt.Length();
			if (slip > 1e-5f)
			{
				// Tangential impulse that would stop the slip: effective mass for a shell is 1 / (1 + r^2/k^2) = 2/5.
				const float stop = slip * 0.4f;
				const float jt = std::min(stop, c.friction * jn);
				const Vec3 dir = vt * (-1.0f / slip);
				b.vel = b.vel + dir * jt;
				// Torque: r x (dir * jt) / I, with I/m = 2/3 r^2.
				b.spin = b.spin + r.Cross(dir * jt) * (1.0f / (2.0f / 3.0f * kBallRadius * kBallRadius));
			}
		}
	}

	Contact Step(Ball& b, const World& w, float dt)
	{
		Contact strongest;
		if (b.held)
			return strongest;
		dt = std::clamp(dt, 0.0f, 0.1f);
		for (float left = dt; left > 1e-6f; left -= kSubstep)
		{
			const float h = std::min(kSubstep, left);
			b.vel = b.vel + Vec3{0, 0, -kGravity * h};
			b.vel = b.vel * (1.0f - kAirDrag * h);
			b.pos = b.pos + b.vel * h;
			const float spinLength = b.spin.Length();
			if (spinLength > 1e-6f)
				b.rot = (Quat::AxisAngle(b.spin, spinLength * h) * b.rot).Normalized();
			const auto consider = [&](const Contact& c) {
				if (!c.hit)
					return;
				const float speed = -b.vel.Dot(c.normal);
				Resolve(b, c);
				if (!strongest.hit || speed > strongest.depth)
				{
					strongest = c;
					strongest.depth = speed; // reported as impact speed
				}
			};
			for (const Plane& p : w.planes)
				consider(Against(b, p));
			for (const Box& box : w.boxes)
				consider(Against(b, box));
			for (const Torus& t : w.tori)
				consider(Against(b, t));
		}
		return strongest;
	}
}
