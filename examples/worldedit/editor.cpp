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

		// roads (straight, curved, deleted), buildings (put up, pulled down: rectangles), the ground's paint and the
		// terrain's shape (brushes, held down)
		enum class Tool : uint8_t { Straight, Curve, Delete, Build, Demolish, Paint, Terrain };
		bool RoadTool(Tool t) { return t <= Tool::Delete; }
		bool BrushTool(Tool t) { return t == Tool::Paint || t == Tool::Terrain; }

		struct Choice
		{
			const char* id;
			const char* label;
			int r, g, b;
		};
		const Choice kPaint[] = {{"park", "公園", 60, 170, 60}, {"land", "草地", 150, 200, 100}, {"beach", "沙灘", 235, 215, 150},
		    {"paved", "鋪面", 150, 150, 150}, {"urban", "建地", 205, 175, 145}, {"water", "水", 50, 120, 230}};
		const Choice kShape[] = {{"raise", "升高", 235, 140, 60}, {"lower", "降低", 90, 140, 235}, {"flatten", "整平", 225, 225, 225}};
		const Choice kKinds[] = {{"house", "住宅", 230, 200, 160}, {"apartments", "公寓", 220, 180, 200}, {"commercial", "商店", 180, 210, 230},
		    {"hotel", "旅館", 240, 230, 190}, {"office", "辦公", 170, 190, 210}};
		const Choice kClimates[] = {{"", "遊戲的天氣", 255, 255, 255}, {"tropical", "熱帶", 255, 255, 255}, {"dry", "乾燥", 255, 255, 255},
		    {"temperate", "溫帶", 255, 255, 255}};

		bool g_active = false;
		re::Network g_net;
		bool g_loaded = false;
		Tool g_tool = Tool::Straight;
		size_t g_type = 0, g_paint = 0, g_shape = 0, g_kind = 0;
		float g_brush = 12, g_height = 12; // m
		bool g_contours = false;
		// the points placed: a road's start (and a curve's bend), a building's corners, a rectangle's first corner, the
		// brush stroke being drawn
		std::vector<P2> g_stroke;
		bool g_painting = false; // (the button held down for a brush stroke that began off the tool bars)
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
		// the editor's other changes, beside the roads in edits.json ("buildings", "demolish", "paint", "terrain",
		// "climate"); the order of this session's changes for undo ("roads" or one of those keys), the climates replaced
		ml::Json g_extra = ml::Json::Object();
		std::vector<std::string> g_history;
		std::vector<std::string> g_climatesWere;

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

		// the edits' other members as they were saved
		void LoadExtra()
		{
			g_extra = ml::Json::Object();
			std::ifstream in(std::filesystem::path(g_cfg.edits), std::ios::binary);
			if (!in)
				return;
			const ml::Json j = ml::Json::Parse(std::string((std::istreambuf_iterator<char>(in)), {}));
			for (const char* key : {"buildings", "demolish", "paint", "terrain", "climate"})
				if (j.Contains(key))
					g_extra[key] = j[key];
		}

		std::string Climate() { return g_extra["climate"].Str(""); }
		size_t ClimateIndex()
		{
			for (size_t k = 0; k < std::size(kClimates); ++k)
				if (Climate() == kClimates[k].id)
					return k;
			return 0;
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

		ml::Json Points(const std::vector<P2>& points)
		{
			ml::Json a = ml::Json::Array({});
			for (const P2& p : points)
				a.Push(ml::Json::Array({std::round(p[0] * 100) / 100, std::round(p[1] * 100) / 100}));
			return a;
		}

		std::vector<P2> PointsOf(const ml::Json& a)
		{
			std::vector<P2> out;
			for (const ml::Json& p : a.Items())
				out.push_back({p[0].Float(), p[1].Float()});
			return out;
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
				LoadExtra();
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
			Say("世界編輯器：下排選工具，上排選種類；左鍵放下，右鍵取消，Ctrl+Z 復原", 6000);
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
			snap = RoadTool(g_tool) ? g_net.SnapTo(g_cursor, SnapRadius()) : re::Snap{};
			if (snap.kind != re::Snap::Kind::None)
				return snap.at;
			P2 p = g_cursor;
			if (!g_stroke.empty() && !BrushTool(g_tool) && PAD::IS_DISABLED_CONTROL_PRESSED(0, kSprint))
			{
				const P2& a = g_stroke.back();
				const float dx = p[0] - a[0], dy = p[1] - a[1], len = std::hypot(dx, dy);
				const float step = 15 * kPi / 180, angle = std::round(std::atan2(dy, dx) / step) * step;
				p = {a[0] + std::cos(angle) * len, a[1] + std::sin(angle) * len};
			}
			return p;
		}

		// The road stroke as it would be put down now (the cursor its last point).
		std::vector<P2> Stroke(const P2& end)
		{
			if (g_stroke.empty())
				return {};
			if (g_tool == Tool::Curve && g_stroke.size() == 2)
				return re::Network::Curve(g_stroke[0], g_stroke[1], end);
			return {g_stroke.front(), end};
		}

		// A building's footprint from its front's two corners and a point as deep as it goes (anticlockwise).
		std::vector<P2> Footprint(const P2& a, const P2& b, const P2& c)
		{
			const float dx = b[0] - a[0], dy = b[1] - a[1], len = std::hypot(dx, dy);
			if (len < 1)
				return {};
			const P2 n{-dy / len, dx / len};
			const float depth = (c[0] - a[0]) * n[0] + (c[1] - a[1]) * n[1];
			if (std::fabs(depth) < 1)
				return {};
			std::vector<P2> ring{a, b, {b[0] + n[0] * depth, b[1] + n[1] * depth}, {a[0] + n[0] * depth, a[1] + n[1] * depth}};
			if (depth < 0)
				std::reverse(ring.begin(), ring.end());
			return ring;
		}

		// A rectangle (world-aligned) between two corners, anticlockwise.
		std::vector<P2> Rectangle(const P2& a, const P2& c)
		{
			const float x0 = std::min(a[0], c[0]), x1 = std::max(a[0], c[0]), y0 = std::min(a[1], c[1]), y1 = std::max(a[1], c[1]);
			return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
		}

		// ---- the changes: saved at once, undone newest first ---------------------------------------

		void Save()
		{
			std::string error;
			if (!g_net.SaveEdits(g_cfg.edits, error, g_extra.Dump()))
				Say("存檔失敗：" + error);
		}

		void AddExtra(const std::string& key, ml::Json item)
		{
			g_extra[key].Push(std::move(item));
			g_history.push_back(key);
			Save();
		}

		bool Undo()
		{
			g_preview.key.clear();
			if (g_history.empty())
			{
				const bool ok = g_net.Undo();
				Save();
				return ok;
			}
			const std::string what = g_history.back();
			g_history.pop_back();
			bool ok = true;
			if (what == "roads")
				ok = g_net.Undo();
			else if (what == "climate")
			{
				g_extra["climate"] = g_climatesWere.back();
				g_climatesWere.pop_back();
			}
			else
			{
				ml::Json kept = ml::Json::Array({});
				const auto& items = g_extra[what].Items();
				for (size_t k = 0; k + 1 < items.size(); ++k)
					kept.Push(items[k]);
				g_extra[what] = std::move(kept);
			}
			Save();
			return ok;
		}

		void CommitRoads(const re::Change& c)
		{
			g_net.Commit(c);
			g_history.push_back("roads");
			Save();
			g_preview.key.clear();
		}

		void SetClimate(const std::string& name)
		{
			g_climatesWere.push_back(Climate());
			g_extra["climate"] = name;
			g_history.push_back("climate");
			Save();
		}

		void CommitBrush(const std::vector<P2>& points)
		{
			if (points.empty())
				return;
			ml::Json s = ml::Json::Object();
			s["what"] = g_tool == Tool::Paint ? kPaint[g_paint].id : kShape[g_shape].id;
			s["radius"] = g_brush;
			s["amount"] = 1.0;
			s["points"] = Points(points);
			AddExtra(g_tool == Tool::Paint ? "paint" : "terrain", std::move(s));
			Say(std::format("{}：{}（筆刷 {:.0f} 公尺）", g_tool == Tool::Paint ? "地面" : "地形", g_tool == Tool::Paint ? kPaint[g_paint].label : kShape[g_shape].label,
			        g_brush),
			    2500);
		}

		void CommitBuilding(const std::vector<P2>& ring)
		{
			if (ring.size() < 3)
				return;
			ml::Json b = ml::Json::Object();
			b["footprint"] = Points(ring);
			b["height"] = g_height;
			b["kind"] = kKinds[g_kind].id;
			AddExtra("buildings", std::move(b));
			Say(std::format("已放下{}（高 {:.0f} 公尺）", kKinds[g_kind].label, g_height), 2500);
		}

		void CommitDemolish(const std::vector<P2>& ring)
		{
			if (ring.size() < 3)
				return;
			AddExtra("demolish", Points(ring));
			Say("已拆除範圍內的建築（重建後生效）", 2500);
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
			// (the carriageways whatever they are made of, worn ones too)
			const auto carriageways = [](const worldgen::roads::Surfaces& s) {
				worldgen::roads::Mesh all;
				for (const worldgen::roads::Mesh* m : {&s.paved[0], &s.paved[1], &s.paved[2], &s.paved[3], &s.paved[4], &s.paved[5], &s.paved[6], &s.paved[7], &s.worn})
				{
					const auto base = static_cast<uint32_t>(all.vertices.size());
					all.vertices.insert(all.vertices.end(), m->vertices.begin(), m->vertices.end());
					for (const uint32_t i : m->indices)
						all.indices.push_back(base + i);
				}
				return all;
			};
			const worldgen::roads::Mesh wasRoads = carriageways(was), willRoads = carriageways(will);
			differ(willRoads, keys(wasRoads), g_preview.asphalt);
			differ(will.sidewalk, keys(was.sidewalk), g_preview.sidewalk);
			differ(will.markings, keys(was.markings), g_preview.markings);
			differ(wasRoads, keys(willRoads), g_preview.gone);
			differ(was.sidewalk, keys(will.sidewalk), g_preview.gone);
		}

		void Poly(const worldgen::roads::P3& a, const worldgen::roads::P3& b, const worldgen::roads::P3& c, int r, int g, int bl, int al)
		{
			// (both faces: the game draws polygons one-sided)
			GRAPHICS::DRAW_POLY(a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2], r, g, bl, al);
			GRAPHICS::DRAW_POLY(a[0], a[1], a[2], c[0], c[1], c[2], b[0], b[1], b[2], r, g, bl, al);
		}

		void DrawPreview()
		{
			const auto draw = [](const auto& tris, int r, int g, int b, int a, float lift) {
				for (const auto& t : tris)
					Poly({t[0][0], t[0][1], t[0][2] + lift}, {t[1][0], t[1][1], t[1][2] + lift}, {t[2][0], t[2][1], t[2][2] + lift}, r, g, b, a);
			};
			draw(g_preview.gone, 255, 60, 40, 110, 0.10f);
			draw(g_preview.asphalt, 60, 130, 255, 150, 0.12f);
			draw(g_preview.sidewalk, 160, 205, 255, 140, 0.12f);
			draw(g_preview.markings, 255, 255, 255, 210, 0.16f);
		}

		// A box on a footprint `height` high (its walls and its top).
		void DrawBox(const std::vector<P2>& ring, float height, int r, int g, int b, int a)
		{
			float base = 1e30f;
			for (const P2& p : ring)
				base = std::min(base, Ground(p[0], p[1]));
			const float top = base + height;
			for (size_t k = 0; k < ring.size(); ++k)
			{
				const P2& p = ring[k];
				const P2& q = ring[(k + 1) % ring.size()];
				Poly({p[0], p[1], base}, {q[0], q[1], base}, {q[0], q[1], top}, r, g, b, a);
				Poly({p[0], p[1], base}, {q[0], q[1], top}, {p[0], p[1], top}, r, g, b, a);
				GRAPHICS::DRAW_LINE(p[0], p[1], top, q[0], q[1], top, 255, 255, 255, 230);
			}
			for (size_t k = 1; k + 1 < ring.size(); ++k)
				Poly({ring[0][0], ring[0][1], top}, {ring[k][0], ring[k][1], top}, {ring[k + 1][0], ring[k + 1][1], top}, r, g, b, a);
		}

		// A circle on the ground.
		void DrawCircle(const P2& c, float radius, int r, int g, int b, int a)
		{
			constexpr int kSides = 32;
			P2 last{c[0] + radius, c[1]};
			float lastZ = Ground(last[0], last[1]) + 0.3f;
			for (int k = 1; k <= kSides; ++k)
			{
				const float t = 2 * kPi * k / kSides;
				const P2 p{c[0] + radius * std::cos(t), c[1] + radius * std::sin(t)};
				const float z = Ground(p[0], p[1]) + 0.3f;
				GRAPHICS::DRAW_LINE(last[0], last[1], lastZ, p[0], p[1], z, r, g, b, a);
				last = p, lastZ = z;
			}
		}

		// The changes so far near the camera: new buildings (boxes), cleared areas (red), strokes (their brushes).
		void DrawChanges()
		{
			const float reach = std::max(300.0f, g_cam.distance * 3);
			const auto inView = [&](const P2& p) { return std::hypot(p[0] - g_cam.target[0], p[1] - g_cam.target[1]) < reach; };
			for (const ml::Json& b : g_extra["buildings"].Items())
			{
				const auto ring = PointsOf(b["footprint"]);
				if (!ring.empty() && inView(ring[0]))
					DrawBox(ring, b["height"].Float(10), 120, 190, 255, 90);
			}
			for (const ml::Json& d : g_extra["demolish"].Items())
			{
				const auto ring = PointsOf(d);
				if (ring.empty() || !inView(ring[0]))
					continue;
				for (size_t k = 0; k < ring.size(); ++k)
				{
					const P2& p = ring[k];
					const P2& q = ring[(k + 1) % ring.size()];
					GRAPHICS::DRAW_LINE(p[0], p[1], Ground(p[0], p[1]) + 0.5f, q[0], q[1], Ground(q[0], q[1]) + 0.5f, 255, 60, 40, 255);
				}
			}
			int circles = 0;
			for (const char* key : {"paint", "terrain"})
				for (const ml::Json& s : g_extra[key].Items())
				{
					const std::string what = s["what"].Str("");
					const Choice* c = &kPaint[0];
					for (const auto& o : kPaint)
						if (what == o.id)
							c = &o;
					for (const auto& o : kShape)
						if (what == o.id)
							c = &o;
					for (const P2& p : PointsOf(s["points"]))
						if (inView(p) && circles++ < 300)
							DrawCircle(p, s["radius"].Float(10), c->r, c->g, c->b, 140);
				}
		}

		// Height lines a metre apart over the ground round the target (marching squares on a lattice of the ground's
		// heights; thicker every 5 m).
		void DrawContours()
		{
			const float step = std::clamp(g_cam.distance / 40, 2.0f, 12.0f), half = std::clamp(g_cam.distance * 1.2f, 60.0f, 600.0f);
			const int n = static_cast<int>(2 * half / step) + 1;
			const float x0 = std::floor((g_cam.target[0] - half) / step) * step, y0 = std::floor((g_cam.target[1] - half) / step) * step;
			std::vector<float> h(static_cast<size_t>(n) * n);
			float lo = 1e30f, hi = -1e30f;
			for (int j = 0; j < n; ++j)
				for (int i = 0; i < n; ++i)
				{
					const float z = Ground(x0 + i * step, y0 + j * step);
					h[static_cast<size_t>(j) * n + i] = z;
					lo = std::min(lo, z), hi = std::max(hi, z);
				}
			const float interval = hi - lo > 40 ? 5.0f : 1.0f;
			for (float level = std::ceil(lo / interval) * interval; level <= hi; level += interval)
			{
				const bool major = std::fmod(std::fabs(level), interval * 5) < 1e-3f;
				for (int j = 0; j + 1 < n; ++j)
					for (int i = 0; i + 1 < n; ++i)
					{
						const float c[4] = {h[static_cast<size_t>(j) * n + i], h[static_cast<size_t>(j) * n + i + 1], h[static_cast<size_t>(j + 1) * n + i + 1],
						    h[static_cast<size_t>(j + 1) * n + i]};
						const P2 corner[4] = {{x0 + i * step, y0 + j * step}, {x0 + (i + 1) * step, y0 + j * step}, {x0 + (i + 1) * step, y0 + (j + 1) * step},
						    {x0 + i * step, y0 + (j + 1) * step}};
						P2 cut[4];
						int cuts = 0;
						for (int e = 0; e < 4; ++e)
						{
							const float a = c[e], b = c[(e + 1) % 4];
							if ((a < level) != (b < level))
							{
								const float t = (level - a) / (b - a);
								cut[cuts++] = {corner[e][0] + (corner[(e + 1) % 4][0] - corner[e][0]) * t, corner[e][1] + (corner[(e + 1) % 4][1] - corner[e][1]) * t};
							}
						}
						for (int k = 0; k + 1 < cuts; k += 2)
							GRAPHICS::DRAW_LINE(cut[k][0], cut[k][1], level + 0.25f, cut[k + 1][0], cut[k + 1][1], level + 0.25f, major ? 255 : 255, major ? 200 : 255,
							    major ? 60 : 255, major ? 255 : 170);
					}
			}
		}

		// ---- the tool bars -----------------------------------------------------------------------

		struct Button
		{
			float x, y, w, h;
			std::string label;
			bool on;
			int action; // 0..6 tools, 10.. choices, 20 undo, 21 apply, 22 leave, 23 contours, 24 climate, 25/26 size -/+
		};

		std::vector<Button> Buttons()
		{
			std::vector<Button> b;
			float x = 0.03f;
			float y = 0.955f;
			const float h = 0.042f;
			const auto add = [&](const std::string& label, float w, bool on, int action) {
				b.push_back({x + w / 2, y, w, h, label, on, action});
				x += w + 0.004f;
			};
			add("直線", 0.045f, g_tool == Tool::Straight, 0);
			add("曲線", 0.045f, g_tool == Tool::Curve, 1);
			add("刪路", 0.045f, g_tool == Tool::Delete, 2);
			x += 0.01f;
			add("建築", 0.045f, g_tool == Tool::Build, 3);
			add("拆除", 0.045f, g_tool == Tool::Demolish, 4);
			x += 0.01f;
			add("地面", 0.045f, g_tool == Tool::Paint, 5);
			add("地形", 0.045f, g_tool == Tool::Terrain, 6);
			x += 0.01f;
			add("等高線", 0.055f, g_contours, 23);
			add("氣候：" + std::string(kClimates[ClimateIndex()].label), 0.1f, !Climate().empty(), 24);
			x += 0.01f;
			add("復原", 0.042f, false, 20);
			add(g_rebuild ? "重建中…" : "套用", 0.055f, false, 21);
			add("離開", 0.042f, false, 22);
			// the tool's choices
			x = 0.03f, y = 0.905f;
			if (RoadTool(g_tool))
			{
				const auto& types = re::Types();
				for (size_t i = 0; i < types.size(); ++i)
					add(std::format("{} {}", i + 1, types[i].label), 0.095f, g_type == i, static_cast<int>(10 + i));
			}
			else if (g_tool == Tool::Build)
			{
				for (size_t i = 0; i < std::size(kKinds); ++i)
					add(std::format("{} {}", i + 1, kKinds[i].label), 0.06f, g_kind == i, static_cast<int>(10 + i));
				x += 0.01f;
				add("－", 0.025f, false, 25);
				add(std::format("高 {:.0f} 公尺", g_height), 0.07f, false, -1);
				add("＋", 0.025f, false, 26);
			}
			else if (BrushTool(g_tool))
			{
				const Choice* list = g_tool == Tool::Paint ? kPaint : kShape;
				const size_t count = g_tool == Tool::Paint ? std::size(kPaint) : std::size(kShape), current = g_tool == Tool::Paint ? g_paint : g_shape;
				for (size_t i = 0; i < count; ++i)
					add(std::format("{} {}", i + 1, list[i].label), 0.055f, current == i, static_cast<int>(10 + i));
				x += 0.01f;
				add("－", 0.025f, false, 25);
				add(std::format("筆刷 {:.0f} 公尺", g_brush), 0.08f, false, -1);
				add("＋", 0.025f, false, 26);
			}
			return b;
		}

		void Text(const std::string& text, float x, float y, float scale, bool centre, int r = 255, int g = 255, int b = 255)
		{
			HUD::SET_TEXT_FONT(0);
			HUD::SET_TEXT_SCALE(0.0f, scale);
			HUD::SET_TEXT_COLOUR(r, g, b, 255);
			HUD::SET_TEXT_OUTLINE();
			HUD::SET_TEXT_CENTRE(centre);
			// (a substring holds 99 bytes at most - 33 Chinese characters: longer text goes in pieces, cut between
			// characters, into a label of ten substrings)
			HUD::BEGIN_TEXT_COMMAND_DISPLAY_TEXT(text.size() > 90 ? "CELL_EMAIL_BCON" : "STRING");
			for (size_t at = 0, pieces = 0; at < text.size() && pieces < 10; ++pieces)
			{
				size_t end = std::min(text.size(), at + 90);
				while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
					--end;
				HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME(text.substr(at, end - at).c_str());
				at = end;
			}
			HUD::END_TEXT_COMMAND_DISPLAY_TEXT(x, y, 0);
		}

		void Apply();

		void Choose(size_t i)
		{
			if (RoadTool(g_tool) && i < re::Types().size())
				g_type = i;
			else if (g_tool == Tool::Build && i < std::size(kKinds))
				g_kind = i;
			else if (g_tool == Tool::Paint && i < std::size(kPaint))
				g_paint = i;
			else if (g_tool == Tool::Terrain && i < std::size(kShape))
				g_shape = i;
		}

		void Resize(bool bigger)
		{
			if (g_tool == Tool::Build)
				g_height = std::clamp(g_height + (bigger ? 3.0f : -3.0f), 3.0f, 300.0f);
			else
				g_brush = std::clamp(g_brush * (bigger ? 1.25f : 0.8f), 2.0f, 200.0f);
		}

		void SetTool(Tool t)
		{
			g_tool = t;
			g_stroke.clear();
			g_painting = false;
			g_preview = {};
		}

		// true when the point is over a tool bar's button
		bool OnBars(float cx, float cy)
		{
			for (const Button& b : Buttons())
				if (std::fabs(cx - b.x) <= b.w / 2 && std::fabs(cy - b.y) <= b.h / 2)
					return true;
			return false;
		}

		// true when the click was the tool bars'
		bool Toolbar(bool clicked, float cx, float cy)
		{
			bool used = false;
			for (const Button& b : Buttons())
			{
				const bool over = std::fabs(cx - b.x) <= b.w / 2 && std::fabs(cy - b.y) <= b.h / 2;
				if (b.action >= 0)
					GRAPHICS::DRAW_RECT(b.x, b.y, b.w, b.h, b.on ? 40 : 15, b.on ? 110 : 20, b.on ? 210 : 30, over ? 235 : 200, FALSE);
				Text(b.label, b.x, b.y - 0.013f, 0.3f, true);
				if (!clicked || !over || b.action < 0)
					continue;
				used = true;
				if (b.action <= 6)
					SetTool(static_cast<Tool>(b.action));
				else if (b.action >= 10 && b.action < 20)
					Choose(static_cast<size_t>(b.action - 10));
				else if (b.action == 20)
					Say(Undo() ? "已復原" : "沒有可以復原的");
				else if (b.action == 21)
					Apply();
				else if (b.action == 22)
					Leave();
				else if (b.action == 23)
					g_contours = !g_contours;
				else if (b.action == 24)
				{
					const size_t k = ClimateIndex();
					SetClimate(kClimates[(k + 1) % std::size(kClimates)].id);
					Say(std::string("氣候：") + kClimates[(k + 1) % std::size(kClimates)].label + "（重建後生效）");
				}
				else if (b.action == 25 || b.action == 26)
					Resize(b.action == 26);
			}
			return used;
		}

		// ---- clicks ----------------------------------------------------------------------------

		void Click()
		{
			re::Snap snap;
			const P2 at = Target(snap);
			switch (g_tool)
			{
			case Tool::Delete:
			{
				const re::Change c = g_net.PlanDelete(g_cursor, SnapRadius());
				if (c.Empty())
					return Say("這裡沒有道路");
				CommitRoads(c);
				return Say("已刪除一段道路");
			}
			case Tool::Build:
				if (g_stroke.size() < 2)
				{
					g_stroke.push_back(at);
					return;
				}
				CommitBuilding(Footprint(g_stroke[0], g_stroke[1], at));
				g_stroke.clear();
				return;
			case Tool::Demolish:
				if (g_stroke.empty())
				{
					g_stroke = {at};
					return;
				}
				CommitDemolish(Rectangle(g_stroke[0], at));
				g_stroke.clear();
				return;
			case Tool::Paint:
			case Tool::Terrain:
				// (a single click without a drag: one dab)
				CommitBrush({at});
				return;
			default:
				break;
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
			CommitRoads(c);
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
			Say("重建中：整片土地在背景重新編譯（可能要幾十分鐘，好了會再通知）", 10000);
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
			for (int k = 0; k < 9; ++k)
				if (KeyHit('1' + k))
					Choose(static_cast<size_t>(k));
			if (KeyHit(VK_OEM_4))
				Resize(false);
			if (KeyHit(VK_OEM_6))
				Resize(true);
			if (Focused() && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && KeyHit('Z'))
				Say(Undo() ? "已復原" : "沒有可以復原的");
			if (PAD::IS_DISABLED_CONTROL_JUST_PRESSED(0, kPause) || PAD::IS_DISABLED_CONTROL_JUST_PRESSED(0, kPauseAlt))
			{
				if (!g_stroke.empty())
					g_stroke.clear(), g_painting = false;
				else
					Leave();
			}
		}

		std::string ToolName()
		{
			switch (g_tool)
			{
			case Tool::Straight:
				return std::string("直線  ") + re::Types()[g_type].label;
			case Tool::Curve:
				return std::string("曲線  ") + re::Types()[g_type].label;
			case Tool::Delete:
				return "刪除道路";
			case Tool::Build:
				return std::format("建築  {}  高 {:.0f} 公尺", kKinds[g_kind].label, g_height);
			case Tool::Demolish:
				return "拆除：框選範圍";
			case Tool::Paint:
				return std::format("地面  {}  筆刷 {:.0f} 公尺", kPaint[g_paint].label, g_brush);
			case Tool::Terrain:
				return std::format("地形  {}  筆刷 {:.0f} 公尺", kShape[g_shape].label, g_brush);
			}
			return {};
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
			// ["straight" | "curve" | "delete" | "build" | "demolish" | "paint" | "terrain", choice index]
			const ml::Json a = ml::Json::Parse(args);
			const std::string tool = a[0].Str("straight");
			SetTool(tool == "curve"      ? Tool::Curve
			        : tool == "delete"   ? Tool::Delete
			        : tool == "build"    ? Tool::Build
			        : tool == "demolish" ? Tool::Demolish
			        : tool == "paint"    ? Tool::Paint
			        : tool == "terrain"  ? Tool::Terrain
			                             : Tool::Straight);
			if (a.Size() > 1)
				Choose(static_cast<size_t>(a[1].Int(0)));
			return std::string("true");
		});
		ml::web::Function("worldedit.size", [](const std::string& args) {
			// [metres]: the brush's radius, or (building) the height
			const ml::Json a = ml::Json::Parse(args);
			(g_tool == Tool::Build ? g_height : g_brush) = a[0].Float(g_tool == Tool::Build ? g_height : g_brush);
			return std::string("true");
		});
		ml::web::Function("worldedit.stroke", [](const std::string& args) {
			// [[x, y], ...]: a brush stroke there with the current brush
			if (!BrushTool(g_tool))
				return std::string("false");
			CommitBrush(PointsOf(ml::Json::Parse(args)));
			return std::string("true");
		});
		ml::web::Function("worldedit.climate", [](const std::string& args) {
			// ["tropical" | "dry" | "temperate" | ""]
			const ml::Json a = ml::Json::Parse(args);
			SetClimate(a[0].Str(""));
			return std::string("true");
		});
		ml::web::Function("worldedit.contours", [](const std::string& args) {
			const ml::Json a = ml::Json::Parse(args);
			g_contours = a[0].Bool(!g_contours);
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
		ml::web::Function("worldedit.undo", [](const std::string&) { return std::string(Undo() ? "true" : "false"); });
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
			for (const char* key : {"buildings", "demolish", "paint", "terrain"})
				j[key] = static_cast<int64_t>(g_extra[key].Size());
			j["climate"] = Climate();
			j["history"] = static_cast<int64_t>(g_history.size());
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
		const bool held = PAD::IS_DISABLED_CONTROL_PRESSED(0, kAccept) != 0;
		const bool onBar = Toolbar(clicked, cx, cy);
		re::Snap snap;
		const P2 at = g_hit ? Target(snap) : P2{};
		if (BrushTool(g_tool))
		{
			// a brush: down starts a stroke, held draws it (a point every half brush), up puts it down
			if (clicked && !onBar && g_hit)
				g_painting = true, g_stroke = {at};
			else if (g_painting && held && g_hit && std::hypot(at[0] - g_stroke.back()[0], at[1] - g_stroke.back()[1]) > g_brush * 0.5f)
				g_stroke.push_back(at);
			else if (g_painting && !held)
			{
				CommitBrush(g_stroke);
				g_stroke.clear(), g_painting = false;
			}
		}
		else if (clicked && !onBar && g_hit)
			Click();
		if (PAD::IS_DISABLED_CONTROL_JUST_PRESSED(0, kCancel))
			g_stroke.clear(), g_painting = false;
		// what the next click would do
		if (g_hit)
		{
			if (g_tool == Tool::Delete)
			{
				const re::Change c = g_net.PlanDelete(g_cursor, SnapRadius());
				MakePreview(c, c.removed.empty() && c.roads.empty() ? "" : std::format("d{:.0f},{:.0f}", g_cursor[0], g_cursor[1]));
			}
			else if (RoadTool(g_tool) && !g_stroke.empty())
			{
				const re::Change c = g_net.Plan(Stroke(at), re::Types()[g_type], g_startSnap, snap);
				MakePreview(c, std::format("s{}{}{:.1f},{:.1f},{}", static_cast<int>(g_tool), g_type, at[0], at[1], g_stroke.size()));
			}
			else
				MakePreview({}, "");
			if (g_tool == Tool::Build)
			{
				// the building as it would go up (its front once two corners are down)
				if (g_stroke.size() == 2)
					DrawBox(Footprint(g_stroke[0], g_stroke[1], at), g_height, 120, 190, 255, 140);
				else if (g_stroke.size() == 1)
					GRAPHICS::DRAW_LINE(g_stroke[0][0], g_stroke[0][1], Ground(g_stroke[0][0], g_stroke[0][1]) + 0.5f, at[0], at[1], Ground(at[0], at[1]) + 0.5f, 120, 190, 255, 255);
			}
			else if (g_tool == Tool::Demolish && !g_stroke.empty())
			{
				const auto ring = Rectangle(g_stroke[0], at);
				for (size_t k = 0; k < ring.size(); ++k)
					GRAPHICS::DRAW_LINE(ring[k][0], ring[k][1], Ground(ring[k][0], ring[k][1]) + 0.5f, ring[(k + 1) % 4][0], ring[(k + 1) % 4][1],
					    Ground(ring[(k + 1) % 4][0], ring[(k + 1) % 4][1]) + 0.5f, 255, 60, 40, 255);
			}
			if (BrushTool(g_tool))
			{
				const Choice& c = g_tool == Tool::Paint ? kPaint[g_paint] : kShape[g_shape];
				DrawCircle(at, g_brush, c.r, c.g, c.b, 255);
				for (const P2& p : g_stroke)
					DrawCircle(p, g_brush, c.r, c.g, c.b, 160);
			}
			else
			{
				// where the click lands (green: on a road)
				const float z = Ground(at[0], at[1]);
				const bool snapped = snap.kind != re::Snap::Kind::None;
				GRAPHICS::DRAW_MARKER(28, at[0], at[1], z + 0.3f, 0, 0, 0, 0, 0, 0, 1.2f, 1.2f, 1.2f, snapped ? 80 : 255, snapped ? 255 : 255, snapped ? 120 : 255, 180, FALSE,
				    FALSE, 2, FALSE, nullptr, nullptr, FALSE);
				for (size_t i = 0; i < g_stroke.size(); ++i)
					GRAPHICS::DRAW_MARKER(28, g_stroke[i][0], g_stroke[i][1], Ground(g_stroke[i][0], g_stroke[i][1]) + 0.3f, 0, 0, 0, 0, 0, 0, 1.0f, 1.0f, 1.0f, 255, 200, 60,
					    200, FALSE, FALSE, 2, FALSE, nullptr, nullptr, FALSE);
			}
		}
		DrawPreview();
		DrawChanges();
		if (g_contours)
			DrawContours();
		// the info line
		std::string info = ToolName();
		if (RoadTool(g_tool) && !g_stroke.empty() && g_hit)
		{
			float length = 0;
			const auto s = Stroke(at);
			for (size_t i = 0; i + 1 < s.size(); ++i)
				length += std::hypot(s[i + 1][0] - s[i][0], s[i + 1][1] - s[i][1]);
			info += std::format("  長 {:.0f} 公尺", length);
		}
		if (g_hit)
			info += std::format("  （{:.0f}, {:.0f}，高 {:.1f} 公尺）", g_cursor[0], g_cursor[1], g_cursorZ);
		info += std::format("  修改：道路 {}、建築 {}、拆除 {}、地面 {}、地形 {}", g_net.Edits(), g_extra["buildings"].Size(), g_extra["demolish"].Size(),
		    g_extra["paint"].Size(), g_extra["terrain"].Size());
		Text(info, 0.02f, 0.02f, 0.4f, false);
		Text("左鍵：放下（筆刷按住拖曳）  右鍵：取消  數字鍵：種類  [ ]：大小/高度  Shift：15° 對齊  WASD/QE/RF/滾輪：視角  Ctrl+Z：復原  Esc：離開", 0.02f, 0.055f,
		    0.3f, false, 210, 210, 210);
		if (ml::TickMs() < g_statusUntil)
			Text(g_status, 0.5f, 0.84f, 0.42f, true, 255, 230, 120);
	}
}
