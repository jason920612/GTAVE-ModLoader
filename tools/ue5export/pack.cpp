#include "pack.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <set>
#include <unordered_set>

#include "GTAVPackFormat.h"
#include "resolve.hpp"

namespace ue5export
{
	namespace fs = std::filesystem;

	namespace
	{
		// A pack file being written: header, entries (16-byte aligned), then the sorted table.
		class PackFile
		{
		public:
			bool Open(const fs::path& file, uint32_t kind, std::string& error)
			{
				out_.open(file, std::ios::binary | std::ios::trunc);
				if (!out_)
					return error = "cannot write " + file.string(), false;
				kind_ = kind;
				const GTAVPack::FileHeader header{};
				out_.write(reinterpret_cast<const char*>(&header), sizeof(header));
				at_ = sizeof(header);
				return true;
			}

			bool Has(uint64_t key) const { return keys_.contains(key); }

			void Add(uint64_t key, const std::vector<uint8_t>& data)
			{
				static const char zeros[16] = {};
				if (const uint64_t pad = (16 - at_ % 16) % 16)
				{
					out_.write(zeros, static_cast<std::streamsize>(pad));
					at_ += pad;
				}
				entries_.push_back({key, at_, static_cast<uint32_t>(data.size()), 0});
				keys_.insert(key);
				out_.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
				at_ += data.size();
				bytes_ += data.size();
			}

			bool Close(std::string& error)
			{
				std::sort(entries_.begin(), entries_.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
				GTAVPack::FileHeader header{};
				std::memcpy(header.magic, "GPK1", 4);
				header.kind = kind_;
				header.version = GTAVPack::kVersion;
				header.entryCount = static_cast<uint32_t>(entries_.size());
				header.tableOffset = at_;
				out_.write(reinterpret_cast<const char*>(entries_.data()), static_cast<std::streamsize>(entries_.size() * sizeof(GTAVPack::Entry)));
				out_.seekp(0);
				out_.write(reinterpret_cast<const char*>(&header), sizeof(header));
				out_.close();
				if (out_.fail())
					return error = "write failed", false;
				return true;
			}

			uint64_t Bytes() const { return bytes_; }

		private:
			std::ofstream out_;
			uint32_t kind_ = 0;
			uint64_t at_ = 0, bytes_ = 0;
			std::vector<GTAVPack::Entry> entries_;
			std::unordered_set<uint64_t> keys_;
		};

		template <typename T>
		void Put(std::vector<uint8_t>& out, const T& value)
		{
			const auto* p = reinterpret_cast<const uint8_t*>(&value);
			out.insert(out.end(), p, p + sizeof(T));
		}
		template <typename T>
		void PutAll(std::vector<uint8_t>& out, const std::vector<T>& values)
		{
			const auto* p = reinterpret_cast<const uint8_t*>(values.data());
			out.insert(out.end(), p, p + values.size() * sizeof(T));
		}

		// Bytes per 4x4 block (block-compressed) or per texel; 0: a format the renderer does not take.
		bool FormatInfo(uint8_t dxgi, bool& blocks, uint32_t& bytes)
		{
			switch (dxgi)
			{
			case 71: case 72: case 80: case 81:                               // BC1, BC4
				return blocks = true, bytes = 8, true;
			case 74: case 75: case 77: case 78: case 83: case 84: case 98: case 99: // BC2, BC3, BC5, BC7
				return blocks = true, bytes = 16, true;
			case 87: case 88: case 28: case 29:                               // B8G8R8A8, R8G8B8A8
				return blocks = false, bytes = 4, true;
			case 65: case 61:                                                 // A8, R8
				return blocks = false, bytes = 1, true;
			default:
				return false;
			}
		}

		// The mips a texture's data holds (the game's count, or fewer when the data is short); 0 if unusable.
		uint8_t UsableMips(const drawable::Texture& t, std::string& why)
		{
			bool blocks = false;
			uint32_t bytes = 0;
			if (t.depth > 1)
				return why = "a volume texture", 0;
			if (!FormatInfo(t.dxgi, blocks, bytes))
				return why = std::format("DXGI format {}", t.dxgi), 0;
			if (!t.width || !t.height || (blocks && (t.width % 4 || t.height % 4)))
				return why = std::format("size {}x{}", t.width, t.height), 0;
			size_t at = 0;
			uint8_t mips = 0;
			for (uint32_t w = t.width, h = t.height; mips < t.mips; ++mips, w = std::max(1u, w / 2), h = std::max(1u, h / 2))
			{
				const size_t size = blocks ? size_t{(w + 3) / 4} * ((h + 3) / 4) * bytes : size_t{w} * h * bytes;
				if (at + size > t.data.size())
					break;
				at += size;
			}
			if (!mips)
				why = "no data";
			return mips;
		}

		uint64_t ContentHash(const drawable::Texture& t, uint8_t mips)
		{
			uint64_t h = 0x9E3779B97F4A7C15ull ^ (uint64_t{t.width} << 40 | uint64_t{t.height} << 16 | uint64_t{mips} << 8 | t.dxgi);
			const uint8_t* p = t.data.data();
			const size_t n = t.data.size();
			size_t i = 0;
			for (; i + 8 <= n; i += 8)
			{
				uint64_t w;
				std::memcpy(&w, p + i, 8);
				h = (h ^ w) * 0xBF58476D1CE4E5B9ull;
				h ^= h >> 31;
			}
			for (; i < n; ++i)
				h = (h ^ p[i]) * 0x94D049BB133111EBull;
			h ^= h >> 30, h *= 0xBF58476D1CE4E5B9ull, h ^= h >> 27, h *= 0x94D049BB133111EBull, h ^= h >> 31;
			return h ? h : 1;
		}

		int8_t Snorm(float v) { return static_cast<int8_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 127.0f)); }

		const std::map<uint32_t, uint8_t>& Slots()
		{
			static const std::map<uint32_t, uint8_t> slots = [] {
				std::map<uint32_t, uint8_t> m;
				const std::pair<const char*, GTAVPack::Slot> names[] = {{"DiffuseTex", GTAVPack::kDiffuse}, {"BumpTex", GTAVPack::kBump},
				    {"SpecularTex", GTAVPack::kSpecular}, {"DetailTex", GTAVPack::kDetail}, {"TintPaletteTex", GTAVPack::kTintPalette},
				    {"DiffuseTexture_layer0", GTAVPack::kLayerDiffuse0}, {"DiffuseTexture_layer1", GTAVPack::kLayerDiffuse1},
				    {"DiffuseTexture_layer2", GTAVPack::kLayerDiffuse2}, {"DiffuseTexture_layer3", GTAVPack::kLayerDiffuse3},
				    {"BumpTexture_layer0", GTAVPack::kLayerBump0}, {"BumpTexture_layer1", GTAVPack::kLayerBump1},
				    {"BumpTexture_layer2", GTAVPack::kLayerBump2}, {"BumpTexture_layer3", GTAVPack::kLayerBump3},
				    {"HeightTexture", GTAVPack::kHeight}, {"DirtTex", GTAVPack::kDirt}};
				for (const auto& [n, s] : names)
					m[Joaat(n)] = s;
				return m;
			}();
			return slots;
		}

		float First(const drawable::Material& m, const char* name, float fallback)
		{
			const auto it = m.constants.find(Joaat(name));
			return it != m.constants.end() && !it->second.empty() ? it->second[0] : fallback;
		}

		struct Writer
		{
			Resolver& resolver;
			PackFile models, textures;
			PackStats stats;
			std::vector<std::string>& warnings;
			std::map<std::string, size_t> unusable;     // texture problems -> count
			std::map<uint32_t, size_t> unknownBuckets;  // draw buckets drawn as opaque
			std::set<std::string> missingTextureNames;

			// The texture into the pack (once); its key, 0 if it cannot be used.
			uint64_t Texture(const drawable::Texture& t)
			{
				std::string why;
				const uint8_t mips = UsableMips(t, why);
				if (!mips)
				{
					++unusable[why];
					return 0;
				}
				const uint64_t key = ContentHash(t, mips);
				if (textures.Has(key))
					return key;
				std::vector<uint8_t> data;
				Put(data, GTAVPack::TextureHeader{t.width, t.height, mips, t.dxgi, 0});
				data.insert(data.end(), t.data.begin(), t.data.end());
				textures.Add(key, data);
				++stats.textures;
				return key;
			}

			std::vector<uint8_t> Model(const Resolver::Model& m)
			{
				std::vector<uint8_t> out;
				GTAVPack::ModelHeader header{};
				std::memcpy(header.magic, "GMDL", 4);
				header.version = GTAVPack::kVersion;
				const auto& b0 = m.model.bbMin;
				const auto& b1 = m.model.bbMax;
				const float lo[3] = {b0[0] * 100, -b1[1] * 100, b0[2] * 100}, hi[3] = {b1[0] * 100, -b0[1] * 100, b1[2] * 100};
				std::memcpy(header.boundsMin, lo, sizeof(lo));
				std::memcpy(header.boundsMax, hi, sizeof(hi));
				header.materialCount = static_cast<uint16_t>(m.model.materials.size());
				header.sectionCount = static_cast<uint16_t>(m.model.sections.size());
				Put(out, header);
				for (size_t i = 0; i < m.model.materials.size(); ++i)
				{
					const drawable::Material& src = m.model.materials[i];
					GTAVPack::Material mat{};
					mat.effect = src.effect;
					mat.bucket = src.drawBucket;
					for (const auto& [param, t] : m.textures[i])
						if (const auto slot = Slots().find(param); slot != Slots().end())
							mat.textures[slot->second] = Texture(*t.texture);
					const bool terrain = src.textures.contains(Joaat("DiffuseTexture_layer0"));
					switch (src.drawBucket)
					{
					case 0: mat.kind = GTAVPack::kOpaque; break;
					case 1: mat.kind = GTAVPack::kTranslucent; break;
					case 2: mat.kind = GTAVPack::kDecal; break;
					case 3: mat.kind = GTAVPack::kCutout; break;
					case 6: mat.kind = GTAVPack::kWater; break;
					default: mat.kind = GTAVPack::kOpaque, ++unknownBuckets[src.drawBucket]; break;
					}
					if (terrain)
						mat.kind = GTAVPack::kTerrain;
					mat.tintRow = First(src, "tintPaletteSelector", 0);
					mat.emissive = First(src, "emissiveMultiplier", 0);
					mat.bumpiness = First(src, "bumpiness", 1);
					mat.specularIntensity = First(src, "specularIntensityMult", 1);
					mat.alphaTest = First(src, "alphaTestValue", 0);
					const auto copy = [&](uint32_t name, float* target) {
						if (const auto it = src.constants.find(name); it != src.constants.end())
							for (size_t k = 0; k < 4 && k < it->second.size(); ++k)
								target[k] = it->second[k];
					};
					copy(0xe9c969fb, mat.color); // (the water's colour)
					copy(Joaat("detailSettings"), mat.detail);
					Put(out, mat);
				}
				for (const auto& s : m.model.sections)
				{
					const uint32_t v = s.vertices;
					const bool wide = v > 0xFFFF;
					const uint8_t flags = (s.normal.empty() ? 0 : GTAVPack::kNormals) | (s.tangent.empty() ? 0 : GTAVPack::kTangents) |
					                      (s.color0.empty() ? 0 : GTAVPack::kColor0) | (s.color1.empty() ? 0 : GTAVPack::kColor1) |
					                      (wide ? GTAVPack::kIndex32 : 0);
					Put(out, GTAVPack::SectionHeader{static_cast<uint16_t>(s.material), static_cast<uint8_t>(s.uv.size()), flags, v,
					             static_cast<uint32_t>(s.indices.size())});
					for (uint32_t i = 0; i < v; ++i)
					{
						Put(out, s.position[3 * i] * 100);
						Put(out, -s.position[3 * i + 1] * 100);
						Put(out, s.position[3 * i + 2] * 100);
					}
					if (!s.normal.empty())
						for (uint32_t i = 0; i < v; ++i)
							Put(out, std::array<int8_t, 4>{Snorm(s.normal[3 * i]), Snorm(-s.normal[3 * i + 1]), Snorm(s.normal[3 * i + 2]), 0});
					if (!s.tangent.empty()) // (mirrored: the binormal's sign flips)
						for (uint32_t i = 0; i < v; ++i)
							Put(out, std::array<int8_t, 4>{Snorm(s.tangent[4 * i]), Snorm(-s.tangent[4 * i + 1]), Snorm(s.tangent[4 * i + 2]),
							             static_cast<int8_t>(s.tangent[4 * i + 3] < 0 ? 127 : -127)});
					PutAll(out, s.color0);
					PutAll(out, s.color1);
					for (const auto& uv : s.uv)
						PutAll(out, uv);
					if (wide)
						PutAll(out, s.indices);
					else
						for (const uint32_t i : s.indices)
							Put(out, static_cast<uint16_t>(i));
					while (out.size() % 4)
						out.push_back(0);
				}
				return out;
			}
		};
	}

	PackStats WritePack(const GameFiles& files, const mapdata::Map& map, const PackOptions& options, std::vector<std::string>& warnings)
	{
		std::error_code ec;
		fs::create_directories(options.dir, ec);
		Resolver resolver(files);
		Writer w{resolver, {}, {}, {}, warnings, {}, {}, {}};
		std::string error;
		if (!w.models.Open(options.dir / "models.gpk", GTAVPack::kModels, error) || !w.textures.Open(options.dir / "textures.gpk", GTAVPack::kTextures, error))
		{
			warnings.push_back(error);
			return w.stats;
		}

		// What to convert: the map's entities at the highest level of detail (a test area's only).
		std::vector<const mapdata::Entity*> placed;
		std::set<uint32_t> archetypes;
		for (const auto& e : map.entities)
		{
			if ((e.lodLevel != mapdata::kHd && e.lodLevel != mapdata::kOrphanHd) || e.interior)
				continue;
			const auto a = map.archetypes.find(e.archetype);
			if (a == map.archetypes.end() || a->second.assetType == mapdata::AssetType::Assetless)
				continue;
			if (options.area && !InArea(a->second, e, *options.area))
				continue;
			placed.push_back(&e);
			archetypes.insert(e.archetype);
		}

		std::set<uint32_t> converted;
		std::map<std::string, size_t> missingModels; // reason -> count
		for (const uint32_t hash : archetypes)
		{
			Resolver::Model model;
			if (!resolver.Load(map.archetypes.at(hash), model, error))
			{
				++w.stats.missingModels;
				++missingModels[error.substr(error.find(':') == std::string::npos ? 0 : error.find(':') + 2)];
				warnings.push_back(resolver.Stem(hash) + ": " + error);
				continue;
			}
			for (const auto& m : model.missing)
				w.missingTextureNames.insert(m);
			w.stats.missingTextures += model.missing.size();
			w.models.Add(hash, w.Model(model));
			converted.insert(hash);
			++w.stats.models;
		}

		// Where things are.
		PackFile world;
		if (!world.Open(options.dir / "world.gpk", GTAVPack::kWorld, error))
		{
			warnings.push_back(error);
			return w.stats;
		}
		std::vector<uint8_t> placements;
		for (const mapdata::Entity* e : placed)
		{
			if (!converted.contains(e->archetype))
				continue;
			GTAVPack::Placement p{};
			p.model = e->archetype;
			p.position[0] = e->position[0] * 100, p.position[1] = -e->position[1] * 100, p.position[2] = e->position[2] * 100;
			// (the mirrored world's rotation: (x, y, z, w) -> (-x, y, -z, w))
			const float q[4] = {-e->rotation[0], e->rotation[1], -e->rotation[2], e->rotation[3]};
			const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
			for (int k = 0; k < 4; ++k)
				p.rotation[k] = n > 0 ? q[k] / n : (k == 3 ? 1.0f : 0.0f);
			p.scale[0] = p.scale[1] = e->scaleXY, p.scale[2] = e->scaleZ;
			p.flags = e->flags;
			Put(placements, p);
			++w.stats.placements;
		}
		world.Add(GTAVPack::kPlacements, placements);
		w.stats.modelBytes = w.models.Bytes();
		w.stats.textureBytes = w.textures.Bytes();
		for (PackFile* f : {&w.models, &w.textures, &world})
			if (!f->Close(error))
				warnings.push_back(error);

		// What could not be converted.
		std::ofstream report(options.dir / "report.txt");
		report << std::format("models: {} converted, {} missing\n", w.stats.models, w.stats.missingModels);
		for (const auto& [why, n] : missingModels)
			report << std::format("  missing model: {} x{}\n", why, n);
		report << std::format("textures: {} distinct, {} references missing ({} names)\n", w.stats.textures, w.stats.missingTextures,
		    w.missingTextureNames.size());
		for (const auto& [why, n] : w.unusable)
			report << std::format("  unusable texture: {} x{}\n", why, n);
		for (const auto& [bucket, n] : w.unknownBuckets)
			report << std::format("  draw bucket {} drawn as opaque: {} materials\n", bucket, n);
		for (const auto& name : w.missingTextureNames)
			report << "  missing texture: " << name << '\n';
		report << std::format("placements: {}\n", w.stats.placements);
		return w.stats;
	}
}
