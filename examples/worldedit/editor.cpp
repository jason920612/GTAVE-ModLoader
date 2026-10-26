#include "editor.hpp"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <modloader/json.hpp>
#include <modloader/natives.hpp>

#include "../../tools/worldgen/roadedit.hpp"
#include "../../tools/worldgen/roads.hpp"

namespace worldedit
{
	namespace
	{
		namespace re = worldgen::roadedit;
		using P2 = worldgen::world::P2;
		constexpr float kPi = 3.14159265f;

		// game controls (read while all are disabled)
		enum : int
		{
			kMoveForward = 32, kMoveBack = 33, kMoveLeft = 34, kMoveRight = 35, kTurnLeft = 44 /* Q */, kTurnRight = 38 /* E */,
			kTiltUp = 45 /* R */, kTiltDown = 23 /* F */, kSprint = 21 /* Shift */, kAccept = 237, kCancel = 238, kCursorX = 239,
			kCursorY = 240, kWheelUp = 241, kWheelDown = 242, kPause = 199, kPauseAlt = 200,
		};

		struct Config
		{
			std::wstring roads, edits, rebuild;
		} g_cfg;

		enum class Tool : uint8_t { Straight, Curve, Delete };

		bool g_active = false;
		re::Network g_net;
		bool g_loaded = false;
		Tool g_tool = Tool::Straight;
		size_t g_type = 0;
		std::vector<P2> g_stroke; // the points placed of the road being drawn: its start (and a curve's bend)
		re::Snap g_startSnap;
		struct Camera
		{
			P2 target{};
			float distance = 140, yaw = 0, pitch = 55; // metres; degrees (yaw as the game's heading: 0 north, + left)
			float z = 0;
			int handle = 0;
		} g_cam;
		std::optional<P2> g_hover; // a tester's cursor (instead of the mouse's)
		bool g_hit = false;
		P2 g_cursor{};
		float g_cursorZ = 0;
		std::string g_status;
		uint64_t g_statusUntil = 0;
		HANDLE g_rebuild = nullptr;
		uint64_t g_rebuildStart = 0;
		std::unordered_map<uint64_t, float> g_ground; // ground heights on a 2 m lattice
		// the preview: drawn from the last stroke it was made for
		struct Preview
		{
			std::vector<std::array<worldgen::roads::P3, 3>> asphalt, sidewalk, markings, gone;
			std::string key;
		} g_preview;

		std::wstring Widen(const std::string& s)
		{
			if (s.empty())
				return {};
			const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
			std::wstring w(static_cast<size_t>(n), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
			return w;
		}

		void Say(std::string text, uint32_t ms = 4000)
		{
			ml::Log("{}", text);
			g_status = std::move(text);
			g_statusUntil = ml::TickMs() + ms;
		}

		void LoadConfig()
		{
			const wchar_t* path = ml::Context().configPath;
			std::ifstream in(path ? std::filesystem::path(path) : std::filesystem::path(), std::ios::binary);
			if (!in)
				return;
			const std::string text((std::istreambuf_iterator<char>(in)), {});
			const ml::Json j = ml::Json::Parse(text);
			g_cfg.roads = Widen(j["roads"].Str(""));
			g_cfg.edits = Widen(j["edits"].Str(""));
			g_cfg.rebuild = Widen(j["rebuild"].Str(""));
		}

		bool Focused()
		{
			DWORD pid = 0;
			GetWindowThreadProcessId(GetForegroundWindow(), &pid);
			return pid == GetCurrentProcessId();
		}

		bool KeyHit(int vk)
		{
			return Focused() && (GetAsyncKeyState(vk) & 1) != 0;
		}

		float Ground(float x, float y)
		{
			const int i = static_cast<int>(std::floor(x / 2)), j = static_cast<int>(std::floor(y / 2));
			const uint64_t key = static_cast<uint64_t>(static_cast<uint32_t>(i)) << 32 | static_cast<uint32_t>(j);
			if (const auto it = g_ground.find(key); it != g_ground.end())
				return it->second;
			float z = 0;
			if (!MISC::GET_GROUND_Z_FOR_3D_COORD(i * 2.0f + 1, j * 2.0f + 1, 1000.0f, &z, FALSE, FALSE))
				return g_cam.z; // (not loaded yet: not kept)
			if (g_ground.size() > 200000)
				g_ground.clear();
			g_ground[key] = z;
			return z;
		}

		// ---- the camera ------------------------------------------------------------------------

		void PlaceCamera()
		{
			const float yaw = g_cam.yaw * kPi / 180, pitch = g_cam.pitch * kPi / 180;
			const P2 forward{-std::sin(yaw), std::cos(yaw)};
			g_cam.z = Ground(g_cam.target[0], g_cam.target[1]);
			const float x = g_cam.target[0] - forward[0] * g_cam.distance * std::cos(pitch), y = g_cam.target[1] - forward[1] * g_cam.distance * std::cos(pitch);
			const float z = g_cam.z + g_cam.distance * std::sin(pitch);
			if (!g_cam.handle)
				g_cam.handle = CAMERA::CREATE_CAM_WITH_PARAMS("DEFAULT_SCRIPTED_CAMERA", x, y, z, -g_cam.pitch, 0, g_cam.yaw, 50.0f, FALSE, 2);
			CAMERA::SET_CAM_COORD(g_cam.handle, x, y, z);
			CAMERA::SET_CAM_ROT(g_cam.handle, -g_cam.pitch, 0, g_cam.yaw, 2);
			CAMERA::SET_CAM_ACTIVE(g_cam.handle, TRUE);
			CAMERA::RENDER_SCRIPT_CAMS(TRUE, FALSE, 0, TRUE, FALSE, 0);
			STREAMING::SET_FOCUS_POS_AND_VEL(g_cam.target[0], g_cam.target[1], g_cam.z, 0, 0, 0);
		}

		void Enter()
		{
			if (g_active)
				return;
			if (!g_loaded)
			{
				std::string error;
				if (g_cfg.roads.empty() || g_cfg.edits.empty())
				{
					Say("世界編輯器：config.json 沒有設定 roads / edits");
					return;
				}
				if (!g_net.Load(g_cfg.roads, g_cfg.edits, error))
				{
					Say("世界編輯器：" + error);
					return;
				}
				g_loaded = true;
				ml::Log("{} roads, {} edits", g_net.Roads().size(), g_net.Edits());
			}
			const Ped me = PLAYER::PLAYER_PED_ID();
			const Vector3 p = ENTITY::GET_ENTITY_COORDS(me, TRUE);
			g_cam.target = {p.x, p.y};
			ENTITY::FREEZE_ENTITY_POSITION(me, TRUE);
			g_active = true;
			g_stroke.clear();
			PlaceCamera();
			Say("世界編輯器：左鍵放道路，右鍵結束，1-6 道路種類，Ctrl+Z 復原", 6000);
		}

		void Leave()
		{
			if (!g_active)
				return;
			g_active = false;
			CAMERA::RENDER_SCRIPT_CAMS(FALSE, FALSE, 0, TRUE, FALSE, 0);
			if (g_cam.handle)
				CAMERA::DESTROY_CAM(g_cam.handle, FALSE);
			g_cam.handle = 0;
			STREAMING::CLEAR_FOCUS();
			ENTITY::FREEZE_ENTITY_POSITION(PLAYER::PLAYER_PED_ID(), FALSE);
		}

		// The cursor's ray onto the world (collision; else the ground's height under the target).
		void Pick()
		{
			if (g_hover)
			{
				g_hit = true;
				g_cursor = *g_hover;
				g_cursorZ = Ground(g_cursor[0], g_cursor[1]);
				return;
			}
			const float cx = PAD::GET_DISABLED_CONTROL_NORMAL(0, kCursorX), cy = PAD::GET_DISABLED_CONTROL_NORMAL(0, kCursorY);
			const float yaw = g_cam.yaw * kPi / 180, pitch = -g_cam.pitch * kPi / 180;
			const float fx = -std::sin(yaw) * std::cos(pitch), fy = std::cos(yaw) * std::cos(pitch), fz = std::sin(pitch);
			const float rx = std::cos(yaw), ry = std::sin(yaw), rz = 0;
			// up = right x forward
			const float ux = ry * fz - rz * fy, uy = rz * fx - rx * fz, uz = rx * fy - ry * fx;
			const float t = std::tan(50.0f * kPi / 360), aspect = GRAPHICS::GET_ASPECT_RATIO(FALSE);
			const float sx = (cx * 2 - 1) * t * aspect, sy = (1 - cy * 2) * t;
			const float dx = fx + rx * sx + ux * sy, dy = fy + ry * sx + uy * sy, dz = fz + rz * sx + uz * sy;
			const Vector3 from = CAMERA::GET_CAM_COORD(g_cam.handle);
			const float reach = 4000;
			const int probe = SHAPETEST::START_EXPENSIVE_SYNCHRONOUS_SHAPE_TEST_LOS_PROBE(from.x, from.y, from.z, from.x + dx * reach, from.y + dy * reach,
			    from.z + dz * reach, 1, 0, 7);
			BOOL hit = FALSE;
			Vector3 end{}, normal{};
			Entity entity = 0;
			SHAPETEST::GET_SHAPE_TEST_RESULT(probe, &hit, &end, &normal, &entity);
			if (hit)
			{
				g_hit = true, g_cursor = {end.x, end.y}, g_cursorZ = end.z;
				return;
			}
			// (the plane at the target's height)
			if (dz < -1e-3f)
			{
				const float s = (g_cam.z - from.z) / dz;
				g_hit = true, g_cursor = {from.x + dx * s, from.y + dy * s}, g_cursorZ = g_cam.z;
				return;
			}
			g_hit = false;
		}

		float SnapRadius() { return std::max(4.0f, g_cam.distance * 0.035f); }

		// The point the next click would put down: snapped to a road, or (Shift) its direction from the last point in
		// steps of 15 degrees.
		P2 Target(re::Snap& snap)
		{
			snap = g_net.SnapTo(g_cursor, SnapRadius());
			if (snap.kind != re::Snap::Kind::None)
				return snap.at;
			P2 p = g_cursor;
			if (!g_stroke.empty() && PAD::IS_DISABLED_CONTROL_PRESSED(0, kSprint))
			{
				const P2& a = g_stroke.back();
				const float dx = p[0] - a[0], dy = p[1] - a[1], len = std::hypot(dx, dy);
				const float step = 15 * kPi / 180, angle = std::round(std::atan2(dy, dx) / step) * step;
				p = {a[0] + std::cos(angle) * len, a[1] + std::sin(angle) * len};
			}
			return p;
		}

		// The stroke as it would be put down now (the cursor its last point).
		std::vector<P2> Stroke(const P2& end)
		{
			if (g_stroke.empty())
				return {};
			if (g_tool == Tool::Curve && g_stroke.size() == 2)
				return re::Network::Curve(g_stroke[0], g_stroke[1], end);
			return {g_stroke.front(), end};
		}

		// ---- the preview: the change's finished surfaces ----------------------------------------

		void MakePreview(const re::Change& change, const std::string& key)
		{
			if (key == g_preview.key)
				return;
			g_preview = {};
			g_preview.key = key;
			if (change.Empty())
				return;
			float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
			const auto grow = [&](const P2& p) { x0 = std::min(x0, p[0]), y0 = std::min(y0, p[1]), x1 = std::max(x1, p[0]), y1 = std::max(y1, p[1]); };
			for (const auto& r : change.roads)
				for (const P2& p : r.points)
					grow(p);
			for (const int64_t id : change.removed)
				if (const auto* r = g_net.Find(id))
					for (const P2& p : r->points)
						grow(p);
			if (x0 > x1)
				return;
			// the area's roads as they are and as they would be: the surfaces that differ are the change's - the new
			// road, the junctions it makes or alters (blue) - and what it takes away (red)
			worldgen::world::World before, after;
			std::unordered_map<int64_t, int> inChange;
			for (const auto& r : change.roads)
				inChange[r.id] = 1, after.roads.push_back(r);
			for (const int64_t id : change.removed)
				inChange[id] = 1;
			for (const size_t r : g_net.Near(x0 - 40, y0 - 40, x1 + 40, y1 + 40))
			{
				const auto& road = g_net.Roads()[r];
				before.roads.push_back(road);
				if (!inChange.contains(road.id))
					after.roads.push_back(road);
			}
			const auto surfaces = [](const worldgen::world::World& w) {
				auto network = worldgen::roads::Build(w);
				worldgen::roads::Heights(network, [](float x, float y) { return Ground(x, y); });
				worldgen::roads::Surfaces s;
				worldgen::roads::Geometry(network, s);
				return s;
			};
			const worldgen::roads::Surfaces was = surfaces(before), will = surfaces(after);
			// (a triangle by its corners to the centimetre, in any order)
			const auto triangleKey = [](const worldgen::roads::P3& a, const worldgen::roads::P3& b, const worldgen::roads::P3& c) {
				std::array<uint64_t, 3> k;
				const std::array<const worldgen::roads::P3*, 3> v{&a, &b, &c};
				for (int i = 0; i < 3; ++i)
					k[i] = static_cast<uint64_t>(static_cast<uint32_t>(std::lround((*v[i])[0] * 100))) << 32 | static_cast<uint32_t>(std::lround((*v[i])[1] * 100));
				std::sort(k.begin(), k.end());
				return k[0] * 0x9E3779B97F4A7C15ull ^ k[1] * 0xC2B2AE3D27D4EB4Full ^ k[2];
			};
			const auto keys = [&](const worldgen::roads::Mesh& m) {
				std::unordered_map<uint64_t, int> set;
				for (size_t t = 0; t + 2 < m.indices.size(); t += 3)
					set[triangleKey(m.vertices[m.indices[t]].position, m.vertices[m.indices[t + 1]].position, m.vertices[m.indices[t + 2]].position)] = 1;
				return set;
			};
			const auto differ = [&](const worldgen::roads::Mesh& m, const std::unordered_map<uint64_t, int>& other, auto& out) {
				for (size_t t = 0; t + 2 < m.indices.size(); t += 3)
				{
					const auto& a = m.vertices[m.indices[t]].position;
					const auto& b = m.vertices[m.indices[t + 1]].position;
					const auto& c = m.vertices[m.indices[t + 2]].position;
					const float mx = (a[0] + b[0] + c[0]) / 3, my = (a[1] + b[1] + c[1]) / 3;
					if (mx < x0 - 30 || mx > x1 + 30 || my < y0 - 30 || my > y1 + 30 || other.contains(triangleKey(a, b, c)))
						continue;
					out.push_back({a, b, c});
				}
			};
			differ(will.asphalt, keys(was.asphalt), g_preview.asphalt);
			differ(will.sidewalk, keys(was.sidewalk), g_preview.sidewalk);
			differ(will.markings, keys(was.markings), g_preview.markings);
			differ(was.asphalt, keys(will.asphalt), g_preview.gone);
			differ(was.sidewalk, keys(will.sidewalk), g_preview.gone);
		}

		void DrawPreview()
		{
			const auto draw = [](const auto& tris, int r, int g, int b, int a, float lift) {
				for (const auto& t : tris)
				{
					// (both faces: the game draws polygons one-sided)
					GRAPHICS::DRAW_POLY(t[0][0], t[0][1], t[0][2] + lift, t[1][0], t[1][1], t[1][2] + lift, t[2][0], t[2][1], t[2][2] + lift, r, g, b, a);
					GRAPHICS::DRAW_POLY(t[0][0], t[0][1], t[0][2] + lift, t[2][0], t[2][1], t[2][2] + lift, t[1][0], t[1][1], t[1][2] + lift, r, g, b, a);
				}
			};
			draw(g_preview.gone, 255, 60, 40, 110, 0.10f);
			draw(g_preview.asphalt, 60, 130, 255, 150, 0.12f);
			draw(g_preview.sidewalk, 160, 205, 255, 140, 0.12f);
			draw(g_preview.markings, 255, 255, 255, 210, 0.16f);
		}

		// ---- the tool bar ----------------------------------------------------------------------

		struct Button
		{
			float x, y, w, h;
			std::string label;
			bool on;
			int action; // 0..2 tools, 10.. types, 20 undo, 21 apply, 22 leave
		};

		std::vector<Button> Buttons()
		{
			std::vector<Button> b;
			float x = 0.05f;
			const float y = 0.95f, h = 0.045f;
			const auto add = [&](const std::string& label, float w, bool on, int action) {
				b.push_back({x + w / 2, y, w, h, label, on, action});
				x += w + 0.004f;
			};
			add("直線", 0.05f, g_tool == Tool::Straight, 0);
			add("曲線", 0.05f, g_tool == Tool::Curve, 1);
			add("刪除", 0.05f, g_tool == Tool::Delete, 2);
			x += 0.015f;
			const auto& types = re::Types();
			for (size_t i = 0; i < types.size(); ++i)
				add(std::format("{} {}", i + 1, types[i].label), 0.095f, g_type == i, static_cast<int>(10 + i));
			x += 0.015f;
			add("復原", 0.045f, false, 20);
			add(g_rebuild ? "重建中…" : "套用", 0.055f, false, 21);
			add("離開", 0.045f, false, 22);
			return b;
		}

		void Text(const std::string& text, float x, float y, float scale, bool centre, int r = 255, int g = 255, int b = 255)
		{
			HUD::SET_TEXT_FONT(0);
			HUD::SET_TEXT_SCALE(0.0f, scale);
			HUD::SET_TEXT_COLOUR(r, g, b, 255);
			HUD::SET_TEXT_OUTLINE();
			HUD::SET_TEXT_CENTRE(centre);
			HUD::BEGIN_TEXT_COMMAND_DISPLAY_TEXT("STRING");
			HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME(text.c_str());
			HUD::END_TEXT_COMMAND_DISPLAY_TEXT(x, y, 0);
		}

		void Apply();

		// true when the click was the tool bar's
		bool Toolbar(bool clicked, float cx, float cy)
		{
			bool used = false;
			for (const Button& b : Buttons())
			{
				const bool over = std::fabs(cx - b.x) <= b.w / 2 && std::fabs(cy - b.y) <= b.h / 2;
				GRAPHICS::DRAW_RECT(b.x, b.y, b.w, b.h, b.on ? 40 : 15, b.on ? 110 : 20, b.on ? 210 : 30, over ? 235 : 200, FALSE);
				Text(b.label, b.x, b.y - 0.013f, 0.3f, true);
				if (!clicked || !over)
					continue;
				used = true;
				if (b.action <= 2)
					g_tool = static_cast<Tool>(b.action), g_stroke.clear();
				else if (b.action >= 10 && b.action < 20)
					g_type = static_cast<size_t>(b.action - 10);
				else if (b.action == 20)
					Say(g_net.Undo() ? "已復原" : "沒有可以復原的");
				else if (b.action == 21)
					Apply();
				else if (b.action == 22)
					Leave();
			}
			return used;
		}

		void Save()
		{
			std::string error;
			if (!g_net.SaveEdits(g_cfg.edits, error))
				Say("存檔失敗：" + error);
		}

		// ---- clicks ----------------------------------------------------------------------------

		void Click()
		{
			re::Snap snap;
			const P2 at = Target(snap);
			if (g_tool == Tool::Delete)
			{
				const re::Change c = g_net.PlanDelete(g_cursor, SnapRadius());
				if (c.Empty())
					return Say("這裡沒有道路");
				g_net.Commit(c);
				Save();
				g_preview.key.clear();
				return Say("已刪除一段道路");
			}
			if (g_stroke.empty())
			{
				g_stroke = {at};
				g_startSnap = snap;
				return;
			}
			if (g_tool == Tool::Curve && g_stroke.size() == 1)
			{
				g_stroke.push_back(at); // (the bend)
				return;
			}
			const re::Change c = g_net.Plan(Stroke(at), re::Types()[g_type], g_startSnap, snap);
			if (c.Empty())
				return;
			g_net.Commit(c);
			Save();
			g_preview.key.clear();
			// (drawing on from the end)
			g_stroke = {c.roads.front().points.back()};
			g_startSnap = g_net.SnapTo(g_stroke.front(), 0.05f);
			Say(std::format("已放下{}（{} 條道路改變）", re::Types()[g_type].label, c.roads.size()), 2500);
		}

		void Apply()
		{
			if (g_rebuild)
				return Say("還在重建");
			if (g_cfg.rebuild.empty())
				return Say("config.json 沒有設定 rebuild");
			Save();
			std::wstring line = L"cmd.exe /c \"\"" + g_cfg.rebuild + L"\"\"";
			STARTUPINFOW si{sizeof(si)};
			PROCESS_INFORMATION pi{};
			if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi))
				return Say(std::format("無法執行重建（錯誤 {}）", GetLastError()));
			CloseHandle(pi.hThread);
			g_rebuild = pi.hProcess;
			g_rebuildStart = ml::TickMs();
			Say("重建中：世界在背景重新編譯", 8000);
		}

		void WatchRebuild()
		{
			if (!g_rebuild || WaitForSingleObject(g_rebuild, 0) != WAIT_OBJECT_0)
				return;
			DWORD code = 1;
			GetExitCodeProcess(g_rebuild, &code);
			CloseHandle(g_rebuild);
			g_rebuild = nullptr;
			Say(code == 0 ? std::format("重建完成（{} 秒）：下次開遊戲生效", (ml::TickMs() - g_rebuildStart) / 1000) : std::format("重建失敗（{}），看 rebuild.log", code),
			    10000);
		}

		void Controls()
		{
			PAD::DISABLE_ALL_CONTROL_ACTIONS(0);
			HUD::HIDE_HUD_AND_RADAR_THIS_FRAME();
			HUD::SET_MOUSE_CURSOR_THIS_FRAME();
			const float step = g_cam.distance * 0.02f * (PAD::IS_DISABLED_CONTROL_PRESSED(0, kSprint) ? 3.0f : 1.0f);
			const float yaw = g_cam.yaw * kPi / 180;
			const P2 forward{-std::sin(yaw), std::cos(yaw)}, right{std::cos(yaw), std::sin(yaw)};
			const auto move = [&](const P2& d, float s) { g_cam.target[0] += d[0] * s, g_cam.target[1] += d[1] * s; };
			if (PAD::IS_DISABLED_CONTROL_PRESSED(0, kMoveForward))
				move(forward, step);
			if (PAD::IS_DISABLED_CONTROL_PRESSED(0, kMoveBack))
				move(forward, -step);
			if (PAD::IS_DISABLED_CONTROL_PRESSED(0, kMoveRight))
				move(right, step);
			if (PAD::IS_DISABLED_CONTROL_PRESSED(0, kMoveLeft))
				move(right, -step);
			if (PAD::IS_DISABLED_CONTROL_PRESSED(0, kTurnLeft))
				g_cam.yaw += 1.5f;
			if (PAD::IS_DISABLED_CONTROL_PRESSED(0, kTurnRight))
				g_cam.yaw -= 1.5f;
			if (PAD::IS_DISABLED_CONTROL_PRESSED(0, kTiltUp))
				g_cam.pitch = std::min(89.0f, g_cam.pitch + 0.8f);
			if (PAD::IS_DISABLED_CONTROL_PRESSED(0, kTiltDown))
				g_cam.pitch = std::max(15.0f, g_cam.pitch - 0.8f);
			if (PAD::IS_DISABLED_CONTROL_JUST_PRESSED(0, kWheelUp))
				g_cam.distance = std::max(15.0f, g_cam.distance * 0.85f);
			if (PAD::IS_DISABLED_CONTROL_JUST_PRESSED(0, kWheelDown))
				g_cam.distance = std::min(2500.0f, g_cam.distance / 0.85f);
			for (int k = 0; k < static_cast<int>(re::Types().size()); ++k)
				if (KeyHit('1' + k))
					g_type = static_cast<size_t>(k);
			if (Focused() && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && KeyHit('Z'))
				Say(g_net.Undo() ? "已復原" : "沒有可以復原的"), g_preview.key.clear(), Save();
			if (PAD::IS_DISABLED_CONTROL_JUST_PRESSED(0, kPause) || PAD::IS_DISABLED_CONTROL_JUST_PRESSED(0, kPauseAlt))
			{
				if (!g_stroke.empty())
					g_stroke.clear();
				else
					Leave();
			}
		}
	}

	void Init()
	{
		LoadConfig();
		ml::Hotkey("editor", "世界編輯器", VK_F7, [] { g_active ? Leave() : Enter(); });
		// (for a tester or a page: the same actions with world coordinates instead of the mouse)
		ml::web::Function("worldedit.enter", [](const std::string&) {
			Enter();
			return std::string(g_active ? "true" : "false");
		});
		ml::web::Function("worldedit.exit", [](const std::string&) {
			Leave();
			return std::string("true");
		});
		ml::web::Function("worldedit.camera", [](const std::string& args) {
			// [x, y, distance, yaw, pitch]
			const ml::Json a = ml::Json::Parse(args);
			g_cam.target = {a[0].Float(g_cam.target[0]), a[1].Float(g_cam.target[1])};
			g_cam.distance = a[2].Float(g_cam.distance), g_cam.yaw = a[3].Float(g_cam.yaw), g_cam.pitch = a[4].Float(g_cam.pitch);
			return std::string("true");
		});
		ml::web::Function("worldedit.tool", [](const std::string& args) {
			// ["straight" | "curve" | "delete", type index]
			const ml::Json a = ml::Json::Parse(args);
			const std::string tool = a[0].Str("straight");
			g_tool = tool == "curve" ? Tool::Curve : tool == "delete" ? Tool::Delete : Tool::Straight;
			g_type = std::min<size_t>(static_cast<size_t>(a[1].Int(static_cast<int>(g_type))), re::Types().size() - 1);
			g_stroke.clear();
			return std::string("true");
		});
		ml::web::Function("worldedit.hover", [](const std::string& args) {
			// [x, y] the cursor there; [] the mouse's again
			const ml::Json a = ml::Json::Parse(args);
			g_hover = a.Size() >= 2 ? std::optional<P2>(P2{a[0].Float(), a[1].Float()}) : std::nullopt;
			return std::string("true");
		});
		ml::web::Function("worldedit.click", [](const std::string& args) {
			// [x, y]: a left click there
			const ml::Json a = ml::Json::Parse(args);
			g_hover = P2{a[0].Float(), a[1].Float()};
			g_cursor = *g_hover;
			Click();
			return std::to_string(g_net.Edits());
		});
		ml::web::Function("worldedit.cancel", [](const std::string&) {
			g_stroke.clear();
			return std::string("true");
		});
		ml::web::Function("worldedit.undo", [](const std::string&) {
			const bool ok = g_net.Undo();
			g_preview.key.clear();
			Save();
			return std::string(ok ? "true" : "false");
		});
		ml::web::Function("worldedit.apply", [](const std::string&) {
			Apply();
			return std::string("true");
		});
		ml::web::Function("worldedit.state", [](const std::string&) {
			ml::Json j = ml::Json::Object();
			j["active"] = g_active;
			j["loaded"] = g_loaded;
			j["roads"] = static_cast<int64_t>(g_net.Roads().size());
			j["edits"] = static_cast<int64_t>(g_net.Edits());
			j["tool"] = static_cast<int>(g_tool);
			j["type"] = static_cast<int64_t>(g_type);
			j["stroke"] = static_cast<int64_t>(g_stroke.size());
			j["rebuilding"] = g_rebuild != nullptr;
			j["status"] = g_status;
			j["preview"] = static_cast<int64_t>(g_preview.asphalt.size());
			return j.Dump();
		});
	}

	void Frame()
	{
		WatchRebuild();
		if (!g_active)
			return;
		Controls();
		if (!g_active)
			return;
		PlaceCamera();
		Pick();
		const float cx = PAD::GET_DISABLED_CONTROL_NORMAL(0, kCursorX), cy = PAD::GET_DISABLED_CONTROL_NORMAL(0, kCursorY);
		const bool clicked = PAD::IS_DISABLED_CONTROL_JUST_PRESSED(0, kAccept) != 0;
		const bool onBar = Toolbar(clicked, cx, cy);
		if (clicked && !onBar && g_hit)
			Click();
		if (PAD::IS_DISABLED_CONTROL_JUST_PRESSED(0, kCancel))
			g_stroke.clear();
		// what the next click would do
		re::Snap snap;
		const P2 at = g_hit ? Target(snap) : P2{};
		if (g_hit)
		{
			if (g_tool == Tool::Delete)
			{
				const re::Change c = g_net.PlanDelete(g_cursor, SnapRadius());
				MakePreview(c, c.removed.empty() && c.roads.empty() ? "" : std::format("d{:.0f},{:.0f}", g_cursor[0], g_cursor[1]));
			}
			else if (!g_stroke.empty())
			{
				const re::Change c = g_net.Plan(Stroke(at), re::Types()[g_type], g_startSnap, snap);
				MakePreview(c, std::format("s{}{}{:.1f},{:.1f},{}", static_cast<int>(g_tool), g_type, at[0], at[1], g_stroke.size()));
			}
			else
				MakePreview({}, "");
			// where the click lands (green: on a road)
			const float z = Ground(at[0], at[1]);
			const bool snapped = snap.kind != re::Snap::Kind::None;
			GRAPHICS::DRAW_MARKER(28, at[0], at[1], z + 0.3f, 0, 0, 0, 0, 0, 0, 1.2f, 1.2f, 1.2f, snapped ? 80 : 255, snapped ? 255 : 255, snapped ? 120 : 255, 180, FALSE,
			    FALSE, 2, FALSE, nullptr, nullptr, FALSE);
			for (size_t i = 0; i < g_stroke.size(); ++i)
				GRAPHICS::DRAW_MARKER(28, g_stroke[i][0], g_stroke[i][1], Ground(g_stroke[i][0], g_stroke[i][1]) + 0.3f, 0, 0, 0, 0, 0, 0, 1.0f, 1.0f, 1.0f, 255, 200, 60,
				    200, FALSE, FALSE, 2, FALSE, nullptr, nullptr, FALSE);
		}
		DrawPreview();
		// the info line
		std::string info = std::format("{}  {}", g_tool == Tool::Straight ? "直線" : g_tool == Tool::Curve ? "曲線" : "刪除", re::Types()[g_type].label);
		if (!g_stroke.empty() && g_hit)
		{
			float length = 0;
			const auto s = Stroke(at);
			for (size_t i = 0; i + 1 < s.size(); ++i)
				length += std::hypot(s[i + 1][0] - s[i][0], s[i + 1][1] - s[i][1]);
			info += std::format("  長 {:.0f} 公尺", length);
		}
		info += std::format("  （{} 處修改）", g_net.Edits());
		Text(info, 0.02f, 0.02f, 0.4f, false);
		Text("左鍵：放下  右鍵：結束  Shift：15° 對齊  1-6：種類  WASD/QE/RF/滾輪：視角  Ctrl+Z：復原  Esc：離開", 0.02f, 0.055f, 0.3f, false, 210, 210, 210);
		if (ml::TickMs() < g_statusUntil)
			Text(g_status, 0.5f, 0.86f, 0.42f, true, 255, 230, 120);
	}
}
