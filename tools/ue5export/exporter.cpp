#include "exporter.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <map>
#include <set>

#include <nlohmann/json.hpp>

#include "drawable.hpp"

namespace ue5export
{
	namespace fs = std::filesystem;
	using loader::convert::Bytes;

	namespace
	{
		std::string Lower(std::string s)
		{
			std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return s;
		}

		// joaat of a lower-cased name: how the game hashes file, archetype and shader parameter names.
		uint32_t Joaat(std::string_view s)
		{
			uint32_t h = 0;
			for (const char c : s)
			{
				h += static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(c)));
				h += h << 10;
				h ^= h >> 6;
			}
			h += h << 3;
			h ^= h >> 11;
			return h + (h << 15);
		}

		// Shader parameter names (matched by hash, research/phase0.md §58.4).
		const std::map<uint32_t, std::string>& ParameterNames()
		{
			static const std::map<uint32_t, std::string> names = [] {
				std::map<uint32_t, std::string> m;
				for (const char* n : {"DiffuseTex", "BumpTex", "SpecularTex", "DetailTex", "DirtTex", "DirtBumpTex", "TintPaletteTex",
				         "DiffuseTexture_layer0", "DiffuseTexture_layer1", "DiffuseTexture_layer2", "DiffuseTexture_layer3",
				         "BumpTexture_layer0", "BumpTexture_layer1", "BumpTexture_layer2", "BumpTexture_layer3", "HeightTexture",
				         "LookupTexture", "NoiseTex", "SnowTex", "DamageTexture", "PlateBgTex", "PlateBgBumpTex", "VolumeTex", "bumpiness",
				         "wetnessMultiplier", "useTessellation", "HardAlphaBlend", "globalAnimUV0", "globalAnimUV1", "emissiveMultiplier",
				         "specularIntensityMult", "specularFalloffMult", "specularFresnel", "detailSettings", "tintPaletteSelector",
				         "AlphaTest", "alphaTestValue", "materialWetnessMultiplier", "parallaxSelfShadowAmount", "heightScale", "heightBias",
				         "umGlobalParams", "WindGlobalParams"})
					m[Joaat(n)] = n;
				return m;
			}();
			return names;
		}

		std::string ParameterName(uint32_t hash)
		{
			const auto& names = ParameterNames();
			const auto it = names.find(hash);
			return it != names.end() ? it->second : std::format("{:08x}", hash);
		}

		template <typename T>
		void Write(std::ofstream& out, const T& value)
		{
			out.write(reinterpret_cast<const char*>(&value), sizeof(T));
		}
		template <typename T>
		void WriteAll(std::ofstream& out, const std::vector<T>& values)
		{
			out.write(reinterpret_cast<const char*>(values.data()), static_cast<std::streamsize>(values.size() * sizeof(T)));
		}

		// gmesh: "GMSH", version 1, section count, bounds; per section: material, vertices, indices, flags (1 normals,
		// 2 tangents, 4 colour 0, 8 colour 1), UV sets; then positions, [normals], [tangents], [colours], UVs, indices.
		void WriteMesh(const fs::path& file, const drawable::Model& m)
		{
			std::ofstream out(file, std::ios::binary);
			out.write("GMSH", 4);
			Write(out, uint32_t{1});
			Write(out, static_cast<uint32_t>(m.sections.size()));
			for (const float f : m.bbMin)
				Write(out, f);
			for (const float f : m.bbMax)
				Write(out, f);
			for (const auto& s : m.sections)
			{
				Write(out, s.material);
				Write(out, s.vertices);
				Write(out, static_cast<uint32_t>(s.indices.size()));
				Write(out, uint32_t{(s.normal.empty() ? 0u : 1u) | (s.tangent.empty() ? 0u : 2u) | (s.color0.empty() ? 0u : 4u) | (s.color1.empty() ? 0u : 8u)});
				Write(out, static_cast<uint32_t>(s.uv.size()));
				WriteAll(out, s.position);
				WriteAll(out, s.normal);
				WriteAll(out, s.tangent);
				WriteAll(out, s.color0);
				WriteAll(out, s.color1);
				for (const auto& uv : s.uv)
					WriteAll(out, uv);
				WriteAll(out, s.indices);
			}
		}

		// The texture data as a DDS with the DX10 header (any DXGI format).
		bool WriteDds(const fs::path& file, const drawable::Texture& t)
		{
			if (t.data.empty() || t.depth > 1)
				return false;
			std::error_code ec;
			fs::create_directories(file.parent_path(), ec);
			std::ofstream out(file, std::ios::binary);
			uint32_t h[31] = {};
			h[0] = 124;                                   // size
			h[1] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000;    // caps, height, width, pixel format, mip count
			h[2] = t.height;
			h[3] = t.width;
			h[6] = t.mips;
			h[18] = 32;                                   // pixel format: size
			h[19] = 0x4;                                  // FourCC
			h[20] = 0x30315844;                           // 'DX10'
			h[26] = 0x1000 | (t.mips > 1 ? 0x400008u : 0u); // texture, complex + mipmap
			out.write("DDS ", 4);
			out.write(reinterpret_cast<const char*>(h), sizeof(h));
			const uint32_t dx10[5] = {t.dxgi, 3, 0, 1, 0}; // format, 2D, flags, array size 1
			out.write(reinterpret_cast<const char*>(dx10), sizeof(dx10));
			out.write(reinterpret_cast<const char*>(t.data.data()), static_cast<std::streamsize>(t.data.size()));
			return true;
		}

		// A texture dictionary (ytd), read once.
		// The texture dictionaries (ytd), each read once, with the game's parent dictionaries (gtxd: shared textures)
		// and HD versions ("<name>+hi.ytd", a drawable's "<name>+hidr.ytd": the textures the game swaps in up close).
		struct Dictionaries
		{
			const GameFiles& files;
			std::map<uint32_t, std::string> names;                                // name hash -> ytd file
			std::map<uint32_t, std::map<std::string, drawable::Texture>> loaded;  // name hash -> textures by lower name
			std::map<uint32_t, uint32_t> parents;                                 // dictionary -> parent dictionary

			const std::map<std::string, drawable::Texture>* Get(uint32_t hash)
			{
				if (const auto it = loaded.find(hash); it != loaded.end())
					return &it->second;
				auto& textures = loaded[hash];
				const auto name = names.find(hash);
				const GameFiles::File* file = name == names.end() ? nullptr : files.Find(name->second);
				Bytes data;
				std::string error;
				loader::convert::Resource resource;
				if (file && files.Read(*file, data, error) && loader::convert::Read(data, resource, error))
				{
					const drawable::View view(resource);
					for (auto& t : drawable::Textures(view, drawable::View::kRoot))
						textures[Lower(t.name)] = std::move(t);
				}
				return &textures;
			}

			std::string Name(uint32_t hash) const
			{
				const auto it = names.find(hash);
				return it == names.end() ? std::string() : it->second.substr(0, it->second.size() - 4);
			}

			// A texture by name in the dictionary or its HD version; the dictionary it was in.
			const drawable::Texture* In(uint32_t hash, const std::string& key, std::string& where)
			{
				const std::string name = Name(hash);
				for (const std::string& suffix : {std::string("+hi"), std::string()})
				{
					if (name.empty() && !suffix.empty())
						continue;
					const uint32_t h = suffix.empty() ? hash : Joaat(name + suffix);
					if (const auto* dict = Get(h); dict && dict->contains(key))
					{
						where = suffix.empty() ? name : name + suffix;
						return &dict->at(key);
					}
				}
				return nullptr;
			}

			// The archetype's dictionary, then its parents.
			const drawable::Texture* Chain(uint32_t hash, const std::string& key, std::string& where)
			{
				for (int depth = 0; hash && depth < 16; ++depth)
				{
					if (const auto* t = In(hash, key, where))
						return t;
					const auto it = parents.find(hash);
					hash = it == parents.end() ? 0 : it->second;
				}
				return nullptr;
			}

			// gtxd: the base game's binary table (RBF: string values "FD FF <u16 length> <u16> text\0", parent then
			// child) and the DLC packs' gtxd.meta (<parent>, <child>).
			void ReadParents()
			{
				const auto add = [&](const std::string& parent, const std::string& child) {
					const uint32_t p = Joaat(parent), c = Joaat(child);
					parents[c] = p;
					names.try_emplace(p, Lower(parent) + ".ytd");
				};
				Bytes data;
				std::string error;
				if (const GameFiles::File* f = files.Find("gtxd.ymt"); f && files.Read(*f, data, error))
				{
					std::vector<std::string> values;
					for (size_t i = 0; i + 6 < data.size(); ++i)
						if (data[i] == 0xFD && data[i + 1] == 0xFF)
						{
							const size_t n = data[i + 2] | data[i + 3] << 8;
							if (n && i + 6 + n <= data.size() && data[i + 5 + n] == 0)
							{
								values.emplace_back(reinterpret_cast<const char*>(data.data() + i + 6), n - 1);
								i += 5 + n;
							}
						}
					for (size_t k = 0; k + 1 < values.size(); k += 2)
						add(values[k], values[k + 1]);
				}
				for (const GameFiles::File& f : files.AllOf("gtxd.meta"))
				{
					if (!files.Read(f, data, error))
						continue;
					const std::string xml(data.begin(), data.end());
					for (size_t at = xml.find("<parent>"); at != std::string::npos; at = xml.find("<parent>", at + 1))
					{
						const size_t end = xml.find("</parent>", at), child = xml.find("<child>", at), childEnd = xml.find("</child>", at);
						if (end != std::string::npos && child != std::string::npos && childEnd != std::string::npos)
							add(xml.substr(at + 8, end - at - 8), xml.substr(child + 7, childEnd - child - 7));
					}
				}
			}
		};
	}

	ExportStats ExportArea(const GameFiles& files, const mapdata::Map& map, const Area& area, const fs::path& out, std::vector<std::string>& warnings)
	{
		ExportStats stats;
		const fs::path root = out / "export";
		std::error_code ec;
		for (const char* dir : {"models", "textures", "areas"})
			fs::create_directories(root / dir, ec);

		// Files by the hash of their name (without extension), per extension.
		std::map<std::string, std::map<uint32_t, std::string>> byHash;
		for (const auto& [name, file] : files.Files())
			if (const size_t dot = name.rfind('.'); dot != std::string::npos)
				byHash[name.substr(dot + 1)][Joaat(name.substr(0, dot))] = name;
		const auto stem = [&](uint32_t hash) -> std::string {
			for (const char* ext : {"ydr", "yft", "ydd", "ytd"})
				if (const auto it = byHash[ext].find(hash); it != byHash[ext].end())
					return it->second.substr(0, it->second.rfind('.'));
			return std::format("{:08x}", hash);
		};
		Dictionaries dictionaries{files, byHash["ytd"], {}, {}};
		dictionaries.ReadParents();

		// The area's entities at the highest level of detail.
		nlohmann::json entities = nlohmann::json::array();
		std::set<uint32_t> archetypes;
		for (const auto& e : map.entities)
		{
			if ((e.lodLevel != mapdata::kHd && e.lodLevel != mapdata::kOrphanHd) || e.interior)
				continue;
			if (e.position[0] < area.bounds[0] || e.position[1] < area.bounds[1] || e.position[0] > area.bounds[2] || e.position[1] > area.bounds[3])
				continue;
			const auto a = map.archetypes.find(e.archetype);
			if (a == map.archetypes.end() || a->second.assetType == mapdata::AssetType::Assetless)
				continue;
			archetypes.insert(e.archetype);
			entities.push_back({{"archetype", stem(e.archetype)},
			    {"position", e.position},
			    {"rotation", e.rotation},
			    {"scale", {e.scaleXY, e.scaleXY, e.scaleZ}},
			    {"flags", e.flags}});
		}
		stats.entities = entities.size();

		std::set<std::string> written; // textures, "dictionary/texture"
		for (const uint32_t hash : archetypes)
		{
			const mapdata::Archetype& a = map.archetypes.at(hash);
			const std::string name = stem(hash);
			if (fs::exists(root / "models" / (name + ".gmesh")))
			{
				++stats.models;
				continue;
			}
			// The model file: a drawable, a fragment, or a drawable in a dictionary.
			std::string file, ext;
			uint32_t inDictionary = 0;
			switch (a.assetType)
			{
			case mapdata::AssetType::Drawable:
				ext = "ydr";
				break;
			case mapdata::AssetType::Fragment:
				ext = "yft";
				break;
			case mapdata::AssetType::DrawableDictionary:
				ext = "ydd", inDictionary = a.assetName;
				break;
			default:
				break;
			}
			const uint32_t fileHash = a.assetType == mapdata::AssetType::DrawableDictionary ? a.drawableDictionary : a.assetName;
			if (const auto it = byHash[ext].find(fileHash); !ext.empty() && it != byHash[ext].end())
				file = it->second;
			const GameFiles::File* f = file.empty() ? nullptr : files.Find(file);
			Bytes data;
			std::string error;
			loader::convert::Resource resource;
			if (!f || !files.Read(*f, data, error) || !loader::convert::Read(data, resource, error))
			{
				++stats.missingModels;
				warnings.push_back(std::format("{}: no model ({}{})", name, file.empty() ? "file not found" : file, error.empty() ? "" : ": " + error));
				continue;
			}
			const drawable::View view(resource);
			const auto drawables = drawable::Drawables(view, ext);
			const auto d = drawables.find(inDictionary);
			drawable::Model model;
			if (d == drawables.end() || !drawable::Read(view, d->second, model, error))
			{
				++stats.missingModels;
				warnings.push_back(std::format("{}: {} has no drawable {:08x} {}", name, file, inDictionary, error));
				continue;
			}
			for (const auto& w : model.warnings)
				warnings.push_back(name + ": " + w);

			// Materials. A texture: the drawable's HD dictionary, its own, then the archetype's dictionary and its
			// parents (HD versions first).
			std::map<std::string, const drawable::Texture*> embedded;
			for (const auto& t : model.embedded)
				embedded[Lower(t.name)] = &t;
			nlohmann::json materials = nlohmann::json::array();
			for (const auto& m : model.materials)
			{
				nlohmann::json textures = nlohmann::json::object(), constants = nlohmann::json::object();
				for (const auto& [param, textureName] : m.textures)
				{
					const std::string key = Lower(textureName);
					std::string where;
					const drawable::Texture* t = nullptr;
					if (const auto* hd = dictionaries.Get(Joaat(name + "+hidr")); hd && hd->contains(key))
						t = &hd->at(key), where = name + "+hidr";
					else if (const auto it = embedded.find(key); it != embedded.end())
						t = it->second, where = name;
					else
						t = dictionaries.Chain(a.textureDictionary, key, where);
					if (!t) // (the map's shared detail textures: env_*)
						t = dictionaries.In(Joaat("mapdetail"), key, where);
					if (!t)
					{
						++stats.missingTextures;
						textures[ParameterName(param)] = "?" + key;
						continue;
					}
					const std::string path = where + "/" + key;
					if (written.insert(path).second && WriteDds(root / "textures" / where / (key + ".dds"), *t))
						++stats.textures;
					textures[ParameterName(param)] = path;
				}
				for (const auto& [param, values] : m.constants)
					constants[ParameterName(param)] = values;
				materials.push_back({{"effect", std::format("{:08x}", m.effect)}, {"bucket", m.drawBucket}, {"textures", textures}, {"constants", constants}});
			}
			WriteMesh(root / "models" / (name + ".gmesh"), model);
			nlohmann::json info = {{"name", name}, {"file", f->path}, {"bbMin", model.bbMin}, {"bbMax", model.bbMax}, {"materials", materials}};
			std::ofstream(root / "models" / (name + ".json")) << info.dump(1);
			++stats.models;
		}
		nlohmann::json areaJson = {{"name", area.name}, {"bounds", area.bounds}, {"entities", entities}};
		std::ofstream(root / "areas" / (area.name + ".json")) << areaJson.dump(1);
		return stats;
	}
}
