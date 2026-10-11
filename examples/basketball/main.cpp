// Basketball (in development): procedural players and ball physics (research/phase0.md §44).
// Development commands, ModLoader\bball.txt (read every half second, removed):
//   dribble <seconds> [close] [slow]   the player dribbles in place with the right hand; logs ModLoader\bball_log.csv
//                                      (close: camera at the hand; slow: time at 0.2, real seconds)
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

#include <modloader/body.hpp>
#include <modloader/game.hpp>

#include "hand.hpp"
#include "physics.hpp"
#include "stance.hpp"

ML_MOD_INFO("Basketball", "0.0.1", "ModLoader", "Street basketball with procedural players (in development)")

namespace bball
{
	namespace b = ml::body;

	float Hermite(float p0, float v0, float p1, float v1, float t, float T)
	{
		const float s = t / T, s2 = s * s, s3 = s2 * s;
		return (2 * s3 - 3 * s2 + 1) * p0 + (s3 - 2 * s2 + s) * v0 * T + (-2 * s3 + 3 * s2) * p1 + (s3 - s2) * v1 * T;
	}

	Cam g_cam = 0;

	void Dribble(float seconds, bool close, bool slow, Stance stance, bool full, bool markers)
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
		ml::Log("dribble: ball model min {:.3f},{:.3f},{:.3f} max {:.3f},{:.3f},{:.3f}", mn.x, mn.y, mn.z, mx.x, mx.y, mx.z);

		// The ped's frame.
		const Vector3 p = ENTITY::GET_ENTITY_COORDS(ped, false);
		const float h = ENTITY::GET_ENTITY_HEADING(ped) * 3.14159265f / 180.0f;
		const Vec3 fwd{-std::sin(h), std::cos(h), 0}, right{std::cos(h), std::sin(h), 0}, up{0, 0, 1};
		const Vec3 spot = Vec3{p.x, p.y, p.z} + right * 0.28f + fwd * 0.27f;
		float floorZ = p.z - 1.0f;
		MISC::GET_GROUND_Z_FOR_3D_COORD(spot.x, spot.y, p.z + 1.0f, &floorZ, false, false);

		// The hand from the skeleton: palm surface and normal, fingers.
		Hand rh;
		if (!rh.Build(sk, true))
		{
			ml::LogError("dribble: no hand bones");
			return;
		}
		ml::Log("dribble: palm surface {:.3f},{:.3f},{:.3f} normal {:.2f},{:.2f},{:.2f} fingers {:.2f},{:.2f},{:.2f} (hand space)", rh.PalmSurface().x,
		    rh.PalmSurface().y, rh.PalmSurface().z, rh.Normal().x, rh.Normal().y, rh.Normal().z, rh.FingerDir().x, rh.FingerDir().y, rh.FingerDir().z);
		// A dribbling stance: knees bent, hips down, chest over the ball (a standing arm does not reach a waist-high ball).
		Body body;
		if (!body.Build(sk))
			return;
		if (full)
		{
			// The whole body from the right side.
			const Vec3 at{p.x, p.y, floorZ + 0.9f}, from = at + right * 3.2f;
			Cam cam = CAMERA::CREATE_CAM_WITH_PARAMS("DEFAULT_SCRIPTED_CAMERA", from.x, from.y, from.z, 0, 0, 0, 40.0f, false, 2);
			CAMERA::POINT_CAM_AT_COORD(cam, at.x, at.y, at.z);
			CAMERA::SET_CAM_ACTIVE(cam, true);
			CAMERA::RENDER_SCRIPT_CAMS(true, false, 0, true, false, 0);
			g_cam = cam;
		}
		else if (close)
		{
			// Close-up from the right side, at the hand.
			const Vec3 at{spot.x, spot.y, floorZ + 0.92f}; // where the hand meets the ball
			const Vec3 from = at + right * 0.9f + fwd * 0.1f;
			Cam cam = CAMERA::CREATE_CAM_WITH_PARAMS("DEFAULT_SCRIPTED_CAMERA", from.x, from.y, from.z, 0, 0, 0, 28.0f, false, 2);
			CAMERA::POINT_CAM_AT_COORD(cam, at.x, at.y, at.z);
			CAMERA::SET_CAM_ACTIVE(cam, true);
			CAMERA::RENDER_SCRIPT_CAMS(true, false, 0, true, false, 0);
			g_cam = cam;
		}

		if (slow)
			MISC::SET_TIME_SCALE(0.2f); // slow motion, to see the contact in screenshots
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
		log << "t,phase,ball_z,palm_target_z,hand_shown_z,impact,held_shown,fingertips,palm,angle,wrist_err,rot_err,reach\n";
		Vec3 shownBall = ball.pos; // the ball where it was drawn last frame (the shown pose is last frame's)
		bool shownHeld = true;
		Vec3 shownWrist = sk.World(hand).pos; // the targets of the pose shown last frame
		Quat shownRot = sk.World(hand).rot;
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
					// Leave at H going down; steer the bounce back towards the dribble spot (a player corrects the drift with
					// each push): the horizontal speed that covers the offset by the next catch.
					ball.pos.z = H;
					const float flight = (vi - v0) / kGravity + (vb - vu) / kGravity; // down to the floor, then up to H
					const Vec3 offset{spot.x - ball.pos.x, spot.y - ball.pos.y, 0};
					ball.vel = Vec3{0, 0, -v0} + offset * (0.8f / flight);
					ball.spin = Vec3{};
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
			// Contact as shown last frame (Liu & Hodgins 2018, eq. 7: fingertip proxies; plus the palm and its normal).
			const Hand::Contact contact = rh.Measure(sk, shownBall, kBallRadius);
			// The palm surface rests on top of the ball while it is held, otherwise waits just above the catch height.
			const float palmZ = ball.held ? ball.pos.z + kBallRadius : std::fmax(ball.pos.z + kBallRadius, H + kBallRadius - 0.03f);
			const Vec3 palm{ball.pos.x, ball.pos.y, palmZ}; // straight above the ball centre
			const Quat handRot = rh.Orient(-up, fwd); // palm down, fingers forward
			const Vec3 wrist = rh.WristFor(palm, handRot);
			const Vec3 pole = sk.World(upper).pos + right * 0.5f - fwd * 0.4f - up * 0.4f;
			b::Pose pose(sk);
			body.Apply(pose, stance, fwd, up);
			const bool reach = pose.ReachTwoBone(upper, fore, hand, wrist, pole);
			pose.SetWorld(hand, handRot);
			// Fingers wrap the ball when it is in the hand or close to it, else open.
			const Vec3 ballHand = handRot.Conjugate().Rotate(ball.pos - wrist);
			if (ball.held || (ball.pos - palm).Length() < kBallRadius + 0.12f)
				rh.Wrap(pose, ballHand, kBallRadius);
			else
				rh.Open(pose);
			pose.Commit();
			if (markers)
			{
				// Verification: where this frame's palm surface (red) and ball top (green) are computed to be.
				const Vec3 palmNow = pose.WorldOf(hand).Apply(rh.PalmSurface()), ballTop = ball.pos + up * kBallRadius;
				GRAPHICS::DRAW_MARKER(28, palmNow.x, palmNow.y, palmNow.z, 0, 0, 0, 0, 0, 0, 0.012f, 0.012f, 0.012f, 255, 0, 0, 255, false, false, 2, false, nullptr, nullptr, false);
				GRAPHICS::DRAW_MARKER(28, ballTop.x, ballTop.y, ballTop.z, 0, 0, 0, 0, 0, 0, 0.012f, 0.012f, 0.012f, 0, 255, 0, 255, false, false, 2, false, nullptr, nullptr, false);
			}
			ENTITY::SET_ENTITY_COORDS_NO_OFFSET(obj, ball.pos.x, ball.pos.y, ball.pos.z, false, false, false);
			ENTITY::SET_ENTITY_QUATERNION(obj, ball.rot.x, ball.rot.y, ball.rot.z, ball.rot.w);
			log << std::format("{:.4f},{},{:.4f},{:.4f},{:.4f},{:.3f},{},{:.4f},{:.4f},{:.2f},{:.4f},{:.2f},{}\n", t, static_cast<int>(phase), ball.pos.z - floorZ, palmZ - floorZ,
			    sk.Shown(hand).pos.z - floorZ, impact.hit ? impact.depth : 0.0f, shownHeld ? 1 : 0, contact.fingertips, contact.palm, contact.angle,
			    (sk.Shown(hand).pos - shownWrist).Length(), shownRot.AngleTo(sk.Shown(hand).rot), reach ? 1 : 0);
			shownBall = ball.pos;
			shownHeld = ball.held;
			shownWrist = wrist;
			shownRot = handRot;
			ml::Wait(0);
		}
		b::Pose::Clear(ped);
		ENTITY::DELETE_ENTITY(&obj);
		if (slow)
			MISC::SET_TIME_SCALE(1.0f);
		if (g_cam)
		{
			CAMERA::RENDER_SCRIPT_CAMS(false, false, 0, true, false, 0);
			CAMERA::DESTROY_CAM(g_cam, false);
			g_cam = 0;
		}
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
		std::string cmd, arg, options;
		in >> cmd >> arg;
		std::getline(in, options);
		in.close();
		std::error_code ec;
		std::filesystem::remove("ModLoader/bball.txt", ec);
		// Options: close, full (cameras), slow, markers, crouch=<m>, lean=<degrees>.
		bball::Stance stance{0.14f, 18.0f};
		std::istringstream words(options);
		for (std::string w; words >> w;)
			if (w.starts_with("crouch="))
				stance.crouch = std::stof(w.substr(7));
			else if (w.starts_with("lean="))
				stance.lean = std::stof(w.substr(5));

		if (cmd == "skeleton")
		{
			// Research: the player's outfit (drawable, texture per component) and rest skeleton, for fitting body capsules.
			const Ped ped = PLAYER::PLAYER_PED_ID();
			std::ofstream out("ModLoader/bball_skeleton.txt");
			out << "model " << ENTITY::GET_ENTITY_MODEL(ped) << "\n";
			for (int c = 0; c < 12; ++c)
				out << std::format("component {} {} {}\n", c, PED::GET_PED_DRAWABLE_VARIATION(ped, c), PED::GET_PED_TEXTURE_VARIATION(ped, c));
			const ml::body::Skeleton sk(ped);
			for (int i = 0; i < sk.Count(); ++i)
			{
				const ml::body::Transform r = sk.Rest(i);
				out << std::format("bone {} {} {} {:.6f} {:.6f} {:.6f} {:.6f} {:.6f} {:.6f} {:.6f}\n", i, sk.Name(i), sk.Parent(i), r.rot.x, r.rot.y, r.rot.z, r.rot.w, r.pos.x,
				    r.pos.y, r.pos.z);
			}
			ml::Log("skeleton: {} bones written", sk.Count());
		}
		if (cmd == "dribble")
			bball::Dribble(arg.empty() ? 8.0f : std::stof(arg), options.find("close") != std::string::npos, options.find("slow") != std::string::npos, stance,
			    options.find("full") != std::string::npos, options.find("markers") != std::string::npos);
	}
}
