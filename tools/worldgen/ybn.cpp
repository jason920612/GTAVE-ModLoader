#include "ybn.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <limits>
#include <map>
#include <numeric>
#include <tuple>

namespace worldgen::ybn
{
	using loader::convert::Bytes;
	using loader::convert::Put;
	using Vec = std::array<float, 3>;

	namespace
	{
		constexpr uint64_t kCompositeVtable = 0x1406B1940, kBvhVtable = 0x1406B2130; // what the game's own files carry there
		constexpr uint32_t kPage = 0x4000, kMaxPages = 127;
		constexpr uint32_t kComposite = 0xB0, kBvhBound = 0x150, kBvh = 0x80;
		constexpr uint32_t kTag = 0x7F800001;
		constexpr float kMargin = 0.005f; // a part's collision margin (the game's files: 0.005 m)
		// a part's type and include flags in the composite (as the game's map collision has them)
		constexpr uint8_t kFlags[8] = {0x3E, 0, 0, 0, 0xC0, 0xBE, 0xF3, 0x07};

		struct Layout
		{
			std::vector<uint32_t> used;
			uint32_t Alloc(uint32_t size, uint32_t align = 16)
			{
				for (size_t p = 0; p < used.size(); ++p)
				{
					const uint32_t at = (used[p] + align - 1) & ~(align - 1);
					if (at + size <= kPage)
					{
						used[p] = at + size;
						return static_cast<uint32_t>(p * kPage + at);
					}
				}
				return Page(size);
			}
			uint32_t Page(uint32_t size)
			{
				used.push_back(size);
				return static_cast<uint32_t>((used.size() - 1) * kPage);
			}
		};

		uint64_t Ptr(uint32_t offset) { return static_cast<uint64_t>(loader::convert::kVirtual) + offset; }

		void PutVec(Bytes& v, uint32_t at, const Vec& p, uint32_t w)
		{
			Put<float>(v, at, p[0]);
			Put<float>(v, at + 4, p[1]);
			Put<float>(v, at + 8, p[2]);
			Put<uint32_t>(v, at + 12, w);
		}

		void PutVecF(Bytes& v, uint32_t at, const Vec& p, float w)
		{
			Put<float>(v, at, p[0]);
			Put<float>(v, at + 4, p[1]);
			Put<float>(v, at + 8, p[2]);
			Put<float>(v, at + 12, w);
		}

		struct Node
		{
			std::array<int16_t, 3> min, max;
			uint16_t a = 0, b = 0;
		};

		// A BVH over items with integer boxes (already in the tree's quantized space): leaves of up to 4, split at the
		// median of the longest axis of the items' centres. `order` gets the items in leaf order.
		struct Tree
		{
			std::vector<Node> nodes;
			std::vector<uint32_t> order;
			std::vector<std::array<uint8_t, 16>> subtrees;

			void Build(const std::vector<std::array<int32_t, 6>>& boxes)
			{
				std::vector<uint32_t> items(boxes.size());
				std::iota(items.begin(), items.end(), 0u);
				nodes.clear();
				order.clear();
				if (!items.empty())
					Split(boxes, items, 0, items.size());
				subtrees.clear();
				if (!nodes.empty())
					Headers(0);
			}

		private:
			uint32_t Split(const std::vector<std::array<int32_t, 6>>& boxes, std::vector<uint32_t>& items, size_t begin, size_t end)
			{
				const uint32_t index = static_cast<uint32_t>(nodes.size());
				nodes.emplace_back();
				std::array<int32_t, 3> lo{INT32_MAX, INT32_MAX, INT32_MAX}, hi{INT32_MIN, INT32_MIN, INT32_MIN};
				for (size_t i = begin; i < end; ++i)
					for (int a = 0; a < 3; ++a)
					{
						lo[a] = std::min(lo[a], boxes[items[i]][a]);
						hi[a] = std::max(hi[a], boxes[items[i]][3 + a]);
					}
				for (int a = 0; a < 3; ++a)
				{
					nodes[index].min[a] = static_cast<int16_t>(std::clamp(lo[a], -32767, 32767));
					nodes[index].max[a] = static_cast<int16_t>(std::clamp(hi[a], -32767, 32767));
				}
				if (end - begin <= 4)
				{
					nodes[index].a = static_cast<uint16_t>(order.size());
					nodes[index].b = static_cast<uint16_t>(end - begin);
					order.insert(order.end(), items.begin() + begin, items.begin() + end);
					return index;
				}
				int axis = 0;
				for (int a = 1; a < 3; ++a)
					if (hi[a] - lo[a] > hi[axis] - lo[axis])
						axis = a;
				const size_t mid = begin + (end - begin) / 2;
				std::nth_element(items.begin() + begin, items.begin() + mid, items.begin() + end, [&](uint32_t x, uint32_t y) {
					return boxes[x][axis] + boxes[x][3 + axis] < boxes[y][axis] + boxes[y][3 + axis];
				});
				Split(boxes, items, begin, mid);
				Split(boxes, items, mid, end);
				nodes[index].a = static_cast<uint16_t>(nodes.size() - index); // the subtree's node count
				nodes[index].b = 0;
				return index;
			}

			void Headers(uint32_t index)
			{
				const Node& n = nodes[index];
				const uint32_t size = n.b ? 1 : n.a;
				if (size <= 128)
				{
					std::array<uint8_t, 16> h{};
					std::memcpy(h.data(), n.min.data(), 6);
					std::memcpy(h.data() + 6, n.max.data(), 6);
					const uint16_t first = static_cast<uint16_t>(index), after = static_cast<uint16_t>(index + size);
					std::memcpy(h.data() + 12, &first, 2);
					std::memcpy(h.data() + 14, &after, 2);
					subtrees.push_back(h);
					return;
				}
				const uint32_t left = index + 1, right = left + (nodes[left].b ? 1 : nodes[left].a);
				Headers(left);
				Headers(right);
			}
		};

		struct Quantized
		{
			Vec min, max, centre, quantum;
		};

		Quantized Space(const Vec& min, const Vec& max)
		{
			Quantized q{min, max, {}, {}};
			for (int a = 0; a < 3; ++a)
			{
				q.centre[a] = (min[a] + max[a]) / 2;
				q.quantum[a] = (max[a] - min[a]) / 2 / 32767.0f;
			}
			return q;
		}

		// The triangles' extent.
		void Extent(const Part& p, Vec& min, Vec& max)
		{
			min = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
			max = {-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
			for (const auto& t : p.triangles)
				for (const uint32_t i : t)
					for (int a = 0; a < 3; ++a)
					{
						min[a] = std::min(min[a], p.vertices[i][a]);
						max[a] = std::max(max[a], p.vertices[i][a]);
					}
		}

		// A part's box, as the game's files have it: the triangles' extent and the margin around it (and at least
		// 0.1 m on every axis: a flat part keeps a usable scale), so a ray along a face of the extent still enters it.
		void Padded(const Part& p, Vec& min, Vec& max)
		{
			Extent(p, min, max);
			for (int a = 0; a < 3; ++a)
			{
				min[a] -= kMargin;
				max[a] += kMargin;
				if (max[a] - min[a] < 0.1f)
				{
					const float mid = (min[a] + max[a]) / 2;
					min[a] = mid - 0.05f;
					max[a] = mid + 0.05f;
				}
			}
		}

		void WriteTree(Bytes& v, uint32_t at, const Tree& t, const Quantized& q, uint32_t nodes, uint32_t subtrees)
		{
			Put<uint64_t>(v, at + 0x00, Ptr(nodes));
			Put<uint32_t>(v, at + 0x08, static_cast<uint32_t>(t.nodes.size()));
			Put<uint32_t>(v, at + 0x0C, static_cast<uint32_t>(t.nodes.size()));
			PutVec(v, at + 0x20, q.min, kTag);
			PutVec(v, at + 0x30, q.max, kTag);
			PutVec(v, at + 0x40, q.centre, kTag);
			PutVec(v, at + 0x50, {1 / q.quantum[0], 1 / q.quantum[1], 1 / q.quantum[2]}, kTag);
			PutVec(v, at + 0x60, q.quantum, kTag);
			Put<uint64_t>(v, at + 0x70, Ptr(subtrees));
			Put<uint16_t>(v, at + 0x78, static_cast<uint16_t>(t.subtrees.size()));
			Put<uint16_t>(v, at + 0x7A, static_cast<uint16_t>(t.subtrees.size()));
			for (size_t i = 0; i < t.nodes.size(); ++i)
			{
				const Node& n = t.nodes[i];
				const uint32_t o = nodes + 16 * static_cast<uint32_t>(i);
				std::memcpy(v.data() + o, n.min.data(), 6);
				std::memcpy(v.data() + o + 6, n.max.data(), 6);
				Put<uint16_t>(v, o + 12, n.a);
				Put<uint16_t>(v, o + 14, n.b);
			}
			for (size_t i = 0; i < t.subtrees.size(); ++i)
				std::memcpy(v.data() + subtrees + 16 * i, t.subtrees[i].data(), 16);
		}

		void Weld(Part& p)
		{
			std::map<std::tuple<float, float, float, uint32_t>, uint32_t> at;
			std::vector<uint32_t> remap(p.vertices.size());
			std::vector<Vec> vertices;
			std::vector<std::array<uint8_t, 4>> colours;
			for (size_t i = 0; i < p.vertices.size(); ++i)
			{
				uint32_t colour = 0;
				if (!p.vertexColours.empty())
					std::memcpy(&colour, p.vertexColours[i].data(), 4);
				auto it = at.find({p.vertices[i][0], p.vertices[i][1], p.vertices[i][2], colour});
				if (it == at.end())
				{
					it = at.emplace(std::make_tuple(p.vertices[i][0], p.vertices[i][1], p.vertices[i][2], colour), static_cast<uint32_t>(vertices.size())).first;
					vertices.push_back(p.vertices[i]);
					if (!p.vertexColours.empty())
						colours.push_back(p.vertexColours[i]);
				}
				remap[i] = it->second;
			}
			std::vector<std::array<uint32_t, 3>> triangles;
			std::vector<uint8_t> materials;
			for (size_t t = 0; t < p.triangles.size(); ++t)
			{
				const std::array<uint32_t, 3> r{remap[p.triangles[t][0]], remap[p.triangles[t][1]], remap[p.triangles[t][2]]};
				if (r[0] == r[1] || r[1] == r[2] || r[0] == r[2])
					continue;
				triangles.push_back(r);
				materials.push_back(p.triangleMaterials[t]);
			}
			p.vertices = std::move(vertices);
			p.vertexColours = std::move(colours);
			p.triangles = std::move(triangles);
			p.triangleMaterials = std::move(materials);
		}

		// The common head of a bound: type, sphere, box, centre.
		void Head(Bytes& v, uint32_t at, uint64_t vtable, uint8_t type, const Vec& min, const Vec& max, float margin, const std::vector<Vec>& points)
		{
			const Vec centre{(min[0] + max[0]) / 2, (min[1] + max[1]) / 2, (min[2] + max[2]) / 2};
			float radius = 0;
			for (const Vec& p : points)
				radius = std::max(radius, std::hypot(p[0] - centre[0], p[1] - centre[1], p[2] - centre[2]));
			Put<uint64_t>(v, at + 0x00, vtable); // (+0x08: the page map, the root's only; the game fixes it up as a pointer)
			v[at + 0x10] = type;
			Put<float>(v, at + 0x14, radius);
			PutVecF(v, at + 0x20, max, margin);
			PutVec(v, at + 0x30, min, 1);
			PutVec(v, at + 0x40, centre, 0);
			PutVec(v, at + 0x50, centre, 0);
			PutVecF(v, at + 0x60, {1, 1, 1}, 1);
		}
	}

	std::array<uint8_t, 8> Material(uint8_t type, uint8_t procedural, uint8_t colour)
	{
		return {type, procedural, 0, 0, 0, colour, 0, 0};
	}

	void Box(const std::vector<Part>& parts, Vec& min, Vec& max)
	{
		min = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
		max = {-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
		for (const Part& p : parts)
		{
			Vec lo, hi;
			Padded(p, lo, hi);
			for (int a = 0; a < 3; ++a)
			{
				min[a] = std::min(min[a], lo[a]);
				max[a] = std::max(max[a], hi[a]);
			}
		}
	}

	bool Write(const std::vector<Part>& given, Bytes& out, std::string& error, bool material)
	{
		// (each part's vertices at one place - and of one colour - made one: the roads' and buildings' triangles come with
		// three of their own, the triangles' neighbours across their edges then unknown; a triangle with two corners at one
		// place left out)
		std::vector<Part> parts = given;
		// (only with WG_WELD set: not yet tried apart from Coarse, with which cars froze - see terrain.cpp's TileCollision)
		if (std::getenv("WG_WELD"))
			for (Part& p : parts)
				Weld(p);
		std::erase_if(parts, [](const Part& p) { return p.triangles.empty(); });
		// (every array within a page of 16 KB: at most 256 parts, a part at most 1000 triangles and 2730 vertices)
		if (parts.empty() || parts.size() > 256)
			return error = "no parts, or more than 256", false;
		struct Built
		{
			Quantized q;
			std::vector<std::array<int16_t, 3>> vertices;
			std::vector<std::array<uint32_t, 3>> triangles; // in leaf order
			std::vector<uint8_t> materials;
			std::vector<std::array<int16_t, 3>> neighbours;
			Tree tree;
			Vec min, max;
			uint32_t object = 0, polys = 0, verts = 0, mats = 0, colours = 0, vertexColours = 0, polyMats = 0, bvh = 0, nodes = 0, subtrees = 0;
		};
		std::vector<Built> built(parts.size());
		for (size_t k = 0; k < parts.size(); ++k)
		{
			const Part& p = parts[k];
			Built& b = built[k];
			if (p.triangles.empty() || p.triangles.size() > 1000 || p.vertices.size() > 2730 || p.triangleMaterials.size() != p.triangles.size() ||
			    p.materials.empty() || p.materials.size() > 255 || p.colours.size() > 255 ||
			    (!p.vertexColours.empty() && p.vertexColours.size() != p.vertices.size()))
				return error = std::format("part {}: no triangles, more than 1000 or 2730 vertices, or materials that do not match", k), false;
			Padded(p, b.min, b.max);
			b.q = Space(b.min, b.max);
			// (a triangle's box in the tree: its vertices' and the margin, as the game's files have them)
			std::array<int32_t, 3> pad{};
			for (int a = 0; a < 3; ++a)
				pad[a] = std::max(1, static_cast<int32_t>(std::ceil(kMargin / b.q.quantum[a])));
			for (const Vec& v : p.vertices)
			{
				std::array<int16_t, 3> q{};
				for (int a = 0; a < 3; ++a)
					q[a] = static_cast<int16_t>(std::clamp(std::lround((v[a] - b.q.centre[a]) / b.q.quantum[a]), -32767L, 32767L));
				b.vertices.push_back(q);
			}
			std::vector<std::array<int32_t, 6>> boxes;
			for (const auto& t : p.triangles)
			{
				std::array<int32_t, 6> box{INT32_MAX, INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN, INT32_MIN};
				for (const uint32_t i : t)
					for (int a = 0; a < 3; ++a)
					{
						box[a] = std::min<int32_t>(box[a], b.vertices[i][a] - pad[a]);
						box[3 + a] = std::max<int32_t>(box[3 + a], b.vertices[i][a] + pad[a]);
					}
				boxes.push_back(box);
			}
			b.tree.Build(boxes);
			for (const uint32_t i : b.tree.order)
			{
				b.triangles.push_back(p.triangles[i]);
				b.materials.push_back(p.triangleMaterials[i]);
			}
			// neighbours: the triangle across each edge (v[k], v[k+1])
			std::map<std::pair<uint32_t, uint32_t>, uint32_t> edges;
			for (size_t t = 0; t < b.triangles.size(); ++t)
				for (int e = 0; e < 3; ++e)
					edges[{b.triangles[t][e], b.triangles[t][(e + 1) % 3]}] = static_cast<uint32_t>(t);
			for (size_t t = 0; t < b.triangles.size(); ++t)
			{
				std::array<int16_t, 3> n{-1, -1, -1};
				for (int e = 0; e < 3; ++e)
					if (const auto it = edges.find({b.triangles[t][(e + 1) % 3], b.triangles[t][e]}); it != edges.end() && it->second <= 0x7FFF)
						n[e] = static_cast<int16_t>(it->second);
				b.neighbours.push_back(n);
			}
		}
		// the composite and its BVH over the parts
		Vec min, max;
		Box(parts, min, max);
		const Quantized cq = Space(min, max);
		std::vector<std::array<int32_t, 6>> partBoxes;
		for (const Built& b : built)
		{
			std::array<int32_t, 6> box{};
			for (int a = 0; a < 3; ++a)
			{
				box[a] = static_cast<int32_t>(std::floor((b.min[a] - cq.centre[a]) / cq.quantum[a]));
				box[3 + a] = static_cast<int32_t>(std::ceil((b.max[a] - cq.centre[a]) / cq.quantum[a]));
			}
			partBoxes.push_back(box);
		}
		Tree ctree;
		ctree.Build(partBoxes);

		Layout layout;
		const uint32_t root = layout.Alloc(kComposite);
		const uint32_t n = static_cast<uint32_t>(parts.size());
		const uint32_t children = layout.Alloc(8 * n), matrices = layout.Alloc(64 * n), boxes = layout.Alloc(32 * n);
		const uint32_t typeFlags = layout.Alloc(8 * n), includeFlags = layout.Alloc(8 * n);
		// (as in the game's files: a BVH over the parts only when there are more than 5)
		const bool tree = n > 5;
		const uint32_t cbvh = tree ? layout.Alloc(kBvh) : 0, cnodes = tree ? layout.Alloc(16 * static_cast<uint32_t>(ctree.nodes.size())) : 0,
		               csub = tree ? layout.Alloc(16 * static_cast<uint32_t>(ctree.subtrees.size())) : 0;
		for (Built& b : built)
		{
			b.object = layout.Alloc(kBvhBound);
			b.polys = layout.Alloc(16 * static_cast<uint32_t>(b.triangles.size()));
			b.verts = layout.Alloc(6 * static_cast<uint32_t>(b.vertices.size()));
			b.mats = layout.Alloc(8 * static_cast<uint32_t>(parts[&b - built.data()].materials.size()));
			if (const size_t c = parts[&b - built.data()].colours.size())
				b.colours = layout.Alloc(4 * static_cast<uint32_t>(c));
			if (const size_t c = parts[&b - built.data()].vertexColours.size())
				b.vertexColours = layout.Alloc(4 * static_cast<uint32_t>(c));
			b.polyMats = layout.Alloc(static_cast<uint32_t>(b.triangles.size()));
			b.bvh = layout.Alloc(kBvh);
			b.nodes = layout.Alloc(16 * static_cast<uint32_t>(b.tree.nodes.size()));
			b.subtrees = layout.Alloc(16 * static_cast<uint32_t>(b.tree.subtrees.size()));
		}
		uint32_t pages = static_cast<uint32_t>(layout.used.size());
		const uint32_t pageMap = layout.Alloc(0x10 + 8 * pages);
		if (layout.used.size() != pages)
		{
			pages = static_cast<uint32_t>(layout.used.size());
			layout.used.back() += 8;
		}
		if (pages > kMaxPages)
			return error = std::format("collision too large: {} pages of 16 KB (at most {})", pages, kMaxPages), false;

		loader::convert::Resource r;
		r.version = 0x2B;
		r.virtualFlags = 1u | (pages << 17);
		r.physicalFlags = 0x20000000;
		Bytes& v = r.virtualBlock;
		v.assign(static_cast<size_t>(pages) * kPage, 0);

		std::vector<Vec> corners;
		for (const Built& b : built)
			for (int c = 0; c < 8; ++c)
				corners.push_back({c & 1 ? b.max[0] : b.min[0], c & 2 ? b.max[1] : b.min[1], c & 4 ? b.max[2] : b.min[2]});
		Head(v, root, kCompositeVtable, 10, min, max, 0, corners);
		Put<uint64_t>(v, root + 0x08, Ptr(pageMap));
		Put<uint64_t>(v, root + 0x70, Ptr(children));
		Put<uint64_t>(v, root + 0x78, Ptr(matrices));
		Put<uint64_t>(v, root + 0x80, Ptr(matrices));
		Put<uint64_t>(v, root + 0x88, Ptr(boxes));
		Put<uint64_t>(v, root + 0x90, Ptr(typeFlags));
		Put<uint64_t>(v, root + 0x98, Ptr(includeFlags));
		Put<uint16_t>(v, root + 0xA0, static_cast<uint16_t>(n));
		Put<uint16_t>(v, root + 0xA2, static_cast<uint16_t>(n));
		Put<float>(v, root + 0x6C, static_cast<float>(n)); // (the parts' summed volume weights, 1 each)
		if (tree)
		{
			Put<uint64_t>(v, root + 0xA8, Ptr(cbvh));
			WriteTree(v, cbvh, ctree, cq, cnodes, csub);
		}
		// (the parts in the composite tree's leaf order: its leaves name them by position)
		for (uint32_t k = 0; k < n; ++k)
		{
			const Built& b = built[ctree.order[k]];
			const Part& p = parts[ctree.order[k]];
			Put<uint64_t>(v, children + 8 * k, Ptr(b.object));
			for (int row = 0; row < 3; ++row)
				Put<float>(v, matrices + 64 * k + 16 * row + 4 * row, 1.0f);
			PutVec(v, boxes + 32 * k, b.min, 0);
			PutVec(v, boxes + 32 * k + 16, b.max, 0);
			if (!material)
			{
				std::memcpy(v.data() + typeFlags + 8 * k, kFlags, 8);
				std::memcpy(v.data() + includeFlags + 8 * k, kFlags, 8);
			}

			std::vector<Vec> points;
			for (const auto& t : b.triangles)
				for (const uint32_t i : t)
					points.push_back(p.vertices[i]);
			Head(v, b.object, kBvhVtable, 8, b.min, b.max, kMargin, points);
			Put<uint32_t>(v, b.object + 0x84, static_cast<uint32_t>(b.vertices.size()));
			Put<uint64_t>(v, b.object + 0x88, Ptr(b.polys));
			PutVecF(v, b.object + 0x90, b.q.quantum, 0);
			PutVecF(v, b.object + 0xA0, b.q.centre, 0.0025f);
			Put<uint64_t>(v, b.object + 0xB0, Ptr(b.verts));
			if (!p.vertexColours.empty())
			{
				v[b.object + 0x82] = 1;
				Put<uint64_t>(v, b.object + 0xB8, Ptr(b.vertexColours));
			}
			Put<uint32_t>(v, b.object + 0xD0, static_cast<uint32_t>(b.vertices.size()));
			Put<uint32_t>(v, b.object + 0xD4, static_cast<uint32_t>(b.triangles.size()));
			Put<uint64_t>(v, b.object + 0xF0, Ptr(b.mats));
			if (!p.colours.empty())
				Put<uint64_t>(v, b.object + 0xF8, Ptr(b.colours));
			Put<uint64_t>(v, b.object + 0x118, Ptr(b.polyMats));
			v[b.object + 0x120] = static_cast<uint8_t>(p.materials.size());
			v[b.object + 0x121] = static_cast<uint8_t>(p.colours.size());
			Put<uint64_t>(v, b.object + 0x130, Ptr(b.bvh));
			Put<uint16_t>(v, b.object + 0x140, 0xFFFF);
			for (size_t t = 0; t < b.triangles.size(); ++t)
			{
				const auto& tri = b.triangles[t];
				const Vec& a0 = p.vertices[tri[0]];
				const Vec& a1 = p.vertices[tri[1]];
				const Vec& a2 = p.vertices[tri[2]];
				const Vec e1{a1[0] - a0[0], a1[1] - a0[1], a1[2] - a0[2]}, e2{a2[0] - a0[0], a2[1] - a0[1], a2[2] - a0[2]};
				const float area = 0.5f * std::hypot(e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]);
				uint32_t bits;
				std::memcpy(&bits, &area, 4);
				bits &= ~7u; // (the low 3 bits: the kind, 0 a triangle)
				const uint32_t o = b.polys + 16 * static_cast<uint32_t>(t);
				Put<uint32_t>(v, o, bits);
				for (int c = 0; c < 3; ++c)
				{
					Put<uint16_t>(v, o + 4 + 2 * c, static_cast<uint16_t>(tri[c]));
					Put<int16_t>(v, o + 10 + 2 * c, b.neighbours[t][c]);
				}
				v[b.polyMats + t] = b.materials[t];
			}
			for (size_t i = 0; i < b.vertices.size(); ++i)
				std::memcpy(v.data() + b.verts + 6 * i, b.vertices[i].data(), 6);
			for (size_t m = 0; m < p.materials.size(); ++m)
				std::memcpy(v.data() + b.mats + 8 * m, p.materials[m].data(), 8);
			for (size_t c = 0; c < p.colours.size(); ++c)
				std::memcpy(v.data() + b.colours + 4 * c, p.colours[c].data(), 4);
			for (size_t c = 0; c < p.vertexColours.size(); ++c)
				std::memcpy(v.data() + b.vertexColours + 4 * c, p.vertexColours[c].data(), 4);
			WriteTree(v, b.bvh, b.tree, b.q, b.nodes, b.subtrees);
		}
		v[pageMap + 8] = static_cast<uint8_t>(pages);
		return loader::convert::Write(r, out, error);
	}
}
