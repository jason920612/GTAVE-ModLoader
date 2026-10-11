// Animation lab (research): plays animations on the player for the custom-animation work (research/phase0.md §41).
// ModLoader\animlab.txt, one command per line, read every half second and removed:
//   play <dictionary> <clip> [loop]   plays a clip on the player (upper and lower body)
//   stop                              clears the player's tasks
#include <Windows.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

#include <modloader/body.hpp>
#include <modloader/game.hpp>

ML_MOD_INFO("Animation Lab", "0.1.0", "ModLoader", "Research: play animations on the player")

extern "C" __declspec(dllexport) int MLOnLoad(const MLApi* api, const MLContext* ctx)
{
	ml::Init(api, ctx);
	return 1;
}

extern "C" __declspec(dllexport) void MLMain()
{
	for (;;)
	{
		ml::Wait(500);
		std::ifstream in("ModLoader/animlab.txt");
		if (!in)
			continue;
		std::string line;
		std::vector<std::string> lines;
		while (std::getline(in, line))
			lines.push_back(line);
		in.close();
		std::error_code ec;
		std::filesystem::remove("ModLoader/animlab.txt", ec);
		for (const std::string& l : lines)
		{
			std::istringstream words(l);
			std::string cmd, dict, clip, flag;
			words >> cmd >> dict >> clip >> flag;
			const Ped ped = PLAYER::PLAYER_PED_ID();
			if (cmd == "stop")
				TASK::CLEAR_PED_TASKS_IMMEDIATELY(ped);
			else if (cmd == "bend")
			{
				// Research: bend <bone tag hex> <degrees> <local|object> — rotates one bone's matrix every frame for 8 s.
				// Entity: GTA5_Enhanced.exe+0x1494b0(handle); skeleton = *(entity+0xff8): +0 skeleton data (+0x18 u16 bones,
				// +0x20 bone records of 0x50, tag u16 at +0x44), +0x10 local matrices, +0x18 object matrices (0x40 each).
				const int tag = std::stoi(dict, nullptr, 16);
				const float angle = std::stof(clip) * 3.14159265f / 180.0f;
				const bool object = flag == "object";
				using GetEntity = uint8_t* (*)(int);
				const auto get = reinterpret_cast<GetEntity>(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + 0x1494b0);
				uint8_t* entity = get(ped);
				uint8_t* skeleton = entity ? *reinterpret_cast<uint8_t**>(entity + 0xff8) : nullptr;
				if (!skeleton)
				{
					ml::LogError("animlab: no skeleton");
					continue;
				}
				uint8_t* data = *reinterpret_cast<uint8_t**>(skeleton);
				const int count = *reinterpret_cast<uint16_t*>(data + 0x18);
				uint8_t* bones = *reinterpret_cast<uint8_t**>(data + 0x20);
				int bone = -1;
				for (int i = 0; i < count; ++i)
					if (*reinterpret_cast<uint16_t*>(bones + 0x50 * i + 0x44) == tag)
						bone = i;
				ml::Log("animlab: bend tag {:04x} -> index {} of {} ({})", tag, bone, count, *reinterpret_cast<const char**>(bones + 0x50 * (bone < 0 ? 0 : bone) + 0x38));
				if (bone < 0)
					continue;
				const uint64_t end = ml::TickMs() + 8000;
				while (ml::TickMs() < end)
				{
					float* m = *reinterpret_cast<float**>(skeleton + (object ? 0x18 : 0x10)) + 16 * bone;
					const float c = std::cos(angle), s = std::sin(angle);
					for (int row = 0; row < 3; ++row) // rotate the basis about the bone's local z
					{
						const float x = m[row * 4], y = m[row * 4 + 1];
						m[row * 4] = c * x - s * y;
						m[row * 4 + 1] = s * x + c * y;
					}
					ml::Wait(0);
				}
				ml::Log("animlab: bend done");
			}
			else if (cmd == "hand")
			{
				// Research: the right hand's bones (children of SKEL_R_Hand, recursively) with world positions.
				namespace b = ml::body;
				const b::Skeleton sk(ped);
				const int hand = sk.Find(static_cast<uint16_t>(0xDEAD));
				for (int i = 0; i < sk.Count(); ++i)
				{
					int a = i;
					while (a >= 0 && a != hand)
						a = sk.Parent(a);
					if (a != hand)
						continue;
					const b::Vec3 w = sk.World(i).pos;
					ml::Log("animlab: hand bone #{} {} parent {} at {:.4f},{:.4f},{:.4f}", i, sk.Name(i), sk.Parent(i), w.x, w.y, w.z);
				}
			}
			else if (cmd == "bones")
			{
				// Research: our world transforms (pose API) against the game's GET_WORLD_POSITION_OF_ENTITY_BONE.
				namespace b = ml::body;
				const b::Skeleton sk(ped);
				for (const int tag : {0x0, 0xE0FD, 0x60F2, 0x9D4D, 0x6E5C, 0xDEAD, 0x796E, 0x3779})
				{
					const int i = sk.Find(static_cast<uint16_t>(tag));
					if (i < 0)
						continue;
					const b::Vec3 ours = sk.World(i).pos;
					const Vector3 game = ENTITY::GET_WORLD_POSITION_OF_ENTITY_BONE(ped, PED::GET_PED_BONE_INDEX(ped, tag));
					float m[16]{};
					ml::Api().PoseGetBone(ped, i, ML_SPACE_OBJECT, m);
					ml::Log("animlab: {:04x} {} #{} parent {} ours {:.3f},{:.3f},{:.3f} game {:.3f},{:.3f},{:.3f} object {:.3f},{:.3f},{:.3f}", tag, sk.Name(i), i,
					    sk.Parent(i), ours.x, ours.y, ours.z, game.x, game.y, game.z, m[12], m[13], m[14]);
				}
				const Vector3 p = ENTITY::GET_ENTITY_COORDS(ped, false);
				ml::Log("animlab: entity at {:.3f},{:.3f},{:.3f} heading {:.1f}", p.x, p.y, p.z, ENTITY::GET_ENTITY_HEADING(ped));
			}
			else if (cmd == "ik")
			{
				// Research: ik <seconds> — the right hand follows a point circling in front of the chest (two-bone IK through
				// the loader's pose API). Draws the target (red) and the arm as the game shows it (green); logs the error.
				namespace b = ml::body;
				const float seconds = dict.empty() ? 8.0f : std::stof(dict);
				const b::Skeleton sk(ped);
				const int hand = sk.Find(static_cast<uint16_t>(0xDEAD)), fore = sk.Parent(hand), upper = sk.Parent(fore);
				ml::Log("animlab: ik bones hand {} ({}) fore {} ({}) upper {} ({}) of {}", hand, sk.Name(hand), fore, sk.Name(fore), upper, sk.Name(upper),
				    sk.Count());
				if (hand < 0 || fore < 0 || upper < 0)
					continue;
				const uint64_t start = ml::TickMs(), end = start + static_cast<uint64_t>(seconds * 1000);
				uint64_t nextLog = start;
				while (ml::TickMs() < end)
				{
					const float t = static_cast<float>(ml::TickMs() - start) / 1000.0f;
					const b::Transform root = sk.World(0);
					// In the ped's frame: x right, y forward, z up (root bone axes vary; use the entity's).
					const float h = ENTITY::GET_ENTITY_HEADING(ped) * 3.14159265f / 180.0f;
					const b::Vec3 fwd{-std::sin(h), std::cos(h), 0}, right{std::cos(h), std::sin(h), 0}, up{0, 0, 1};
					const Vector3 p = ENTITY::GET_ENTITY_COORDS(ped, false);
					const b::Vec3 chest = b::Vec3{p.x, p.y, p.z} + up * 0.35f;
					const b::Vec3 target = chest + fwd * 0.45f + right * (0.15f + 0.15f * std::cos(t * 2)) + up * (0.15f * std::sin(t * 2));
					const b::Vec3 pole = sk.World(fore).pos + right * 0.3f - up * 0.5f;
					b::Pose pose(sk);
					const b::Vec3 before = pose.WorldOf(hand).pos;
					pose.ReachTwoBone(upper, fore, hand, target, pole);
					const b::Vec3 predicted = pose.WorldOf(hand).pos;
					pose.Commit();
					if (ml::TickMs() >= nextLog)
						ml::Log("animlab: ik target {:.3f},{:.3f},{:.3f} before {:.3f},{:.3f},{:.3f} predicted {:.3f},{:.3f},{:.3f} (miss {:.3f})", target.x, target.y,
						    target.z, before.x, before.y, before.z, predicted.x, predicted.y, predicted.z, (predicted - target).Length());
					// What the game shows (last frame's pose, overrides applied).
					const b::Vec3 sa = sk.Shown(upper).pos, sb = sk.Shown(fore).pos, sc = sk.Shown(hand).pos;
					GRAPHICS::DRAW_LINE(sa.x, sa.y, sa.z, sb.x, sb.y, sb.z, 0, 255, 0, 255);
					GRAPHICS::DRAW_LINE(sb.x, sb.y, sb.z, sc.x, sc.y, sc.z, 0, 255, 0, 255);
					for (const b::Vec3 axis : {b::Vec3{0.05f, 0, 0}, b::Vec3{0, 0.05f, 0}, b::Vec3{0, 0, 0.05f}})
						GRAPHICS::DRAW_LINE(target.x - axis.x, target.y - axis.y, target.z - axis.z, target.x + axis.x, target.y + axis.y, target.z + axis.z, 255, 0, 0, 255);
					if (ml::TickMs() >= nextLog)
					{
						nextLog += 500;
						ml::Log("animlab: ik t {:.1f} hand-target {:.3f} m (root {:.2f},{:.2f},{:.2f})", t, (sc - target).Length(), root.pos.x, root.pos.y, root.pos.z);
					}
					ml::Wait(0);
				}
				b::Pose::Clear(ped);
				ml::Log("animlab: ik done");
			}
			else if (cmd == "cam")
			{
				// Research: cam <side|front|off> — a fixed scripted camera 2.5 m from the player, at chest height.
				static Cam cam = 0;
				if (cam)
				{
					CAMERA::RENDER_SCRIPT_CAMS(false, false, 0, true, false, 0);
					CAMERA::DESTROY_CAM(cam, false);
					cam = 0;
				}
				if (dict != "off")
				{
					const Vector3 p = ENTITY::GET_ENTITY_COORDS(ped, false);
					const float h = ENTITY::GET_ENTITY_HEADING(ped) * 3.14159265f / 180.0f;
					// Ped forward = (-sin h, cos h); right = (cos h, sin h).
					const float fx = -std::sin(h), fy = std::cos(h), rx = std::cos(h), ry = std::sin(h);
					const bool side = dict == "side";
					const float cx = p.x + (side ? rx : fx) * 2.5f, cy = p.y + (side ? ry : fy) * 2.5f;
					cam = CAMERA::CREATE_CAM_WITH_PARAMS("DEFAULT_SCRIPTED_CAMERA", cx, cy, p.z + 0.2f, 0, 0, 0, 50.0f, false, 2);
					CAMERA::POINT_CAM_AT_COORD(cam, p.x, p.y, p.z + 0.1f);
					CAMERA::SET_CAM_ACTIVE(cam, true);
					CAMERA::RENDER_SCRIPT_CAMS(true, false, 0, true, false, 0);
				}
			}
			else if (cmd == "ent")
			{
				using GetEntity = uint8_t* (*)(int);
				const auto get = reinterpret_cast<GetEntity>(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + 0x1494b0);
				uint8_t* entity = get(ped);
				uint8_t* skeleton = entity ? *reinterpret_cast<uint8_t**>(entity + 0xff8) : nullptr;
				ml::Log("animlab: entity {} skeleton {} local {} object {}", static_cast<void*>(entity), static_cast<void*>(skeleton),
				    skeleton ? *reinterpret_cast<void**>(skeleton + 0x10) : nullptr, skeleton ? *reinterpret_cast<void**>(skeleton + 0x18) : nullptr);
			}
			else if (cmd == "where")
			{
				// Research: the player's position as raw float bytes (to find the ped in memory) and some bones.
				const Vector3 p = ENTITY::GET_ENTITY_COORDS(ped, false);
				const auto hex = [](float f) {
					uint32_t u;
					std::memcpy(&u, &f, 4);
					return std::format("{:02x}{:02x}{:02x}{:02x}", u & 0xFF, u >> 8 & 0xFF, u >> 16 & 0xFF, u >> 24);
				};
				ml::Log("animlab: player {} {} {} heading {} bytes {}{}{}", p.x, p.y, p.z, ENTITY::GET_ENTITY_HEADING(ped), hex(p.x), hex(p.y), hex(p.z));
				for (int tag : {0, 0xDEAD, 0x188E, 0x9995, 0x60F2, 0x796E, 0xE0FD, 0x3779, 0xCC4D, 0xB1C5, 0xEB95})
				{
					const int index = PED::GET_PED_BONE_INDEX(ped, tag);
					const Vector3 b = ENTITY::GET_WORLD_POSITION_OF_ENTITY_BONE(ped, index);
					ml::Log("animlab: bone tag {:04x} index {} at {:.4f} {:.4f} {:.4f} (rel {:.4f} {:.4f} {:.4f})", tag, index, b.x, b.y, b.z, b.x - p.x, b.y - p.y,
					    b.z - p.z);
				}
			}
			else if (cmd == "play")
			{
				const auto loaded = ml::LoadAnimDict(dict.c_str());
				if (!loaded)
				{
					ml::LogError("animlab: dictionary {} did not load", dict);
					continue;
				}
				const bool loop = flag == "loop";
				TASK::TASK_PLAY_ANIM(ped, dict.c_str(), clip.c_str(), 8.0f, -8.0f, -1, loop ? 1 : 0, 0.0f, false, false, false);
				ml::Log("animlab: playing {} / {}{}", dict, clip, loop ? " (loop)" : "");
				ml::Wait(1000);
			}
		}
	}
}
