#include "pack.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "GTAVPackFormat.h"
#include "resolve.hpp"

namespace ue5export
{
	namespace fs = std::filesystem;

	namespace
	{
		// A pack file being written: header, entries (16-byte aligned), then the sorted table. Add is thread-safe.
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

			// Adds the entry unless the key is there already; false if it was.
			bool Add(uint64_t key, const std::vector<uint8_t>& data)
			{
				static const char zeros[16] = {};
				std::lock_guard lock(mutex_);
				if (!keys_.insert(key).second)
					return false;
				if (const uint64_t pad = (16 - at_ % 16) % 16)
				{
					out_.write(zeros, static_cast<std::streamsize>(pad));
					at_ += pad;
				}
				entries_.push_back({key, at_, static_cast<uint32_t>(data.size()), 0});
				out_.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
				at_ += data.size();
				bytes_ += data.size();
				return true;
			}

			bool Has(uint64_t key)
			{
				std::lock_guard lock(mutex_);
				return keys_.contains(key);
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
			std::mutex mutex_;
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
		template <typename T>
		std::vector<uint8_t> Bytes(const std::vector<T>& values)
		{
			std::vector<uint8_t> out;
			PutAll(out, values);
			return out;
		}

		// Bytes per 4x4 block (block-compressed) or per texel; false: a format the renderer does not take.
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
			case 61: case 65:                                                 // R8, A8
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

		// GTA's world (metres, right-handed, z up) -> Unreal's (centimetres, left-handed): y mirrored.
		void Point(const mapdata::Vec3& v, float* out)
		{
			out[0] = v[0] * 100, out[1] = -v[1] * 100, out[2] = v[2] * 100;
		}
		void Box(const mapdata::Vec3& lo, const mapdata::Vec3& hi, float* outMin, float* outMax)
		{
			outMin[0] = lo[0] * 100, outMin[1] = -hi[1] * 100, outMin[2] = lo[2] * 100;
			outMax[0] = hi[0] * 100, outMax[1] = -lo[1] * 100, outMax[2] = hi[2] * 100;
		}
		// (the mirrored world's rotation: (x, y, z, w) -> (-x, y, -z, w))
		// The game's rotation (x, y, z, w) in Unreal's space (y mirrored). inverted: the map stores the inverse, as it does
		// for the entities of a ymap (an MLO placement's is the rotation itself): a road at the Zancudo bridge lines up with
		// the bridge only this way.
		void Rotation(const mapdata::Quat& q, float* out, bool inverted)
		{
			const float r[4] = {inverted ? q[0] : -q[0], inverted ? -q[1] : q[1], inverted ? q[2] : -q[2], q[3]};
			const float n = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
			for (int k = 0; k < 4; ++k)
				out[k] = n > 0 ? r[k] / n : (k == 3 ? 1.0f : 0.0f);
		}

		bool HasModel(const mapdata::Archetype& a)
		{
			return a.assetType == mapdata::AssetType::Drawable || a.assetType == mapdata::AssetType::Fragment ||
			       a.assetType == mapdata::AssetType::DrawableDictionary;
		}

		// What the workers share.
		struct Shared
		{
			const mapdata::Map& map;
			const PackOptions& options;
			PackFile models, textures;
			std::mutex mutex; // the rest
			std::unordered_map<std::string, uint64_t> byPath; // "dictionary/texture" -> key (0: unusable)
			PackStats stats;
			std::map<std::string, size_t> unusable, missingModels;
			std::map<uint32_t, size_t> unknownBuckets;
			std::set<std::string> missingTextureNames;
			std::set<uint32_t> converted;
			std::vector<std::string> warnings;
		};

		// The texture into the pack (once); its key, 0 if it cannot be used.
		uint64_t Texture(Shared& sh, const Resolver::Texture& t)
		{
			const std::string path = t.where + "/" + t.key;
			{
				std::lock_guard lock(sh.mutex);
				if (const auto it = sh.byPath.find(path); it != sh.byPath.end())
					return it->second;
			}
			std::string why;
			const uint8_t mips = UsableMips(*t.texture, why);
			const uint64_t key = mips ? ContentHash(*t.texture, mips) : 0;
			{
				std::lock_guard lock(sh.mutex);
				sh.byPath[path] = key;
				if (!mips)
				{
					++sh.unusable[why];
					return 0;
				}
			}
			if (!sh.textures.Has(key))
			{
				const drawable::Texture& x = *t.texture;
				std::vector<uint8_t> data;
				Put(data, GTAVPack::TextureHeader{x.width, x.height, mips, x.dxgi, 0});
				data.insert(data.end(), x.data.begin(), x.data.end());
				if (sh.textures.Add(key, data))
				{
					std::lock_guard lock(sh.mutex);
					++sh.stats.textures;
				}
			}
			return key;
		}

		std::vector<uint8_t> ModelBytes(Shared& sh, const Resolver::Model& m)
		{
			std::vector<uint8_t> out;
			GTAVPack::ModelHeader header{};
			std::memcpy(header.magic, "GMDL", 4);
			header.version = GTAVPack::kVersion;
			Box(m.model.bbMin, m.model.bbMax, header.boundsMin, header.boundsMax);
			Point({m.model.sphere[0], m.model.sphere[1], m.model.sphere[2]}, header.sphere);
			header.sphere[3] = m.model.sphere[3] * 100;
			for (int i = 0; i < 4; ++i)
				header.lodDistances[i] = m.model.lodDistances[i];
			header.lodCount = std::max<uint8_t>(1, m.model.lodCount);
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
						mat.textures[slot->second] = Texture(sh, t);
				switch (src.drawBucket)
				{
				case 0: mat.kind = GTAVPack::kOpaque; break;
				case 1: mat.kind = GTAVPack::kTranslucent; break;
				case 2: mat.kind = GTAVPack::kDecal; break;
				case 3: mat.kind = GTAVPack::kCutout; break;
				case 6: mat.kind = GTAVPack::kWater; break;
				default:
				{
					mat.kind = GTAVPack::kOpaque;
					std::lock_guard lock(sh.mutex);
					++sh.unknownBuckets[src.drawBucket];
					break;
				}
				}
				if (src.textures.contains(Joaat("DiffuseTexture_layer0")))
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
				GTAVPack::SectionHeader sh2{};
				sh2.material = static_cast<uint16_t>(s.material);
				sh2.uvCount = static_cast<uint8_t>(s.uv.size());
				sh2.flags = flags;
				sh2.vertexCount = v;
				sh2.indexCount = static_cast<uint32_t>(s.indices.size());
				sh2.lod = s.lod;
				Put(out, sh2);
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

		// A worker: its own view of the game's files, models taken from the shared list in chunks.
		void Work(Shared& sh, const std::vector<uint32_t>& work, std::atomic<size_t>& next, std::atomic<size_t>& done, std::atomic<unsigned>& active)
		{
			struct Leave
			{
				std::atomic<unsigned>& active;
				~Leave() { --active; }
			} leave{active};
			GameFiles files;
			std::string error;
			if (!files.Open(sh.options.gameDir, *sh.options.decrypt, error))
			{
				std::lock_guard lock(sh.mutex);
				sh.warnings.push_back("worker: " + error);
				return;
			}
			Resolver resolver(files, size_t{768} << 20);
			constexpr size_t kChunk = 16;
			for (size_t first; (first = next.fetch_add(kChunk)) < work.size();)
				for (size_t i = first; i < std::min(work.size(), first + kChunk); ++i, ++done)
				{
					const uint32_t hash = work[i];
					Resolver::Model model;
					if (!resolver.Load(sh.map.archetypes.at(hash), model, error))
					{
						std::lock_guard lock(sh.mutex);
						++sh.stats.missingModels;
						const size_t colon = error.find(": ");
						++sh.missingModels[colon == std::string::npos ? error : error.substr(colon + 2)];
						sh.warnings.push_back(resolver.Stem(hash) + ": " + error);
						continue;
					}
					const std::vector<uint8_t> bytes = ModelBytes(sh, model);
					sh.models.Add(hash, bytes);
					std::lock_guard lock(sh.mutex);
					for (const auto& m : model.missing)
						sh.missingTextureNames.insert(m);
					sh.stats.missingTextures += model.missing.size();
					sh.converted.insert(hash);
					++sh.stats.models;
				}
		}

		// The models to convert: every archetype with a model that an entity, an interior or a grass batch uses (in the
		// test area only, with one).
		std::vector<uint32_t> ModelsToConvert(const mapdata::Map& map, const std::optional<Area>& area)
		{
			std::set<uint32_t> want;
			const auto use = [&](uint32_t archetype) {
				if (const auto a = map.archetypes.find(archetype); a != map.archetypes.end() && HasModel(a->second))
					want.insert(archetype);
			};
			for (const auto& e : map.entities)
			{
				const auto a = map.archetypes.find(e.archetype);
				if (a == map.archetypes.end() || (area && !InArea(a->second, e, *area)))
					continue;
				if (a->second.interior >= 0)
				{
					const auto& in = map.interiors[a->second.interior];
					for (const auto& x : in.entities)
						use(x.archetype);
					for (const auto& s : in.entitySets)
						for (const auto& x : s.entities)
							use(x.archetype);
				}
				use(e.archetype);
			}
			for (const auto& y : map.ymaps)
				for (const auto& b : y.grass)
					if (!area || (b.bbMax[0] >= area->bounds[0] && b.bbMax[1] >= area->bounds[1] && b.bbMin[0] <= area->bounds[2] &&
					                 b.bbMin[1] <= area->bounds[3]))
						use(b.archetype);
			// (archetypes sharing a texture dictionary next to each other: the workers' caches keep them)
			std::vector<uint32_t> out(want.begin(), want.end());
			std::stable_sort(out.begin(), out.end(), [&](uint32_t a, uint32_t b) {
				return map.archetypes.at(a).textureDictionary < map.archetypes.at(b).textureDictionary;
			});
			return out;
		}

		// water.xml (the story map's: levels/gta5): <WaterQuads>, <CalmingQuads>, <WaveQuads>, each a list of <Item>s
		// with <name value="..." /> fields.
		struct Water
		{
			std::vector<GTAVPack::WaterQuad> quads;
			std::vector<GTAVPack::CalmingQuad> calming;
			std::vector<GTAVPack::WaveQuad> waves;
		};

		Water ReadWater(const GameFiles& files, std::vector<std::string>& warnings)
		{
			Water w;
			const GameFiles::File* file = nullptr;
			std::vector<GameFiles::File> versions = files.AllOf("water.xml");
			for (const auto& f : versions)
				if (Lower(f.path).ends_with("levels/gta5/water.xml"))
					file = &f;
			loader::convert::Bytes data;
			std::string error;
			if (!file || !files.Read(*file, data, error))
			{
				warnings.push_back("no levels/gta5/water.xml: no sea");
				return w;
			}
			const std::string xml(data.begin(), data.end());
			const auto items = [&](const std::string& list) {
				std::vector<std::string> out;
				const size_t begin = xml.find("<" + list + ">"), end = xml.find("</" + list + ">");
				if (begin == std::string::npos || end == std::string::npos)
					return out;
				for (size_t at = xml.find("<Item>", begin); at != std::string::npos && at < end; at = xml.find("<Item>", at + 1))
					out.push_back(xml.substr(at, xml.find("</Item>", at) - at));
				return out;
			};
			const auto value = [](const std::string& item, const std::string& name) {
				const size_t at = item.find("<" + name + " value=\"");
				return at == std::string::npos ? std::string() : item.substr(at + name.size() + 9, item.find('"', at + name.size() + 9) - at - name.size() - 9);
			};
			const auto number = [&](const std::string& item, const std::string& name) {
				const std::string v = value(item, name);
				return v.empty() ? 0.0f : std::stof(v);
			};
			for (const auto& item : items("WaterQuads"))
			{
				GTAVPack::WaterQuad q{};
				q.minX = number(item, "minX"), q.minY = number(item, "minY"), q.maxX = number(item, "maxX"), q.maxY = number(item, "maxY");
				q.z = number(item, "z");
				q.type = static_cast<uint8_t>(number(item, "Type"));
				q.flags = (value(item, "IsInvisible") == "true" ? GTAVPack::kInvisible : 0) | (value(item, "HasLimitedDepth") == "true" ? GTAVPack::kLimitedDepth : 0) |
				          (value(item, "NoStencil") == "true" ? GTAVPack::kNoStencil : 0);
				for (int k = 0; k < 4; ++k)
					q.alpha[k] = static_cast<uint8_t>(number(item, "a" + std::to_string(k + 1)));
				w.quads.push_back(q);
			}
			for (const auto& item : items("CalmingQuads"))
				w.calming.push_back({number(item, "minX"), number(item, "minY"), number(item, "maxX"), number(item, "maxY"), number(item, "fDampening")});
			for (const auto& item : items("WaveQuads"))
				w.waves.push_back({number(item, "minX"), number(item, "minY"), number(item, "maxX"), number(item, "maxY"), number(item, "Amplitude"),
				    number(item, "XDirection"), number(item, "YDirection")});
			return w;
		}

		// The map itself.
		struct WorldWriter
		{
			const mapdata::Map& map;
			const std::set<uint32_t>& converted;
			const Water& water;
			std::vector<uint32_t> lists;

			uint32_t List(const std::vector<uint32_t>& values)
			{
				const uint32_t first = static_cast<uint32_t>(lists.size());
				lists.insert(lists.end(), values.begin(), values.end());
				return first;
			}

			GTAVPack::InteriorEntity InteriorEntity(const mapdata::Entity& e)
			{
				GTAVPack::InteriorEntity x{};
				x.archetype = e.archetype;
				Point(e.position, x.position);
				Rotation(e.rotation, x.rotation, false);
				x.scaleXY = e.scaleXY, x.scaleZ = e.scaleZ;
				x.lodDist = e.lodDist;
				x.flags = e.flags;
				x.tintValue = e.tintValue;
				x.lodLevel = static_cast<uint8_t>(e.lodLevel);
				return x;
			}

			bool Write(PackFile& world, std::string& error, size_t& entityCount)
			{
				// Archetypes (sorted by name: the map's order).
				std::vector<GTAVPack::Archetype> archetypes;
				std::map<int32_t, uint32_t> interiorArchetype; // interior index -> archetype name
				for (const auto& [name, a] : map.archetypes)
				{
					GTAVPack::Archetype x{};
					x.name = name;
					x.flags = a.flags;
					x.lodDist = a.lodDist;
					x.hdTextureDist = a.hdTextureDist;
					Box(a.bbMin, a.bbMax, x.boundsMin, x.boundsMax);
					Point(a.bsCentre, x.sphere);
					x.sphere[3] = a.bsRadius * 100;
					x.timeFlags = a.timeFlags;
					x.interior = a.interior >= 0 ? static_cast<uint32_t>(a.interior) : GTAVPack::kNone;
					x.specialAttribute = a.specialAttribute;
					x.kind = a.interior >= 0 ? GTAVPack::kInterior : a.timed ? GTAVPack::kTimed : GTAVPack::kBase;
					x.hasModel = converted.contains(name);
					archetypes.push_back(x);
					if (a.interior >= 0)
						interiorArchetype[a.interior] = name;
				}

				// Interiors: their entities (then the entity sets' ones), rooms, portals, sets.
				std::vector<GTAVPack::Interior> interiors;
				std::vector<GTAVPack::InteriorEntity> interiorEntities;
				std::vector<GTAVPack::Room> rooms;
				std::vector<GTAVPack::Portal> portals;
				std::vector<GTAVPack::EntitySet> sets;
				for (size_t i = 0; i < map.interiors.size(); ++i)
				{
					const auto& in = map.interiors[i];
					GTAVPack::Interior x{};
					x.archetype = interiorArchetype.contains(static_cast<int32_t>(i)) ? interiorArchetype[static_cast<int32_t>(i)] : 0;
					x.mloFlags = in.mloFlags;
					x.firstEntity = static_cast<uint32_t>(interiorEntities.size());
					x.entityCount = static_cast<uint32_t>(in.entities.size());
					for (const auto& e : in.entities)
						interiorEntities.push_back(InteriorEntity(e));
					x.firstRoom = static_cast<uint32_t>(rooms.size());
					x.roomCount = static_cast<uint32_t>(in.rooms.size());
					for (const auto& r : in.rooms)
					{
						GTAVPack::Room room{};
						Box(r.bbMin, r.bbMax, room.boundsMin, room.boundsMax);
						room.flags = r.flags;
						room.floorId = r.floorId;
						room.firstObject = List(r.attachedObjects);
						room.objectCount = static_cast<uint32_t>(r.attachedObjects.size());
						rooms.push_back(room);
					}
					x.firstPortal = static_cast<uint32_t>(portals.size());
					x.portalCount = static_cast<uint32_t>(in.portals.size());
					for (const auto& p : in.portals)
					{
						GTAVPack::Portal portal{};
						portal.roomFrom = p.roomFrom, portal.roomTo = p.roomTo, portal.flags = p.flags, portal.opacity = p.opacity;
						for (size_t c = 0; c < 4 && c < p.corners.size(); ++c)
							Point(p.corners[c], portal.corners[c]);
						portal.firstObject = List(p.attachedObjects);
						portal.objectCount = static_cast<uint32_t>(p.attachedObjects.size());
						portals.push_back(portal);
					}
					x.firstSet = static_cast<uint32_t>(sets.size());
					x.setCount = static_cast<uint32_t>(in.entitySets.size());
					for (const auto& s : in.entitySets)
					{
						GTAVPack::EntitySet set{};
						set.name = s.name;
						set.firstEntity = static_cast<uint32_t>(interiorEntities.size());
						set.entityCount = static_cast<uint32_t>(s.entities.size());
						for (const auto& e : s.entities)
							interiorEntities.push_back(InteriorEntity(e));
						set.firstLocation = List(s.locations);
						set.locationCount = static_cast<uint32_t>(s.locations.size());
						sets.push_back(set);
					}
					interiors.push_back(x);
				}

				// Entities, the interiors they place, grass, ymaps.
				std::vector<GTAVPack::Entity> entities;
				std::vector<GTAVPack::InteriorPlacement> placements;
				entities.reserve(map.entities.size());
				for (size_t i = 0; i < map.entities.size(); ++i)
				{
					const auto& e = map.entities[i];
					GTAVPack::Entity x{};
					x.archetype = e.archetype;
					x.ymap = e.ymap;
					Point(e.position, x.position);
					Rotation(e.rotation, x.rotation, !e.interior);
					x.scaleXY = e.scaleXY, x.scaleZ = e.scaleZ;
					x.lodDist = e.lodDist, x.childLodDist = e.childLodDist;
					const int64_t parent = map.Parent(i);
					x.parent = parent >= 0 ? static_cast<uint32_t>(parent) : GTAVPack::kNone;
					x.flags = e.flags;
					x.tintValue = e.tintValue;
					x.interiorPlacement = GTAVPack::kNone;
					x.lodLevel = static_cast<uint8_t>(e.lodLevel);
					x.priorityLevel = static_cast<uint8_t>(e.priorityLevel);
					x.numChildren = static_cast<uint16_t>(std::min<uint32_t>(e.numChildren, 0xFFFF));
					if (e.interior)
					{
						GTAVPack::InteriorPlacement p{};
						p.entity = static_cast<uint32_t>(i);
						const auto a = map.archetypes.find(e.archetype);
						p.interior = a != map.archetypes.end() && a->second.interior >= 0 ? static_cast<uint32_t>(a->second.interior) : GTAVPack::kNone;
						p.groupId = e.groupId, p.floorId = e.floorId, p.flags = e.mloFlags;
						p.firstDefaultSet = List(e.defaultEntitySets);
						p.defaultSetCount = static_cast<uint32_t>(e.defaultEntitySets.size());
						x.interiorPlacement = static_cast<uint32_t>(placements.size());
						placements.push_back(p);
					}
					entities.push_back(x);
				}
				entityCount = entities.size();
				std::vector<GTAVPack::Ymap> ymaps;
				std::vector<GTAVPack::GrassBatch> batches;
				std::vector<GTAVPack::GrassInstance> grass;
				for (size_t i = 0; i < map.ymaps.size(); ++i)
				{
					const auto& y = map.ymaps[i];
					GTAVPack::Ymap x{};
					x.name = y.name;
					x.parent = y.parent >= 0 ? static_cast<uint32_t>(y.parent) : GTAVPack::kNone;
					x.flags = y.flags, x.contentFlags = y.contentFlags;
					Box(y.streamingMin, y.streamingMax, x.streamingMin, x.streamingMax);
					Box(y.entitiesMin, y.entitiesMax, x.entitiesMin, x.entitiesMax);
					x.firstEntity = y.firstEntity, x.entityCount = y.entityCount;
					x.firstGrass = static_cast<uint32_t>(batches.size());
					x.grassCount = static_cast<uint32_t>(y.grass.size());
					x.group = y.group >= 0 ? static_cast<uint32_t>(y.group) : GTAVPack::kNone;
					for (const auto& b : y.grass)
					{
						GTAVPack::GrassBatch g{};
						g.archetype = b.archetype;
						g.ymap = static_cast<uint32_t>(i);
						for (int k = 0; k < 3; ++k)
							g.boundsMin[k] = b.bbMin[k], g.boundsMax[k] = b.bbMax[k], g.scaleRange[k] = b.scaleRange[k];
						g.lodDist = static_cast<float>(b.lodDist);
						g.fadeStart = b.fadeStart, g.fadeRange = b.fadeRange, g.orientToTerrain = b.orientToTerrain;
						g.firstInstance = static_cast<uint32_t>(grass.size());
						g.instanceCount = static_cast<uint32_t>(b.instances.size());
						for (const auto& in : b.instances)
						{
							GTAVPack::GrassInstance gi;
							std::memcpy(&gi, &in, sizeof(gi));
							grass.push_back(gi);
						}
						batches.push_back(g);
					}
					ymaps.push_back(x);
				}
				std::vector<GTAVPack::Group> groups;
				for (const auto& g : map.groups)
				{
					GTAVPack::Group x{};
					x.name = g.name, x.flags = g.flags, x.hoursOnOff = g.hoursOnOff;
					const auto y = map.ymapIndex.find(g.name);
					x.ymap = y != map.ymapIndex.end() ? y->second : GTAVPack::kNone;
					x.firstWeather = List(g.weathers);
					x.weatherCount = static_cast<uint32_t>(g.weathers.size());
					groups.push_back(x);
				}

				world.Add(GTAVPack::kArchetypes, Bytes(archetypes));
				world.Add(GTAVPack::kYmaps, Bytes(ymaps));
				world.Add(GTAVPack::kEntities, Bytes(entities));
				world.Add(GTAVPack::kGroups, Bytes(groups));
				world.Add(GTAVPack::kInteriors, Bytes(interiors));
				world.Add(GTAVPack::kInteriorEntities, Bytes(interiorEntities));
				world.Add(GTAVPack::kRooms, Bytes(rooms));
				world.Add(GTAVPack::kPortals, Bytes(portals));
				world.Add(GTAVPack::kEntitySets, Bytes(sets));
				world.Add(GTAVPack::kInteriorPlacements, Bytes(placements));
				world.Add(GTAVPack::kGrassBatches, Bytes(batches));
				world.Add(GTAVPack::kGrassInstances, Bytes(grass));
				world.Add(GTAVPack::kLists, Bytes(lists));
				world.Add(GTAVPack::kWaterQuads, Bytes(water.quads));
				world.Add(GTAVPack::kCalmingQuads, Bytes(water.calming));
				world.Add(GTAVPack::kWaveQuads, Bytes(water.waves));
				return world.Close(error);
			}
		};
	}

	// The renderer's own textures from the game's shared dictionaries (water, rain): globals.gpk, keyed by the
	// joaat of their names.
	void WriteGlobals(const GameFiles& files, const fs::path& dir, std::vector<std::string>& warnings)
	{
		PackFile out;
		std::string error;
		if (!out.Open(dir / "globals.gpk", GTAVPack::kTextures, error))
		{
			warnings.push_back(error);
			return;
		}
		Resolver resolver(files);
		std::vector<std::string> names = {"waterbump", "waterbump2", "waternoise", "waterfoam", "caustic", "oceanwaves", "shorewaves", "waterfog",
		    "puddle_layout"};
		for (int i = 0; i < 64; ++i)
			names.push_back(std::format("puddlenormal_{:04}", i));
		size_t written = 0;
		for (const std::string& name : names)
		{
			Resolver::Texture t;
			if (!resolver.Find("graphics", name, t))
			{
				warnings.push_back("globals: no " + name);
				continue;
			}
			std::string why;
			const uint8_t mips = UsableMips(*t.texture, why);
			if (!mips)
			{
				warnings.push_back("globals: " + name + ": " + why);
				continue;
			}
			std::vector<uint8_t> data;
			Put(data, GTAVPack::TextureHeader{t.texture->width, t.texture->height, mips, t.texture->dxgi, 0});
			data.insert(data.end(), t.texture->data.begin(), t.texture->data.end());
			written += out.Add(Joaat(name), data);
		}
		if (!out.Close(error))
			warnings.push_back(error);
		std::printf("  %zu global textures\n", written);
	}

	// groups.txt: the names of the map data groups the map has (their ymaps'): the game is asked by name which are on.
	void WriteGroupNames(const mapdata::Map& map, const fs::path& dir)
	{
		std::ofstream out(dir / "groups.txt");
		for (const auto& g : map.groups)
			if (const auto y = map.ymapIndex.find(g.name); y != map.ymapIndex.end())
			{
				const std::string& file = map.ymaps[y->second].file;
				const size_t slash = file.find_last_of('/') + 1, dot = file.rfind('.');
				out << Lower(file.substr(slash, dot - slash)) << '\n';
			}
	}

	PackStats WritePack(const GameFiles& files, const mapdata::Map& map, const PackOptions& options, std::vector<std::string>& warnings)
	{
		std::error_code ec;
		fs::create_directories(options.dir, ec);
		Shared sh{map, options};
		std::string error;
		if (options.worldOnly)
		{
			// The models there are: models.gpk's table.
			std::ifstream in(options.dir / "models.gpk", std::ios::binary);
			GTAVPack::FileHeader header{};
			in.read(reinterpret_cast<char*>(&header), sizeof(header));
			std::vector<GTAVPack::Entry> entries(header.entryCount);
			in.seekg(static_cast<std::streamoff>(header.tableOffset));
			in.read(reinterpret_cast<char*>(entries.data()), static_cast<std::streamsize>(entries.size() * sizeof(GTAVPack::Entry)));
			if (!in || std::memcmp(header.magic, "GPK1", 4) != 0)
			{
				warnings.push_back("no models.gpk to keep");
				return sh.stats;
			}
			for (const auto& e : entries)
				sh.converted.insert(static_cast<uint32_t>(e.key));
			PackFile world;
			size_t entities = 0;
			const Water water = ReadWater(files, warnings);
			WorldWriter writer{map, sh.converted, water, {}};
			if (!world.Open(options.dir / "world.gpk", GTAVPack::kWorld, error) || !writer.Write(world, error, entities))
				warnings.push_back(error);
			WriteGlobals(files, options.dir, warnings);
			WriteGroupNames(map, options.dir);
			sh.stats.entities = entities;
			sh.stats.models = sh.converted.size();
			sh.stats.worldBytes = world.Bytes();
			return sh.stats;
		}
		if (!sh.models.Open(options.dir / "models.gpk", GTAVPack::kModels, error) || !sh.textures.Open(options.dir / "textures.gpk", GTAVPack::kTextures, error))
		{
			warnings.push_back(error);
			return sh.stats;
		}

		// The models, by worker threads.
		const std::vector<uint32_t> work = ModelsToConvert(map, options.area);
		const unsigned workers = options.workers ? options.workers : std::max(1u, std::thread::hardware_concurrency() - 2);
		std::atomic<size_t> next = 0, done = 0;
		std::atomic<unsigned> active = workers;
		std::vector<std::thread> threads;
		const auto start = std::chrono::steady_clock::now();
		for (unsigned i = 0; i < workers; ++i)
			threads.emplace_back(Work, std::ref(sh), std::cref(work), std::ref(next), std::ref(done), std::ref(active));
		for (size_t last = 0; active > 0;)
		{
			std::this_thread::sleep_for(std::chrono::seconds(2));
			if (done - last >= 2000 || done == work.size())
			{
				last = done;
				const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
				std::lock_guard lock(sh.mutex);
				std::printf("  %zu / %zu models, %zu textures (%.0f MB) in %.0f s\n", done.load(), work.size(), sh.stats.textures,
				    sh.textures.Bytes() / 1048576.0, s);
				std::fflush(stdout);
			}
		}
		for (auto& t : threads)
			t.join();

		// The map.
		PackFile world;
		size_t entities = 0;
		const Water water = ReadWater(files, warnings);
		WorldWriter writer{map, sh.converted, water, {}};
		if (!world.Open(options.dir / "world.gpk", GTAVPack::kWorld, error) || !writer.Write(world, error, entities))
			warnings.push_back(error);
		WriteGlobals(files, options.dir, warnings);
		WriteGroupNames(map, options.dir);
		sh.stats.entities = entities;
		sh.stats.modelBytes = sh.models.Bytes();
		sh.stats.textureBytes = sh.textures.Bytes();
		sh.stats.worldBytes = world.Bytes();
		for (PackFile* f : {&sh.models, &sh.textures})
			if (!f->Close(error))
				warnings.push_back(error);

		// What could not be converted.
		std::ofstream report(options.dir / "report.txt");
		report << std::format("models: {} converted, {} missing\n", sh.stats.models, sh.stats.missingModels);
		for (const auto& [why, n] : sh.missingModels)
			report << std::format("  missing model: {} x{}\n", why, n);
		report << std::format("textures: {} distinct, {} references missing ({} names)\n", sh.stats.textures, sh.stats.missingTextures,
		    sh.missingTextureNames.size());
		for (const auto& [why, n] : sh.unusable)
			report << std::format("  unusable texture: {} x{}\n", why, n);
		for (const auto& [bucket, n] : sh.unknownBuckets)
			report << std::format("  draw bucket {} drawn as opaque: {} materials\n", bucket, n);
		for (const auto& name : sh.missingTextureNames)
			report << "  missing texture: " << name << '\n';
		report << std::format("entities: {}\n", entities);
		for (const auto& w : sh.warnings)
			report << "warning: " << w << '\n';
		warnings.insert(warnings.end(), sh.warnings.begin(), sh.warnings.begin() + std::min<size_t>(sh.warnings.size(), 50));
		return sh.stats;
	}
}
