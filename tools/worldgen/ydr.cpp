#include "ydr.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <map>
#include <optional>

#include "convert/block.hpp"

namespace worldgen::ydr
{
	using loader::convert::Bytes;
	using loader::convert::Put;
	using loader::convert::kVirtual;

	namespace
	{
		// File-form vtables (build base 0x140000000; the game replaces them), as the game's own files carry them.
		constexpr uint64_t kVtDrawable = 0x14068C7F0, kVtShaderGroup = 0x1406BB978, kVtModel = 0x1406BB948, kVtGeometry = 0x1406BB7E8,
		                   kVtVertexBuffer = 0x1406B9108, kVtIndexBuffer = 0x1406B90B8, kVtView = 0x1406B77D8, kVtTextureRef = 0x1406B7940,
		                   kVtDictionary = 0x14068E798;
		constexpr uint32_t kTag = 0x7F800001;
		constexpr uint32_t kVersion = 159;

		// An element's DXGI format and size.
		struct Element
		{
			uint8_t dxgi;
			uint32_t size;
		};
		Element Format(uint8_t slot)
		{
			switch (slot)
			{
			case kPosition:
			case kNormal: return {6, 12};  // R32G32B32_FLOAT
			case kTangent: return {2, 16}; // R32G32B32A32_FLOAT
			case kColour0:
			case kColour1: return {28, 4}; // R8G8B8A8_UNORM
			default: return {16, 8};       // R32G32_FLOAT (texture coordinates)
			}
		}

		uint32_t Stride(const std::vector<uint8_t>& slots)
		{
			uint32_t stride = 0;
			for (const uint8_t s : slots)
				stride += Format(s).size;
			return stride;
		}

		// The resource being written: objects in the converter's working space, every pointer recorded where it is put.
		struct Out
		{
			loader::convert::Block blk;
			std::vector<size_t> pointers;
			bool full = false;

			size_t Alloc(size_t size, size_t align = 16)
			{
				const auto at = blk.Alloc(std::max<size_t>(size, 1), align);
				if (!at)
				{
					full = true;
					return 0;
				}
				std::fill(blk.data.begin() + *at, blk.data.begin() + *at + size, uint8_t{0});
				return *at;
			}
			void Ptr(size_t at, size_t target)
			{
				Put<uint64_t>(blk.data, at, kVirtual + target);
				pointers.push_back(at);
			}
			size_t String(const std::string& text)
			{
				const size_t at = Alloc(text.size() + 1, 1);
				std::memcpy(blk.data.data() + at, text.data(), text.size());
				return at;
			}
			template <typename T>
			void Set(size_t at, T value)
			{
				Put<T>(blk.data, at, value);
			}
			void Vec(size_t at, const std::array<float, 3>& v, uint32_t w)
			{
				Set<float>(at, v[0]);
				Set<float>(at + 4, v[1]);
				Set<float>(at + 8, v[2]);
				Set<uint32_t>(at + 12, w);
			}
		};

		void View(Out& o, size_t at)
		{
			o.Set<uint64_t>(at, kVtView);
			o.Set<uint16_t>(at + 0x10, 0x14);
			std::fill(o.blk.data.begin() + at + 0x12, o.blk.data.begin() + at + 0x18, uint8_t{0xFF});
		}

		// A shader instance with its parameter table (research/phase0.md §15, §16): textures in our array (their slots
		// in the table), constants back to back in buffer 0 of the parameter block (16-byte aligned).
		size_t Shader(Out& o, const Material& m)
		{
			Bytes constants;
			std::vector<std::pair<uint32_t, uint32_t>> table;
			for (size_t i = 0; i < m.textures.size(); ++i)
				table.emplace_back(m.textures[i].first, static_cast<uint32_t>(i << 2));
			for (const auto& [name, bytes] : m.constants)
			{
				const uint32_t offset = static_cast<uint32_t>(constants.size());
				table.emplace_back(name, 3u | (offset << 8) | (static_cast<uint32_t>(bytes.size()) << 20));
				constants.insert(constants.end(), bytes.begin(), bytes.end());
				constants.resize((constants.size() + 15) & ~size_t{15}, 0);
			}
			const size_t block = (std::max<size_t>(m.block ? m.block : 4096, 0x10 + constants.size()) + 15) & ~size_t{15};
			const size_t inst = o.Alloc(0x40);
			const size_t meta = o.Alloc(8 + 8 * table.size());
			const size_t params = o.Alloc(block);
			const size_t textures = o.Alloc(8 * std::max<size_t>(1, m.textures.size()));
			if (o.full)
				return 0;
			o.Set<uint8_t>(meta + 0, 1);
			o.Set<uint8_t>(meta + 1, static_cast<uint8_t>(m.textures.size()));
			o.Set<uint8_t>(meta + 4, static_cast<uint8_t>(table.size()));
			o.Set<uint8_t>(meta + 7, 0x0C);
			for (size_t i = 0; i < table.size(); ++i)
			{
				o.Set<uint32_t>(meta + 8 + 8 * i, table[i].first);
				o.Set<uint32_t>(meta + 12 + 8 * i, table[i].second);
			}
			o.Ptr(params, params + 0x10);
			std::copy(constants.begin(), constants.end(), o.blk.data.begin() + params + 0x10);
			for (size_t i = 0; i < m.textures.size(); ++i)
			{
				const size_t ref = o.Alloc(0x50);
				const size_t name = o.String(m.textures[i].second);
				if (o.full)
					return 0;
				o.Set<uint64_t>(ref, kVtTextureRef);
				o.Set<uint32_t>(ref + 0x10, 0x00260000);
				o.Set<uint16_t>(ref + 0x1C, 1);
				o.Set<uint16_t>(ref + 0x1E, 1);
				o.Set<uint8_t>(ref + 0x20, 0xFF);
				o.Set<uint16_t>(ref + 0x22, 1);
				o.Set<uint16_t>(ref + 0x26, 1);
				o.Ptr(ref + 0x28, name);
				o.Ptr(textures + 8 * i, ref);
			}
			o.Set<uint32_t>(inst, m.effect);
			o.Set<uint32_t>(inst + 4, 0x6D657461); // "meta": the table at +0x20 is there
			o.Ptr(inst + 0x08, params);
			o.Ptr(inst + 0x10, textures);
			o.Ptr(inst + 0x20, meta);
			o.Set<uint8_t>(inst + 0x39, m.bucket);
			o.Set<uint16_t>(inst + 0x3A, static_cast<uint16_t>(block));
			o.Set<uint8_t>(inst + 0x3C, 1);
			o.Set<uint8_t>(inst + 0x3D, 0xFF);
			return inst;
		}

		// A vertex declaration (0x140): each slot's offset (an absent slot: the next present one's), each present
		// slot's stride at +0xD0 and DXGI format at +0x104, the stride << 2 at +0x138.
		size_t Declaration(Out& o, const std::vector<uint8_t>& slots)
		{
			const size_t at = o.Alloc(0x140);
			if (o.full)
				return 0;
			const uint32_t stride = Stride(slots);
			uint32_t running = 0;
			for (int slot = 0; slot < 52; ++slot)
			{
				o.Set<uint32_t>(at + 4 * slot, running);
				if (std::find(slots.begin(), slots.end(), static_cast<uint8_t>(slot)) != slots.end())
				{
					o.Set<uint8_t>(at + 0xD0 + slot, static_cast<uint8_t>(stride));
					o.Set<uint8_t>(at + 0x104 + slot, Format(static_cast<uint8_t>(slot)).dxgi);
					running += Format(static_cast<uint8_t>(slot)).size;
				}
			}
			o.Set<uint32_t>(at + 0x138, stride << 2);
			return at;
		}

		void PutVertex(uint8_t* p, const Vertex& v, const std::vector<uint8_t>& slots)
		{
			for (const uint8_t s : slots)
			{
				switch (s)
				{
				case kPosition: std::memcpy(p, v.position.data(), 12); break;
				case kNormal: std::memcpy(p, v.normal.data(), 12); break;
				case kTangent: std::memcpy(p, v.tangent.data(), 16); break;
				case kColour0: std::memcpy(p, v.colour0.data(), 4); break;
				case kColour1: std::memcpy(p, v.colour1.data(), 4); break;
				default: std::memcpy(p, v.uv[s - kUv0].data(), 8); break;
				}
				p += Format(s).size;
			}
		}

		void MeshBox(const Mesh& m, std::array<float, 3>& lo, std::array<float, 3>& hi)
		{
			lo = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
			hi = {-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
			for (const Vertex& v : m.vertices)
				for (int a = 0; a < 3; ++a)
				{
					lo[a] = std::min(lo[a], v.position[a]);
					hi[a] = std::max(hi[a], v.position[a]);
				}
		}
	}

	void Material::Texture(uint32_t parameter, const std::string& name)
	{
		for (auto& [p, n] : textures)
			if (p == parameter)
			{
				n = name;
				return;
			}
		textures.emplace_back(parameter, name);
	}

	bool Material::Constant(uint32_t parameter, float value)
	{
		for (auto& [p, bytes] : constants)
			if (p == parameter && bytes.size() >= 4)
			{
				std::memcpy(bytes.data(), &value, 4);
				return true;
			}
		return false;
	}

	bool ReadMaterial(const ue5export::drawable::View& view, uint64_t at, uint32_t index, Material& out, std::string& error)
	{
		const uint64_t group = view.Get<uint64_t>(at + 0x10);
		if (index >= view.Get<uint16_t>(group + 0x18))
			return error = std::format("no shader {}", index), false;
		const uint64_t s = view.Get<uint64_t>(view.Get<uint64_t>(group + 0x10) + 8u * index);
		out = {};
		out.effect = view.Get<uint32_t>(s);
		out.bucket = view.Get<uint8_t>(s + 0x39);
		out.block = view.Get<uint16_t>(s + 0x3A);
		const uint64_t meta = view.Get<uint64_t>(s + 0x20), textures = view.Get<uint64_t>(s + 0x10), buffers = view.Get<uint64_t>(s + 0x08);
		const uint8_t entries = view.Get<uint8_t>(meta + 4);
		for (uint8_t i = 0; i < entries; ++i)
		{
			const uint32_t name = view.Get<uint32_t>(meta + 8 + 8u * i), info = view.Get<uint32_t>(meta + 12 + 8u * i);
			if ((info & 3) == 0)
			{
				const uint64_t ref = view.Get<uint64_t>(textures + 8u * ((info >> 2) & 0x3F));
				if (ref)
					out.textures.emplace_back(name, view.String(view.Get<uint64_t>(ref + 0x28)));
			}
			else if ((info & 3) == 3)
			{
				const uint32_t size = info >> 20;
				const uint8_t* p = view.At(view.Get<uint64_t>(buffers + 8u * ((info >> 2) & 0x3F)) + ((info >> 8) & 0xFFF), size);
				if (p)
					out.constants.emplace_back(name, std::vector<uint8_t>(p, p + size));
			}
		}
		// the vertex elements: the first geometry drawn with it
		for (int lod = 0; lod < 4 && out.slots.empty(); ++lod)
		{
			const uint64_t list = view.Get<uint64_t>(at + 0x50 + 8u * lod);
			if (!list)
				continue;
			const uint64_t models = view.Get<uint64_t>(list);
			for (uint16_t m = 0; m < view.Get<uint16_t>(list + 8) && out.slots.empty(); ++m)
			{
				const uint64_t model = view.Get<uint64_t>(models + 8u * m);
				const uint64_t geometries = view.Get<uint64_t>(model + 0x08), mapping = view.Get<uint64_t>(model + 0x20);
				for (uint16_t g = 0; g < view.Get<uint16_t>(model + 0x10) && out.slots.empty(); ++g)
				{
					if (view.Get<uint16_t>(mapping + 2u * g) != index)
						continue;
					const uint64_t vb = view.Get<uint64_t>(view.Get<uint64_t>(geometries + 8u * g) + 0x18);
					if (const uint8_t* decl = view.At(view.Get<uint64_t>(vb + 0x38), 0x140))
						for (uint8_t slot = 0; slot < 36; ++slot)
							if (decl[0x104 + slot])
								out.slots.push_back(slot);
				}
			}
		}
		for (const uint8_t slot : out.slots)
			if (slot != kPosition && slot != kNormal && slot != kTangent && slot != kColour0 && slot != kColour1 && (slot < kUv0 || slot > kUv3))
				return error = std::format("vertex element in slot {} not supported", slot), false;
		if (out.slots.empty())
			return error = std::format("no geometry is drawn with shader {}", index), false;
		return true;
	}

	void Box(const Drawable& d, std::array<float, 3>& min, std::array<float, 3>& max)
	{
		min = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
		max = {-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
		for (const Mesh& m : d.lods[0])
		{
			std::array<float, 3> lo, hi;
			MeshBox(m, lo, hi);
			for (int a = 0; a < 3; ++a)
			{
				min[a] = std::min(min[a], lo[a]);
				max[a] = std::max(max[a], hi[a]);
			}
		}
	}

	namespace
	{
		bool Check(const Drawable& d, std::string& error)
		{
			if (d.lods[0].empty())
				return error = "no meshes", false;
			for (const auto& lod : d.lods)
				for (const Mesh& m : lod)
					if (m.material >= d.materials.size() || m.vertices.empty() || m.vertices.size() > 0xFFFF || m.indices.empty() || m.indices.size() % 3 ||
					    std::any_of(m.indices.begin(), m.indices.end(), [&](uint32_t i) { return i >= m.vertices.size(); }))
						return error = "a mesh with no or too many vertices (65535), a bad index or no material", false;
			return true;
		}

		// The drawable and everything it points to, from `root` (0xD0 bytes the caller allocated).
		bool Put(Out& o, size_t root, const Drawable& d, std::string& error);

		// The page map (room for every page: the game fills in their addresses) at `root` + 8, then the pages packed.
		bool Finish(Out& o, size_t root, Bytes& file, std::string& error)
		{
			const size_t pageMap = o.Alloc(16 + 8 * 256);
			if (o.full)
				return error = "out of space", false;
			o.Ptr(root + 0x08, pageMap);
			loader::convert::Resource res;
			uint32_t pageCount = 0;
			if (!o.blk.Relayout(res.virtualBlock, res.virtualFlags, pageCount, error, &o.pointers))
				return false;
			const size_t newPageMap = static_cast<size_t>(loader::convert::Get<uint64_t>(res.virtualBlock, 8) - kVirtual);
			res.virtualBlock[newPageMap + 8] = static_cast<uint8_t>(pageCount);
			res.version = kVersion;
			res.virtualFlags |= (kVersion >> 4) << 28;
			res.physicalFlags = (kVersion & 0xF) << 28;
			return loader::convert::Write(res, file, error);
		}
	}

	bool Write(const Drawable& d, Bytes& file, std::string& error)
	{
		if (!Check(d, error))
			return false;
		Out o;
		const size_t root = o.Alloc(0xD0);
		if (root != 0)
			return error = "the drawable is not at the start", false;
		return Put(o, root, d, error) && Finish(o, root, file, error);
	}

	bool WriteDictionary(const std::vector<std::pair<uint32_t, Drawable>>& drawables, Bytes& file, std::string& error)
	{
		if (drawables.empty() || drawables.size() > 0xFFFF)
			return error = "no drawables", false;
		// (the game looks a name up by halving: the hashes in order, the drawables in theirs)
		std::vector<size_t> order(drawables.size());
		for (size_t i = 0; i < order.size(); ++i)
			order[i] = i;
		std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return drawables[a].first < drawables[b].first; });
		for (size_t i = 0; i + 1 < order.size(); ++i)
			if (drawables[order[i]].first == drawables[order[i + 1]].first)
				return error = std::format("two drawables named {:08x}", drawables[order[i]].first), false;
		for (const auto& [hash, d] : drawables)
			if (!Check(d, error))
				return error = std::format("drawable {:08x}: {}", hash, error), false;
		// Dictionary (0x40): +0x08 page map, +0x18 1, +0x20 the hashes {u32s, u16 count, capacity}, +0x30 the drawables.
		Out o;
		const size_t root = o.Alloc(0x40);
		if (root != 0)
			return error = "the dictionary is not at the start", false;
		const size_t n = drawables.size();
		const size_t hashes = o.Alloc(4 * n), list = o.Alloc(8 * n);
		o.Set<uint64_t>(root, kVtDictionary);
		o.Set<uint32_t>(root + 0x18, 1);
		o.Ptr(root + 0x20, hashes);
		o.Set<uint16_t>(root + 0x28, static_cast<uint16_t>(n));
		o.Set<uint16_t>(root + 0x2A, static_cast<uint16_t>(n));
		o.Ptr(root + 0x30, list);
		o.Set<uint16_t>(root + 0x38, static_cast<uint16_t>(n));
		o.Set<uint16_t>(root + 0x3A, static_cast<uint16_t>(n));
		for (size_t i = 0; i < n; ++i)
		{
			const auto& [hash, d] = drawables[order[i]];
			const size_t at = o.Alloc(0xD0);
			if (o.full)
				return error = "out of space", false;
			o.Set<uint32_t>(hashes + 4 * i, hash);
			o.Ptr(list + 8 * i, at);
			if (!Put(o, at, d, error))
				return false;
		}
		return Finish(o, root, file, error);
	}

	namespace
	{
		bool Put(Out& o, size_t root, const Drawable& d, std::string& error)
		{
			o.Set<uint64_t>(root, kVtDrawable);
			std::array<float, 3> min, max;
			Box(d, min, max);
			const std::array<float, 3> centre{(min[0] + max[0]) / 2, (min[1] + max[1]) / 2, (min[2] + max[2]) / 2};
			float radius = 0;
			for (const Mesh& m : d.lods[0])
				for (const Vertex& v : m.vertices)
					radius = std::max(radius, std::hypot(v.position[0] - centre[0], v.position[1] - centre[1], v.position[2] - centre[2]));
			o.Set<float>(root + 0x20, centre[0]);
			o.Set<float>(root + 0x24, centre[1]);
			o.Set<float>(root + 0x28, centre[2]);
			o.Set<float>(root + 0x2C, radius);
			o.Vec(root + 0x30, min, kTag);
			o.Vec(root + 0x40, max, kTag);
			for (int lod = 0; lod < 4; ++lod)
				o.Set<float>(root + 0x70 + 4 * lod, d.lodDistances[lod]);
			o.Set<uint8_t>(root + 0x80, 1);
			o.Set<uint8_t>(root + 0x81, 0xFF);
			o.Ptr(root + 0xA8, o.String(d.name + ".#dr"));

			// shaders
			const size_t group = o.Alloc(0x40), shaders = o.Alloc(8 * d.materials.size());
			o.Set<uint64_t>(group, kVtShaderGroup);
			o.Ptr(group + 0x10, shaders);
			o.Set<uint16_t>(group + 0x18, static_cast<uint16_t>(d.materials.size()));
			o.Set<uint16_t>(group + 0x1A, static_cast<uint16_t>(d.materials.size()));
			o.Ptr(root + 0x10, group);
			std::map<std::vector<uint8_t>, size_t> declarations;
			for (size_t i = 0; i < d.materials.size(); ++i)
			{
				const size_t s = Shader(o, d.materials[i]);
				if (o.full)
					return error = "out of space", false;
				o.Ptr(shaders + 8 * i, s);
			}

			// a model per level of detail, a geometry per mesh
			for (int lod = 0; lod < 4; ++lod)
			{
				const auto& meshes = d.lods[lod];
				if (meshes.empty())
					continue;
				const size_t n = meshes.size();
				const size_t list = o.Alloc(0x10), models = o.Alloc(8), model = o.Alloc(0x30);
				const size_t geometries = o.Alloc(8 * n), boxes = o.Alloc(32 * (n > 1 ? n + 1 : 1)), mapping = o.Alloc(2 * n);
				o.Ptr(root + 0x50 + 8 * lod, list);
				o.Ptr(list, models);
				o.Set<uint16_t>(list + 8, 1);
				o.Set<uint16_t>(list + 10, 1);
				o.Ptr(models, model);
				o.Set<uint64_t>(model, kVtModel);
				o.Ptr(model + 0x08, geometries);
				o.Set<uint16_t>(model + 0x10, static_cast<uint16_t>(n));
				o.Set<uint16_t>(model + 0x12, static_cast<uint16_t>(n));
				o.Ptr(model + 0x18, boxes);
				o.Ptr(model + 0x20, mapping);
				o.Set<uint8_t>(model + 0x2C, 0xFF);
				o.Set<uint16_t>(model + 0x2E, static_cast<uint16_t>(n));
				std::array<float, 3> all0{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()},
				    all1{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
				for (size_t g = 0; g < n; ++g)
				{
					const Mesh& m = meshes[g];
					const Material& mat = d.materials[m.material];
					const uint32_t stride = Stride(mat.slots);
					auto decl = declarations.find(mat.slots);
					if (decl == declarations.end())
						decl = declarations.emplace(mat.slots, Declaration(o, mat.slots)).first;
					const size_t geo = o.Alloc(0xA0), vb = o.Alloc(0x40), vbView = o.Alloc(0x20), ib = o.Alloc(0x40), ibView = o.Alloc(0x20);
					const size_t vertices = o.Alloc(static_cast<size_t>(stride) * m.vertices.size()), indices = o.Alloc(2 * m.indices.size());
					if (o.full)
						return error = "out of space", false;
					for (size_t v = 0; v < m.vertices.size(); ++v)
						PutVertex(o.blk.data.data() + vertices + v * stride, m.vertices[v], mat.slots);
					for (size_t i = 0; i < m.indices.size(); ++i)
						o.Set<uint16_t>(indices + 2 * i, static_cast<uint16_t>(m.indices[i]));
					o.Set<uint64_t>(vb, kVtVertexBuffer);
					o.Set<uint32_t>(vb + 0x08, static_cast<uint32_t>(m.vertices.size()));
					o.Set<uint16_t>(vb + 0x0C, static_cast<uint16_t>(stride));
					o.Set<uint32_t>(vb + 0x10, 0x00580409);
					o.Ptr(vb + 0x18, vertices);
					o.Ptr(vb + 0x30, vbView);
					o.Ptr(vb + 0x38, decl->second);
					View(o, vbView);
					o.Set<uint64_t>(ib, kVtIndexBuffer);
					o.Set<uint32_t>(ib + 0x08, static_cast<uint32_t>(m.indices.size()));
					o.Set<uint16_t>(ib + 0x0C, 2);
					o.Set<uint32_t>(ib + 0x10, 0x0058020A);
					o.Ptr(ib + 0x18, indices);
					o.Ptr(ib + 0x30, ibView);
					View(o, ibView);
					o.Set<uint64_t>(geo, kVtGeometry);
					o.Ptr(geo + 0x18, vb);
					o.Ptr(geo + 0x38, ib);
					o.Set<uint32_t>(geo + 0x58, static_cast<uint32_t>(m.indices.size()));
					o.Set<uint32_t>(geo + 0x5C, static_cast<uint32_t>(m.indices.size() / 3));
					o.Set<uint16_t>(geo + 0x60, static_cast<uint16_t>(m.vertices.size()));
					o.Set<uint16_t>(geo + 0x62, 3);
					o.Set<uint16_t>(geo + 0x70, static_cast<uint16_t>(stride));
					o.Ptr(geo + 0x78, vertices);
					o.Ptr(geometries + 8 * g, geo);
					o.Set<uint16_t>(mapping + 2 * g, static_cast<uint16_t>(m.material));
					std::array<float, 3> lo, hi;
					MeshBox(m, lo, hi);
					// (each box's w repeats its x, as in the game's files)
					const size_t box = boxes + 32 * (n > 1 ? g + 1 : 0);
					uint32_t wx0, wx1;
					std::memcpy(&wx0, &lo[0], 4);
					std::memcpy(&wx1, &hi[0], 4);
					o.Vec(box, lo, wx0);
					o.Vec(box + 16, hi, wx1);
					for (int a = 0; a < 3; ++a)
					{
						all0[a] = std::min(all0[a], lo[a]);
						all1[a] = std::max(all1[a], hi[a]);
					}
				}
				if (n > 1)
				{
					uint32_t wx0, wx1;
					std::memcpy(&wx0, &all0[0], 4);
					std::memcpy(&wx1, &all1[0], 4);
					o.Vec(boxes, all0, wx0);
					o.Vec(boxes + 16, all1, wx1);
				}
			}
			return !o.full || (error = "out of space", false);
		}
	}
}
