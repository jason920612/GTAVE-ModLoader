// Basketball (in development): procedural players and ball physics (research/phase0.md §44).
// Development commands, ModLoader\bball.txt (read every half second, removed):
//   dribble <seconds>   the player dribbles in place with the right hand; logs ModLoader\bball_log.csv
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

#include <modloader/body.hpp>
#include <modloader/game.hpp>

#include "physics.hpp"

ML_MOD_INFO("Basketball", "0.0.1", "ModLoader", "Street basketball with procedural players (in development)")

namespace bball
{
	namespace b = ml::body;

	// Which local axes of a bone point along two world directions in the current pose (for calibration).
	struct Axes
	{
		Vec3 palm, finger; // local unit axes
	};
	Vec3 BestAxis(const Quat& rot, Vec3 world)
	{
		const Vec3 axes[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
		Vec3 best = axes[0];
		float score = -2;
		for (const Vec3& a : axes)
			if (const float s = rot.Rotate(a).Dot(world); s > score)
				score = s, best = a;
		return best;
	}

	// The rotation whose local axes a, b map to world directions wa, wb (b orthogonalised against a).
	Quat Align(Vec3 a, Vec3 bLocal, Vec3 wa, Vec3 wb)
	{
		const Quat first = Quat::FromTo(a, wa);
		const Vec3 bNow = first.Rotate(bLocal);
		Vec3 target = wb - wa * wb.Dot(wa);
		Vec3 cur = bNow - wa * bNow.Dot(wa);
		if (target.Length() < 1e-5f || cur.Length() < 1e-5f)
			return first;
		target = target.Normalized(), cur = cur.Normalized();
		float angle = std::acos(std::fmax(-1.0f, std::fmin(1.0f, cur.Dot(target))));
		if (cur.Cross(target).Dot(wa) < 0)
			angle = -angle;
		return Quat::AxisAngle(wa, angle) * first;
	}

	float Hermite(float p0, float v0, float p1, float v1, float t, float T)
	{
		const float s = t / T, s2 = s * s, s3 = s2 * s;
		return (2 * s3 - 3 * s2 + 1) * p0 + (s3 - 2 * s2 + s) * v0 * T + (-2 * s3 + 3 * s2) * p1 + (s3 - s2) * v1 * T;
	}

	void Dribble(float seconds)
	{
		const Ped ped = PLAYER::PLAYER_PED_ID();
		const b::Skeleton sk(ped);
		const int hand = sk.Find(static_cast<uint16_t>(0xDEAD)), fore = sk.Parent(hand), upper = sk.Parent(fore);
		if (hand < 0 || fore < 0 || upper < 0)
		{
			ml::LogError("dribble: no arm bones");
			return;
		}
		const Hash model = MISC::GET_HASH_KEY("prop_bskball_01");
		auto loaded = ml::LoadModel(model);
		if (!loaded)
		{
			ml::LogError("dribble: ball model did not load");
			return;
		}
		Vector3 mn{}, mx{};
		MISC::GET_MODEL_DIMENSIONS(model, &mn, &mx);
		ml::Log("dribble: ball model size {:.3f} x {:.3f} x {:.3f}", mx.x - mn.x, mx.y - mn.y, mx.z - mn.z);

		// The ped's frame.
		const Vector3 p = ENTITY::GET_ENTITY_COORDS(ped, false);
		const float h = ENTITY::GET_ENTITY_HEADING(ped) * 3.14159265f / 180.0f;
		const Vec3 fwd{-std::sin(h), std::cos(h), 0}, right{std::cos(h), std::sin(h), 0}, up{0, 0, 1};
		const Vec3 spot = Vec3{p.x, p.y, p.z} + right * 0.28f + fwd * 0.27f;
		float floorZ = p.z - 1.0f;
		MISC::GET_GROUND_Z_FOR_3D_COORD(spot.x, spot.y, p.z + 1.0f, &floorZ, false, false);

		// Hand calibration from the idle pose: palm towards the body, fingers down.
		const Quat idle = sk.World(hand).rot;
		const Axes axes{BestAxis(idle, -right), BestAxis(idle, -up)};
		ml::Log("dribble: palm axis {:.0f},{:.0f},{:.0f} finger axis {:.0f},{:.0f},{:.0f}", axes.palm.x, axes.palm.y, axes.palm.z, axes.finger.x, axes.finger.y,
		    axes.finger.z);

		World world;
		world.planes.push_back({{spot.x, spot.y, floorZ}, up});

		// Dribble: the hand catches the ball at height H (centre) moving up at vu, absorbs it, pushes it down to leave at v0.
		const float H = floorZ + 0.78f, vu = 1.6f, Ta = 0.09f, Tp = 0.11f;
		const float drop = H - (floorZ + kBallRadius), e = world.planes[0].restitution;
		const float vb = std::sqrt(vu * vu + 2 * kGravity * drop), vi = vb / e, v0 = std::sqrt(std::fmax(0.0f, vi * vi - 2 * kGravity * drop));
		const float top = H + vu * Ta / 2; // where absorbing stops
		ml::Log("dribble: floor {:.3f} catch height {:.3f} push speed {:.2f} m/s", floorZ, H, v0);

		Ball ball;
		ball.pos = Vec3{spot.x, spot.y, top};
		ball.held = true;
		Object obj = OBJECT::CREATE_OBJECT_NO_OFFSET(model, ball.pos.x, ball.pos.y, ball.pos.z, false, false, false, 0);
		ENTITY::SET_ENTITY_COLLISION(obj, false, false);
		ENTITY::FREEZE_ENTITY_POSITION(obj, true);

		enum class Phase { Push, Free, Absorb } phase = Phase::Push;
		float phaseTime = 0, catchZ = top, t = 0;
		std::ofstream log("ModLoader/bball_log.csv");
		log << "t,phase,ball_z,palm_target_z,hand_shown_z,impact\n";
		const uint64_t end = ml::TickMs() + static_cast<uint64_t>(seconds * 1000);
		while (ml::TickMs() < end)
		{
			const float dt = std::fmin(MISC::GET_FRAME_TIME(), 0.05f);
			t += dt;
			phaseTime += dt;
			Contact impact;
			switch (phase)
			{
			case Phase::Push:
				ball.held = true;
				ball.pos.z = Hermite(top, 0, H, -v0, std::fmin(phaseTime, Tp), Tp);
				if (phaseTime >= Tp)
				{
					phase = Phase::Free, phaseTime = 0;
					ball.held = false;
					ball.pos = Vec3{spot.x, spot.y, H};
					ball.vel = Vec3{0, 0, -v0};
					ball.spin = right * -6.0f; // a little backspin from the fingers
				}
				break;
			case Phase::Free:
				impact = Step(ball, world, dt);
				if (ball.vel.z > 0 && ball.pos.z >= H)
				{
					phase = Phase::Absorb, phaseTime = 0, catchZ = ball.pos.z;
					ball.held = true;
				}
				break;
			case Phase::Absorb:
				ball.pos.z = Hermite(catchZ, vu, top, 0, std::fmin(phaseTime, Ta), Ta);
				if (phaseTime >= Ta)
					phase = Phase::Push, phaseTime = 0;
				break;
			}
			// The palm rests on top of the ball while it is held, otherwise waits just above the catch height.
			const float palmZ = ball.held ? ball.pos.z + kBallRadius : std::fmax(ball.pos.z + kBallRadius, H + kBallRadius - 0.03f);
			const Vec3 palm{spot.x, spot.y, palmZ};
			const Vec3 wrist = palm + up * 0.035f - fwd * 0.05f; // wrist above and behind the palm centre
			const Vec3 pole = sk.World(upper).pos + right * 0.5f - fwd * 0.4f - up * 0.4f;
			b::Pose pose(sk);
			pose.ReachTwoBone(upper, fore, hand, wrist, pole);
			pose.SetWorld(hand, Align(axes.palm, axes.finger, -up, fwd));
			pose.Commit();

			ENTITY::SET_ENTITY_COORDS_NO_OFFSET(obj, ball.pos.x, ball.pos.y, ball.pos.z, false, false, false);
			ENTITY::SET_ENTITY_QUATERNION(obj, ball.rot.x, ball.rot.y, ball.rot.z, ball.rot.w);
			log << std::format("{:.4f},{},{:.4f},{:.4f},{:.4f},{:.3f}\n", t, static_cast<int>(phase), ball.pos.z - floorZ, palmZ - floorZ,
			    sk.Shown(hand).pos.z - floorZ, impact.hit ? impact.depth : 0.0f);
			ml::Wait(0);
		}
		b::Pose::Clear(ped);
		ENTITY::DELETE_ENTITY(&obj);
		ml::Log("dribble: done");
	}
}

extern "C" __declspec(dllexport) int MLOnLoad(const MLApi* api, const MLContext* ctx)
{
	ml::Init(api, ctx);
	if (!ml::body::Available())
	{
		ml::LogError("this loader has no pose API; basketball needs it");
		return 0;
	}
	return 1;
}

extern "C" __declspec(dllexport) void MLMain()
{
	for (;;)
	{
		ml::Wait(500);
		std::ifstream in("ModLoader/bball.txt");
		if (!in)
			continue;
		std::string cmd, arg;
		in >> cmd >> arg;
		in.close();
		std::error_code ec;
		std::filesystem::remove("ModLoader/bball.txt", ec);
		if (cmd == "dribble")
			bball::Dribble(arg.empty() ? 8.0f : std::stof(arg));
	}
}
