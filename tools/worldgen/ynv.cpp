#include "ynv.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <functional>

namespace worldgen::ynv
{
	using loader::convert::Bytes;
	using loader::convert::Get;
	using loader::convert::Put;

	namespace
	{
		constexpr uint64_t kVtable = 0x1406D2160, kSplitVtable = 0x1406D21A8; // what the game's own files carry there
		constexpr uint32_t kPage = 0x4000, kRoot = 0x170, kNode = 0x60, kLeaf = 0x20, kSplit = 0x30;
		// elements per chunk of the split arrays (a chunk fills a 16 KB page)
		constexpr uint32_t kVerticesPerChunk = 2730, kIndicesPerChunk = 8192, kEdgesPerChunk = 2048, kPolysPerChunk = 341;
		constexpr uint32_t kMaxPages = 127;

		struct Reader
		{
			const Bytes& v;
			std::string& error;

			bool Offset(uint64_t pointer, uint32_t& out, size_t size)
			{
				if ((pointer >> 28) != 5 || (pointer >> 32) != 0 || (pointer & 0x0FFFFFFF) + size > v.size())
					return error = std::format("pointer {:#x} outside the block", pointer), false;
				out = static_cast<uint32_t>(pointer & 0x0FFFFFFF);
				return true;
			}

			// A split array: every chunk's elements in order.
			bool Split(uint32_t field, uint32_t elementSize, Bytes& out, uint32_t& count)
			{
				uint32_t object;
				if (!Offset(Get<uint64_t>(v, field), object, kSplit))
					return false;
				count = Get<uint32_t>(v, object + 0x08);
				const uint32_t chunks = Get<uint32_t>(v, object + 0x20);
				uint32_t list = 0;
				if (chunks && !Offset(Get<uint64_t>(v, object + 0x10), list, 16ull * chunks))
					return false;
				out.clear();
				for (uint32_t k = 0; k < chunks; ++k)
				{
					const uint32_t n = Get<uint32_t>(v, list + 16 * k + 8);
					uint32_t data;
					if (!Offset(Get<uint64_t>(v, list + 16 * k), data, static_cast<size_t>(n) * elementSize))
						return false;
					out.insert(out.end(), v.begin() + data, v.begin() + data + static_cast<size_t>(n) * elementSize);
				}
				if (out.size() != static_cast<size_t>(count) * elementSize)
					return error = std::format("split array at {:#x}: {} elements in its chunks, {} counted", field, out.size() / elementSize, count), false;
				return true;
			}

			bool Node(uint32_t at, Navmesh& m, int depth, int& index)
			{
				if (depth > 8 || at + kNode > v.size())
					return error = "quadtree too deep or outside the block", false;
				index = static_cast<int>(m.nodes.size());
				m.nodes.emplace_back();
				{
					ynv::Node& n = m.nodes.back();
					std::memcpy(n.min.data(), v.data() + at, 16);
					std::memcpy(n.max.data(), v.data() + at + 0x10, 16);
					std::memcpy(n.box.data(), v.data() + at + 0x20, 12);
				}
				if (const uint64_t leaf = Get<uint64_t>(v, at + 0x2C))
				{
					uint32_t l;
					if (!Offset(leaf, l, kLeaf))
						return false;
					const uint16_t polys = Get<uint16_t>(v, l + 0x18), cover = Get<uint16_t>(v, l + 0x1A);
					uint32_t p = 0, c = 0;
					if ((polys && !Offset(Get<uint64_t>(v, l + 0x08), p, polys * 2ull)) || (cover && !Offset(Get<uint64_t>(v, l + 0x10), c, cover * 8ull)))
						return false;
					ynv::Node& n = m.nodes[index];
					n.leaf = true;
					n.firstCover = Get<uint32_t>(v, l);
					n.polys.resize(polys);
					if (polys)
						std::memcpy(n.polys.data(), v.data() + p, polys * 2ull);
					n.cover.resize(cover);
					if (cover)
						std::memcpy(n.cover.data(), v.data() + c, cover * 8ull);
				}
				for (int k = 0; k < 4; ++k)
					if (const uint64_t child = Get<uint64_t>(v, at + 0x34 + 8 * k))
					{
						uint32_t c;
						int ci;
						if (!Offset(child, c, kNode) || !Node(c, m, depth + 1, ci))
							return false;
						m.nodes[index].children[k] = ci;
					}
				return true;
			}
		};

		// Pages of 16 KB: an object never crosses a page; a split array chunk takes a page of its own.
		struct Layout
		{
			std::vector<uint32_t> used; // bytes used per page

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
				used.push_back(size);
				return static_cast<uint32_t>((used.size() - 1) * kPage);
			}

			uint32_t Page(uint32_t size)
			{
				used.push_back(size);
				return static_cast<uint32_t>((used.size() - 1) * kPage);
			}
		};

		uint64_t Ptr(uint32_t offset)
		{
			return static_cast<uint64_t>(loader::convert::kVirtual) + offset;
		}
	}

	uint32_t Poly::VertexCount() const { uint16_t v; std::memcpy(&v, raw + 2, 2); return v >> 5; }
	uint16_t Poly::FirstIndex() const { uint16_t v; std::memcpy(&v, raw + 4, 2); return v; }
	uint16_t Poly::Navmesh() const { uint16_t v; std::memcpy(&v, raw + 6, 2); return v; }
	void Poly::SetVertexCount(uint32_t n)
	{
		uint16_t v;
		std::memcpy(&v, raw + 2, 2);
		v = static_cast<uint16_t>((v & 0x1F) | (n << 5));
		std::memcpy(raw + 2, &v, 2);
	}
	void Poly::SetFirstIndex(uint16_t v) { std::memcpy(raw + 4, &v, 2); }
	void Poly::SetNavmesh(uint16_t v) { std::memcpy(raw + 6, &v, 2); }

	bool Read(const Bytes& file, Navmesh& out, std::string& error)
	{
		loader::convert::Resource r;
		if (!loader::convert::Read(file, r, error))
			return false;
		const Bytes& v = r.virtualBlock;
		if (v.size() < kRoot)
			return error = "too small", false;
		Reader rd{v, error};
		out = {};
		out.flags = Get<uint32_t>(v, 0x10);
		out.format = Get<uint32_t>(v, 0x14);
		std::memcpy(out.matrix.data(), v.data() + 0x20, 0x40);
		std::memcpy(out.size.data(), v.data() + 0x60, 16);
		const uint32_t adjacent = Get<uint32_t>(v, 0x94);
		if (adjacent > 32)
			return error = "adjacency table too long", false;
		for (uint32_t k = 0; k < adjacent; ++k)
			out.adjacent.push_back(Get<uint32_t>(v, 0x98 + 4 * k));
		Bytes data;
		uint32_t count;
		if (!rd.Split(0x70, 6, data, count))
			return false;
		out.vertices.resize(count);
		std::memcpy(out.vertices.data(), data.data(), data.size());
		if (!rd.Split(0x80, 2, data, count))
			return false;
		out.indices.resize(count);
		std::memcpy(out.indices.data(), data.data(), data.size());
		if (!rd.Split(0x88, 8, data, count))
			return false;
		out.edges.resize(count);
		std::memcpy(out.edges.data(), data.data(), data.size());
		if (!rd.Split(0x118, 0x30, data, count))
			return false;
		out.polys.resize(count);
		std::memcpy(out.polys.data(), data.data(), data.size());
		if (out.indices.size() != Get<uint32_t>(v, 0x90) || out.edges.size() != out.indices.size() || out.vertices.size() != Get<uint32_t>(v, 0x138) ||
		    out.polys.size() != Get<uint32_t>(v, 0x13C))
			return error = "counts disagree", false;
		uint32_t root;
		int rootIndex;
		if (!rd.Offset(Get<uint64_t>(v, 0x120), root, kNode) || !rd.Node(root, out, 0, rootIndex))
			return false;
		const uint32_t links = Get<uint32_t>(v, 0x14C), linkIndices = Get<uint32_t>(v, 0x150);
		uint32_t at;
		if (links)
		{
			if (!rd.Offset(Get<uint64_t>(v, 0x128), at, links * 0x1Cull))
				return false;
			out.links.resize(links);
			std::memcpy(out.links.data(), v.data() + at, links * 0x1Cull);
		}
		if (linkIndices)
		{
			if (!rd.Offset(Get<uint64_t>(v, 0x130), at, linkIndices * 2ull))
				return false;
			out.linkIndices.resize(linkIndices);
			std::memcpy(out.linkIndices.data(), v.data() + at, linkIndices * 2ull);
		}
		out.index = Get<uint32_t>(v, 0x140);
		out.dataSize = Get<uint32_t>(v, 0x144);
		out.tag160 = Get<uint32_t>(v, 0x160);
		uint32_t cover = 0;
		for (const ynv::Node& n : out.nodes)
			cover += static_cast<uint32_t>(n.cover.size());
		if (cover != Get<uint32_t>(v, 0x148))
			return error = std::format("{} cover points in the leaves, {} counted", cover, Get<uint32_t>(v, 0x148)), false;
		return true;
	}

	bool Write(const Navmesh& m, Bytes& out, std::string& error)
	{
		if (m.adjacent.size() > 32 || m.polys.size() > 0x7FFF || m.indices.size() != m.edges.size() || m.nodes.empty())
			return error = "navmesh out of range (adjacency, polygons, edges or quadtree)", false;
		Layout layout;
		const uint32_t root = layout.Alloc(kRoot);
		// the split arrays: their objects, chunk lists and index tables with the small things, each chunk a page
		struct SplitPlan
		{
			uint32_t object, list, table;
			std::vector<std::pair<uint32_t, uint32_t>> chunks; // offset, count
		};
		const auto plan = [&](size_t count, uint32_t perChunk, uint32_t elementSize) {
			SplitPlan p;
			const uint32_t chunks = static_cast<uint32_t>((count + perChunk - 1) / perChunk);
			p.object = layout.Alloc(kSplit);
			p.list = layout.Alloc(16 * std::max(chunks, 1u));
			p.table = layout.Alloc(4 * (chunks + 1));
			for (uint32_t k = 0; k < chunks; ++k)
			{
				const uint32_t n = static_cast<uint32_t>(std::min<size_t>(perChunk, count - static_cast<size_t>(k) * perChunk));
				p.chunks.emplace_back(layout.Page(n * elementSize), n);
			}
			return p;
		};
		const SplitPlan vertices = plan(m.vertices.size(), kVerticesPerChunk, 6);
		const SplitPlan indices = plan(m.indices.size(), kIndicesPerChunk, 2);
		const SplitPlan edges = plan(m.edges.size(), kEdgesPerChunk, 8);
		const SplitPlan polys = plan(m.polys.size(), kPolysPerChunk, 0x30);
		std::vector<uint32_t> nodes(m.nodes.size()), leaves(m.nodes.size()), polyLists(m.nodes.size()), coverLists(m.nodes.size());
		for (size_t i = 0; i < m.nodes.size(); ++i)
		{
			nodes[i] = layout.Alloc(kNode);
			if (m.nodes[i].leaf)
			{
				leaves[i] = layout.Alloc(kLeaf);
				if (!m.nodes[i].polys.empty())
					polyLists[i] = layout.Alloc(static_cast<uint32_t>(m.nodes[i].polys.size() * 2));
				if (!m.nodes[i].cover.empty())
					coverLists[i] = layout.Alloc(static_cast<uint32_t>(m.nodes[i].cover.size() * 8));
			}
		}
		const uint32_t links = m.links.empty() ? 0 : layout.Alloc(static_cast<uint32_t>(m.links.size() * 0x1C));
		const uint32_t linkIndices = m.linkIndices.empty() ? 0 : layout.Alloc(static_cast<uint32_t>(m.linkIndices.size() * 2));
		// the page map last: 16 bytes and a pointer per page (which the game fills in)
		uint32_t pages = static_cast<uint32_t>(layout.used.size());
		uint32_t pageMap = layout.Alloc(0x10 + 8 * pages);
		if (layout.used.size() != pages)
		{
			// (it took a page of its own: one more pointer, and it still fits there)
			pages = static_cast<uint32_t>(layout.used.size());
			layout.used.back() += 8;
		}
		if (pages > kMaxPages)
			return error = std::format("navmesh too large: {} pages of 16 KB (at most {})", pages, kMaxPages), false;
		for (const uint32_t u : layout.used)
			if (u > kPage)
				return error = "an object larger than a page", false;

		loader::convert::Resource r;
		r.version = 2;
		r.virtualFlags = 1u | (pages << 17); // pages of 0x2000 << 1
		r.physicalFlags = 0x20000000;
		Bytes& v = r.virtualBlock;
		v.assign(static_cast<size_t>(pages) * kPage, 0);

		Put<uint64_t>(v, root + 0x00, kVtable);
		Put<uint64_t>(v, root + 0x08, Ptr(pageMap));
		Put<uint32_t>(v, root + 0x10, m.flags);
		Put<uint32_t>(v, root + 0x14, m.format);
		std::memcpy(v.data() + root + 0x20, m.matrix.data(), 0x40);
		std::memcpy(v.data() + root + 0x60, m.size.data(), 16);
		const auto writeSplit = [&](const SplitPlan& p, uint32_t field, const uint8_t* data, size_t count, uint32_t elementSize) {
			Put<uint64_t>(v, root + field, Ptr(p.object));
			Put<uint64_t>(v, p.object + 0x00, kSplitVtable);
			Put<uint32_t>(v, p.object + 0x08, static_cast<uint32_t>(count));
			Put<uint64_t>(v, p.object + 0x10, Ptr(p.list));
			Put<uint64_t>(v, p.object + 0x18, Ptr(p.table));
			Put<uint32_t>(v, p.object + 0x20, static_cast<uint32_t>(p.chunks.size()));
			uint32_t start = 0;
			for (size_t k = 0; k < p.chunks.size(); ++k)
			{
				const auto [at, n] = p.chunks[k];
				Put<uint64_t>(v, p.list + 16 * static_cast<uint32_t>(k), Ptr(at));
				Put<uint32_t>(v, p.list + 16 * static_cast<uint32_t>(k) + 8, n);
				Put<uint32_t>(v, p.table + 4 * static_cast<uint32_t>(k), start);
				std::memcpy(v.data() + at, data + static_cast<size_t>(start) * elementSize, static_cast<size_t>(n) * elementSize);
				start += n;
			}
		};
		writeSplit(vertices, 0x70, reinterpret_cast<const uint8_t*>(m.vertices.data()), m.vertices.size(), 6);
		writeSplit(indices, 0x80, reinterpret_cast<const uint8_t*>(m.indices.data()), m.indices.size(), 2);
		writeSplit(edges, 0x88, reinterpret_cast<const uint8_t*>(m.edges.data()), m.edges.size(), 8);
		writeSplit(polys, 0x118, reinterpret_cast<const uint8_t*>(m.polys.data()), m.polys.size(), 0x30);
		Put<uint32_t>(v, root + 0x90, static_cast<uint32_t>(m.indices.size()));
		Put<uint32_t>(v, root + 0x94, static_cast<uint32_t>(m.adjacent.size()));
		for (size_t k = 0; k < m.adjacent.size(); ++k)
			Put<uint32_t>(v, root + 0x98 + 4 * static_cast<uint32_t>(k), m.adjacent[k]);
		uint32_t cover = 0;
		for (size_t i = 0; i < m.nodes.size(); ++i)
		{
			const Node& n = m.nodes[i];
			const uint32_t at = nodes[i];
			std::memcpy(v.data() + at, n.min.data(), 16);
			std::memcpy(v.data() + at + 0x10, n.max.data(), 16);
			std::memcpy(v.data() + at + 0x20, n.box.data(), 12);
			if (n.leaf)
			{
				Put<uint64_t>(v, at + 0x2C, Ptr(leaves[i]));
				const uint32_t l = leaves[i];
				Put<uint32_t>(v, l + 0x00, n.firstCover);
				if (!n.polys.empty())
				{
					Put<uint64_t>(v, l + 0x08, Ptr(polyLists[i]));
					std::memcpy(v.data() + polyLists[i], n.polys.data(), n.polys.size() * 2);
				}
				if (!n.cover.empty())
				{
					Put<uint64_t>(v, l + 0x10, Ptr(coverLists[i]));
					std::memcpy(v.data() + coverLists[i], n.cover.data(), n.cover.size() * 8);
				}
				Put<uint16_t>(v, l + 0x18, static_cast<uint16_t>(n.polys.size()));
				Put<uint16_t>(v, l + 0x1A, static_cast<uint16_t>(n.cover.size()));
				cover += static_cast<uint32_t>(n.cover.size());
			}
			for (int k = 0; k < 4; ++k)
				if (n.children[k] >= 0)
					Put<uint64_t>(v, at + 0x34 + 8 * k, Ptr(nodes[static_cast<size_t>(n.children[k])]));
		}
		Put<uint64_t>(v, root + 0x118, Ptr(polys.object));
		Put<uint64_t>(v, root + 0x120, Ptr(nodes[0]));
		if (links)
		{
			Put<uint64_t>(v, root + 0x128, Ptr(links));
			std::memcpy(v.data() + links, m.links.data(), m.links.size() * 0x1C);
		}
		if (linkIndices)
		{
			Put<uint64_t>(v, root + 0x130, Ptr(linkIndices));
			std::memcpy(v.data() + linkIndices, m.linkIndices.data(), m.linkIndices.size() * 2);
		}
		Put<uint32_t>(v, root + 0x138, static_cast<uint32_t>(m.vertices.size()));
		Put<uint32_t>(v, root + 0x13C, static_cast<uint32_t>(m.polys.size()));
		Put<uint32_t>(v, root + 0x140, m.index);
		uint32_t used = 0;
		for (const uint32_t u : layout.used)
			used += u;
		Put<uint32_t>(v, root + 0x144, used);
		Put<uint32_t>(v, root + 0x148, cover);
		Put<uint32_t>(v, root + 0x14C, static_cast<uint32_t>(m.links.size()));
		Put<uint32_t>(v, root + 0x150, static_cast<uint32_t>(m.linkIndices.size()));
		Put<uint32_t>(v, root + 0x160, m.tag160);
		v[pageMap + 8] = static_cast<uint8_t>(pages);
		return loader::convert::Write(r, out, error);
	}

	std::string Compare(const Navmesh& a, const Navmesh& b)
	{
		const auto same = [](const auto& x, const auto& y) {
			return x.size() == y.size() && (x.empty() || std::memcmp(x.data(), y.data(), x.size() * sizeof(x[0])) == 0);
		};
		if (a.flags != b.flags || a.format != b.format || a.index != b.index || a.tag160 != b.tag160)
			return "root fields";
		if (a.matrix != b.matrix || a.size != b.size)
			return "matrix or size";
		if (a.adjacent != b.adjacent)
			return "adjacent navmeshes";
		if (!same(a.vertices, b.vertices))
			return "vertices";
		if (!same(a.indices, b.indices))
			return "indices";
		if (!same(a.edges, b.edges))
			return "edges";
		if (!same(a.polys, b.polys))
			return "polygons";
		if (!same(a.links, b.links) || a.linkIndices != b.linkIndices)
			return "special links";
		if (a.nodes.size() != b.nodes.size())
			return "quadtree size";
		for (size_t i = 0; i < a.nodes.size(); ++i)
		{
			const Node& x = a.nodes[i];
			const Node& y = b.nodes[i];
			if (x.min != y.min || x.max != y.max || x.box != y.box || x.leaf != y.leaf || x.firstCover != y.firstCover || x.polys != y.polys ||
			    !same(x.cover, y.cover) || x.children != y.children)
				return std::format("quadtree node {}", i);
		}
		return "";
	}
}
