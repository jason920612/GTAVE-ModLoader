#include "mapfiles.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "../ue5export/meta.hpp"
#include "meta.hpp"
#include "pages.hpp"

namespace worldgen::mapfiles
{
	using loader::convert::Bytes;

	namespace
	{
		// A structure's fields by name, from the template's definitions.
		struct Fields
		{
			const meta::Writer& w;
			uint32_t structure;
			Bytes data;
			bool ok = true;

			Fields(const meta::Writer& writer, const char* name) : w(writer), structure(meta::Hash(name)), data(writer.Size(meta::Hash(name)), 0) {}
			void Set(const char* field, const void* value, size_t size)
			{
				const int at = w.Offset(structure, meta::Hash(field));
				if (at < 0 || at + size > data.size())
					ok = false;
				else
					std::memcpy(data.data() + at, value, size);
			}
			template <typename T>
			void Set(const char* field, T value)
			{
				Set(field, &value, sizeof(T));
			}
			void Vec(const char* field, const std::array<float, 3>& v, float w4 = 0)
			{
				const float f[4] = {v[0], v[1], v[2], w4};
				Set(field, f, 16);
			}
			// an array field: pointer, count, capacity
			void Array(const char* field, uint64_t pointer, uint16_t count)
			{
				const int at = w.Offset(structure, meta::Hash(field));
				if (at < 0 || at + 12 > static_cast<int>(data.size()))
				{
					ok = false;
					return;
				}
				std::memcpy(data.data() + at, &pointer, 8);
				std::memcpy(data.data() + at + 8, &count, 2);
				std::memcpy(data.data() + at + 10, &count, 2);
			}
		};

		bool Template(const ue5export::GameFiles& files, const char* file, meta::Writer& w, std::string& error)
		{
			Bytes data;
			const auto* f = files.Find(file);
			if (!f || !files.Read(*f, data, error) || !w.Template(data, error))
				return error = std::string("template ") + file + ": " + error, false;
			return true;
		}

		// Structures of one kind, as many to a block as fit, and the block of pointers to them.
		template <typename Fill>
		bool Blocks(meta::Writer& w, const char* structure, size_t count, Fill fill, uint64_t& pointers, std::string& error)
		{
			const uint32_t hash = meta::Hash(structure), size = w.Size(hash);
			if (!size)
				return error = std::string(structure) + " is not in the template", false;
			std::vector<uint64_t> list;
			const uint32_t perBlock = Pages::kSize / size;
			for (size_t i = 0; i < count; i += perBlock)
			{
				const size_t n = std::min<size_t>(perBlock, count - i);
				Bytes block(n * size, 0);
				const uint32_t number = w.AddBlock(hash, {});
				for (size_t k = 0; k < n; ++k)
				{
					Fields f(w, structure);
					fill(i + k, f);
					if (!f.ok)
						return error = std::string("a field of ") + structure + " is not in the template", false;
					std::memcpy(block.data() + k * size, f.data.data(), size);
					list.push_back(meta::Writer::Pointer(number, static_cast<uint32_t>(k * size)));
				}
				w.Block(number) = std::move(block);
			}
			Bytes pointerBlock(list.size() * 8);
			std::memcpy(pointerBlock.data(), list.data(), pointerBlock.size());
			pointers = list.empty() ? 0 : meta::Writer::Pointer(w.AddBlock(7, std::move(pointerBlock)), 0);
			return true;
		}
	}

	bool FindArchetype(const ue5export::GameFiles& files, const std::string& name, Archetype& out, std::string& error)
	{
		using ue5export::meta::Hash;
		const uint32_t wanted = loader::convert::Joaat(name);
		for (const auto& [file, f] : files.Files())
		{
			if (!file.ends_with(".ytyp"))
				continue;
			Bytes data;
			ue5export::meta::Meta m;
			std::string ignored;
			if (!files.Read(f, data, ignored) || !m.Read(data, ignored))
				continue;
			for (const ue5export::meta::Ref& a : m.Root().Array(Hash("archetypes")))
			{
				if (!a || a.Get<uint32_t>(Hash("name")) != wanted)
					continue;
				out = {};
				out.name = name;
				out.textureDictionary = a.Get<uint32_t>(Hash("textureDictionary"));
				out.physicsDictionary = a.Get<uint32_t>(Hash("physicsDictionary"));
				out.flags = a.Get<uint32_t>(Hash("flags"));
				out.lodDist = a.Get<float>(Hash("lodDist"));
				out.hdTextureDist = a.Get<float>(Hash("hdTextureDist"));
				const auto lo = a.Get<std::array<float, 4>>(Hash("bbMin")), hi = a.Get<std::array<float, 4>>(Hash("bbMax"));
				out.min = {lo[0], lo[1], lo[2]};
				out.max = {hi[0], hi[1], hi[2]};
				return true;
			}
		}
		return error = "no archetype " + name, false;
	}

	bool WriteTypes(const ue5export::GameFiles& files, const std::string& name, const std::vector<Archetype>& archetypes, Bytes& out, std::string& error)
	{
		meta::Writer w;
		if (!Template(files, "venice_metadata_005_strm.ytyp", w, error))
			return false;
		uint64_t list = 0;
		const auto fill = [&](size_t i, Fields& f) {
			const Archetype& a = archetypes[i];
			const std::array<float, 3> centre{(a.min[0] + a.max[0]) / 2, (a.min[1] + a.max[1]) / 2, (a.min[2] + a.max[2]) / 2};
			const float radius = std::hypot(a.max[0] - centre[0], a.max[1] - centre[1], a.max[2] - centre[2]);
			const uint32_t hash = loader::convert::Joaat(a.name);
			f.Set<float>("lodDist", a.lodDist);
			f.Set<uint32_t>("flags", a.flags);
			f.Vec("bbMin", a.min);
			f.Vec("bbMax", a.max);
			f.Vec("bsCentre", centre);
			f.Set<float>("bsRadius", radius);
			f.Set<float>("hdTextureDist", a.hdTextureDist);
			f.Set<uint32_t>("name", hash);
			f.Set<uint32_t>("textureDictionary", a.textureDictionary);
			f.Set<uint32_t>("physicsDictionary", a.physicsDictionary);
			f.Set<uint32_t>("assetType", 2); // a drawable
			f.Set<uint32_t>("assetName", hash);
		};
		if (!Blocks(w, "CBaseArchetypeDef", archetypes.size(), fill, list, error))
			return false;
		Fields root(w, "CMapTypes");
		root.Set<uint32_t>("name", loader::convert::Joaat(name));
		root.Array("archetypes", list, static_cast<uint16_t>(archetypes.size()));
		if (!root.ok)
			return error = "a field of CMapTypes is not in the template", false;
		return w.Write(w.AddBlock(meta::Hash("CMapTypes"), std::move(root.data)), out, error);
	}

	bool WriteMap(const ue5export::GameFiles& files, const std::string& name, const std::vector<Entity>& entities, Bytes& out, std::string& error)
	{
		meta::Writer w;
		if (!Template(files, "cs1_10_strm_0.ymap", w, error))
			return false;
		std::array<float, 3> lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()},
		    hi{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()},
		    streamLo = lo, streamHi = hi;
		const uint32_t mapHash = loader::convert::Joaat(name);
		uint64_t list = 0;
		const auto fill = [&](size_t i, Fields& f) {
			const Entity& e = entities[i];
			// (a map stores the inverse of an entity's rotation)
			const float position[4] = {e.position[0], e.position[1], e.position[2], 0};
			const float rotation[4] = {-e.rotation[0], -e.rotation[1], -e.rotation[2], e.rotation[3]};
			f.Set<uint32_t>("archetypeName", e.archetype);
			f.Set<uint32_t>("flags", e.flags);
			f.Set<uint32_t>("guid", mapHash ^ static_cast<uint32_t>(i * 0x9E3779B1u));
			f.Set("position", position, 16);
			f.Set("rotation", rotation, 16);
			f.Set<float>("scaleXY", e.scaleXY);
			f.Set<float>("scaleZ", e.scaleZ);
			f.Set<int32_t>("parentIndex", e.parentIndex);
			f.Set<float>("lodDist", e.lodDist);
			f.Set<float>("childLodDist", 0);
			f.Set<uint32_t>("lodLevel", e.lodLevel);
			f.Set<uint32_t>("ambientOcclusionMultiplier", 255);
			f.Set<uint32_t>("artificialAmbientOcclusion", 255);
			for (int a = 0; a < 3; ++a)
			{
				lo[a] = std::min(lo[a], e.position[a] - e.radius);
				hi[a] = std::max(hi[a], e.position[a] + e.radius);
				streamLo[a] = std::min(streamLo[a], e.position[a] - e.radius - e.lodDist);
				streamHi[a] = std::max(streamHi[a], e.position[a] + e.radius + e.lodDist);
			}
		};
		if (!Blocks(w, "CEntityDef", entities.size(), fill, list, error))
			return false;
		Fields root(w, "CMapData");
		root.Set<uint32_t>("name", mapHash);
		root.Set<uint32_t>("parent", 0);
		root.Set<uint32_t>("flags", 0);
		root.Set<uint32_t>("contentFlags", 0x41); // HD entities, with collision
		root.Vec("streamingExtentsMin", streamLo);
		root.Vec("streamingExtentsMax", streamHi);
		root.Vec("entitiesExtentsMin", lo);
		root.Vec("entitiesExtentsMax", hi);
		root.Array("entities", list, static_cast<uint16_t>(entities.size()));
		if (!root.ok)
			return error = "a field of CMapData is not in the template", false;
		return w.Write(w.AddBlock(meta::Hash("CMapData"), std::move(root.data)), out, error);
	}
}
