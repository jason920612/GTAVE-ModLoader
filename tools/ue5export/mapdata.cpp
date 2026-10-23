#include "mapdata.hpp"

#include "meta.hpp"

namespace ue5export::mapdata
{
	namespace
	{
		using meta::Hash;

		constexpr uint32_t kCMapTypes = Hash("CMapTypes"), kCMapData = Hash("CMapData");
		constexpr uint32_t kCBaseArchetypeDef = Hash("CBaseArchetypeDef"), kCTimeArchetypeDef = Hash("CTimeArchetypeDef");
		constexpr uint32_t kCMloArchetypeDef = Hash("CMloArchetypeDef"), kCMloInstanceDef = Hash("CMloInstanceDef");

		std::array<float, 3> Vec3(const meta::Ref& r, uint32_t field)
		{
			const auto v = r.Get<std::array<float, 4>>(field);
			return {v[0], v[1], v[2]};
		}

		void ReadArchetype(const meta::Ref& a, const std::string& source, Map& map)
		{
			Archetype t;
			t.name = a.Get<uint32_t>(Hash("name"));
			t.textureDictionary = a.Get<uint32_t>(Hash("textureDictionary"));
			t.drawableDictionary = a.Get<uint32_t>(Hash("drawableDictionary"));
			t.assetName = a.Get<uint32_t>(Hash("assetName"));
			t.assetType = static_cast<AssetType>(a.Get<uint32_t>(Hash("assetType")));
			t.flags = a.Get<uint32_t>(Hash("flags"));
			t.lodDist = a.Get<float>(Hash("lodDist"));
			t.bbMin = Vec3(a, Hash("bbMin"));
			t.bbMax = Vec3(a, Hash("bbMax"));
			t.interior = a.type->name == kCMloArchetypeDef;
			t.timed = a.type->name == kCTimeArchetypeDef;
			t.source = source;
			map.archetypes[t.name] = std::move(t);
		}

		Entity ReadEntity(const meta::Ref& e, uint32_t ymap)
		{
			Entity x;
			x.archetype = e.Get<uint32_t>(Hash("archetypeName"));
			x.position = Vec3(e, Hash("position"));
			x.rotation = e.Get<std::array<float, 4>>(Hash("rotation"));
			x.scaleXY = e.Get<float>(Hash("scaleXY"));
			x.scaleZ = e.Get<float>(Hash("scaleZ"));
			x.flags = e.Get<uint32_t>(Hash("flags"));
			x.lodLevel = e.Get<uint32_t>(Hash("lodLevel"));
			x.lodDist = e.Get<float>(Hash("lodDist"));
			x.parentIndex = e.Get<int32_t>(Hash("parentIndex"));
			x.interior = e.type->name == kCMloInstanceDef;
			x.ymap = ymap;
			return x;
		}
	}

	Map Read(const GameFiles& files)
	{
		Map map;
		for (const auto& [name, file] : files.Files())
		{
			const bool ytyp = name.ends_with(".ytyp"), ymap = name.ends_with(".ymap");
			if (!ytyp && !ymap)
				continue;
			loader::convert::Bytes data;
			std::string error;
			meta::Meta m;
			if (!files.Read(file, data, error) || !m.Read(data, error))
			{
				map.errors.push_back(file.path + ": " + error);
				continue;
			}
			const meta::Ref root = m.Root();
			if (ytyp)
			{
				++map.ytypCount;
				if (!root || root.type->name != kCMapTypes)
				{
					map.errors.push_back(file.path + ": no CMapTypes");
					continue;
				}
				for (const meta::Ref& a : root.Array(Hash("archetypes")))
					if (a)
						ReadArchetype(a, file.path, map);
				continue;
			}
			++map.ymapCount;
			if (!root || root.type->name != kCMapData)
			{
				map.errors.push_back(file.path + ": no CMapData");
				continue;
			}
			const uint32_t ymapName = Hash(name.substr(0, name.size() - 5));
			map.ymapNames[ymapName] = name;
			for (const meta::Ref& e : root.Array(Hash("entities")))
				if (e)
					map.entities.push_back(ReadEntity(e, ymapName));
		}
		return map;
	}
}
