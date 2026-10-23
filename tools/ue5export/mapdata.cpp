#include "mapdata.hpp"

#include "meta.hpp"
#include "pso.hpp"

namespace ue5export::mapdata
{
	namespace
	{
		using meta::Hash;

		constexpr uint32_t kCMapTypes = Hash("CMapTypes"), kCMapData = Hash("CMapData");
		constexpr uint32_t kCTimeArchetypeDef = Hash("CTimeArchetypeDef"), kCMloArchetypeDef = Hash("CMloArchetypeDef");
		constexpr uint32_t kCMloInstanceDef = Hash("CMloInstanceDef");

		Vec3 V3(const meta::Ref& r, uint32_t field)
		{
			const auto v = r.Get<std::array<float, 4>>(field);
			return {v[0], v[1], v[2]};
		}

		Entity ReadEntity(const meta::Ref& e, uint32_t ymap)
		{
			Entity x;
			x.archetype = e.Get<uint32_t>(Hash("archetypeName"));
			x.position = V3(e, Hash("position"));
			x.rotation = e.Get<std::array<float, 4>>(Hash("rotation"));
			x.scaleXY = e.Get<float>(Hash("scaleXY"));
			x.scaleZ = e.Get<float>(Hash("scaleZ"));
			x.flags = e.Get<uint32_t>(Hash("flags"));
			x.guid = e.Get<uint32_t>(Hash("guid"));
			x.lodLevel = e.Get<uint32_t>(Hash("lodLevel"));
			x.lodDist = e.Get<float>(Hash("lodDist"));
			x.childLodDist = e.Get<float>(Hash("childLodDist"));
			x.parentIndex = e.Get<int32_t>(Hash("parentIndex"));
			x.numChildren = e.Get<uint32_t>(Hash("numChildren"));
			x.priorityLevel = e.Get<uint32_t>(Hash("priorityLevel"));
			x.tintValue = e.Get<uint32_t>(Hash("tintValue"));
			x.ymap = ymap;
			if (e.type->name == kCMloInstanceDef)
			{
				x.interior = true;
				x.groupId = e.Get<uint32_t>(Hash("groupId"));
				x.floorId = e.Get<uint32_t>(Hash("floorId"));
				x.mloFlags = e.Get<uint32_t>(Hash("MLOInstflags"));
				x.defaultEntitySets = e.U32Array(Hash("defaultEntitySets"));
			}
			return x;
		}

		Interior ReadInterior(const meta::Ref& a)
		{
			Interior in;
			in.mloFlags = a.Get<uint32_t>(Hash("mloFlags"));
			for (const meta::Ref& e : a.Array(Hash("entities")))
				if (e)
					in.entities.push_back(ReadEntity(e, 0));
			for (const meta::Ref& r : a.Array(Hash("rooms")))
				if (r)
					in.rooms.push_back({V3(r, Hash("bbMin")), V3(r, Hash("bbMax")), r.Get<uint32_t>(Hash("flags")), r.Get<int32_t>(Hash("floorId")),
					    r.U32Array(Hash("attachedObjects"))});
			for (const meta::Ref& p : a.Array(Hash("portals")))
			{
				if (!p)
					continue;
				Portal portal{p.Get<uint32_t>(Hash("roomFrom")), p.Get<uint32_t>(Hash("roomTo")), p.Get<uint32_t>(Hash("flags")),
				    p.Get<uint32_t>(Hash("opacity")), {}, p.U32Array(Hash("attachedObjects"))};
				for (const auto& c : p.VectorArray(Hash("corners")))
					portal.corners.push_back({c[0], c[1], c[2]});
				in.portals.push_back(std::move(portal));
			}
			for (const meta::Ref& s : a.Array(Hash("entitySets")))
			{
				if (!s)
					continue;
				EntitySet set{s.Get<uint32_t>(Hash("name")), s.U32Array(Hash("locations")), {}};
				for (const meta::Ref& e : s.Array(Hash("entities")))
					if (e)
						set.entities.push_back(ReadEntity(e, 0));
				in.entitySets.push_back(std::move(set));
			}
			return in;
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
			t.specialAttribute = a.Get<uint32_t>(Hash("specialAttribute"));
			t.lodDist = a.Get<float>(Hash("lodDist"));
			t.hdTextureDist = a.Get<float>(Hash("hdTextureDist"));
			t.bbMin = V3(a, Hash("bbMin"));
			t.bbMax = V3(a, Hash("bbMax"));
			t.bsCentre = V3(a, Hash("bsCentre"));
			t.bsRadius = a.Get<float>(Hash("bsRadius"));
			if (a.type->name == kCTimeArchetypeDef)
			{
				t.timed = true;
				t.timeFlags = a.Get<uint32_t>(Hash("timeFlags"));
			}
			if (a.type->name == kCMloArchetypeDef)
			{
				t.interior = static_cast<int32_t>(map.interiors.size());
				map.interiors.push_back(ReadInterior(a));
			}
			t.source = source;
			map.archetypes[t.name] = std::move(t);
		}

		void ReadGrass(const meta::Ref& root, Ymap& y)
		{
			const meta::Ref data = root.Struct(Hash("instancedData"), Hash("rage__fwInstancedMapData"));
			if (!data)
				return;
			for (const meta::Ref& g : data.Array(Hash("GrassInstanceList")))
			{
				if (!g)
					continue;
				GrassBatch b;
				const meta::Ref box = g.Struct(Hash("BatchAABB"), Hash("rage__spdAABB"));
				b.bbMin = V3(box, Hash("min"));
				b.bbMax = V3(box, Hash("max"));
				b.scaleRange = V3(g, Hash("ScaleRange"));
				b.archetype = g.Get<uint32_t>(Hash("archetypeName"));
				b.lodDist = g.Get<uint32_t>(Hash("lodDist"));
				b.fadeStart = g.Get<float>(Hash("LodFadeStartDist"));
				b.fadeRange = g.Get<float>(Hash("LodInstFadeRange"));
				b.orientToTerrain = g.Get<float>(Hash("OrientToTerrain"));
				const auto list = g.Array(Hash("InstanceList"));
				b.instances.resize(list.size());
				for (size_t i = 0; i < list.size(); ++i)
					if (list[i].at + sizeof(GrassInstance) <= list[i].meta->Size())
						std::memcpy(&b.instances[i], list[i].meta->At(list[i].at), sizeof(GrassInstance));
				y.grass.push_back(std::move(b));
			}
		}

		// The map data groups of every archive's manifest.
		void ReadGroups(const GameFiles& files, Map& map)
		{
			for (const GameFiles::File& f : files.AllOf("_manifest.ymf"))
			{
				loader::convert::Bytes data;
				std::string error;
				pso::Pso p;
				if (!files.Read(f, data, error) || !p.Read(data, error))
				{
					map.errors.push_back(f.path + ": " + error);
					continue;
				}
				for (const pso::Ref& g : p.Root().Array(Hash("MapDataGroups")))
					map.groups.push_back({g.U32(Hash("Name")), g.U32(Hash("Flags")), g.U32(Hash("HoursOnOff")), g.U32Array(Hash("Bounds")),
					    g.U32Array(Hash("WeatherTypes"))});
			}
		}
	}

	// The parent is the next level of detail out: in the parent ymap, unless the entity's own ymap holds that level (a
	// LOD ymap keeps its LODs and their SLOD1s together; research/phase0.md §59.3).
	int64_t Map::Parent(size_t entity) const
	{
		const Entity& e = entities[entity];
		const Ymap& y = ymaps[e.ymap];
		if (e.parentIndex < 0)
			return -1;
		const uint32_t coarser = e.lodLevel == kHd || e.lodLevel == kOrphanHd ? kLod : e.lodLevel == kLod ? kSlod1 : e.lodLevel == kSlod1 ? kSlod2
		                       : e.lodLevel == kSlod2 ? kSlod3 : kSlod4;
		if ((y.lodLevels >> coarser) & 1)
			return static_cast<uint32_t>(e.parentIndex) < y.entityCount ? int64_t{y.firstEntity} + e.parentIndex : -1;
		if (y.parent < 0)
			return -1;
		const Ymap& p = ymaps[y.parent];
		return static_cast<uint32_t>(e.parentIndex) < p.entityCount ? int64_t{p.firstEntity} + e.parentIndex : -1;
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
			Ymap y;
			y.name = Hash(name.substr(0, name.size() - 5)); // (lower case, as the file names are)
			y.parentName = root.Get<uint32_t>(Hash("parent"));
			y.flags = root.Get<uint32_t>(Hash("flags"));
			y.contentFlags = root.Get<uint32_t>(Hash("contentFlags"));
			y.streamingMin = V3(root, Hash("streamingExtentsMin"));
			y.streamingMax = V3(root, Hash("streamingExtentsMax"));
			y.entitiesMin = V3(root, Hash("entitiesExtentsMin"));
			y.entitiesMax = V3(root, Hash("entitiesExtentsMax"));
			y.firstEntity = static_cast<uint32_t>(map.entities.size());
			y.file = file.path;
			const uint32_t index = static_cast<uint32_t>(map.ymaps.size());
			for (const meta::Ref& e : root.Array(Hash("entities")))
				if (e)
					map.entities.push_back(ReadEntity(e, index));
			y.entityCount = static_cast<uint32_t>(map.entities.size()) - y.firstEntity;
			for (uint32_t i = 0; i < y.entityCount; ++i)
				y.lodLevels |= 1u << (map.entities[y.firstEntity + i].lodLevel & 31);
			ReadGrass(root, y);
			map.ymapIndex[y.name] = index;
			map.ymaps.push_back(std::move(y));
		}
		for (Ymap& y : map.ymaps)
			if (const auto it = map.ymapIndex.find(y.parentName); y.parentName && it != map.ymapIndex.end())
				y.parent = static_cast<int32_t>(it->second);
		ReadGroups(files, map);
		// (a group is the ymap of the same name: what REQUEST_IPL switches on)
		for (size_t g = 0; g < map.groups.size(); ++g)
			if (const auto it = map.ymapIndex.find(map.groups[g].name); it != map.ymapIndex.end() && map.ymaps[it->second].group < 0)
				map.ymaps[it->second].group = static_cast<int32_t>(g);
		return map;
	}
}
