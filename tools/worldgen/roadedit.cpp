#include "roadedit.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

#include <modloader/json.hpp>

namespace worldgen::roadedit
{
	namespace
	{
		constexpr float kCell = 64;

		uint64_t CellKey(int i, int j)
		{
			return static_cast<uint64_t>(static_cast<uint32_t>(i)) << 32 | static_cast<uint32_t>(j);
		}
		float Dist(const P2& a, const P2& b) { return std::hypot(b[0] - a[0], b[1] - a[1]); }

		// the point of segment a-b nearest p, and its parameter
		P2 Nearest(const P2& p, const P2& a, const P2& b, float& t)
		{
			const float dx = b[0] - a[0], dy = b[1] - a[1], l2 = dx * dx + dy * dy;
			t = l2 > 0 ? std::clamp(((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / l2, 0.0f, 1.0f) : 0;
			return {a[0] + dx * t, a[1] + dy * t};
		}

		// where segments a-b and c-d cross (s along a-b, t along c-d, both inside)
		bool Cross(const P2& a, const P2& b, const P2& c, const P2& d, float& s, float& t)
		{
			const float rx = b[0] - a[0], ry = b[1] - a[1], qx = d[0] - c[0], qy = d[1] - c[1];
			const float den = rx * qy - ry * qx;
			if (std::fabs(den) < 1e-6f)
				return false;
			const float ex = c[0] - a[0], ey = c[1] - a[1];
			s = (ex * qy - ey * qx) / den;
			t = (ex * ry - ey * rx) / den;
			return s > 0 && s < 1 && t > 0 && t < 1;
		}

		ml::Json ToJson(const world::Road& r)
		{
			ml::Json pts = ml::Json::Array({});
			for (const P2& p : r.points)
				pts.Push(ml::Json::Array({std::round(p[0] * 100) / 100, std::round(p[1] * 100) / 100}));
			ml::Json o = ml::Json::Object();
			o["id"] = r.id;
			o["kind"] = r.kind;
			o["name"] = r.name;
			o["forward"] = r.forward;
			o["backward"] = r.backward;
			o["laneWidth"] = r.laneWidth;
			o["sidewalks"] = r.sidewalks;
			o["bridge"] = r.bridge;
			o["tunnel"] = r.tunnel;
			o["layer"] = r.layer;
			o["speed"] = r.speed;
			o["points"] = std::move(pts);
			return o;
		}

		world::Road FromJson(const ml::Json& o)
		{
			world::Road r;
			r.id = o["id"].Int64();
			r.kind = o["kind"].Str("residential");
			r.name = o["name"].Str("");
			r.forward = o["forward"].Int(1);
			r.backward = o["backward"].Int(1);
			r.laneWidth = o["laneWidth"].Float(3.2f);
			r.sidewalks = o["sidewalks"].Bool(true);
			r.bridge = o["bridge"].Bool(false);
			r.tunnel = o["tunnel"].Bool(false);
			r.layer = o["layer"].Int(0);
			r.speed = o["speed"].Float(0);
			const ml::Json& pts = o["points"];
			for (size_t i = 0; i < pts.Size(); ++i)
				r.points.push_back({pts[i][0].Float(), pts[i][1].Float()});
			return r;
		}
	}

	const std::vector<Type>& Types()
	{
		static const std::vector<Type> kTypes = {
		    {"street", "雙線道", "residential", 1, 1, 3.2f, true},
		    {"lane", "小路（無人行道）", "service", 1, 1, 2.8f, false},
		    {"avenue", "四線大道", "secondary", 2, 2, 3.5f, true},
		    {"boulevard", "六線大道", "primary", 3, 3, 3.5f, true},
		    {"oneway", "單行道（兩線）", "tertiary", 2, 0, 3.3f, true},
		    {"highway", "快速道路（單向三線）", "motorway", 3, 0, 3.7f, false},
		};
		return kTypes;
	}

	std::vector<P2> Network::Curve(P2 a, P2 c, P2 b, float step)
	{
		const float approx = Dist(a, c) + Dist(c, b);
		const int n = std::max(2, static_cast<int>(std::ceil(approx / step)));
		std::vector<P2> out;
		for (int k = 0; k <= n; ++k)
		{
			const float t = static_cast<float>(k) / n, u = 1 - t;
			out.push_back({u * u * a[0] + 2 * u * t * c[0] + t * t * b[0], u * u * a[1] + 2 * u * t * c[1] + t * t * b[1]});
		}
		return out;
	}

	bool Network::Load(const std::filesystem::path& index, const std::filesystem::path& edits, std::string& error)
	{
		roads_.clear(), byId_.clear(), grid_.clear(), edited_.clear(), removed_.clear(), original_.clear(), history_.clear();
		std::ifstream in(index, std::ios::binary);
		if (!in)
			return error = "cannot read " + index.string() + " (worldgen roads-index writes it)", false;
		const auto get = [&](auto& v) { in.read(reinterpret_cast<char*>(&v), sizeof(v)); };
		char magic[4];
		uint32_t version = 0, count = 0;
		in.read(magic, 4);
		get(version), get(count);
		if (std::memcmp(magic, "WBRD", 4) != 0 || version != 1)
			return error = index.string() + " is not a road index", false;
		roads_.reserve(count);
		for (uint32_t k = 0; k < count && in; ++k)
		{
			world::Road r;
			uint8_t kindLength = 0, forward = 0, backward = 0, flags = 0;
			uint16_t nameLength = 0;
			int8_t layer = 0;
			uint32_t points = 0;
			get(r.id);
			get(kindLength);
			r.kind.resize(kindLength);
			in.read(r.kind.data(), kindLength);
			get(nameLength);
			r.name.resize(nameLength);
			in.read(r.name.data(), nameLength);
			get(forward), get(backward), get(flags), get(layer);
			get(r.laneWidth), get(r.speed), get(points);
			r.forward = forward, r.backward = backward, r.layer = layer;
			r.sidewalks = flags & 1, r.bridge = (flags & 2) != 0, r.tunnel = (flags & 4) != 0;
			r.points.resize(points);
			for (P2& p : r.points)
				get(p[0]), get(p[1]);
			original_[r.id] = 1;
			byId_[r.id] = roads_.size();
			roads_.push_back(std::move(r));
		}
		if (!in)
			return error = index.string() + " ends early", false;
		for (size_t i = 0; i < roads_.size(); ++i)
			Index(i);
		// the edits so far
		std::ifstream e(edits, std::ios::binary);
		if (e)
		{
			const std::string text((std::istreambuf_iterator<char>(e)), {});
			const ml::Json j = ml::Json::Parse(text);
			if (j.IsNull())
				return error = edits.string() + " is not an edits file", false;
			const ml::Json& removed = j["removed"];
			for (size_t k = 0; k < removed.Size(); ++k)
			{
				const int64_t id = removed[k].Int64();
				if (const auto it = byId_.find(id); it != byId_.end())
				{
					Unindex(it->second);
					roads_[it->second].points.clear();
				}
				removed_[id] = 1;
			}
			const ml::Json& changed = j["roads"];
			for (size_t k = 0; k < changed.Size(); ++k)
			{
				const world::Road r = FromJson(changed[k]);
				if (r.points.size() < 2)
					continue;
				Apply(r);
				edited_[r.id] = 1;
				removed_.erase(r.id);
				nextId_ = std::max(nextId_, r.id + 1);
			}
		}
		return true;
	}

	bool Network::SaveEdits(const std::filesystem::path& file, std::string& error, const std::string& extra) const
	{
		ml::Json j = ml::Json::Object();
		if (!extra.empty())
		{
			const ml::Json more = ml::Json::Parse(extra); // (kept: its members are iterated)
			for (const auto& [key, value] : more.Members())
				j[key] = value;
		}
		j["version"] = 2;
		ml::Json roads = ml::Json::Array({});
		for (const auto& [id, unused] : edited_)
			if (const auto it = byId_.find(id); it != byId_.end() && roads_[it->second].points.size() >= 2)
				roads.Push(ToJson(roads_[it->second]));
		ml::Json removed = ml::Json::Array({});
		for (const auto& [id, unused] : removed_)
			removed.Push(id);
		j["roads"] = std::move(roads);
		j["removed"] = std::move(removed);
		const std::filesystem::path temp = file.string() + ".tmp";
		{
			std::ofstream o(temp, std::ios::binary);
			if (!o)
				return error = "cannot write " + temp.string(), false;
			o << j.Dump();
		}
		std::error_code ec;
		std::filesystem::rename(temp, file, ec);
		if (ec)
			return error = "cannot replace " + file.string() + ": " + ec.message(), false;
		return true;
	}

	const world::Road* Network::Find(int64_t id) const
	{
		const auto it = byId_.find(id);
		return it == byId_.end() || roads_[it->second].points.size() < 2 ? nullptr : &roads_[it->second];
	}

	void Network::Index(size_t road)
	{
		const auto& p = roads_[road].points;
		for (size_t s = 0; s + 1 < p.size(); ++s)
		{
			const int i0 = static_cast<int>(std::floor(std::min(p[s][0], p[s + 1][0]) / kCell)), i1 = static_cast<int>(std::floor(std::max(p[s][0], p[s + 1][0]) / kCell));
			const int j0 = static_cast<int>(std::floor(std::min(p[s][1], p[s + 1][1]) / kCell)), j1 = static_cast<int>(std::floor(std::max(p[s][1], p[s + 1][1]) / kCell));
			for (int j = j0; j <= j1; ++j)
				for (int i = i0; i <= i1; ++i)
				{
					auto& list = grid_[CellKey(i, j)];
					if (list.empty() || list.back() != road)
						list.push_back(road);
				}
		}
	}

	void Network::Unindex(size_t road)
	{
		const auto& p = roads_[road].points;
		for (size_t s = 0; s + 1 < p.size(); ++s)
		{
			const int i0 = static_cast<int>(std::floor(std::min(p[s][0], p[s + 1][0]) / kCell)), i1 = static_cast<int>(std::floor(std::max(p[s][0], p[s + 1][0]) / kCell));
			const int j0 = static_cast<int>(std::floor(std::min(p[s][1], p[s + 1][1]) / kCell)), j1 = static_cast<int>(std::floor(std::max(p[s][1], p[s + 1][1]) / kCell));
			for (int j = j0; j <= j1; ++j)
				for (int i = i0; i <= i1; ++i)
					if (const auto it = grid_.find(CellKey(i, j)); it != grid_.end())
						std::erase(it->second, road);
		}
	}

	std::vector<size_t> Network::Near(float x0, float y0, float x1, float y1) const
	{
		std::vector<size_t> out;
		for (int j = static_cast<int>(std::floor(y0 / kCell)); j <= static_cast<int>(std::floor(y1 / kCell)); ++j)
			for (int i = static_cast<int>(std::floor(x0 / kCell)); i <= static_cast<int>(std::floor(x1 / kCell)); ++i)
				if (const auto it = grid_.find(CellKey(i, j)); it != grid_.end())
					out.insert(out.end(), it->second.begin(), it->second.end());
		std::sort(out.begin(), out.end());
		out.erase(std::unique(out.begin(), out.end()), out.end());
		std::erase_if(out, [&](size_t r) { return roads_[r].points.size() < 2; });
		return out;
	}

	bool Network::Shared(const P2& p, size_t except) const
	{
		for (const size_t r : Near(p[0] - 0.1f, p[1] - 0.1f, p[0] + 0.1f, p[1] + 0.1f))
		{
			if (r == except)
				continue;
			for (const P2& q : roads_[r].points)
				if (std::fabs(q[0] - p[0]) < 0.01f && std::fabs(q[1] - p[1]) < 0.01f)
					return true;
		}
		return false;
	}

	Snap Network::SnapTo(P2 p, float radius) const
	{
		Snap best;
		float bestD = radius;
		for (const size_t r : Near(p[0] - radius, p[1] - radius, p[0] + radius, p[1] + radius))
			for (size_t k = 0; k < roads_[r].points.size(); ++k)
				if (const float d = Dist(p, roads_[r].points[k]); d < bestD)
					bestD = d, best = {Snap::Kind::Point, r, k, roads_[r].points[k], d};
		if (best.kind != Snap::Kind::None)
			return best;
		bestD = radius;
		for (const size_t r : Near(p[0] - radius, p[1] - radius, p[0] + radius, p[1] + radius))
		{
			const auto& pts = roads_[r].points;
			for (size_t s = 0; s + 1 < pts.size(); ++s)
			{
				float t = 0;
				const P2 q = Nearest(p, pts[s], pts[s + 1], t);
				if (const float d = Dist(p, q); d < bestD)
					bestD = d, best = {Snap::Kind::Piece, r, s, q, d};
			}
		}
		return best;
	}

	Change Network::Plan(const std::vector<P2>& stroke, const Type& type, const Snap& start, const Snap& end) const
	{
		Change c;
		std::vector<P2> pts;
		for (const P2& p : stroke)
			if (pts.empty() || Dist(pts.back(), p) > 0.05f)
				pts.push_back(p);
		if (pts.size() < 2)
			return c;
		if (start.kind != Snap::Kind::None)
			pts.front() = start.at;
		if (end.kind != Snap::Kind::None)
			pts.back() = end.at;
		float length = 0;
		for (size_t i = 0; i + 1 < pts.size(); ++i)
			length += Dist(pts[i], pts[i + 1]);
		if (length < 2)
			return c;
		// points to put into roads: (road, segment, along, point)
		struct Insert
		{
			size_t segment;
			float t;
			P2 p;
		};
		std::unordered_map<size_t, std::vector<Insert>> into;
		std::vector<Insert> own; // into the new road
		const auto onPiece = [&](const Snap& s) {
			if (s.kind != Snap::Kind::Piece)
				return;
			const auto& rp = roads_[s.road].points;
			float t = 0;
			Nearest(s.at, rp[s.segment], rp[s.segment + 1], t);
			into[s.road].push_back({s.segment, t, s.at});
		};
		onPiece(start);
		onPiece(end);
		// crossings: both roads get the point (near a road's own point: that point, into the new road only)
		for (size_t i = 0; i + 1 < pts.size(); ++i)
		{
			const P2& a = pts[i];
			const P2& b = pts[i + 1];
			for (const size_t r : Near(std::min(a[0], b[0]) - 1, std::min(a[1], b[1]) - 1, std::max(a[0], b[0]) + 1, std::max(a[1], b[1]) + 1))
			{
				const world::Road& road = roads_[r];
				if (road.bridge || road.tunnel)
					continue; // (passes over or under)
				const auto& rp = road.points;
				for (size_t j = 0; j + 1 < rp.size(); ++j)
				{
					float s = 0, t = 0;
					if (!Cross(a, b, rp[j], rp[j + 1], s, t))
						continue;
					P2 x{a[0] + (b[0] - a[0]) * s, a[1] + (b[1] - a[1]) * s};
					// (at the stroke's own ends the snaps decide)
					if (Dist(x, pts.front()) < 1.5f || Dist(x, pts.back()) < 1.5f)
						continue;
					const bool nearOwn = Dist(x, rp[j]) < 1.5f || Dist(x, rp[j + 1]) < 1.5f;
					if (nearOwn)
						x = Dist(x, rp[j]) < Dist(x, rp[j + 1]) ? rp[j] : rp[j + 1];
					else
						into[r].push_back({j, t, x});
					own.push_back({i, s, x});
				}
			}
		}
		// the new road with its crossing points in order
		std::sort(own.begin(), own.end(), [](const Insert& x, const Insert& y) { return x.segment != y.segment ? x.segment < y.segment : x.t < y.t; });
		world::Road added;
		added.id = nextId_;
		added.kind = type.kind;
		added.forward = type.forward, added.backward = type.backward;
		added.laneWidth = type.laneWidth;
		added.sidewalks = type.sidewalks;
		size_t k = 0;
		for (size_t i = 0; i < pts.size(); ++i)
		{
			added.points.push_back(pts[i]);
			for (; k < own.size() && own[k].segment == i; ++k)
				if (Dist(added.points.back(), own[k].p) > 0.05f && (i + 1 >= pts.size() || Dist(own[k].p, pts[i + 1]) > 0.05f))
					added.points.push_back(own[k].p);
		}
		c.roads.push_back(std::move(added));
		// the roads met, each with its new points
		for (auto& [r, list] : into)
		{
			std::sort(list.begin(), list.end(), [](const Insert& x, const Insert& y) { return x.segment != y.segment ? x.segment < y.segment : x.t < y.t; });
			world::Road changed = roads_[r];
			std::vector<P2> out;
			size_t q = 0;
			for (size_t i = 0; i < changed.points.size(); ++i)
			{
				out.push_back(changed.points[i]);
				for (; q < list.size() && list[q].segment == i; ++q)
					if (Dist(out.back(), list[q].p) > 0.05f && (i + 1 >= changed.points.size() || Dist(list[q].p, changed.points[i + 1]) > 0.05f))
						out.push_back(list[q].p);
			}
			changed.points = std::move(out);
			c.roads.push_back(std::move(changed));
		}
		return c;
	}

	Change Network::PlanDelete(P2 p, float radius) const
	{
		Change c;
		Snap s;
		float bestD = radius;
		for (const size_t r : Near(p[0] - radius, p[1] - radius, p[0] + radius, p[1] + radius))
		{
			const auto& pts = roads_[r].points;
			for (size_t k = 0; k + 1 < pts.size(); ++k)
			{
				float t = 0;
				const P2 q = Nearest(p, pts[k], pts[k + 1], t);
				if (const float d = Dist(p, q); d < bestD)
					bestD = d, s = {Snap::Kind::Piece, r, k, q, d};
			}
		}
		if (s.kind == Snap::Kind::None)
			return c;
		const world::Road& road = roads_[s.road];
		const auto& pts = road.points;
		// the junctions round the piece (or the road's ends)
		size_t i0 = s.segment, i1 = s.segment + 1;
		while (i0 > 0 && !Shared(pts[i0], s.road))
			--i0;
		while (i1 + 1 < pts.size() && !Shared(pts[i1], s.road))
			++i1;
		std::vector<std::vector<P2>> keep;
		if (i0 > 0)
			keep.emplace_back(pts.begin(), pts.begin() + static_cast<ptrdiff_t>(i0) + 1);
		if (i1 + 1 < pts.size())
			keep.emplace_back(pts.begin() + static_cast<ptrdiff_t>(i1), pts.end());
		if (keep.empty())
		{
			c.removed.push_back(road.id);
			return c;
		}
		int64_t next = nextId_;
		for (size_t k = 0; k < keep.size(); ++k)
		{
			world::Road part = road;
			part.points = keep[k];
			if (k)
				part.id = next++;
			c.roads.push_back(std::move(part));
		}
		return c;
	}

	void Network::Apply(const world::Road& r)
	{
		if (const auto it = byId_.find(r.id); it != byId_.end())
		{
			Unindex(it->second);
			roads_[it->second] = r;
			Index(it->second);
			return;
		}
		byId_[r.id] = roads_.size();
		roads_.push_back(r);
		Index(roads_.size() - 1);
	}

	void Network::Commit(const Change& c)
	{
		if (c.Empty())
			return;
		Step step;
		const auto remember = [&](int64_t id) {
			const world::Road* now = Find(id);
			step.before.push_back({id, now ? std::optional<world::Road>(*now) : std::nullopt});
			step.wasEdited.push_back({id, edited_.contains(id)});
			step.wasRemoved.push_back({id, removed_.contains(id)});
		};
		for (const world::Road& r : c.roads)
			remember(r.id);
		for (const int64_t id : c.removed)
			remember(id);
		for (const world::Road& r : c.roads)
		{
			Apply(r);
			edited_[r.id] = 1;
			removed_.erase(r.id);
			nextId_ = std::max(nextId_, r.id + 1);
		}
		for (const int64_t id : c.removed)
		{
			if (const auto it = byId_.find(id); it != byId_.end())
			{
				Unindex(it->second);
				roads_[it->second].points.clear();
			}
			edited_.erase(id);
			if (original_.contains(id))
				removed_[id] = 1;
		}
		history_.push_back(std::move(step));
	}

	bool Network::Undo()
	{
		if (history_.empty())
			return false;
		const Step step = std::move(history_.back());
		history_.pop_back();
		for (const auto& [id, before] : step.before)
		{
			if (before)
				Apply(*before);
			else if (const auto it = byId_.find(id); it != byId_.end())
			{
				Unindex(it->second);
				roads_[it->second].points.clear();
			}
		}
		for (const auto& [id, was] : step.wasEdited)
			if (was)
				edited_[id] = 1;
			else
				edited_.erase(id);
		for (const auto& [id, was] : step.wasRemoved)
			if (was)
				removed_[id] = 1;
			else
				removed_.erase(id);
		return true;
	}
}
