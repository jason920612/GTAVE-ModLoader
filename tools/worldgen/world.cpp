#include "world.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>

#include <nlohmann/json.hpp>

namespace worldgen::world
{
	namespace
	{
		using osm::Tag;
		constexpr double kPi = 3.14159265358979323846;

		std::vector<P2> Points(const osm::Data& d, const osm::Way& w, const Projection& p)
		{
			std::vector<P2> out;
			for (const int64_t n : w.nodes)
				if (const auto it = d.nodes.find(n); it != d.nodes.end())
					out.push_back(p.To(it->second.lat, it->second.lon));
			return out;
		}

		float SignedArea(const std::vector<P2>& r)
		{
			double a = 0;
			for (size_t i = 0; i < r.size(); ++i)
			{
				const P2& u = r[i];
				const P2& v = r[(i + 1) % r.size()];
				a += static_cast<double>(u[0]) * v[1] - static_cast<double>(v[0]) * u[1];
			}
			return static_cast<float>(a / 2);
		}

		// A closed way's ring without the repeated last point, anticlockwise when `ccw`.
		std::vector<P2> Ring(std::vector<P2> r, bool ccw)
		{
			if (r.size() > 1 && r.front() == r.back())
				r.pop_back();
			if (r.size() >= 3 && (SignedArea(r) > 0) != ccw)
				std::reverse(r.begin(), r.end());
			return r;
		}

		// A multipolygon's rings of one role: its member ways joined end to end.
		std::vector<std::vector<P2>> Rings(const osm::Data& d, const osm::Relation& rel, const std::string& role, const Projection& p)
		{
			std::vector<std::vector<int64_t>> parts;
			for (const osm::Member& m : rel.members)
				if (m.type == 'w' && (m.role == role || (role == "outer" && m.role.empty())))
					if (const auto it = d.ways.find(m.ref); it != d.ways.end() && it->second.nodes.size() >= 2)
						parts.push_back(it->second.nodes);
			std::vector<std::vector<P2>> rings;
			while (!parts.empty())
			{
				std::vector<int64_t> ring = std::move(parts.back());
				parts.pop_back();
				for (bool grew = true; grew && ring.front() != ring.back();)
				{
					grew = false;
					for (size_t i = 0; i < parts.size(); ++i)
					{
						auto& q = parts[i];
						if (q.front() == ring.back())
							ring.insert(ring.end(), q.begin() + 1, q.end());
						else if (q.back() == ring.back())
							ring.insert(ring.end(), q.rbegin() + 1, q.rend());
						else
							continue;
						parts.erase(parts.begin() + static_cast<ptrdiff_t>(i));
						grew = true;
						break;
					}
				}
				if (ring.front() != ring.back())
					continue; // (open: part of it lies outside the data)
				osm::Way w;
				w.nodes = std::move(ring);
				auto pts = Ring(Points(d, w, p), role == "outer");
				if (pts.size() >= 3)
					rings.push_back(std::move(pts));
			}
			return rings;
		}

		// An area's kind from its tags, or "".
		std::string AreaKind(const osm::Tags& t)
		{
			const std::string &leisure = Tag(t, "leisure"), &landuse = Tag(t, "landuse"), &natural = Tag(t, "natural"), &amenity = Tag(t, "amenity");
			if (natural == "water" || landuse == "basin" || landuse == "reservoir" || Tag(t, "waterway") == "riverbank" || Tag(t, "waterway") == "dock" ||
			    natural == "bay")
				return "water";
			if (natural == "beach" || natural == "sand")
				return "beach";
			if (natural == "wetland")
				return "wetland";
			if (natural == "wood" || natural == "scrub" || landuse == "forest")
				return "wood";
			if (leisure == "park" || leisure == "garden" || leisure == "recreation_ground" || leisure == "playground" || leisure == "dog_park" ||
			    leisure == "nature_reserve")
				return "park";
			if (leisure == "pitch" || leisure == "golf_course" || leisure == "stadium" || leisure == "track")
				return "pitch";
			// (a cemetery: its graves - research/phase0.md §63.10)
			if (landuse == "cemetery" || amenity == "grave_yard")
				return "cemetery";
			if (landuse == "grass" || landuse == "recreation_ground" || landuse == "village_green" || landuse == "meadow" || natural == "grassland")
				return "grass";
			if (amenity == "parking")
				return "parking";
			// (a university's or college's grounds: their lines underground)
			if (amenity == "university" || amenity == "college" || landuse == "education")
				return "campus";
			if (!Tag(t, "aeroway").empty())
				return "paved";
			if (landuse == "residential" || landuse == "commercial" || landuse == "retail" || landuse == "industrial" || landuse == "construction" ||
			    landuse == "railway" || landuse == "military")
				return landuse;
			return {};
		}

		float Number(const std::string& s)
		{
			try
			{
				size_t used = 0;
				const float v = std::stof(s, &used);
				const std::string unit = s.substr(used);
				if (unit.find('\'') != std::string::npos || unit.find("ft") != std::string::npos)
					return v * 0.3048f;
				return v;
			}
			catch (...)
			{
				return 0;
			}
		}

		float Speed(const std::string& s)
		{
			const float v = Number(s);
			return s.find("mph") != std::string::npos ? v * 0.44704f : v / 3.6f;
		}

		float BuildingHeight(const osm::Tags& t)
		{
			if (const float h = Number(Tag(t, "height")); h > 0)
				return h;
			if (const float l = Number(Tag(t, "building:levels")); l > 0)
				return l * 3.3f + 1.5f;
			const std::string& b = Tag(t, "building");
			if (b == "house" || b == "detached" || b == "residential" || b == "semidetached_house" || b == "terrace" || b == "bungalow")
				return 6;
			if (b == "apartments" || b == "dormitory")
				return 14;
			if (b == "hotel")
				return 24;
			if (b == "office")
				return 18;
			if (b == "industrial" || b == "warehouse")
				return 9;
			if (b == "garage" || b == "garages" || b == "shed" || b == "roof" || b == "carport")
				return 3;
			if (b == "church" || b == "cathedral")
				return 12;
			return 7;
		}
	}

	Projection Projection::Around(double south, double west, double north, double east, double x0, double y0)
	{
		// metres per degree at the box's middle latitude (WGS84)
		const double phi = (south + north) / 2 * kPi / 180;
		Projection p;
		p.lat0 = south, p.lon0 = west, p.x0 = x0, p.y0 = y0;
		p.ky = 111132.92 - 559.82 * std::cos(2 * phi) + 1.175 * std::cos(4 * phi);
		p.kx = 111412.84 * std::cos(phi) - 93.5 * std::cos(3 * phi) + 0.118 * std::cos(5 * phi);
		(void)east;
		return p;
	}

	bool FromOsm(const osm::Data& d, const Projection& p, World& out, std::string& error)
	{
		error.clear();
		out.projection = p;
		out.min = p.To(d.south, d.west);
		out.max = p.To(d.north, d.east);
		out.attribution = "Map data (c) OpenStreetMap contributors, ODbL";
		// (and the tracks: the countryside's dirt roads - Florida's woods and farms are crossed by them, plain in the aerial
		// pictures)
		static const std::set<std::string> kRoads{"motorway", "trunk", "primary", "secondary", "tertiary", "unclassified", "residential", "living_street",
		    "service", "road", "track", "motorway_link", "trunk_link", "primary_link", "secondary_link", "tertiary_link"};
		for (const auto& [id, w] : d.ways)
		{
			if (w.nodes.size() < 2)
				continue;
			const osm::Tags& t = w.tags;
			const bool closed = w.nodes.front() == w.nodes.back() && w.nodes.size() >= 4;
			if (Tag(t, "natural") == "coastline")
			{
				// (split where its nodes were not read: far outside the box, a straight jump would cut across the land)
				std::vector<P2> piece;
				for (const int64_t node : w.nodes)
				{
					const auto it = d.nodes.find(node);
					if (it == d.nodes.end())
					{
						if (piece.size() >= 2)
							out.coastlines.push_back(std::move(piece));
						piece.clear();
						continue;
					}
					piece.push_back(p.To(it->second.lat, it->second.lon));
				}
				if (piece.size() >= 2)
					out.coastlines.push_back(std::move(piece));
				continue;
			}
			const std::string& highway = Tag(t, "highway");
			if (kRoads.contains(highway) && Tag(t, "area") != "yes")
			{
				const std::string& service = Tag(t, "service");
				// (a driveway: not a street - drawn as a walk of its own, concrete, by its house; parking aisles and
				// drive-throughs not at all)
				if (highway == "service" && service == "driveway")
				{
					Walk wk;
					wk.id = id;
					wk.points = Points(d, w, p);
					wk.kind = "driveway";
					wk.surface = Tag(t, "surface");
					wk.width = static_cast<float>(Number(Tag(t, "width")));
					if (wk.points.size() >= 2)
						out.walks.push_back(std::move(wk));
					continue;
				}
				if (highway == "service" && (service == "parking_aisle" || service == "drive-through"))
					continue;
				Road r;
				r.id = id;
				r.points = Points(d, w, p);
				r.kind = highway;
				r.name = Tag(t, "name");
				const bool link = highway.ends_with("_link");
				const std::string base = link ? highway.substr(0, highway.size() - 5) : highway;
				const std::string& oneway = Tag(t, "oneway");
				const bool one = oneway == "yes" || oneway == "true" || oneway == "1" || oneway == "-1" || (highway == "motorway" && oneway != "no") ||
				                 Tag(t, "junction") == "roundabout";
				if (oneway == "-1")
					std::reverse(r.points.begin(), r.points.end());
				const int lanes = static_cast<int>(Number(Tag(t, "lanes")));
				// (untagged: a one-way street of the small kinds has one lane, as most have)
				const int defaultLanes = base == "motorway" ? (one ? 3 : 6) : base == "trunk" || base == "primary" ? (one ? 2 : 4) : link ? 1
				                         : one && (base == "residential" || base == "service" || base == "unclassified" || base == "living_street" || base == "road")
				                             ? 1
				                             : 2;
				// (a two-way left turn lane down the middle - lanes:both_ways, or turn:lanes:both_ways left -, Florida's
				// arterials' fifth lane: in the lanes counted, not one of either way's)
				const int both = static_cast<int>(Number(Tag(t, "lanes:both_ways")));
				// (or a main road's odd lane out: two-way, 3, 5 or 7 lanes and no count either way given - in the US the middle
				// one is a two-way left turn lane; few are tagged so)
				const bool oddMain = lanes >= 3 && lanes % 2 == 1 && Tag(t, "lanes:forward").empty() && Tag(t, "lanes:backward").empty() &&
				                     (base == "trunk" || base == "primary" || base == "secondary" || base == "tertiary") && !link;
				r.centreLanes = !one && (both > 0 || Tag(t, "turn:lanes:both_ways").find("left") != std::string::npos || oddMain) ? 1 : 0;
				const int total = lanes > 0 ? std::max(r.centreLanes ? 2 : 1, lanes - r.centreLanes) : defaultLanes;
				if (one)
					r.forward = std::max(1, total), r.backward = 0;
				else
				{
					const int f = static_cast<int>(Number(Tag(t, "lanes:forward"))), b = static_cast<int>(Number(Tag(t, "lanes:backward")));
					r.forward = f > 0 ? f : std::max(1, (total + 1) / 2);
					r.backward = b > 0 ? b : std::max(1, total / 2);
				}
				// (an alley: one lane's width for both ways, 3-3.5 m between the fences - the reviewers saw Miami Beach's twice as
				// wide)
				r.laneWidth = base == "motorway" || base == "trunk" ? 3.7f : base == "primary" || base == "secondary" ? 3.5f : base == "tertiary" ? 3.3f
				              : highway == "service" && service == "alley"                                               ? 1.6f
				              : highway == "service"                                                                     ? 2.8f
				              : highway == "track"                                                                       ? 1.75f
				                                                                                                         : 3.0f;
				const std::string& sidewalk = Tag(t, "sidewalk");
				// (separate: drawn as footways of their own - not twice)
				r.sidewalks = !link && base != "motorway" && base != "trunk" && highway != "service" && highway != "track" && sidewalk != "no" &&
				              sidewalk != "none" && sidewalk != "separate";
				r.sidewalkTagged = !sidewalk.empty();
				for (const char* key : {"cycleway", "cycleway:both", "cycleway:right", "cycleway:left"})
					if (const std::string& v = Tag(t, key); v == "lane" || v == "exclusive_lane")
						r.bikeLanes = true;
				r.bridge = !Tag(t, "bridge").empty() && Tag(t, "bridge") != "no";
				r.tunnel = !Tag(t, "tunnel").empty() && Tag(t, "tunnel") != "no";
				r.layer = static_cast<int>(Number(Tag(t, "layer")));
				r.speed = Speed(Tag(t, "maxspeed"));
				r.surface = Tag(t, "surface");
				// (a track's surface, where not tagged, by its grade: grade1 solid, grade2 gravel, softer ones earth)
				if (highway == "track" && r.surface.empty())
				{
					const std::string& grade = Tag(t, "tracktype");
					r.surface = grade == "grade1" ? "compacted" : grade == "grade2" ? "gravel" : "dirt";
				}
				r.smoothness = Tag(t, "smoothness");
				if (r.points.size() >= 2)
					out.roads.push_back(std::move(r));
				continue;
			}
			// railways in use (not in tunnels: the subway's underground stretches; a loop - the Metromover's - as a line
			// back to its start)
			if (const std::string& railway = Tag(t, "railway");
			    (railway == "rail" || railway == "light_rail" || railway == "subway" || railway == "monorail" || railway == "narrow_gauge") &&
			    (Tag(t, "tunnel").empty() || Tag(t, "tunnel") == "no"))
			{
				Rail rl;
				rl.points = Points(d, w, p);
				rl.kind = railway;
				rl.bridge = !Tag(t, "bridge").empty() && Tag(t, "bridge") != "no";
				rl.layer = static_cast<int>(Number(Tag(t, "layer")));
				if (rl.points.size() >= 2)
					out.rails.push_back(std::move(rl));
				continue;
			}
			// barriers along lines: fences, walls, hedges, guard rails, retaining walls
			if (const std::string& barrier = Tag(t, "barrier");
			    barrier == "fence" || barrier == "wall" || barrier == "retaining_wall" || barrier == "hedge" || barrier == "guard_rail" ||
			    barrier == "city_wall" || barrier == "handrail")
			{
				Barrier br;
				br.points = Points(d, w, p);
				br.kind = barrier == "city_wall" ? "wall" : barrier;
				br.material = !Tag(t, "fence_type").empty() ? Tag(t, "fence_type") : Tag(t, "material");
				br.height = static_cast<float>(Number(Tag(t, "height")));
				if (br.points.size() >= 2)
					out.barriers.push_back(std::move(br));
				// (a fenced area - a pond's, a field's - or a walled building is that too)
				if (AreaKind(t).empty() && Tag(t, "building").empty())
					continue;
			}
			// ways people walk: sidewalks drawn on their own, footways, paths, steps (not indoors, not the squares - area=yes)
			if (const std::string& hw = Tag(t, "highway");
			    (hw == "footway" || hw == "path" || hw == "cycleway" || hw == "pedestrian" || hw == "steps" || hw == "bridleway") && Tag(t, "area") != "yes" &&
			    Tag(t, "indoor") != "yes" && (Tag(t, "tunnel").empty() || Tag(t, "tunnel") == "no"))
			{
				Walk wk;
				wk.id = id;
				wk.points = Points(d, w, p);
				const std::string& footway = Tag(t, "footway");
				wk.kind = hw == "footway" && (footway == "sidewalk" || footway == "crossing") ? footway : hw;
				wk.surface = Tag(t, "surface");
				wk.width = static_cast<float>(Number(Tag(t, "width")));
				wk.bridge = !Tag(t, "bridge").empty() && Tag(t, "bridge") != "no";
				wk.layer = static_cast<int>(Number(Tag(t, "layer")));
				if (wk.points.size() >= 2)
					out.walks.push_back(std::move(wk));
				if (AreaKind(t).empty() && Tag(t, "building").empty())
					continue;
			}
			const std::string& waterway = Tag(t, "waterway");
			if (!closed && (waterway == "canal" || waterway == "river" || waterway == "stream" || waterway == "ditch" || waterway == "drain"))
			{
				// (not in a culvert or covered: Tallahassee's piped creeks had come out as ponds beside the streets)
				if ((!Tag(t, "tunnel").empty() && Tag(t, "tunnel") != "no") || Tag(t, "covered") == "yes" || Tag(t, "location") == "underground")
					continue;
				Waterway ww;
				ww.points = Points(d, w, p);
				ww.kind = waterway;
				const float width = Number(Tag(t, "width"));
				ww.width = width > 0 ? width : waterway == "river" ? 25.0f : waterway == "canal" ? 18.0f : 4.0f;
				out.waterways.push_back(std::move(ww));
				continue;
			}
			if (!closed)
				continue;
			if (!Tag(t, "building").empty() || !Tag(t, "building:part").empty())
			{
				if (!Tag(t, "building:part").empty() && Tag(t, "building").empty())
					continue; // (parts: the building's own outline carries the height)
				Building b;
				b.footprint = Ring(Points(d, w, p), true);
				b.height = BuildingHeight(t);
				b.kind = Tag(t, "building");
				b.name = Tag(t, "name");
				if (b.footprint.size() >= 3)
					out.buildings.push_back(std::move(b));
				continue;
			}
			if (const std::string kind = AreaKind(t); !kind.empty())
			{
				Area a;
				a.kind = kind;
				a.name = Tag(t, "name");
				a.outer.push_back(Ring(Points(d, w, p), true));
				if (a.outer.back().size() >= 3)
					out.areas.push_back(std::move(a));
			}
		}
		for (const auto& [id, r] : d.relations)
		{
			const osm::Tags& t = r.tags;
			if (Tag(t, "type") != "multipolygon")
				continue;
			if (!Tag(t, "building").empty())
			{
				for (auto& ring : Rings(d, r, "outer", p))
				{
					Building b;
					b.footprint = std::move(ring);
					b.height = BuildingHeight(t);
					b.kind = Tag(t, "building");
					b.name = Tag(t, "name");
					out.buildings.push_back(std::move(b));
				}
				continue;
			}
			if (const std::string kind = AreaKind(t); !kind.empty())
			{
				Area a;
				a.kind = kind;
				a.name = Tag(t, "name");
				a.outer = Rings(d, r, "outer", p);
				a.inner = Rings(d, r, "inner", p);
				if (!a.outer.empty())
					out.areas.push_back(std::move(a));
			}
		}
		for (const auto& [id, t] : d.nodeTags)
		{
			const auto n = d.nodes.find(id);
			if (n == d.nodes.end())
				continue;
			std::string kind;
			if (Tag(t, "natural") == "tree")
				kind = "tree";
			else if (Tag(t, "highway") == "traffic_signals")
				kind = "traffic_signals";
			else if (Tag(t, "highway") == "stop")
				kind = "stop";
			else if (Tag(t, "emergency") == "fire_hydrant")
				kind = "fire_hydrant";
			else if (Tag(t, "highway") == "street_lamp")
				kind = "street_lamp";
			else if (Tag(t, "highway") == "crossing" || (Tag(t, "footway") == "crossing" && Tag(t, "highway").empty()))
			{
				// (painted or not: the crossing tag's kinds, or crossing:markings)
				const std::string& c = Tag(t, "crossing");
				const std::string& m = Tag(t, "crossing:markings");
				kind = c == "unmarked" || c == "no" || m == "no" ? "crossing:unmarked"
				     : c == "marked" || c == "zebra" || c == "traffic_signals" || c == "uncontrolled" || (!m.empty() && m != "no") ? "crossing:marked"
				                                                                                                                  : "crossing";
			}
			else if (const std::string& place = Tag(t, "place");
			         !Tag(t, "name").empty() && (place == "city" || place == "town" || place == "village" || place == "hamlet" || place == "suburb" ||
			                                     place == "neighbourhood" || place == "quarter" || place == "locality" || place == "island"))
			{
				// (the places' names: the population zones', the area names the game shows)
				out.points.push_back({"place:" + place, p.To(n->second.lat, n->second.lon), Tag(t, "name")});
				continue;
			}
			if (!kind.empty())
				out.points.push_back({kind, p.To(n->second.lat, n->second.lon)});
		}
		// (nothing at all: a sector of wild land - marsh, woods - all land; whether a box is worth building is the
		// caller's to say: tools/worldgen/sectors.py's survey)
		if (out.roads.empty() && out.buildings.empty() && out.coastlines.empty() && out.areas.empty())
			std::printf("nothing but land in the data\n");
		return true;
	}

	namespace
	{
		nlohmann::json Line(const std::vector<P2>& pts)
		{
			nlohmann::json a = nlohmann::json::array();
			// (as they are: the projection's lattice of 1/1024 m - rounded to centimetres in floats they were not the same
			// place in two lands)
			for (const P2& q : pts)
				a.push_back({q[0], q[1]});
			return a;
		}
		std::vector<P2> Line(const nlohmann::json& a)
		{
			std::vector<P2> pts;
			for (const auto& q : a)
				pts.push_back({q[0].get<float>(), q[1].get<float>()});
			return pts;
		}
	}

	bool Save(const World& w, const std::filesystem::path& file, std::string& error)
	{
		nlohmann::json j;
		j["name"] = w.name;
		j["attribution"] = w.attribution;
		j["min"] = {w.min[0], w.min[1]};
		j["max"] = {w.max[0], w.max[1]};
		const Projection& pr = w.projection;
		j["projection"] = {{"lat0", pr.lat0}, {"lon0", pr.lon0}, {"x0", pr.x0}, {"y0", pr.y0}, {"kx", pr.kx}, {"ky", pr.ky}};
		auto& coast = j["coastlines"] = nlohmann::json::array();
		for (const auto& c : w.coastlines)
			coast.push_back(Line(c));
		auto& areas = j["areas"] = nlohmann::json::array();
		for (const Area& a : w.areas)
		{
			nlohmann::json o{{"kind", a.kind}, {"outer", nlohmann::json::array()}, {"inner", nlohmann::json::array()}};
			for (const auto& r : a.outer)
				o["outer"].push_back(Line(r));
			for (const auto& r : a.inner)
				o["inner"].push_back(Line(r));
			if (!a.name.empty())
				o["name"] = a.name;
			areas.push_back(std::move(o));
		}
		auto& waterways = j["waterways"] = nlohmann::json::array();
		for (const Waterway& ww : w.waterways)
			waterways.push_back({{"kind", ww.kind}, {"width", ww.width}, {"points", Line(ww.points)}});
		auto& rails = j["rails"] = nlohmann::json::array();
		for (const Rail& rl : w.rails)
			rails.push_back({{"kind", rl.kind}, {"bridge", rl.bridge}, {"layer", rl.layer}, {"points", Line(rl.points)}});
		auto& barriers = j["barriers"] = nlohmann::json::array();
		for (const Barrier& br : w.barriers)
			barriers.push_back({{"kind", br.kind}, {"material", br.material}, {"height", br.height}, {"points", Line(br.points)}});
		auto& walks = j["walks"] = nlohmann::json::array();
		for (const Walk& wk : w.walks)
		{
			nlohmann::json o{{"id", wk.id}, {"kind", wk.kind}, {"points", Line(wk.points)}};
			if (!wk.surface.empty())
				o["surface"] = wk.surface;
			if (wk.width > 0)
				o["width"] = wk.width;
			if (wk.bridge)
				o["bridge"] = true;
			if (wk.tunnel)
				o["tunnel"] = true;
			if (wk.layer)
				o["layer"] = wk.layer;
			walks.push_back(std::move(o));
		}
		auto& roads = j["roads"] = nlohmann::json::array();
		for (const Road& r : w.roads)
		{
			nlohmann::json o{{"id", r.id}, {"kind", r.kind}, {"name", r.name}, {"forward", r.forward}, {"backward", r.backward}, {"laneWidth", r.laneWidth},
			    {"sidewalks", r.sidewalks}, {"sidewalkTagged", r.sidewalkTagged}, {"bridge", r.bridge}, {"tunnel", r.tunnel}, {"layer", r.layer}, {"speed", r.speed}, {"points", Line(r.points)}};
			if (r.bikeLanes)
				o["bikeLanes"] = true;
			if (r.centreLanes)
				o["centre"] = r.centreLanes;
			if (!r.surface.empty())
				o["surface"] = r.surface;
			if (!r.smoothness.empty())
				o["smoothness"] = r.smoothness;
			roads.push_back(std::move(o));
		}
		auto& buildings = j["buildings"] = nlohmann::json::array();
		for (const Building& b : w.buildings)
		{
			buildings.push_back({{"kind", b.kind}, {"name", b.name}, {"height", b.height}, {"footprint", Line(b.footprint)}});
			if (b.traced)
				buildings.back()["source"] = "ms";
		}
		auto& points = j["points"] = nlohmann::json::array();
		for (const Point& p : w.points)
			if (p.name.empty())
				points.push_back({{"kind", p.kind}, {"at", {p.position[0], p.position[1]}}});
			else
				points.push_back({{"kind", p.kind}, {"at", {p.position[0], p.position[1]}}, {"name", p.name}});
		std::ofstream o(file, std::ios::binary);
		if (!o)
			return error = "cannot write " + file.string(), false;
		o << j.dump(1);
		return true;
	}

	namespace
	{
		Road RoadFrom(const nlohmann::json& o)
		{
			Road r;
			r.id = o["id"];
			r.kind = o["kind"];
			r.name = o.value("name", "");
			r.forward = o["forward"];
			r.backward = o["backward"];
			r.laneWidth = o["laneWidth"];
			r.sidewalks = o["sidewalks"];
			r.sidewalkTagged = o.value("sidewalkTagged", false);
			r.bikeLanes = o.value("bikeLanes", false);
			r.centreLanes = o.value("centre", 0);
			r.bridge = o.value("bridge", false);
			r.tunnel = o.value("tunnel", false);
			r.layer = o.value("layer", 0);
			r.speed = o.value("speed", 0.0f);
			r.surface = o.value("surface", "");
			r.smoothness = o.value("smoothness", "");
			r.points = Line(o["points"]);
			return r;
		}
	}

	bool Load(const std::filesystem::path& file, World& w, std::string& error)
	{
		std::ifstream in(file, std::ios::binary);
		const auto j = nlohmann::json::parse(in, nullptr, false);
		if (j.is_discarded())
			return error = file.string() + " is not a world description", false;
		w = {};
		w.name = j.value("name", "");
		w.attribution = j.value("attribution", "");
		w.min = {j["min"][0].get<float>(), j["min"][1].get<float>()};
		w.max = {j["max"][0].get<float>(), j["max"][1].get<float>()};
		if (const auto p = j.find("projection"); p != j.end())
		{
			const auto& o = *p;
			w.projection = {o["lat0"].get<double>(), o["lon0"].get<double>(), o["x0"].get<double>(), o["y0"].get<double>(), o["kx"].get<double>(),
			    o["ky"].get<double>()};
		}
		for (const auto& c : j["coastlines"])
			w.coastlines.push_back(Line(c));
		for (const auto& o : j["areas"])
		{
			Area a;
			a.kind = o["kind"];
			for (const auto& r : o["outer"])
				a.outer.push_back(Line(r));
			for (const auto& r : o["inner"])
				a.inner.push_back(Line(r));
			a.name = o.value("name", "");
			w.areas.push_back(std::move(a));
		}
		for (const auto& o : j["waterways"])
			w.waterways.push_back({Line(o["points"]), o["width"].get<float>(), o["kind"].get<std::string>()});
		if (j.contains("rails"))
			for (const auto& o : j["rails"])
				w.rails.push_back({Line(o["points"]), o["kind"].get<std::string>(), o["bridge"].get<bool>(), o["layer"].get<int>()});
		if (j.contains("barriers"))
			for (const auto& o : j["barriers"])
				w.barriers.push_back({Line(o["points"]), o["kind"].get<std::string>(), o.value("material", ""), o.value("height", 0.0f)});
		for (const auto& o : j["roads"])
			w.roads.push_back(RoadFrom(o));
		if (j.contains("walks"))
			for (const auto& o : j["walks"])
				w.walks.push_back({o["id"].get<int64_t>(), Line(o["points"]), o["kind"].get<std::string>(), o.value("surface", ""), o.value("width", 0.0f),
				    o.value("bridge", false), o.value("tunnel", false), o.value("layer", 0)});
		for (const auto& o : j["buildings"])
			w.buildings.push_back({Line(o["footprint"]), o["height"].get<float>(), o["kind"].get<std::string>(), o["name"].get<std::string>(), false,
			    o.value("source", "") == "ms"});
		for (const auto& o : j["points"])
			w.points.push_back({o["kind"].get<std::string>(), {o["at"][0].get<float>(), o["at"][1].get<float>()}, o.value("name", "")});
		return true;
	}

	bool ApplyEdits(World& w, const std::filesystem::path& file, std::string& error)
	{
		std::ifstream in(file, std::ios::binary);
		if (!in)
			return true; // (no edits yet)
		const auto j = nlohmann::json::parse(in, nullptr, false);
		if (j.is_discarded())
			return error = file.string() + " is not an edits file", false;
		std::set<int64_t> removed;
		for (const auto& id : j.value("removed", nlohmann::json::array()))
			removed.insert(id.get<int64_t>());
		std::map<int64_t, Road> changed;
		for (const auto& o : j.value("roads", nlohmann::json::array()))
		{
			Road r = RoadFrom(o);
			if (r.points.size() >= 2)
				changed[r.id] = std::move(r);
		}
		size_t replaced = 0;
		std::erase_if(w.roads, [&](const Road& r) { return removed.contains(r.id) && !changed.contains(r.id); });
		for (Road& r : w.roads)
			if (const auto it = changed.find(r.id); it != changed.end())
			{
				r = it->second;
				changed.erase(it);
				++replaced;
			}
		// new roads clear their way, as in a city builder: buildings their asphalt and sidewalks would cross go
		size_t demolished = 0;
		for (const auto& [id, r] : changed)
		{
			const float reach = (r.forward + r.backward) * r.laneWidth / 2 + 0.3f + (r.sidewalks ? 2.6f : 0.5f);
			float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
			for (const P2& p : r.points)
				x0 = std::min(x0, p[0]), y0 = std::min(y0, p[1]), x1 = std::max(x1, p[0]), y1 = std::max(y1, p[1]);
			const auto distance = [&](const P2& q) {
				float best = 1e30f;
				for (size_t i = 0; i + 1 < r.points.size(); ++i)
				{
					const P2& a = r.points[i];
					const P2& b = r.points[i + 1];
					const float dx = b[0] - a[0], dy = b[1] - a[1], l2 = dx * dx + dy * dy;
					const float t = l2 > 0 ? std::clamp(((q[0] - a[0]) * dx + (q[1] - a[1]) * dy) / l2, 0.0f, 1.0f) : 0;
					best = std::min(best, std::hypot(q[0] - a[0] - dx * t, q[1] - a[1] - dy * t));
				}
				return best;
			};
			const auto inside = [](const std::vector<P2>& ring, const P2& p) {
				bool in = false;
				for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
					if ((ring[i][1] > p[1]) != (ring[j][1] > p[1]) &&
					    p[0] < (ring[j][0] - ring[i][0]) * (p[1] - ring[i][1]) / (ring[j][1] - ring[i][1]) + ring[i][0])
						in = !in;
				return in;
			};
			const size_t before = w.buildings.size();
			std::erase_if(w.buildings, [&](const Building& b) {
				if (b.footprint.empty())
					return false;
				float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
				for (const P2& p : b.footprint)
					bx0 = std::min(bx0, p[0]), by0 = std::min(by0, p[1]), bx1 = std::max(bx1, p[0]), by1 = std::max(by1, p[1]);
				if (bx1 < x0 - reach || bx0 > x1 + reach || by1 < y0 - reach || by0 > y1 + reach)
					return false;
				// (a corner or an edge's middle within reach of the road, or the road running through it)
				for (size_t i = 0; i < b.footprint.size(); ++i)
				{
					const P2& a = b.footprint[i];
					const P2& c = b.footprint[(i + 1) % b.footprint.size()];
					if (distance(a) < reach || distance({(a[0] + c[0]) / 2, (a[1] + c[1]) / 2}) < reach)
						return true;
				}
				for (const P2& p : r.points)
					if (inside(b.footprint, p))
						return true;
				for (size_t i = 0; i + 1 < r.points.size(); ++i)
					for (float t = 0.25f; t < 1; t += 0.25f)
						if (inside(b.footprint, {r.points[i][0] + (r.points[i + 1][0] - r.points[i][0]) * t, r.points[i][1] + (r.points[i + 1][1] - r.points[i][1]) * t}))
							return true;
				return false;
			});
			demolished += before - w.buildings.size();
		}
		for (auto& [id, r] : changed)
			w.roads.push_back(std::move(r));
		std::printf("edits: %zu roads changed, %zu added, %zu removed; %zu buildings in the new roads' way demolished\n", replaced, changed.size(),
		    removed.size(), demolished);
		// areas cleared of buildings (a building whose middle is inside goes), new buildings, the brush strokes, the climate
		for (const auto& ring : j.value("demolish", nlohmann::json::array()))
			if (auto r = Line(ring); r.size() >= 3)
				w.demolish.push_back(std::move(r));
		const size_t standing = w.buildings.size();
		std::erase_if(w.buildings, [&](const Building& b) { return Demolished(w, b.footprint); });
		const size_t cleared = standing - w.buildings.size();
		size_t added = 0;
		for (const auto& o : j.value("buildings", nlohmann::json::array()))
		{
			Building b;
			b.footprint = Line(o.value("footprint", nlohmann::json::array()));
			b.height = o.value("height", 10.0f);
			b.kind = o.value("kind", "yes");
			b.planned = true;
			if (b.footprint.size() >= 3)
				w.buildings.push_back(std::move(b)), ++added;
		}
		for (const auto& [key, list] : {std::pair{"paint", &w.paint}, std::pair{"terrain", &w.terrain}})
			for (const auto& o : j.value(key, nlohmann::json::array()))
			{
				Stroke s;
				s.what = o.value("what", "");
				s.radius = o.value("radius", 10.0f);
				s.amount = o.value("amount", 1.0f);
				s.points = Line(o.value("points", nlohmann::json::array()));
				if (!s.what.empty() && !s.points.empty())
					list->push_back(std::move(s));
			}
		w.climate = j.value("climate", "");
		std::printf("edits: %zu buildings demolished in %zu areas, %zu added; %zu paint strokes, %zu terrain strokes; climate %s\n",
		    cleared, w.demolish.size(), added, w.paint.size(), w.terrain.size(), w.climate.empty() ? "(the game's)" : w.climate.c_str());
		return true;
	}

	bool ApplyParcels(World& w, const std::filesystem::path& file, ParcelStats& stats, std::string& error)
	{
		std::ifstream in(file, std::ios::binary);
		const auto j = nlohmann::json::parse(in, nullptr, false);
		if (j.is_discarded() || !j.contains("buildings"))
			return error = "cannot read " + file.string(), false;
		const auto guessed = [](float h) { return h == 4.5f || h == 6.0f || h == 7.0f || h == 9.0f || h == 14.0f || h == 18.0f; };
		for (const auto& e : j["buildings"])
		{
			const size_t bi = e.value("building", static_cast<size_t>(-1));
			if (bi >= w.buildings.size())
				continue;
			Building& b = w.buildings[bi];
			const std::string use = e.value("use", "");
			const float living = e.value("living", 0.0f);
			const int storeys = e.value("storeys", 0);
			const bool main = e.value("main", true);
			const int code = use.empty() ? -1 : std::atoi(use.c_str());
			// (vacant land: nothing there now - where one of Microsoft's traced footprints of a house's size stands, its house
			// demolished since the pictures it was traced from; the map's buildings stay: a condominium's land is rolled as
			// common land with no living area, and Miami Beach's apartment blocks - a tenth of a land's buildings - had gone)
			const auto areaOf = [](const std::vector<P2>& ring) {
				double a = 0;
				for (size_t i = 0, k = ring.size() - 1; i < ring.size(); k = i++)
					a += static_cast<double>(ring[k][0]) * ring[i][1] - static_cast<double>(ring[i][0]) * ring[k][1];
				return std::fabs(a) / 2;
			};
			if ((code == 0 || code == 10 || code == 40 || code == 70 || code == 80) && living <= 0 && b.traced && !b.footprint.empty() &&
			    areaOf(b.footprint) < 250)
			{
				// (its footprint emptied - it keeps its place in the list, which the street pictures' walls and the rolls are
				// keyed by -, the outline demolished: the scan's own building there left out too)
				w.demolish.push_back(std::move(b.footprint));
				b.footprint.clear();
				++stats.vacant;
				continue;
			}
			if ((b.kind == "yes" || b.kind.empty()) && code >= 0)
			{
				const char* kind = code == 1                              ? (main ? "house" : "garage")
				                 : code == 2                              ? "static_caravan"
				                 : code == 3 || code == 4 || code == 5 || code == 8 ? "apartments"
				                 : code == 11                             ? "retail"
				                 : code >= 12 && code <= 39               ? "commercial"
				                 : code >= 41 && code <= 49               ? "industrial"
				                 : code == 71                             ? "church"
				                 : code == 72 || code == 83 || code == 84 ? "school"
				                 : code == 73 || code == 85               ? "hospital"
				                 : code >= 81 && code <= 89               ? "public"
				                                                          : nullptr;
				if (kind)
					b.kind = kind, ++stats.retyped;
			}
			if (main && storeys > 0 && guessed(b.height))
			{
				const bool home = b.kind == "house" || b.kind == "static_caravan" || b.kind == "detached" || b.kind == "residential" ||
				                  b.kind == "bungalow" || b.kind == "semidetached_house" || b.kind == "terrace";
				const int n = home ? std::min(storeys, 3) : std::min(storeys, 30);
				const float h = home ? (n == 1 ? (b.kind == "static_caravan" ? 3.6f : 4.8f) : 3.0f * n + 2.5f) : 3.6f * n + 1.0f;
				if (h != b.height)
					b.height = h, ++stats.raised;
			}
		}
		return true;
	}

	size_t KindsByLandUse(World& w)
	{
		struct Use
		{
			const Area* area;
			P2 lo, hi;
			const char* kind;
		};
		std::vector<Use> uses;
		for (const Area& a : w.areas)
		{
			const char* kind = a.kind == "commercial" || a.kind == "retail" ? "commercial" : a.kind == "industrial" ? "industrial" : nullptr;
			if (!kind || a.outer.empty())
				continue;
			Use u{&a, {1e30f, 1e30f}, {-1e30f, -1e30f}, kind};
			for (const auto& ring : a.outer)
				for (const P2& p : ring)
					u.lo = {std::min(u.lo[0], p[0]), std::min(u.lo[1], p[1])}, u.hi = {std::max(u.hi[0], p[0]), std::max(u.hi[1], p[1])};
			uses.push_back(u);
		}
		const auto inside = [](const std::vector<std::vector<P2>>& rings, const P2& c) {
			bool in = false;
			for (const auto& ring : rings)
				for (size_t i = 0, k = ring.size() - 1; i < ring.size(); k = i++)
					if ((ring[i][1] > c[1]) != (ring[k][1] > c[1]) && c[0] < (ring[k][0] - ring[i][0]) * (c[1] - ring[i][1]) / (ring[k][1] - ring[i][1]) + ring[i][0])
						in = !in;
			return in;
		};
		size_t changed = 0;
		for (Building& b : w.buildings)
		{
			if ((b.kind != "yes" && !b.kind.empty()) || b.footprint.empty())
				continue;
			P2 c{0, 0};
			for (const P2& p : b.footprint)
				c[0] += p[0] / b.footprint.size(), c[1] += p[1] / b.footprint.size();
			for (const Use& u : uses)
				if (c[0] >= u.lo[0] && c[0] <= u.hi[0] && c[1] >= u.lo[1] && c[1] <= u.hi[1] && inside(u.area->outer, c) && !inside(u.area->inner, c))
				{
					b.kind = u.kind, ++changed;
					break;
				}
		}
		return changed;
	}

	bool Demolished(const World& w, const std::vector<P2>& footprint)
	{
		if (footprint.empty() || w.demolish.empty())
			return false;
		P2 c{0, 0};
		for (const P2& p : footprint)
			c[0] += p[0] / footprint.size(), c[1] += p[1] / footprint.size();
		for (const auto& ring : w.demolish)
		{
			bool in = false;
			for (size_t i = 0, k = ring.size() - 1; i < ring.size(); k = i++)
				if ((ring[i][1] > c[1]) != (ring[k][1] > c[1]) && c[0] < (ring[k][0] - ring[i][0]) * (c[1] - ring[i][1]) / (ring[k][1] - ring[i][1]) + ring[i][0])
					in = !in;
			if (in)
				return true;
		}
		return false;
	}

	bool SaveRoadIndex(const World& w, const std::filesystem::path& file, std::string& error)
	{
		std::ofstream o(file, std::ios::binary);
		if (!o)
			return error = "cannot write " + file.string(), false;
		const auto put = [&](const auto& v) { o.write(reinterpret_cast<const char*>(&v), sizeof(v)); };
		o.write("WBRD", 4);
		put(uint32_t{1});
		put(static_cast<uint32_t>(w.roads.size()));
		for (const Road& r : w.roads)
		{
			put(r.id);
			put(static_cast<uint8_t>(std::min<size_t>(r.kind.size(), 255)));
			o.write(r.kind.data(), static_cast<std::streamsize>(std::min<size_t>(r.kind.size(), 255)));
			put(static_cast<uint16_t>(std::min<size_t>(r.name.size(), 65535)));
			o.write(r.name.data(), static_cast<std::streamsize>(std::min<size_t>(r.name.size(), 65535)));
			put(static_cast<uint8_t>(r.forward));
			put(static_cast<uint8_t>(r.backward));
			put(static_cast<uint8_t>((r.sidewalks ? 1 : 0) | (r.bridge ? 2 : 0) | (r.tunnel ? 4 : 0)));
			put(static_cast<int8_t>(r.layer));
			put(r.laneWidth);
			put(r.speed);
			put(static_cast<uint32_t>(r.points.size()));
			for (const P2& p : r.points)
				put(p[0]), put(p[1]);
		}
		return static_cast<bool>(o);
	}
}
