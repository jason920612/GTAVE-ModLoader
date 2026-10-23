// ue5export: the game's world out to Unreal Engine 5, for the graphics mod (research/phase0.md §58). Everything it
// writes comes from the player's own game files and stays on their computer.
//
// usage:
//   ue5export keys  --out <dir> [--dump <image>]  the archive keys, once, from the running game (or a dump of it)
//   ue5export index --out <dir> --game <dir>      every file in the game's archives: <out>\index.tsv
//   ue5export extract --out <dir> --game <dir> --file <index path>   one file, plain: <out>\files\<name>
//   ue5export map --out <dir> --game <dir>         the map as story mode sees it: archetypes and entities
//   ue5export area --out <dir> --game <dir> --area x0,y0,x1,y1 --name <n>   an area's models, textures, entities (<out>\export)
//   ue5export pack --out <dir> --game <dir> [--pack <name>] [--area x0,y0,x1,y1]   the renderer's pack files (<out>\<name>)
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "convert/archive.hpp"
#include "drawable.hpp"
#include "exporter.hpp"
#include "gamefiles.hpp"
#include "mapdata.hpp"
#include "meta.hpp"
#include "ng.hpp"
#include "pack.hpp"
#include "pso.hpp"

namespace fs = std::filesystem;
using loader::convert::Archive;

namespace
{
	std::map<std::string, std::string> g_options;

	fs::path KeysFile() { return fs::path(g_options["out"]) / "keys" / "ng.bin"; }

	int Fail(const std::string& message)
	{
		std::fprintf(stderr, "%s\n", message.c_str());
		return 1;
	}

	int Keys()
	{
		std::vector<uint8_t> image;
		std::string error;
		const bool read = g_options.contains("dump") ? ue5export::ng::ReadDumpImage(g_options["dump"], image, error)
		                                             : ue5export::ng::ReadProcessImage(image, error);
		ue5export::ng::Keys keys;
		if (!read || !ue5export::ng::Capture(image, keys, error) || !ue5export::ng::Save(keys, KeysFile(), error))
			return Fail(error);
		std::printf("keys saved: %s (%zu tables)\n", KeysFile().string().c_str(), keys.tables.size());
		return 0;
	}

	struct Indexer
	{
		std::ofstream out;
		std::map<std::string, std::pair<uint64_t, uint64_t>> byType; // extension -> files, bytes
		uint64_t archives = 0;
		std::vector<std::string> errors;

		void Walk(const Archive& archive, uint32_t node, const std::string& path)
		{
			const Archive::Node& n = archive.Nodes()[node];
			for (const uint32_t c : n.children)
			{
				const Archive::Node& child = archive.Nodes()[c];
				const std::string childPath = path + "/" + child.name;
				if (child.directory)
				{
					Walk(archive, c, childPath);
					continue;
				}
				const size_t dot = child.name.rfind('.');
				const std::string ext = dot == std::string::npos ? "" : child.name.substr(dot + 1);
				out << childPath << '\t' << child.size << '\t' << (child.resource ? "r" : "b") << '\n';
				auto& [files, bytes] = byType[ext];
				++files, bytes += child.resource ? child.DiskSize() : child.size;
				if (ext == "rpf")
				{
					Archive nested;
					std::string error;
					if (archive.OpenNested(c, nested, error))
						Open(nested, childPath);
					else
						errors.push_back(childPath + ": " + error);
				}
			}
		}

		void Open(const Archive& archive, const std::string& path)
		{
			++archives;
			if (!archive.Nodes().empty())
				Walk(archive, 0, path);
		}
	};

	// One file out of the archives, by its index path ("x64n.rpf/levels/.../ch1_09.rpf/ch1_09_terrain+hi.ytd").
	int Extract(const loader::convert::Decryptor& decrypt)
	{
		const fs::path game = g_options["game"];
		std::string path = g_options["file"], error;
		const size_t first = path.find(".rpf/");
		if (first == std::string::npos)
			return Fail("not a path inside an archive");
		const fs::path file = game / path.substr(0, first + 4);
		auto in = std::make_shared<std::ifstream>(file, std::ios::binary);
		std::error_code ec;
		Archive archive;
		if (!archive.Open(in, 0, fs::file_size(file, ec), file.filename().string(), &decrypt, error))
			return Fail(error);
		std::string rest = path.substr(first + 5);
		for (uint32_t node = 0;;)
		{
			const size_t slash = rest.find('/');
			const std::string name = rest.substr(0, slash);
			const auto& children = archive.Nodes()[node].children;
			const auto it = std::find_if(children.begin(), children.end(), [&](uint32_t c) { return archive.Nodes()[c].name == name; });
			if (it == children.end())
				return Fail("not found: " + name);
			const Archive::Node& n = archive.Nodes()[*it];
			if (slash == std::string::npos)
			{
				loader::convert::Bytes data;
				if (!archive.ReadFile(*it, data, error))
					return Fail(error);
				const fs::path out = fs::path(g_options["out"]) / "files" / name;
				fs::create_directories(out.parent_path(), ec);
				std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), data.size());
				std::printf("%s: %zu bytes\n", out.string().c_str(), data.size());
				return 0;
			}
			rest = rest.substr(slash + 1);
			if (n.directory)
				node = *it;
			else
			{
				Archive nested;
				if (!archive.OpenNested(*it, nested, error))
					return Fail(error);
				archive = std::move(nested);
				node = 0;
			}
		}
	}

	// The map's metadata as story mode sees it: counts, and the entities of an area (--area x0,y0,x1,y1).
	int Map(const loader::convert::Decryptor& decrypt)
	{
		ue5export::GameFiles files;
		std::string error;
		const auto start = std::chrono::steady_clock::now();
		if (!files.Open(g_options["game"], decrypt, error))
			return Fail(error);
		const auto map = ue5export::mapdata::Read(files);
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		std::printf("%zu active archives, %zu files; %zu ytyp, %zu archetypes; %zu ymap, %zu entities (%.1f s)\n", files.activeArchives,
		    files.Files().size(), map.ytypCount, map.archetypes.size(), map.ymapCount, map.entities.size(), seconds);
		std::map<uint32_t, size_t> byLevel;
		size_t missing = 0, interiors = 0;
		std::map<std::string, size_t> missingByYmap;
		for (const auto& e : map.entities)
		{
			++byLevel[e.lodLevel];
			interiors += e.interior;
			if (!map.archetypes.contains(e.archetype))
				++missing, ++missingByYmap[map.ymaps[e.ymap].file];
		}
		for (const auto& [level, count] : byLevel)
			std::printf("  lod level %u: %zu\n", level, count);
		std::printf("  interiors placed: %zu; entities without a known archetype: %zu\n", interiors, missing);
		size_t interiorEntities = 0, grassBatches = 0, grassInstances = 0, grouped = 0;
		for (const auto& in : map.interiors)
		{
			interiorEntities += in.entities.size();
			for (const auto& s : in.entitySets)
				interiorEntities += s.entities.size();
		}
		for (const auto& y : map.ymaps)
		{
			grassBatches += y.grass.size();
			for (const auto& b : y.grass)
				grassInstances += b.instances.size();
			grouped += y.group >= 0;
		}
		std::printf("  interior types: %zu (%zu entities); grass: %zu batches, %zu instances; groups: %zu (%zu ymaps)\n", map.interiors.size(),
		    interiorEntities, grassBatches, grassInstances, map.groups.size(), grouped);
		for (const auto& [ymap, count] : missingByYmap)
			std::printf("    %s: %zu\n", ymap.c_str(), count);
		for (const auto& w : files.warnings)
			std::printf("warning: %s\n", w.c_str());
		for (const auto& e : map.errors)
			std::printf("error: %s\n", e.c_str());
		return 0;
	}

	// The models, textures and entities of an area: --area x0,y0,x1,y1 --name <name>.
	int ExportArea(const loader::convert::Decryptor& decrypt)
	{
		ue5export::Area area;
		area.name = g_options.contains("name") ? g_options["name"] : "area";
		if (sscanf_s(g_options["area"].c_str(), "%f,%f,%f,%f", &area.bounds[0], &area.bounds[1], &area.bounds[2], &area.bounds[3]) != 4)
			return Fail("--area x0,y0,x1,y1 is needed");
		ue5export::GameFiles files;
		std::string error;
		const auto start = std::chrono::steady_clock::now();
		if (!files.Open(g_options["game"], decrypt, error))
			return Fail(error);
		const auto map = ue5export::mapdata::Read(files);
		std::vector<std::string> warnings;
		const auto stats = ue5export::ExportArea(files, map, area, g_options["out"], warnings);
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		std::printf("%s: %zu entities, %zu models (%zu missing), %zu textures written (%zu missing) in %.1f s\n", area.name.c_str(), stats.entities,
		    stats.models, stats.missingModels, stats.textures, stats.missingTextures, seconds);
		for (const auto& w : warnings)
			std::printf("warning: %s\n", w.c_str());
		return 0;
	}

	// Field and structure names to recognise in metadata files (their hashes; research/phase0.md §59.2).
	const std::map<uint32_t, std::string>& MetaNames()
	{
		static const std::map<uint32_t, std::string> names = [] {
			std::map<uint32_t, std::string> m;
			for (const char* n : {"CPackFileMetaData", "MapDataGroups", "HDTxdBindingArray", "imapDependencies", "imapDependencies_2", "itypDependencies_2",
			         "Interiors", "CMapDataGroup", "Name", "Bounds", "Flags", "WeatherTypes", "HoursOnOff", "CImapDependency", "CImapDependencies",
			         "CItypDependencies", "imapName", "itypName", "packFileName", "manifestFlags", "itypDepArray", "CInteriorBoundsFiles",
			         "CHDTxdAssetBinding", "assetType", "targetAsset", "HDTxd", "CMapData", "name", "parent", "flags", "contentFlags",
			         "streamingExtentsMin", "streamingExtentsMax", "entitiesExtentsMin", "entitiesExtentsMax", "entities", "containerLods",
			         "boxOccluders", "occludeModels", "physicsDictionaries", "instancedData", "timeCycleModifiers", "carGenerators",
			         "LODLightsSOA", "DistantLODLightsSOA", "block", "rage__fwInstancedMapData", "ImapLink", "PropInstanceList",
			         "GrassInstanceList", "rage__fwGrassInstanceListDef", "BatchAABB", "ScaleRange", "archetypeName", "lodDist",
			         "LodFadeStartDist", "LodInstFadeRange", "OrientToTerrain", "InstanceList", "rage__fwGrassInstanceListDef__InstanceData",
			         "Position", "NormalX", "NormalY", "Color", "Scale", "Ao", "Pad", "rage__spdAABB", "min", "max", "CEntityDef", "guid",
			         "position", "rotation", "scaleXY", "scaleZ", "parentIndex", "childLodDist", "lodLevel", "numChildren", "priorityLevel",
			         "extensions", "ambientOcclusionMultiplier", "artificialAmbientOcclusion", "tintValue", "CMloInstanceDef", "groupId",
			         "floorId", "defaultEntitySets", "numExitPortals", "MLOInstflags", "CMloArchetypeDef", "mloFlags", "rooms", "portals",
			         "entitySets", "CMloRoomDef", "bbMin", "bbMax", "blend", "timecycleName", "secondaryTimecycleName", "portalCount",
			         "exteriorVisibiltyDepth", "attachedObjects", "CMloPortalDef", "roomFrom", "roomTo", "mirrorPriority", "opacity",
			         "audioOcclusion", "corners", "CMloEntitySet", "locations", "CMloTimeCycleModifier", "sphere", "percentage", "range",
			         "startHour", "endHour", "CBaseArchetypeDef", "specialAttribute", "bsCentre", "bsRadius", "hdTextureDist",
			         "textureDictionary", "clipDictionary", "drawableDictionary", "physicsDictionary", "assetName", "CTimeArchetypeDef",
			         "timeFlags", "CMapTypes", "archetypes", "dependencies", "compositeEntityTypes", "CLODLight", "direction", "falloff",
			         "falloffExponent", "timeAndStateFlags", "hash", "coneInnerAngle", "coneOuterAngleOrCapExt", "coronaIntensity",
			         "CDistantLODLight", "RGBI", "numStreetLights", "category", "CBlockDesc", "version", "exportedBy", "owner", "time",
			         "CCarGen", "CTimeCycleModifier", "minExtents", "maxExtents", "CExtensionDefLightEffect", "instances", "CLightAttrDef",
			         "posn", "colour", "flashiness", "intensity", "boneTag", "lightType", "falloff", "cullingPlane", "shadowBlur",
			         "extraFlags", "coneOuterAngle", "coronaSize", "coronaZBias", "volumeIntensity", "volumeSizeScale", "volumeOuterColour",
			         "lightHash", "volumeOuterIntensity", "volumeOuterExponent", "lightFadeDistance", "shadowFadeDistance",
			         "specularFadeDistance", "volumetricFadeDistance", "shadowNearClip", "projectedTextureKey", "tangent", "padding1",
			         "padding2", "padding3", "CExtensionDefParticleEffect", "CExtensionDefDoor", "CExtensionDefSpawnPoint", "offsetPosition",
			         "offsetRotation", "CBoxOccluder", "COccludeModel", "CContainerLodDef", "BoxOccluder", "iCenterX", "iCenterY",
			         "iCenterZ", "iCosZ", "iLength", "iWidth", "iHeight", "iSinZ", "verts", "dataSize", "bmin", "bmax", "Entities"})
				m[ue5export::meta::Hash(n)] = n;
			return m;
		}();
		return names;
	}

	std::string MetaName(uint32_t hash)
	{
		const auto it = MetaNames().find(hash);
		return it != MetaNames().end() ? it->second : std::format("<{:08x}>", hash);
	}

	// The structures of a metadata file (--file <name> [--version n]: the nth file of that name, manifests have many).
	int MetaDump(const loader::convert::Decryptor& decrypt)
	{
		ue5export::GameFiles files;
		std::string error;
		if (!files.Open(g_options["game"], decrypt, error))
			return Fail(error);
		const std::string name = g_options["file"];
		std::vector<ue5export::GameFiles::File> versions = files.AllOf(name);
		if (versions.empty())
			if (const auto* f = files.Find(name))
				versions.push_back(*f);
		const size_t version = g_options.contains("version") ? std::stoul(g_options["version"]) : 0;
		if (version >= versions.size())
			return Fail(std::format("{}: {} version(s)", name, versions.size()));
		loader::convert::Bytes data;
		if (!files.Read(versions[version], data, error))
			return Fail(error);
		std::printf("%s (%zu versions)\n", versions[version].path.c_str(), versions.size());
		if (data.size() > 4 && std::memcmp(data.data(), "PSIN", 4) == 0)
		{
			ue5export::pso::Pso p;
			if (!p.Read(data, error))
				return Fail(error);
			const auto root = p.Root();
			std::printf("PSO, root %s\n", root ? MetaName(root.type->name).c_str() : "-");
			for (const uint32_t hash : {ue5export::meta::Hash("CPackFileMetaData"), ue5export::meta::Hash("CMapDataGroup"),
			         ue5export::meta::Hash("CHDTxdAssetBinding"), ue5export::meta::Hash("CImapDependencies")})
				if (const auto* s = p.Find(hash))
				{
					std::printf("struct %s (%u bytes)\n", MetaName(hash).c_str(), s->size);
					for (const auto& f : s->fields)
						std::printf("  +%-4u type %02x/%02x ref %08x %s\n", f.offset, f.type, f.subtype, f.ref, MetaName(f.name).c_str());
				}
			for (const auto& g : root.Array(ue5export::meta::Hash("MapDataGroups")))
			{
				std::printf("group %08x flags %x hours %06x:", g.U32(ue5export::meta::Hash("Name")), g.U32(ue5export::meta::Hash("Flags")),
				    g.U32(ue5export::meta::Hash("HoursOnOff")));
				for (const uint32_t y : g.U32Array(ue5export::meta::Hash("Bounds")))
					std::printf(" %08x", y);
				std::printf("\n");
			}
			for (const auto& b : root.Array(ue5export::meta::Hash("HDTxdBindingArray")))
				std::printf("hd txd: %u %s -> %s\n", b.U32(ue5export::meta::Hash("assetType")), b.String(ue5export::meta::Hash("targetAsset")).c_str(),
				    b.String(ue5export::meta::Hash("HDTxd")).c_str());
			return 0;
		}
		ue5export::meta::Meta m;
		if (!m.Read(data, error))
			return Fail(error);
		for (const auto& [hash, s] : m.Structures())
		{
			std::printf("struct %s (%u bytes)\n", MetaName(hash).c_str(), s.size);
			for (const auto& f : s.fields)
				std::printf("  +%-4d type %02x %s\n", f.offset, f.type, MetaName(f.name).c_str());
		}
		const auto root = m.Root();
		std::printf("root: %s\n", root ? MetaName(root.type->name).c_str() : "-");
		return 0;
	}

	// The textures of a dictionary: --file <name.ytd>.
	int TextureList(const loader::convert::Decryptor& decrypt)
	{
		ue5export::GameFiles files;
		std::string error;
		if (!files.Open(g_options["game"], decrypt, error))
			return Fail(error);
		const auto* f = files.Find(g_options["file"]);
		loader::convert::Bytes data;
		loader::convert::Resource resource;
		if (!f || !files.Read(*f, data, error) || !loader::convert::Read(data, resource, error))
			return Fail(f ? error : "not found");
		const ue5export::drawable::View view(resource);
		for (const auto& t : ue5export::drawable::Textures(view, ue5export::drawable::View::kRoot))
			std::printf("%-40s %4ux%-4u dxgi %3u mips %2u\n", t.name.c_str(), t.width, t.height, t.dxgi, t.mips);
		return 0;
	}

	// The renderer's pack files: --pack <name> (default "pack"), --area x0,y0,x1,y1 for a test area.
	int Pack(const loader::convert::Decryptor& decrypt)
	{
		ue5export::PackOptions options;
		options.dir = fs::path(g_options["out"]) / (g_options.contains("pack") ? g_options["pack"] : "pack");
		options.gameDir = g_options["game"];
		options.decrypt = &decrypt;
		if (g_options.contains("workers"))
			options.workers = std::stoul(g_options["workers"]);
		options.worldOnly = g_options.contains("world-only") && g_options["world-only"] == "1";
		if (g_options.contains("area"))
		{
			ue5export::Area area;
			if (sscanf_s(g_options["area"].c_str(), "%f,%f,%f,%f", &area.bounds[0], &area.bounds[1], &area.bounds[2], &area.bounds[3]) != 4)
				return Fail("--area x0,y0,x1,y1");
			options.area = area;
		}
		ue5export::GameFiles files;
		std::string error;
		const auto start = std::chrono::steady_clock::now();
		if (!files.Open(g_options["game"], decrypt, error))
			return Fail(error);
		const auto map = ue5export::mapdata::Read(files);
		std::vector<std::string> warnings;
		const auto stats = ue5export::WritePack(files, map, options, warnings);
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		std::printf("%s: %zu models (%zu missing, %.1f MB), %zu textures (%.1f MB, %zu references missing), %zu entities (%.1f MB) in %.1f s\n",
		    options.dir.string().c_str(), stats.models, stats.missingModels, stats.modelBytes / 1048576.0, stats.textures, stats.textureBytes / 1048576.0,
		    stats.missingTextures, stats.entities, stats.worldBytes / 1048576.0, seconds);
		for (const auto& w : warnings)
			std::printf("warning: %s\n", w.c_str());
		return 0;
	}

	// Texture formats over every --step'th texture dictionary of the map (DXGI format -> textures).
	int TextureStats(const loader::convert::Decryptor& decrypt)
	{
		ue5export::GameFiles files;
		std::string error;
		if (!files.Open(g_options["game"], decrypt, error))
			return Fail(error);
		const size_t step = g_options.contains("step") ? std::stoul(g_options["step"]) : 20;
		std::map<int, size_t> formats;
		size_t n = 0, dictionaries = 0;
		for (const auto& [name, file] : files.Files())
		{
			if (!name.ends_with(".ytd") || n++ % step)
				continue;
			loader::convert::Bytes data;
			loader::convert::Resource resource;
			if (!files.Read(file, data, error) || !loader::convert::Read(data, resource, error))
				continue;
			++dictionaries;
			const ue5export::drawable::View view(resource);
			for (const auto& t : ue5export::drawable::Textures(view, ue5export::drawable::View::kRoot))
				++formats[t.dxgi];
		}
		std::printf("%zu dictionaries sampled\n", dictionaries);
		for (const auto& [format, count] : formats)
			std::printf("  DXGI %d: %zu\n", format, count);
		return 0;
	}

	int Index(const loader::convert::Decryptor& decrypt)
	{
		const fs::path game = g_options["game"];
		std::string error;
		Indexer indexer;
		indexer.out.open(fs::path(g_options["out"]) / "index.tsv", std::ios::binary);
		const auto start = std::chrono::steady_clock::now();
		std::error_code ec;
		for (auto it = fs::recursive_directory_iterator(game, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
		{
			if (it->is_directory() && it->path().filename() == "ModLoader")
			{
				it.disable_recursion_pending();
				continue;
			}
			if (it->path().extension() != ".rpf")
				continue;
			const auto in = std::make_shared<std::ifstream>(it->path(), std::ios::binary);
			Archive archive;
			const std::string rel = fs::relative(it->path(), game).generic_string();
			if (archive.Open(in, 0, it->file_size(), it->path().filename().string(), &decrypt, error))
				indexer.Open(archive, rel);
			else
				indexer.errors.push_back(rel + ": " + error);
		}
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		std::printf("%llu archives in %.1f s\n", static_cast<unsigned long long>(indexer.archives), seconds);
		for (const auto& [ext, count] : indexer.byType)
			std::printf("  %-8s %8llu files %10.1f MB\n", ext.c_str(), static_cast<unsigned long long>(count.first), count.second / 1048576.0);
		for (const auto& e : indexer.errors)
			std::printf("error: %s\n", e.c_str());
		return 0;
	}
}

int main(int argc, char** argv)
{
	if (argc < 2)
		return Fail("usage: ue5export keys|index --out <dir> [--game <dir>] [--dump <image>]");
	for (int i = 2; i + 1 < argc; i += 2)
		if (std::string(argv[i]).starts_with("--"))
			g_options[argv[i] + 2] = argv[i + 1];
	if (!g_options.contains("out"))
		return Fail("--out <dir> is needed");
	const std::string command = argv[1];
	if (command == "keys")
		return Keys();
	if (!g_options.contains("game"))
		return Fail("--game <dir> is needed");
	ue5export::ng::Keys keys;
	std::string error;
	if (!ue5export::ng::Load(KeysFile(), keys, error))
		return Fail(error);
	const loader::convert::Decryptor decrypt = [&keys](uint32_t key, uint8_t* data, uint32_t size) { ue5export::ng::Decrypt(keys, key, data, size); };
	SetDllDirectoryW(fs::path(g_options["game"]).c_str()); // (the game's zlib1.dll)
	if (command == "index")
		return Index(decrypt);
	if (command == "map")
		return Map(decrypt);
	if (command == "area")
		return ExportArea(decrypt);
	if (command == "pack")
		return Pack(decrypt);
	if (command == "ytdlist")
		return g_options.contains("file") ? TextureList(decrypt) : Fail("--file <name.ytd> is needed");
	if (command == "metadump")
		return g_options.contains("file") ? MetaDump(decrypt) : Fail("--file <name> is needed");
	if (command == "texstats")
		return TextureStats(decrypt);
	if (command == "extract")
		return g_options.contains("file") ? Extract(decrypt) : Fail("--file <path> is needed");
	return Fail("unknown command " + command);
}
