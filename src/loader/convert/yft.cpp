#include "yft.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <optional>
#include <set>

#include "block.hpp"
#include "ytd.hpp"

namespace loader::convert
{
	namespace
	{
		// Enhanced file-form vtables (build base 0x140000000; the game replaces them).
		constexpr uint64_t kVtVertexBuffer = 0x1406B9108, kVtIndexBuffer = 0x1406B90B8, kVtView = 0x1406B77D8, kVtTextureRef = 0x1406B7940;

		// Legacy declaration bit -> Enhanced slot: position, weights, indices, normal, colour 0/1, UV 0-7, tangent, binormal.
		constexpr std::array<int, 16> kBitToSlot = {0, 16, 20, 4, 24, 25, 28, 29, 30, 31, 32, 33, 34, 35, 8, 12};

		// Legacy texture parameter (sampler) name -> Enhanced texture name. Learned by pairing legacy files with
		// the game's own Enhanced conversions of the same files through the textures they reference
		// (tools/learn_texture_names.py, research/phase0.md §18); DamageSampler from the vehicle effects.
		// The mapping does not depend on the effect.
		constexpr std::pair<uint32_t, uint32_t> kTextureNames[] = {
			{0x09bbebe0, 0xf9c0e345},
			{0x0ad3a268, 0x0df47048},
			{0x2420afd1, 0xd883aa9e},
			{0x2e8e5039, 0xf6acd6d8},
			{0x2f0b625e, 0xe8de684f},
			{0x31934ab6, 0x32b9df09},
			{0x3fff9563, 0x27dbd2be},
			{0x46b7c64f, 0x1e77bf4c},
			{0x50022388, 0x1f385efc},
			{0x54cdbeff, 0x7e1f7f44},
			{0x608799c6, 0xc6f46e79},
			{0x65df0bce, 0x6c3fb130},
			{0x78b758fd, 0x3d10f3b7},
			{0x7a141f5c, 0xca588abf},
			{0x7e9a27fe, 0xa6f4cf58},
			{0x7f354429, 0xf5805ad5},
			{0x85b0fe81, 0x1761183c},
			{0x86cb389d, 0xf19d5b70},
			{0x883768d9, 0x0ce80993},
			{0x88cc3d90, 0xc075f9b6},
			{0x8be08ae0, 0x4041e9fd},
			{0x92bc3625, 0xd5a68397},
			{0x9936a58c, 0x31f34d60},
			{0xa3a2dca8, 0x4c759bf1},
			{0xab98831e, 0x340f6e97},
			{0xb0660bce, 0xf04e58de},
			{0xb1597815, 0xde84c011},
			{0xbb302c18, 0xca3af504},
			{0xbbf891f0, 0x91cae00c},
			{0xbc38845d, 0x3959fca2},
			{0xbda82652, 0xe1f3bc29},
			{0xca4299e4, 0x8dc1631c},
			{0xd4a3d449, 0x020efc53},
			{0xd52b11df, 0xce7b968e},
			{0xd5588afc, 0xdf186a0e},
			{0xf165e62b, 0xf7864a2a},
			{0xf1fe2b71, 0xc8e8c282},
			{0xf648a067, 0x2c8d7182},
		};

		std::optional<uint32_t> EnhancedTextureName(uint32_t legacy)
		{
			for (const auto& [from, to] : kTextureNames)
				if (from == legacy)
					return to;
			return std::nullopt;
		}

		struct LegacyType
		{
			uint32_t size;
			uint8_t dxgi;
		};
		std::optional<LegacyType> TypeOf(uint32_t type)
		{
			switch (type)
			{
			case 0: return LegacyType{2, 54};  // half      -> R16_FLOAT
			case 1: return LegacyType{4, 34};  // half2     -> R16G16_FLOAT
			case 3: return LegacyType{8, 10};  // half4     -> R16G16B16A16_FLOAT
			case 4: return LegacyType{4, 41};  // float     -> R32_FLOAT
			case 5: return LegacyType{8, 16};  // float2    -> R32G32_FLOAT
			case 6: return LegacyType{12, 6};  // float3    -> R32G32B32_FLOAT
			case 7: return LegacyType{16, 2};  // float4    -> R32G32B32A32_FLOAT
			case 8: return LegacyType{4, 30};  // ubyte4    -> R8G8B8A8_UINT
			case 9: return LegacyType{4, 28};  // colour    -> R8G8B8A8_UNORM
			default: return std::nullopt;
			}
		}

		struct Element
		{
			uint32_t legacyOffset, size;
			int slot;
			uint8_t dxgi;
		};

		struct Declaration
		{
			size_t at; // Enhanced declaration
			std::vector<Element> elements; // sorted by slot
			uint32_t stride;
		};

		bool ConvertDeclaration(Block& blk, size_t o, Declaration& out, std::string& error)
		{
			const uint32_t mask = blk.U32(o);
			const uint32_t stride = blk.U16(o + 4);
			const uint64_t types = blk.U64(o + 8);
			uint32_t offset = 0;
			for (int bit = 0; bit < 16; ++bit)
			{
				if (!(mask & (1u << bit)))
					continue;
				const auto type = TypeOf((types >> (4 * bit)) & 0xF);
				if (!type)
				{
					error = std::format("vertex element type {} not supported", (types >> (4 * bit)) & 0xF);
					return false;
				}
				const int slot = kBitToSlot[bit];
				out.elements.push_back({offset, type->size, slot, slot == 20 && type->size == 4 ? uint8_t{30} : type->dxgi});
				offset += type->size;
			}
			if (offset != stride)
			{
				error = std::format("vertex elements add up to {:#x}, stride {:#x}", offset, stride);
				return false;
			}
			std::sort(out.elements.begin(), out.elements.end(), [](const Element& a, const Element& b) { return a.slot < b.slot; });
			out.stride = stride;
			const auto at = blk.Alloc(0x140);
			if (!at)
			{
				error = "out of space";
				return false;
			}
			out.at = *at;
			// u32 offset per slot (absent slots: offset of the next present one), u8 stride per present slot at
			// +0xD0, u8 DXGI format per slot at +0x104, stride << 2 at +0x138.
			uint32_t running = 0;
			for (int slot = 0; slot < 52; ++slot)
			{
				Put<uint32_t>(blk.data, out.at + 4 * slot, running);
				for (const Element& e : out.elements)
					if (e.slot == slot)
					{
						running += e.size;
						blk.data[out.at + 0xD0 + slot] = static_cast<uint8_t>(stride);
						blk.data[out.at + 0x104 + slot] = e.dxgi;
					}
			}
			Put<uint32_t>(blk.data, out.at + 0x138, stride << 2);
			return true;
		}

		void ReorderVertices(Bytes& b, size_t data, uint32_t count, uint32_t stride, const std::vector<Element>& elements)
		{
			Bytes vertex(stride);
			for (uint32_t v = 0; v < count; ++v)
			{
				uint8_t* p = b.data() + data + static_cast<size_t>(v) * stride;
				size_t at = 0;
				for (const Element& e : elements)
				{
					memcpy(vertex.data() + at, p + e.legacyOffset, e.size);
					at += e.size;
				}
				memcpy(p, vertex.data(), stride);
			}
		}

		void WriteView(Bytes& b, size_t o)
		{
			std::fill(b.begin() + o, b.begin() + o + 0x20, uint8_t{0});
			Put<uint64_t>(b, o, kVtView);
			Put<uint16_t>(b, o + 0x10, 0x14);
			std::fill(b.begin() + o + 0x12, b.begin() + o + 0x18, uint8_t{0xFF});
		}

		void ConvertTextureRef(Bytes& b, size_t o)
		{
			if (Get<uint32_t>(b, o + 8))
				return; // a real texture, not a by-name reference
			const uint64_t name = Get<uint64_t>(b, o + 0x28);
			std::fill(b.begin() + o, b.begin() + o + 0x50, uint8_t{0});
			Put<uint64_t>(b, o, kVtTextureRef);
			Put<uint32_t>(b, o + 0x10, 0x00260000);
			Put<uint16_t>(b, o + 0x1C, 1);
			Put<uint16_t>(b, o + 0x1E, 1);
			b[o + 0x20] = 0xFF;
			Put<uint16_t>(b, o + 0x22, 1);
			Put<uint16_t>(b, o + 0x26, 1);
			Put<uint64_t>(b, o + 0x28, name);
		}

		struct ShaderGroup
		{
			size_t array;
			std::vector<size_t> shaders;
		};

		std::vector<ShaderGroup> FindShaderGroups(const Block& blk, std::vector<size_t>& groupOffsets)
		{
			std::vector<ShaderGroup> found;
			for (size_t o = 0; o + 0x20 <= blk.data.size(); o += 16)
			{
				const auto array = blk.Ptr(blk.U64(o + 0x10));
				const uint16_t n = blk.U16(o + 0x18), cap = blk.U16(o + 0x1A);
				if (!array || n == 0 || n > cap || cap > 1024)
					continue;
				ShaderGroup group{*array, {}};
				bool ok = true;
				for (uint16_t i = 0; i < n && ok; ++i)
				{
					const auto s = blk.Ptr(blk.U64(*array + 8 * i));
					ok = s && blk.Ptr(blk.U64(*s)) && blk.U8(*s + 0x13) == 0x80;
					if (ok)
						group.shaders.push_back(*s);
				}
				if (ok)
				{
					found.push_back(std::move(group));
					groupOffsets.push_back(o);
				}
			}
			return found;
		}

		struct Geometry
		{
			size_t at, vb, ib;
		};

		std::vector<Geometry> FindGeometries(const Block& blk)
		{
			const auto isVb = [&](size_t o) {
				const auto data = blk.Ptr(blk.U64(o + 0x10));
				const auto decl = blk.Ptr(blk.U64(o + 0x30));
				return data && blk.U64(o + 0x20) == blk.U64(o + 0x10) && decl && blk.U16(*decl + 4) == blk.U16(o + 8);
			};
			std::vector<Geometry> found;
			for (size_t o = 0; o + 0x80 <= blk.data.size(); o += 16)
			{
				const auto vb = blk.Ptr(blk.U64(o + 0x18)), ib = blk.Ptr(blk.U64(o + 0x38));
				if (vb && ib && isVb(*vb) && blk.Ptr(blk.U64(*ib + 0x10)) && blk.U32(*ib + 8))
					found.push_back({o, *vb, *ib});
			}
			return found;
		}

		struct Param
		{
			uint32_t name;
			uint8_t type; // 0 texture, n = n float4s
			std::optional<size_t> data;
		};

		std::vector<Param> LegacyParams(const Block& blk, size_t shader)
		{
			std::vector<Param> out;
			const auto params = blk.Ptr(blk.U64(shader));
			if (!params)
				return out;
			const uint8_t count = blk.U8(shader + 0x10);
			size_t hashes = *params + 16 * static_cast<size_t>(count);
			for (uint8_t i = 0; i < count; ++i)
				hashes += 16 * static_cast<size_t>(blk.U8(*params + 16 * i));
			for (uint8_t i = 0; i < count; ++i)
			{
				const uint64_t p = blk.U64(*params + 16 * i + 8);
				out.push_back({blk.U32(hashes + 4 * i), blk.U8(*params + 16 * i), p ? blk.Ptr(p) : std::nullopt});
			}
			return out;
		}

		// A texture dictionary embedded in a shader group: legacy textures (0x90) become Enhanced ones (0x80) in place,
		// their pixels move to 4 KB aligned pages of their own.
		bool ConvertEmbeddedDictionary(Block& blk, size_t dict, std::string& error)
		{
			const uint16_t count = blk.U16(dict + 0x38);
			const auto list = blk.Ptr(blk.U64(dict + 0x30));
			if (count && !list)
			{
				error = "corrupt embedded texture dictionary";
				return false;
			}
			for (uint16_t i = 0; i < count; ++i)
			{
				const auto o = blk.Ptr(blk.U64(*list + 8 * i));
				if (!o)
				{
					error = "corrupt embedded texture dictionary";
					return false;
				}
				const auto format = TextureFormatFromD3D(blk.U32(*o + 0x58));
				const auto namePtr = blk.U64(*o + 0x28);
				std::string name;
				if (const auto n = blk.Ptr(namePtr))
					for (size_t c = *n; c < blk.data.size() && blk.data[c]; ++c)
						name += static_cast<char>(blk.data[c]);
				if (!format)
				{
					error = std::format("embedded texture {}: unsupported format {:#x}", name, blk.U32(*o + 0x58));
					return false;
				}
				const uint16_t width = blk.U16(*o + 0x50), height = blk.U16(*o + 0x52);
				const uint16_t depth = std::max<uint16_t>(1, blk.U16(*o + 0x54));
				const uint8_t mips = std::max<uint8_t>(1, blk.U8(*o + 0x5D));
				const size_t size = MipChainSize(*format, width, height, mips);
				const auto pixels = blk.Ptr(blk.U64(*o + 0x70));
				if (!pixels || *pixels + size > blk.data.size())
				{
					error = std::format("embedded texture {}: pixel data out of range", name);
					return false;
				}
				Bytes texels(blk.data.begin() + *pixels, blk.data.begin() + *pixels + size);
				TextureFormat fmt = *format;
				uint8_t levels = mips;
				if (IsRenderTargetName(name))
					MakeRenderTargetFormat(fmt, levels, width, height, texels);
				const uint32_t stored = static_cast<uint32_t>((texels.size() + 0xFFF) & ~size_t{0xFFF});
				const auto at = blk.Alloc(stored, 0x1000);
				if (!at)
				{
					error = "out of space for texture data";
					return false;
				}
				std::copy(texels.begin(), texels.end(), blk.data.begin() + *at);
				WriteTextureHeader(blk.data, *o, kVirtual + *o, {namePtr, width, height, depth, levels, fmt, stored, kVirtual + *at,
				    IsRenderTargetName(name), IsNormalMapName(name)});
			}
			return true;
		}

		// Legacy shader (0x30) -> Enhanced instance (0x40) with a parameter table ("meta") the game matches by name.
		std::optional<size_t> ConvertShader(Block& blk, size_t s, const std::unordered_map<uint32_t, Effect>& effects,
		    std::vector<std::string>& warnings, std::string& error)
		{
			const uint32_t effectHash = blk.U32(s + 8);
			const auto found = effects.find(effectHash);
			if (found == effects.end())
			{
				error = std::format("shader effect {:08x} does not exist in Enhanced", effectHash);
				return std::nullopt;
			}
			const Effect& effect = found->second;
			const auto params = LegacyParams(blk, s);

			// Constants: one data block, legacy values back to back; renamed to the Enhanced names.
			Bytes constants;
			std::vector<std::pair<uint32_t, uint32_t>> entries; // (name hash, info)
			std::vector<std::optional<size_t>> textures;
			std::vector<uint32_t> textureNames;
			for (const Param& p : params)
			{
				if (p.type == 0)
				{
					textures.push_back(p.data);
					textureNames.push_back(p.name);
					continue;
				}
				const uint32_t size = 16u * p.type;
				const auto renamed = effect.constants.find(p.name);
				const uint32_t name = renamed == effect.constants.end() ? p.name : renamed->second;
				entries.emplace_back(name, 3u | (static_cast<uint32_t>(constants.size()) << 8) | (size << 20));
				if (p.data && *p.data + size <= blk.data.size())
					constants.insert(constants.end(), blk.data.begin() + *p.data, blk.data.begin() + *p.data + size);
				else
					constants.insert(constants.end(), size, uint8_t{0});
			}
			if (constants.size() > 0xFFF)
			{
				error = std::format("shader {:08x}: {:#x} bytes of constants", effectHash, constants.size());
				return std::nullopt;
			}
			// Textures: legacy parameters are named after samplers. Known names are renamed from the table; the rest
			// take the effect's textures that are still free, in order (the game's own conversion kept that order).
			// Every legacy texture goes into the table: names the effect does not have are simply not matched.
			std::vector<uint32_t> names(textures.size());
			std::vector<bool> used(effect.textures.size());
			for (size_t i = 0; i < textures.size(); ++i)
				if (const auto name = EnhancedTextureName(textureNames[i]))
				{
					names[i] = *name;
					for (size_t k = 0; k < effect.textures.size(); ++k)
						if (effect.textures[k] == *name)
							used[k] = true;
				}
			for (size_t i = 0, next = 0; i < textures.size(); ++i)
			{
				if (names[i])
					continue;
				while (next < used.size() && used[next])
					++next;
				if (next < used.size())
				{
					names[i] = effect.textures[next];
					used[next] = true;
				}
				else
				{
					names[i] = textureNames[i];
					warnings.push_back(std::format("效果 {:08x} 的貼圖 {:08x} 在強化版沒有對應，保留原名", effectHash, textureNames[i]));
				}
			}
			std::vector<std::pair<uint32_t, uint32_t>> meta;
			for (size_t i = 0; i < textures.size(); ++i)
				meta.emplace_back(names[i], static_cast<uint32_t>(i << 2));
			meta.insert(meta.end(), entries.begin(), entries.end());

			// +0x08 points at the parameter block: our pointer array and data at first, which the game reads
			// through the table and then overwrites with the effect's layout. It must hold the effect's block
			// (size at +0x3A, as in the game's own files) or the game keeps a separate allocation.
			const size_t block = (std::max<size_t>(effect.block, 0x10 + constants.size()) + 15) & ~size_t{15};
			const auto inst = blk.Alloc(0x40);
			const auto table = inst ? blk.Alloc(8 + 8 * meta.size()) : std::nullopt;
			const auto paramBlock = table ? blk.Alloc(block) : std::nullopt;
			const auto texArray = paramBlock ? blk.Alloc(8 * std::max<size_t>(1, textures.size())) : std::nullopt;
			if (!texArray || block > 0xFFFF)
			{
				error = "out of space for shader data";
				return std::nullopt;
			}
			Bytes& b = blk.data;
			b[*table + 0] = 1;
			b[*table + 1] = static_cast<uint8_t>(textures.size());
			b[*table + 4] = static_cast<uint8_t>(meta.size());
			b[*table + 7] = 0x0C;
			for (size_t i = 0; i < meta.size(); ++i)
			{
				Put<uint32_t>(b, *table + 8 + 8 * i, meta[i].first);
				Put<uint32_t>(b, *table + 12 + 8 * i, meta[i].second);
			}
			Put<uint64_t>(b, *paramBlock, kVirtual + *paramBlock + 0x10);
			std::copy(constants.begin(), constants.end(), b.begin() + *paramBlock + 0x10);
			for (size_t i = 0; i < textures.size(); ++i)
			{
				Put<uint64_t>(b, *texArray + 8 * i, textures[i] ? kVirtual + *textures[i] : 0);
				if (textures[i])
					ConvertTextureRef(b, *textures[i]);
			}
			Put<uint32_t>(b, *inst, effectHash);
			Put<uint32_t>(b, *inst + 4, 0x6D657461); // "meta"
			Put<uint64_t>(b, *inst + 0x08, kVirtual + *paramBlock);
			Put<uint64_t>(b, *inst + 0x10, kVirtual + *texArray);
			Put<uint64_t>(b, *inst + 0x20, kVirtual + *table);
			b[*inst + 0x39] = b[s + 0x11]; // draw bucket
			Put<uint16_t>(b, *inst + 0x3A, static_cast<uint16_t>(block));
			std::copy(b.begin() + s + 0x20, b.begin() + s + 0x24, b.begin() + *inst + 0x3C);
			return inst;
		}
	}

	bool ConvertEmbeddedTextures(Block& blk, size_t dict, std::string& error)
	{
		return ConvertEmbeddedDictionary(blk, dict, error);
	}

	bool ConvertDrawables(Block& blk, const std::unordered_map<uint32_t, Effect>& effects, std::vector<std::string>& warnings, std::string& error)
	{
		std::vector<size_t> groupOffsets;
		const auto groups = FindShaderGroups(blk, groupOffsets);
		const auto geometries = FindGeometries(blk);
		for (size_t g : groupOffsets)
			if (const auto dict = blk.Ptr(blk.U64(g + 8)); dict && !ConvertEmbeddedDictionary(blk, *dict, error))
				return false;

		// Drawables (+0x10 shader group, +0x50 LOD models): legacy files repeat the LOD pointer at +0xA0 with a count
		// at +0x9A; the game's own Enhanced files have zeros there, and peds stay invisible otherwise.
		const std::set<size_t> groupSet(groupOffsets.begin(), groupOffsets.end());
		for (size_t o = 0; o + 0xA8 <= blk.data.size(); o += 16)
		{
			const auto group = blk.Ptr(blk.U64(o + 0x10));
			if (!group || !groupSet.contains(*group) || !blk.Ptr(blk.U64(o + 0x50)) || blk.U64(o + 0xA0) != blk.U64(o + 0x50))
				continue;
			Put<uint16_t>(blk.data, o + 0x9A, 0);
			Put<uint64_t>(blk.data, o + 0xA0, 0);
		}

		std::map<size_t, Declaration> declarations;
		std::set<size_t> doneVb, doneIb;
		for (const Geometry& g : geometries)
		{
			if (doneVb.insert(g.vb).second)
			{
				const auto d = blk.Ptr(blk.U64(g.vb + 0x30));
				if (!declarations.contains(*d))
				{
					Declaration decl;
					if (!ConvertDeclaration(blk, *d, decl, error))
						return false;
					declarations.emplace(*d, std::move(decl));
				}
				const Declaration& decl = declarations.at(*d);
				const uint64_t dataPtr = blk.U64(g.vb + 0x10);
				const uint32_t count = blk.U32(g.vb + 0x18);
				const auto data = blk.Ptr(dataPtr);
				if (!data || *data + static_cast<size_t>(count) * decl.stride > blk.data.size())
				{
					error = "vertex data out of range";
					return false;
				}
				ReorderVertices(blk.data, *data, count, decl.stride, decl.elements);
				Bytes& b = blk.data;
				std::fill(b.begin() + g.vb, b.begin() + g.vb + 0x80, uint8_t{0});
				Put<uint64_t>(b, g.vb, kVtVertexBuffer);
				Put<uint32_t>(b, g.vb + 0x08, count);
				Put<uint16_t>(b, g.vb + 0x0C, static_cast<uint16_t>(decl.stride));
				Put<uint32_t>(b, g.vb + 0x10, 0x00580409);
				Put<uint64_t>(b, g.vb + 0x18, dataPtr);
				Put<uint64_t>(b, g.vb + 0x30, kVirtual + g.vb + 0x40);
				Put<uint64_t>(b, g.vb + 0x38, kVirtual + decl.at);
				WriteView(b, g.vb + 0x40);
			}
			if (doneIb.insert(g.ib).second)
			{
				Bytes& b = blk.data;
				const uint64_t dataPtr = blk.U64(g.ib + 0x10);
				const uint32_t count = blk.U32(g.ib + 8);
				std::fill(b.begin() + g.ib, b.begin() + g.ib + 0x60, uint8_t{0});
				Put<uint64_t>(b, g.ib, kVtIndexBuffer);
				Put<uint32_t>(b, g.ib + 0x08, count);
				Put<uint16_t>(b, g.ib + 0x0C, 2);
				Put<uint32_t>(b, g.ib + 0x10, 0x0058020A);
				Put<uint64_t>(b, g.ib + 0x18, dataPtr);
				Put<uint64_t>(b, g.ib + 0x30, kVirtual + g.ib + 0x40);
				WriteView(b, g.ib + 0x40);
			}
			// Geometry counts (some tools leave them zero).
			if (!blk.U32(g.at + 0x5C))
				Put<uint32_t>(blk.data, g.at + 0x5C, blk.U32(g.at + 0x58) / 3);
			if (!blk.U16(g.at + 0x60))
				Put<uint16_t>(blk.data, g.at + 0x60, static_cast<uint16_t>(std::min<uint32_t>(0xFFFF, blk.U32(g.vb + 0x08))));
		}

		for (const size_t g : groupOffsets)
			Put<uint64_t>(blk.data, g + 0x30, 0); // legacy shader groups have a small count here, the game's files 0
		for (const ShaderGroup& group : groups)
			for (size_t i = 0; i < group.shaders.size(); ++i)
			{
				const auto inst = ConvertShader(blk, group.shaders[i], effects, warnings, error);
				if (!inst)
					return false;
				Put<uint64_t>(blk.data, group.array + 8 * i, kVirtual + *inst);
			}

		return true;
	}

	bool FinishResource(Block& blk, uint32_t enhancedVersion, Bytes& out, std::string& error)
	{
		// Page map with room for every page (the game fills in the addresses).
		const auto pageMap = blk.Alloc(16 + 8 * 256);
		if (!pageMap)
		{
			error = "out of space for the page map";
			return false;
		}
		Put<uint64_t>(blk.data, 8, kVirtual + *pageMap);

		Resource res;
		uint32_t pageCount = 0;
		if (!blk.Relayout(res.virtualBlock, res.virtualFlags, pageCount, error))
			return false;
		const auto newPageMap = static_cast<size_t>(Get<uint64_t>(res.virtualBlock, 8) - kVirtual);
		res.virtualBlock[newPageMap + 8] = static_cast<uint8_t>(pageCount);
		res.virtualBlock[newPageMap + 9] = 0;
		res.version = enhancedVersion;
		res.virtualFlags |= (enhancedVersion >> 4) << 28;
		res.physicalFlags = (enhancedVersion & 0xF) << 28;
		return Write(res, out, error);
	}

	bool ConvertDrawableResource(const Bytes& legacy, uint32_t legacyVersion, uint32_t enhancedVersion,
	    const std::unordered_map<uint32_t, Effect>& effects, Bytes& out, std::vector<std::string>& warnings, std::string& error)
	{
		Resource in;
		if (!Read(legacy, in, error))
			return false;
		if (in.version != legacyVersion)
		{
			error = std::format("unexpected resource version {}", in.version);
			return false;
		}
		Block blk;
		blk.Init(in);

		return ConvertDrawables(blk, effects, warnings, error) && FinishResource(blk, enhancedVersion, out, error);
	}
}
