#include "exporter.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <map>
#include <set>

#include <nlohmann/json.hpp>

#include "drawable.hpp"
#include "resolve.hpp"

namespace ue5export
{
	namespace fs = std::filesystem;

	namespace
	{
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
	}

	bool InArea(const mapdata::Archetype& a, const mapdata::Entity& e, const Area& area)
	{
		// In the area when its bounds reach into it (big ground and road meshes have their origin far away).
		float radius = 0;
		for (int i = 0; i < 3; ++i)
			radius = std::max({radius, std::abs(a.bbMin[i]), std::abs(a.bbMax[i])});
		radius *= std::max(e.scaleXY, e.scaleZ) * 1.7321f;
		return e.position[0] + radius >= area.bounds[0] && e.position[1] + radius >= area.bounds[1] && e.position[0] - radius <= area.bounds[2] &&
		       e.position[1] - radius <= area.bounds[3];
	}

	ExportStats ExportArea(const GameFiles& files, const mapdata::Map& map, const Area& area, const fs::path& out, std::vector<std::string>& warnings)
	{
		ExportStats stats;
		const fs::path root = out / "export";
		std::error_code ec;
		for (const char* dir : {"models", "textures", "areas"})
			fs::create_directories(root / dir, ec);
		Resolver resolver(files);

		// The area's entities at the highest level of detail.
		nlohmann::json entities = nlohmann::json::array();
		std::set<uint32_t> archetypes;
		for (const auto& e : map.entities)
		{
			if (e.interior && area.interiors)
			{
				// (an interior's models: what is in its rooms)
				const auto a = map.archetypes.find(e.archetype);
				if (a != map.archetypes.end() && a->second.interior >= 0 && InArea(a->second, e, area))
					for (const auto& inside : map.interiors[a->second.interior].entities)
						if (const auto b = map.archetypes.find(inside.archetype); b != map.archetypes.end() && b->second.assetType != mapdata::AssetType::Assetless)
							archetypes.insert(inside.archetype);
				continue;
			}
			if ((e.lodLevel != mapdata::kHd && e.lodLevel != mapdata::kOrphanHd) || e.interior)
				continue;
			const auto a = map.archetypes.find(e.archetype);
			if (a == map.archetypes.end() || a->second.assetType == mapdata::AssetType::Assetless || !InArea(a->second, e, area))
				continue;
			archetypes.insert(e.archetype);
			entities.push_back({{"archetype", resolver.Stem(e.archetype)},
			    {"position", e.position},
			    {"rotation", e.rotation},
			    {"scale", {e.scaleXY, e.scaleXY, e.scaleZ}},
			    {"flags", e.flags}});
		}
		stats.entities = entities.size();

		std::set<std::string> written; // textures, "dictionary/texture"
		for (const uint32_t hash : archetypes)
		{
			const std::string name = resolver.Stem(hash);
			if (fs::exists(root / "models" / (name + ".gmesh")))
			{
				++stats.models;
				continue;
			}
			Resolver::Model model;
			std::string error;
			if (!resolver.Load(map.archetypes.at(hash), model, error))
			{
				++stats.missingModels;
				warnings.push_back(name + ": " + error);
				continue;
			}
			for (const auto& w : model.model.warnings)
				warnings.push_back(name + ": " + w);
			stats.missingTextures += model.missing.size();
			nlohmann::json materials = nlohmann::json::array();
			for (size_t i = 0; i < model.model.materials.size(); ++i)
			{
				const auto& m = model.model.materials[i];
				nlohmann::json textures = nlohmann::json::object(), constants = nlohmann::json::object();
				for (const auto& [param, textureName] : m.textures)
				{
					const auto t = model.textures[i].find(param);
					if (t == model.textures[i].end())
					{
						textures[ParameterName(param)] = "?" + Lower(textureName);
						continue;
					}
					const std::string path = t->second.where + "/" + t->second.key;
					if (written.insert(path).second && WriteDds(root / "textures" / t->second.where / (t->second.key + ".dds"), *t->second.texture))
						++stats.textures;
					textures[ParameterName(param)] = path;
				}
				for (const auto& [param, values] : m.constants)
					constants[ParameterName(param)] = values;
				materials.push_back({{"effect", std::format("{:08x}", m.effect)}, {"bucket", m.drawBucket}, {"textures", textures}, {"constants", constants}});
			}
			WriteMesh(root / "models" / (name + ".gmesh"), model.model);
			nlohmann::json info = {{"name", name}, {"file", model.file}, {"bbMin", model.model.bbMin}, {"bbMax", model.model.bbMax}, {"materials", materials}};
			std::ofstream(root / "models" / (name + ".json")) << info.dump(1);
			++stats.models;
		}
		nlohmann::json areaJson = {{"name", area.name}, {"bounds", area.bounds}, {"entities", entities}};
		std::ofstream(root / "areas" / (area.name + ".json")) << areaJson.dump(1);
		return stats;
	}
}
