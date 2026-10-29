#include "ynd.hpp"

#include <cstring>

namespace worldgen::ynd
{
	using loader::convert::Bytes;
	using loader::convert::Get;
	using loader::convert::Put;

	namespace
	{
		constexpr uint64_t kVtable = 0x1407466E0; // what the game's own files carry there
		constexpr uint32_t kRoot = 0x70;

		uint32_t Offset(uint64_t pointer)
		{
			return pointer ? static_cast<uint32_t>(pointer - loader::convert::kVirtual) : 0;
		}
	}

	uint16_t Node::Area() const { uint16_t v; std::memcpy(&v, raw + 0x10, 2); return v; }
	uint16_t Node::Index() const { uint16_t v; std::memcpy(&v, raw + 0x12, 2); return v; }
	uint16_t Node::FirstLink() const { uint16_t v; std::memcpy(&v, raw + 0x1A, 2); return v; }
	int16_t Node::X() const { int16_t v; std::memcpy(&v, raw + 0x1C, 2); return v; }
	int16_t Node::Y() const { int16_t v; std::memcpy(&v, raw + 0x1E, 2); return v; }
	int16_t Node::Z() const { int16_t v; std::memcpy(&v, raw + 0x22, 2); return v; }
	uint32_t Node::LinkCount() const { return raw[0x25] >> 3; }
	void Node::SetArea(uint16_t v) { std::memcpy(raw + 0x10, &v, 2); }
	void Node::SetIndex(uint16_t v) { std::memcpy(raw + 0x12, &v, 2); }
	void Node::SetFirstLink(uint16_t v) { std::memcpy(raw + 0x1A, &v, 2); }

	bool Read(const Bytes& file, Region& out, std::string& error)
	{
		loader::convert::Resource r;
		if (!loader::convert::Read(file, r, error))
			return false;
		const Bytes& v = r.virtualBlock;
		if (v.size() < kRoot)
		{
			error = "too small";
			return false;
		}
		const uint32_t nodes = Offset(Get<uint64_t>(v, 0x10));
		const uint32_t count = Get<uint32_t>(v, 0x18);
		out.vehicleNodes = Get<uint32_t>(v, 0x1C);
		const uint32_t links = Offset(Get<uint64_t>(v, 0x28));
		const uint32_t linkCount = Get<uint32_t>(v, 0x30);
		const uint32_t junctions = Offset(Get<uint64_t>(v, 0x38));
		const uint32_t heights = Offset(Get<uint64_t>(v, 0x40));
		out.unknown48 = Get<uint32_t>(v, 0x48);
		const uint32_t refs = Offset(Get<uint64_t>(v, 0x50));
		const uint16_t refCount = Get<uint16_t>(v, 0x58);
		const uint32_t junctionCount = Get<uint32_t>(v, 0x60);
		const uint32_t heightBytes = Get<uint32_t>(v, 0x64);
		if (nodes + count * 0x28ull > v.size() || links + linkCount * 8ull > v.size() || junctions + junctionCount * 12ull > v.size() ||
		    heights + heightBytes > v.size() || refs + refCount * 8ull > v.size() || out.vehicleNodes > count)
		{
			error = "arrays outside the block";
			return false;
		}
		out.nodes.resize(count);
		for (uint32_t i = 0; i < count; ++i)
			std::memcpy(out.nodes[i].raw, v.data() + nodes + i * 0x28, 0x28);
		out.links.resize(linkCount);
		if (linkCount)
			std::memcpy(out.links.data(), v.data() + links, linkCount * 8);
		out.junctions.resize(junctionCount);
		if (junctionCount)
			std::memcpy(out.junctions.data(), v.data() + junctions, junctionCount * 12);
		out.heights.assign(v.begin() + heights, v.begin() + heights + heightBytes);
		out.refs.resize(refCount);
		if (refCount)
			std::memcpy(out.refs.data(), v.data() + refs, refCount * 8);
		return true;
	}

	bool Write(const Region& region, Bytes& out, std::string& error)
	{
		auto align = [](uint32_t x) { return (x + 15) & ~15u; };
		// one page: root, nodes, links, junctions, their height maps, the refs, then the page map
		const uint32_t nodes = kRoot;
		const uint32_t links = align(nodes + static_cast<uint32_t>(region.nodes.size()) * 0x28);
		const uint32_t junctions = align(links + static_cast<uint32_t>(region.links.size()) * 8);
		const uint32_t heights = align(junctions + static_cast<uint32_t>(region.junctions.size()) * 12);
		const uint32_t refs = align(heights + static_cast<uint32_t>(region.heights.size()));
		const uint32_t pageMap = align(refs + static_cast<uint32_t>(region.refs.size()) * 8);
		const uint32_t used = pageMap + 24;
		uint32_t shift = 0;
		while ((0x2000u << shift) < used)
			++shift;
		if (shift > 15 || region.nodes.size() > 0xFFFF || region.refs.size() > 0xFFFF)
		{
			error = "region too large";
			return false;
		}
		loader::convert::Resource r;
		r.version = 1;
		r.virtualFlags = shift | (1u << 17); // one page of 0x2000 << shift
		r.physicalFlags = 0x10000000;
		Bytes& v = r.virtualBlock;
		v.assign(0x2000u << shift, 0);
		auto ptr = [](uint32_t offset) { return static_cast<uint64_t>(loader::convert::kVirtual) + offset; };
		Put<uint64_t>(v, 0x00, kVtable);
		Put<uint64_t>(v, 0x08, ptr(pageMap));
		Put<uint64_t>(v, 0x10, ptr(nodes)); // (also when there are none: a region with no nodes array reads as not loaded)
		Put<uint32_t>(v, 0x18, static_cast<uint32_t>(region.nodes.size()));
		Put<uint32_t>(v, 0x1C, region.vehicleNodes);
		Put<uint32_t>(v, 0x20, static_cast<uint32_t>(region.nodes.size()) - region.vehicleNodes);
		Put<uint64_t>(v, 0x28, ptr(links));
		Put<uint32_t>(v, 0x30, static_cast<uint32_t>(region.links.size()));
		Put<uint64_t>(v, 0x38, ptr(junctions));
		Put<uint64_t>(v, 0x40, ptr(heights));
		Put<uint32_t>(v, 0x48, region.unknown48);
		Put<uint64_t>(v, 0x50, ptr(refs));
		Put<uint16_t>(v, 0x58, static_cast<uint16_t>(region.refs.size()));
		Put<uint16_t>(v, 0x5A, static_cast<uint16_t>(region.refs.size()));
		Put<uint32_t>(v, 0x60, static_cast<uint32_t>(region.junctions.size()));
		Put<uint32_t>(v, 0x64, static_cast<uint32_t>(region.heights.size()));
		for (size_t i = 0; i < region.nodes.size(); ++i)
			std::memcpy(v.data() + nodes + i * 0x28, region.nodes[i].raw, 0x28);
		if (!region.links.empty())
			std::memcpy(v.data() + links, region.links.data(), region.links.size() * 8);
		if (!region.junctions.empty())
			std::memcpy(v.data() + junctions, region.junctions.data(), region.junctions.size() * 12);
		if (!region.heights.empty())
			std::memcpy(v.data() + heights, region.heights.data(), region.heights.size());
		if (!region.refs.empty())
			std::memcpy(v.data() + refs, region.refs.data(), region.refs.size() * 8);
		v[pageMap + 8] = 1; // the page map: one virtual page (its pointer filled in by the game)
		return loader::convert::Write(r, out, error);
	}
}
