#include "landfiles.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <tuple>
#include <vector>

#include "../convert/archive.hpp"
#include "../convert/resource.hpp"
#include "../log.hpp"
#include "../paths.hpp"
#include "../pattern.hpp"
#include "dlcpacks.hpp"
#include "minimap.hpp"

namespace loader::game::landfiles
{
	namespace
	{
		// The streaming objects (GTA5_Enhanced.exe +0x42F9E48): +0 their entries (8 bytes: the handle - archive << 16 | its
		// file -, then the flags: the low two bits 0 not there, 1 loaded, 2 asked for, 3 loading; 0xFFFC what holds it),
		// +0x1A0 the modules by range (16 bytes: the module, the end of its range), +0x1A8 how many. A store is its module:
		// +8 its first object, its slot pool at +0x38 (+0x48 a byte a slot, the high bit set: free), its names at +0x78
		// (nodes of 12 bytes: the hash, the slot, the next), +0x80 the buckets, +0x88 how many; vtable +0x50 RemoveSlot,
		// +0xC8 how many hold the object. RemoveObject (+0xCE4CD0: the manager, the object, 0) unloads one nothing holds.
		constexpr uintptr_t kStreaming = 0x42F9E48, kRemoveObject = 0xCE4CD0, kEmptyBox = 0x248F460;
		constexpr uint8_t kRemoveObjectStart[] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57, 0x55, 0x53, 0x48, 0x81, 0xEC, 0x58, 0x0C, 0x00, 0x00};
		// The map store's box streamer: two boxes a slot (32 bytes each, +0xE8 and +0x1D0) and a word of flags (+0xF8); its
		// AddSlot sets them, its RemoveSlot (the stores' generic one) leaves them - reset here as the collision store's own
		// RemoveSlot does its (the empty box at +0x248F460, the flags 0).
		constexpr size_t kMapBoxes = 0xE8, kMapBoxes2 = 0x1D0, kMapFlags = 0xF8;
		// The collision store's box streamer: a box a slot (+0xD8, 32 bytes) and a word of flags (+0xE8: bits 6 on the kinds of
		// search that want it, 4 off - its group reverted), set when a pack's
		// files are first registered (from its cache) and emptied by its RemoveSlot (0x93AD80) - but not set again when the
		// land's change set registers them anew: with empty boxes none of a land gone back to was ever asked for, and there
		// was no ground under it (cars put down on its roads fell through the world). Kept as they were, put back then.
		constexpr size_t kBoundsBoxes = 0xD8, kBoundsFlags = 0xE8;
		// Its searches go through a tree of the boxes (the box streamer at +0xB0: +0x108 the tree, searched by 0x1EBFB20 -
		// the slot << 2 each), made from the box array by the box streamer's vtable +0x10 (0x92C420: the old tree freed, a new
		// one built, 0x1EC1070) when a pack's cache has set the boxes. The boxes put back, the tree is made again.
		constexpr size_t kBoxStreamer = 0xB0;
		constexpr uintptr_t kRebuildTree = 0x92C420;

		// The kinds, in the order they are given back: maps and archetype files hold the models by slot (an archetype its
		// drawable, dictionary, fragment), the models their texture dictionaries; the collision files nothing.
		enum Kind
		{
			kMap,
			kTypes,
			kDrawable,
			kDwd,
			kFragment,
			kTxd,
			kBounds,
			kKinds
		};
		struct KindInfo
		{
			const char* extension;
			uintptr_t vtable;
		};
		constexpr KindInfo kKindInfo[kKinds] = {{"ymap", 0x2612200}, {"ytyp", 0x2612380}, {"ydr", 0x2611D80}, {"ydd", 0x2611F00}, {"yft", 0x2612080},
		    {"ytd", 0x2612BC0}, {"ybn", 0x2612820}};

		struct Names
		{
			bool read = false;
			std::array<std::vector<uint32_t>, kKinds> hashes;
			std::vector<uint32_t> pictures; // (the pause map's: among the texture dictionaries too)
			std::map<uint32_t, std::array<uint8_t, 34>> bounds; // (the collision files' boxes and flags when given back)
			// (the maps' and collision files' slots, looked up once while they are registered: SwitchArea)
			std::vector<int32_t> mapSlots, boundsSlots;
			bool slotsKnown = false;
		};
		std::map<std::string, Names> g_names;

		// (a land's names read on a thread of their own, ahead of the frames that want them: the pack's archives' tables read
		// from the disk - the external one too - took the game thread a frame of its own)
		struct Prefetching
		{
			std::thread worker;
			std::atomic<bool> done = false;
			Names names;
		};
		std::map<std::string, std::unique_ptr<Prefetching>> g_prefetch;

		// A land's files given back over frames (ReleaseSome): unloaded by the streaming engine, pass after pass until none
		// is loaded, then their slots given back - where it is, kind and file.
		struct Releasing
		{
			bool givingBack = false;
			int kind = 0;
			size_t at = 0;
			std::array<size_t, kKinds> loaded{}; // (this pass)
			std::array<bool, kKinds> give{};
			Progress progress;
		};
		std::map<std::string, Releasing> g_releasing;

		void Collect(const convert::Archive& archive, Names& out, bool top)
		{
			const auto& nodes = archive.Nodes();
			for (uint32_t i = 0; i < nodes.size(); ++i)
			{
				const auto& n = nodes[i];
				if (n.directory)
					continue;
				if (!n.resource && !n.stored && n.name.ends_with(".rpf"))
				{
					// (the map archives only: the overlay's files - "<land>_over*.rpf" - are the game's road regions and navmesh
					// squares)
					if (!top || n.name.find("_over") != std::string::npos)
						continue;
					convert::Archive inner;
					std::string error;
					if (archive.OpenNested(i, inner, error))
						Collect(inner, out, false);
					continue;
				}
				if (top)
					continue;
				const size_t dot = n.name.rfind('.');
				if (dot == std::string::npos)
					continue;
				const std::string extension = n.name.substr(dot + 1);
				// (the pause map's pictures, minimap_<layer>_<land>_<row>_<column>.ytd, let go before the rest:
				// LetPicturesGo)
				if (extension == "ytd" && n.name.starts_with("minimap_"))
					out.pictures.push_back(convert::Joaat(std::string_view(n.name).substr(0, dot)));
				for (int k = 0; k < kKinds; ++k)
					if (extension == kKindInfo[k].extension)
						out.hashes[k].push_back(convert::Joaat(std::string_view(n.name).substr(0, dot)));
			}
		}

		void Read(const std::string& land, const std::filesystem::path& pack, Names& names)
		{
			names.read = true;
			std::error_code ec;
			auto in = std::make_shared<std::ifstream>(pack, std::ios::binary);
			convert::Archive archive;
			std::string error;
			if (!*in || !archive.Open(in, 0, std::filesystem::file_size(pack, ec), "dlc.rpf", dlcpacks::Decryptor(), error))
			{
				log::Warn("land files: {}'s pack not read ({}); its files stay registered", land, error);
				return;
			}
			Collect(archive, names, true);
			size_t total = 0;
			for (const auto& h : names.hashes)
				total += h.size();
			log::Info("land files: {}: {} map files ({} maps, {} archetype files, {} drawables, {} dictionaries of drawables, {} texture "
			          "dictionaries, {} collision files)",
			    land, total, names.hashes[kMap].size(), names.hashes[kTypes].size(), names.hashes[kDrawable].size(), names.hashes[kDwd].size(),
			    names.hashes[kTxd].size(), names.hashes[kBounds].size());
		}

		// a land's names (game thread): read on its thread ahead (Prefetch) - waited for if still being read -, else now
		Names& NamesOf(const std::string& land, const std::filesystem::path& pack)
		{
			Names& names = g_names[land];
			if (names.read)
				return names;
			if (const auto it = g_prefetch.find(land); it != g_prefetch.end())
			{
				it->second->worker.join();
				names = std::move(it->second->names);
				g_prefetch.erase(it);
				return names;
			}
			Read(land, pack, names);
			return names;
		}

		uint8_t* ModuleOf(uintptr_t base, uintptr_t vtable)
		{
			const auto* streaming = reinterpret_cast<const uint8_t*>(base + kStreaming);
			const auto* ranges = *reinterpret_cast<const uint8_t* const*>(streaming + 0x1A0);
			const uint16_t modules = *reinterpret_cast<const uint16_t*>(streaming + 0x1A8);
			for (uint16_t m = 0; ranges && m < modules; ++m)
			{
				auto* module = *reinterpret_cast<uint8_t* const*>(ranges + m * 16);
				if (module && *reinterpret_cast<const uintptr_t*>(module) == base + vtable)
					return module;
			}
			return nullptr;
		}

		// the store's slot of a name (-1: none)
		int32_t Slot(const uint8_t* module, uint32_t hash)
		{
			const auto* nodes = *reinterpret_cast<const uint8_t* const*>(module + 0x78);
			const auto* buckets = *reinterpret_cast<const uint32_t* const*>(module + 0x80);
			const uint32_t count = *reinterpret_cast<const uint32_t*>(module + 0x88);
			if (!nodes || !buckets || !count)
				return -1;
			for (uint32_t i = buckets[hash % count], guard = 0; i != 0xFFFFFFFF && guard < 1000000; ++guard)
			{
				const auto* n = nodes + static_cast<size_t>(i) * 12;
				if (*reinterpret_cast<const uint32_t*>(n) == hash)
					return *reinterpret_cast<const int32_t*>(n + 4);
				i = *reinterpret_cast<const uint32_t*>(n + 8);
			}
			return -1;
		}

		bool Used(const uint8_t* module, int32_t slot)
		{
			const auto* used = *reinterpret_cast<const uint8_t* const*>(module + 0x48);
			const int32_t size = *reinterpret_cast<const int32_t*>(module + 0x50);
			return used && slot >= 0 && slot < size && !(used[slot] & 0x80);
		}

		using RemoveObjectFn = uint8_t (*)(void* streaming, uint32_t index, uint32_t flags);
		using RemoveSlotFn = void (*)(uint8_t* module, int32_t slot);
		using RefsFn = int32_t (*)(uint8_t* module, int32_t slot);
		using GetPtrFn = void* (*)(uint8_t* module, int32_t slot);
		// a store's holds by kind (vtable +0xB0 take one, +0xB8 let one go: the slot, the kind)
		using RefFn = void (*)(uint8_t* module, int32_t slot, int32_t kind);
		// 0xCEBBA0 (the streaming objects, the object, the request flags): those of its requests let go (a map's box streamer
		// asks with 1); one asked for and not loaded with none left is taken off the queue
		constexpr uintptr_t kLetRequestGo = 0xCEBBA0;
		using LetRequestGoFn = void (*)(void* streaming, uint32_t index, uint32_t flags);
		// the archives (+0x2C075B0: one a handle's archive number; +0x88 its path, "dlc_<pack>:/...")
		constexpr uintptr_t kArchives = 0x2C075B0;

		// whether a handle is another's file than `land`'s (another land's or the game's: a name they share - the radar's
		// supertiles -, which that land has now); 0, none, is nobody's
		bool Theirs(uint32_t handle, const std::string& land)
		{
			if (!handle)
				return false;
			if ((handle & 0xC0000000) == 0x80000000)
				return true;
			const auto base = pattern::Module::Main().base;
			const auto* archive = *reinterpret_cast<const uint8_t* const*>(base + kArchives + static_cast<uintptr_t>(handle >> 16) * 8);
			const char* path = archive ? *reinterpret_cast<const char* const*>(archive + 0x88) : nullptr;
			return !path || std::strncmp(path, "dlc_", 4) != 0 || std::strncmp(path + 4, land.c_str(), land.size()) != 0 || path[4 + land.size()] != ':';
		}

		// A loaded map unloaded as the game's switch of a map group off does it (0x931770): its box streamer's request let go,
		// its own hold of kind 2 let go, unloaded by the streaming engine - the hold taken again if it could not be. Whether it
		// is no longer loaded.
		bool UnloadMap(uint8_t* module, int32_t slot, uint32_t index)
		{
			const auto base = pattern::Module::Main().base;
			auto* streaming = reinterpret_cast<uint8_t*>(base + kStreaming);
			const uint32_t* entries = *reinterpret_cast<const uint32_t* const*>(streaming);
			const auto vtable = *reinterpret_cast<uintptr_t*>(module);
			reinterpret_cast<LetRequestGoFn>(base + kLetRequestGo)(streaming, index, 1);
			if ((entries[static_cast<size_t>(index) * 2 + 1] & 3) == 1)
			{
				const bool held = (*reinterpret_cast<RefsFn*>(vtable + 0xC8))(module, slot) > 0;
				if (held)
					(*reinterpret_cast<RefFn*>(vtable + 0xB8))(module, slot, 2);
				if (!reinterpret_cast<RemoveObjectFn>(base + kRemoveObject)(streaming, index, 0) && held)
					(*reinterpret_cast<RefFn*>(vtable + 0xB0))(module, slot, 2);
			}
			return (entries[static_cast<size_t>(index) * 2 + 1] & 3) == 0;
		}
		std::set<uint32_t> g_noObject; // (said once each)
		std::set<uint64_t> g_audited;  // (index << 32 | name hash: said once each)

		// Every land's files by name hash, the pause map's pictures too ("<land>/<file>"; a name more than one land has, all
		// of them): for telling what a slot is. Read once, when first asked.
		std::map<uint32_t, std::string> g_allNames;
		bool g_allRead = false;

		void CollectAll(const convert::Archive& archive, const std::string& land, bool top)
		{
			const auto& nodes = archive.Nodes();
			for (uint32_t i = 0; i < nodes.size(); ++i)
			{
				const auto& n = nodes[i];
				if (n.directory)
					continue;
				if (!n.resource && !n.stored && n.name.ends_with(".rpf"))
				{
					convert::Archive inner;
					std::string error;
					if (archive.OpenNested(i, inner, error))
						CollectAll(inner, land, false);
					continue;
				}
				const size_t dot = n.name.rfind('.');
				if (top || dot == std::string::npos)
					continue;
				std::string& name = g_allNames[convert::Joaat(std::string_view(n.name).substr(0, dot))];
				name += (name.empty() ? "" : " | ") + land + "/" + n.name;
			}
		}

		void ReadAll()
		{
			g_allRead = true;
			std::error_code ec;
			for (const auto& dir : paths::LandFolders())
			{
				const auto pack = dir / L"dlc.rpf";
				if (!std::filesystem::exists(pack, ec))
					continue;
				auto in = std::make_shared<std::ifstream>(pack, std::ios::binary);
				convert::Archive archive;
				std::string error;
				if (*in && archive.Open(in, 0, std::filesystem::file_size(pack, ec), "dlc.rpf", dlcpacks::Decryptor(), error))
					CollectAll(archive, dir.filename().string(), true);
			}
		}

		// a slot's name hash (its node in the store's names), or 0
		uint32_t HashOf(const uint8_t* module, int32_t slot)
		{
			const auto* nodes = *reinterpret_cast<const uint8_t* const*>(module + 0x78);
			const auto* buckets = *reinterpret_cast<const uint32_t* const*>(module + 0x80);
			const uint32_t count = *reinterpret_cast<const uint32_t*>(module + 0x88);
			if (!nodes || !buckets)
				return 0;
			for (uint32_t b = 0; b < count; ++b)
				for (uint32_t i = buckets[b], guard = 0; i != 0xFFFFFFFF && guard < 1000000; ++guard)
				{
					const auto* n = nodes + static_cast<size_t>(i) * 12;
					if (*reinterpret_cast<const int32_t*>(n + 4) == slot)
						return *reinterpret_cast<const uint32_t*>(n);
					i = *reinterpret_cast<const uint32_t*>(n + 8);
				}
			return 0;
		}
	}

	bool LetPicturesGo(const std::string& land, const std::filesystem::path& pack, std::string* held)
	{
		if (!Supported())
			return true;
		Names& names = NamesOf(land, pack);
		const auto base = pattern::Module::Main().base;
		auto* streaming = reinterpret_cast<uint8_t*>(base + kStreaming);
		const auto* entries = *reinterpret_cast<const uint32_t* const*>(streaming);
		uint8_t* module = ModuleOf(base, kKindInfo[kTxd].vtable);
		const uint32_t first = *reinterpret_cast<const uint32_t*>(module + 8);
		const auto refs = *reinterpret_cast<RefsFn*>(*reinterpret_cast<uintptr_t*>(module) + 0xC8);
		const auto getPtr = *reinterpret_cast<GetPtrFn*>(*reinterpret_cast<uintptr_t*>(module) + 0x78);
		std::vector<int32_t> slots;
		for (const uint32_t hash : names.pictures)
			if (const int32_t slot = Slot(module, hash); Used(module, slot))
				slots.push_back(slot);
		// the squares holding them let go first (the render thread's last use of each with them), then the dictionaries
		// nothing holds unloaded by the streaming engine
		const bool squares = minimap::LetGo(slots);
		bool done = squares;
		for (const int32_t slot : slots)
		{
			const uint32_t index = first + static_cast<uint32_t>(slot);
			const uint32_t flags = entries[static_cast<size_t>(index) * 2 + 1];
			if (!(flags & 3))
				continue;
			if (squares && (flags & 3) == 1 && !(flags & 0xFFFC) && refs(module, slot) == 0)
			{
				if ((flags & 0x8000000) && !getPtr(module, slot))
				{
					if (g_noObject.insert(index).second)
						log::Warn("land files: {}: a pause map picture loaded with no object: {}", land, Describe(module, slot));
					continue;
				}
				reinterpret_cast<RemoveObjectFn>(base + kRemoveObject)(streaming, index, 0);
			}
			if (entries[static_cast<size_t>(index) * 2 + 1] & 3)
			{
				done = false;
				if (held)
					*held += std::format("{}slot {} (flags {:08x}, held by {}{})", held->empty() ? "" : "; ", slot, entries[static_cast<size_t>(index) * 2 + 1],
					    refs(module, slot), squares ? "" : ", a pause map square has it");
			}
		}
		return done;
	}

	size_t PutBoxesBack(const std::string& land)
	{
		const auto it = g_names.find(land);
		if (it == g_names.end() || it->second.bounds.empty() || !Supported())
			return 0;
		const auto base = pattern::Module::Main().base;
		const auto* emptyBox = reinterpret_cast<const uint8_t*>(base + kEmptyBox);
		const uint8_t* module = ModuleOf(base, kKindInfo[kBounds].vtable);
		auto* boxes = *reinterpret_cast<uint8_t* const*>(module + kBoundsBoxes);
		auto* flags = *reinterpret_cast<uint16_t* const*>(module + kBoundsFlags);
		if (!boxes || !flags)
			return 0;
		size_t back = 0;
		for (const auto& [hash, saved] : it->second.bounds)
		{
			const int32_t slot = Slot(module, hash);
			if (!Used(module, slot) || std::memcmp(boxes + static_cast<size_t>(slot) * 32, emptyBox, 32) != 0)
				continue;
			std::memcpy(boxes + static_cast<size_t>(slot) * 32, saved.data(), 32);
			// (on: 4 is the map group's revert switching them off - set when they were kept; the searches skip those)
			uint16_t word = 0;
			std::memcpy(&word, saved.data() + 32, 2);
			flags[slot] = static_cast<uint16_t>(word & ~4);
			++back;
		}
		if (back)
		{
			auto* streamer = const_cast<uint8_t*>(module) + kBoxStreamer;
			const auto rebuild = *reinterpret_cast<void (**)(uint8_t*)>(*reinterpret_cast<uintptr_t*>(streamer) + 0x10);
			if (reinterpret_cast<uintptr_t>(rebuild) == base + kRebuildTree)
				rebuild(streamer);
			else
				log::Warn("land files: the collision store's box tree is not made where this build has it; {}'s collision stays away", land);
			log::Info("land files: {}: {} collision files' boxes put back, their tree made again", land, back);
		}
		return back;
	}

	size_t SwitchArea(const std::string& land, const std::filesystem::path& pack, const float* box, bool on)
	{
		if (!Supported())
			return 0;
		Names& names = NamesOf(land, pack);
		const auto base = pattern::Module::Main().base;
		if (!names.slotsKnown)
		{
			for (const auto& [kind, slots] : {std::pair<Kind, std::vector<int32_t>*>{kMap, &names.mapSlots}, {kBounds, &names.boundsSlots}})
			{
				const uint8_t* module = ModuleOf(base, kKindInfo[kind].vtable);
				slots->clear();
				for (const uint32_t hash : names.hashes[kind])
					if (const int32_t slot = Slot(module, hash); Used(module, slot))
						slots->push_back(slot);
			}
			names.slotsKnown = true;
		}
		size_t switched = 0;
		// (the map store's box streamer at +0xC0: boxes +0xE8, flags +0xF8; the collision store's at +0xB0: +0xD8, +0xE8)
		for (const auto& [kind, boxesAt, flagsAt] : {std::tuple{kMap, kMapBoxes, kMapFlags}, std::tuple{kBounds, kBoundsBoxes, kBoundsFlags}})
		{
			uint8_t* module = ModuleOf(base, kKindInfo[kind].vtable);
			const auto* boxes = *reinterpret_cast<const float* const*>(module + boxesAt);
			auto* flags = *reinterpret_cast<uint16_t* const*>(module + flagsAt);
			if (!boxes || !flags)
				continue;
			for (const int32_t slot : kind == kMap ? names.mapSlots : names.boundsSlots)
			{
				if (!Used(module, slot))
					continue;
				const float* b = boxes + static_cast<size_t>(slot) * 8;
				if (b[0] > box[2] || b[4] < box[0] || b[1] > box[3] || b[5] < box[1])
					continue;
				const uint16_t was = flags[slot];
				flags[slot] = static_cast<uint16_t>(on ? was & ~4 : was | 4);
				switched += flags[slot] != was;
			}
		}
		return switched;
	}

	std::string Boxes(const std::string& land, const std::filesystem::path& pack)
	{
		if (!Supported())
			return "not supported";
		Names& names = NamesOf(land, pack);
		const auto base = pattern::Module::Main().base;
		const auto* emptyBox = reinterpret_cast<const uint8_t*>(base + kEmptyBox);
		const auto* entries = *reinterpret_cast<const uint32_t* const*>(base + kStreaming);
		std::string out;
		// the collision store's boxes (+0xD8, 32 bytes a slot) and the map store's two (+0xE8, +0x1D0) and flags (+0xF8)
		for (const auto& [kind, at] : {std::pair{kBounds, size_t{0xD8}}, std::pair{kMap, kMapBoxes}, std::pair{kMap, kMapBoxes2}})
		{
			uint8_t* module = ModuleOf(base, kKindInfo[kind].vtable);
			const uint32_t first = *reinterpret_cast<const uint32_t*>(module + 8);
			const auto* boxes = *reinterpret_cast<const uint8_t* const*>(module + at);
			size_t used = 0, empty = 0, loaded = 0, zero = 0;
			for (const uint32_t hash : names.hashes[kind])
			{
				const int32_t slot = Slot(module, hash);
				if (!Used(module, slot))
					continue;
				++used;
				const uint8_t* box = boxes + static_cast<size_t>(slot) * 32;
				if (std::memcmp(box, emptyBox, 32) == 0)
					++empty;
				static constexpr uint8_t kZero[32] = {};
				if (std::memcmp(box, kZero, 32) == 0)
					++zero;
				if (entries[static_cast<size_t>(first + slot) * 2 + 1] & 3)
					++loaded;
			}
			out += std::format("{}{} +{:#x}: {} registered, {} empty boxes, {} zero boxes, {} loaded", out.empty() ? "" : "; ", kKindInfo[kind].extension, at,
			    used, empty, zero, loaded);
		}
		// (the first three collision files as they are: the slot's pool entry, box and flags, the streaming entry)
		const uint8_t* module = ModuleOf(base, kKindInfo[kBounds].vtable);
		const auto* pool = *reinterpret_cast<const uint8_t* const*>(module + 0x40);
		const int32_t stride = *reinterpret_cast<const int32_t*>(module + 0x54);
		const auto* boxes = *reinterpret_cast<const uint8_t* const*>(module + kBoundsBoxes);
		const auto* flags = *reinterpret_cast<const uint16_t* const*>(module + kBoundsFlags);
		const uint32_t first = *reinterpret_cast<const uint32_t*>(module + 8);
		for (size_t i = 0; i < std::min<size_t>(3, names.hashes[kBounds].size()); ++i)
		{
			const uint32_t hash = names.hashes[kBounds][i];
			const int32_t slot = Slot(module, hash);
			if (!Used(module, slot))
				continue;
			std::string bytes;
			for (int32_t b = 0; b < stride; ++b)
				bytes += std::format("{:02x}", pool[static_cast<size_t>(slot) * stride + b]);
			const auto* box = reinterpret_cast<const float*>(boxes + static_cast<size_t>(slot) * 32);
			out += std::format(" | {:08x} slot {} entry {} box ({:.0f} {:.0f} {:.0f} {:g}) ({:.0f} {:.0f} {:.0f} {:g}) flags {:04x} streaming {:08x} {:08x}", hash, slot,
			    bytes, box[0], box[1], box[2], box[3], box[4], box[5], box[6], box[7], flags[slot], entries[static_cast<size_t>(first + slot) * 2],
			    entries[static_cast<size_t>(first + slot) * 2 + 1]);
		}
		return out;
	}

	std::string Describe(const void* module, int32_t slot)
	{
		const auto base = pattern::Module::Main().base;
		auto* m = static_cast<uint8_t*>(const_cast<void*>(module));
		const char* kind = "?";
		for (const KindInfo& k : kKindInfo)
			if (*reinterpret_cast<const uintptr_t*>(m) == base + k.vtable)
				kind = k.extension;
		const uint32_t hash = HashOf(m, slot);
		if (!g_allRead)
			ReadAll();
		const auto name = g_allNames.find(hash);
		const uint32_t index = *reinterpret_cast<const uint32_t*>(m + 8) + static_cast<uint32_t>(slot);
		const auto* entries = *reinterpret_cast<const uint32_t* const*>(base + kStreaming);
		const auto refs = *reinterpret_cast<RefsFn*>(*reinterpret_cast<uintptr_t*>(m) + 0xC8);
		return std::format("{} slot {} (object {}, hash {:08x}: {}), handle {:08x}, flags {:08x}, held by {}", kind, slot, index, hash,
		    name != g_allNames.end() ? name->second : std::string("not a land's file"), entries[static_cast<size_t>(index) * 2],
		    entries[static_cast<size_t>(index) * 2 + 1], Used(m, slot) ? refs(m, slot) : -1);
	}

	void Audit(const std::string& when)
	{
		if (!Supported())
			return;
		const auto base = pattern::Module::Main().base;
		const auto* entries = *reinterpret_cast<const uint32_t* const*>(base + kStreaming);
		for (const KindInfo& k : kKindInfo)
		{
			uint8_t* module = ModuleOf(base, k.vtable);
			if (!module)
				continue;
			const uint32_t first = *reinterpret_cast<const uint32_t*>(module + 8);
			const int32_t size = *reinterpret_cast<const int32_t*>(module + 0x50);
			const auto getPtr = *reinterpret_cast<GetPtrFn*>(*reinterpret_cast<uintptr_t*>(module) + 0x78);
			for (int32_t slot = 0; slot < size; ++slot)
			{
				if (!Used(module, slot))
					continue;
				const uint32_t index = first + static_cast<uint32_t>(slot);
				const uint32_t flags = entries[static_cast<size_t>(index) * 2 + 1];
				if ((flags & 3) != 1 || !(flags & 0x8000000) || getPtr(module, slot))
					continue;
				const uint64_t key = uint64_t{index} << 32 | HashOf(module, slot);
				if (g_audited.insert(key).second)
					log::Warn("land files: {}: loaded with no object: {}", when, Describe(module, slot));
			}
		}
	}

	bool Supported()
	{
		static const bool supported = [] {
			const auto base = pattern::Module::Main().base;
			if (std::memcmp(reinterpret_cast<const void*>(base + kRemoveObject), kRemoveObjectStart, sizeof(kRemoveObjectStart)) != 0)
			{
				log::Warn("land files: the streaming engine's RemoveObject is not where it was; a land's files stay registered when it goes");
				return false;
			}
			for (const KindInfo& k : kKindInfo)
				if (!ModuleOf(base, k.vtable))
				{
					log::Warn("land files: no store of {} files found; a land's files stay registered when it goes", k.extension);
					return false;
				}
			return true;
		}();
		return supported;
	}

	Progress Release(const std::string& land, const std::filesystem::path& pack, bool last)
	{
		return ReleaseSome(land, pack, -1, last);
	}

	Progress ReleaseSome(const std::string& land, const std::filesystem::path& pack, double budget, bool last)
	{
		if (!Supported())
		{
			Progress p;
			p.done = true;
			return p;
		}
		using Clock = std::chrono::steady_clock;
		const auto start = Clock::now();
		const auto spent = [&] { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); };
		Names& names = NamesOf(land, pack);
		Releasing& r = g_releasing[land];
		r.progress.released = 0;
		const auto base = pattern::Module::Main().base;
		auto* streaming = reinterpret_cast<uint8_t*>(base + kStreaming);
		auto* entries = *reinterpret_cast<uint32_t**>(streaming);
		const auto removeObject = reinterpret_cast<RemoveObjectFn>(base + kRemoveObject);
		std::array<uint8_t*, kKinds> modules{};
		for (int k = 0; k < kKinds; ++k)
			modules[k] = ModuleOf(base, kKindInfo[k].vtable);
		// (the budget looked at every 64 files: none, no end)
		size_t steps = 0, removals = 0;
		constexpr size_t kRemovalsPerCall = 32;
		const auto over = [&] { return budget >= 0 && (++steps & 63) == 0 && spent() > budget; };

		// (a kind's objects unloaded only once those of the kinds that hold them are: a map still in the world has its
		// archetypes, they their models, the models their texture dictionaries - this pass's count so far)
		const auto holdersLoaded = [&r](int kind) {
			const auto& n = r.loaded;
			switch (kind)
			{
			case kTypes:
				return n[kMap] > 0;
			case kDrawable:
			case kDwd:
			case kFragment:
				return n[kMap] + n[kTypes] > 0;
			case kTxd:
				return n[kMap] + n[kTypes] + n[kDrawable] + n[kDwd] + n[kFragment] > 0;
			default:
				return false;
			}
		};

		// unloaded: what nothing holds, by the streaming engine - a pass through all the files, as many frames as it takes, and
		// again until none is loaded
		while (!r.givingBack)
		{
			for (; r.kind < kKinds; ++r.kind, r.at = 0)
			{
				uint8_t* module = modules[r.kind];
				if (!module)
					continue;
				const uint32_t first = *reinterpret_cast<const uint32_t*>(module + 8);
				const auto refs = *reinterpret_cast<RefsFn*>(*reinterpret_cast<uintptr_t*>(module) + 0xC8);
				const auto getPtr = *reinterpret_cast<GetPtrFn*>(*reinterpret_cast<uintptr_t*>(module) + 0x78);
				const auto& hashes = names.hashes[r.kind];
				const bool held = holdersLoaded(r.kind);
				for (; r.at < hashes.size(); ++r.at)
				{
					if (over())
					{
						r.progress.loaded = 0;
						for (const size_t n : r.loaded)
							r.progress.loaded += n;
						return r.progress;
					}
					const uint32_t hash = hashes[r.at];
					const int32_t slot = Slot(module, hash);
					if (!Used(module, slot))
						continue;
					const uint32_t index = first + static_cast<uint32_t>(slot);
					uint32_t& flags = entries[static_cast<size_t>(index) * 2 + 1];
					if (!(flags & 3) || Theirs(entries[static_cast<size_t>(index) * 2], land))
						continue;
					// (a resource the streaming engine has as loaded but its store has no object for: RemoveObject breaks into an
					// int3 on one, +0xCE4FB2 - kept, and said)
					if ((flags & 3) == 1 && (flags & 0x8000000) && !getPtr(module, slot))
					{
						if (!g_noObject.contains(index))
						{
							g_noObject.insert(index);
							log::Warn("land files: {}: a {} (slot {}, hash {:08x}) loaded with no object (flags {:08x}, handle {:08x}); kept", land,
							    kKindInfo[r.kind].extension, slot, hash, flags, entries[static_cast<size_t>(index) * 2]);
						}
						++r.loaded[r.kind];
						continue;
					}
					if (r.kind == kMap)
						UnloadMap(module, slot, index);
					else if (!held && !(flags & 0xFFFC) && refs(module, slot) == 0)
					{
						// (the models and dictionaries a few dozen a frame: their memory is the GPU's, let go by the render
						// thread - a land's all in one frame held it 40 ms)
						if (budget >= 0 && r.kind != kTypes && r.kind != kBounds && ++removals > kRemovalsPerCall)
						{
							r.progress.loaded = 0;
							for (const size_t n : r.loaded)
								r.progress.loaded += n;
							return r.progress;
						}
						removeObject(streaming, index, 0);
					}
					if (flags & 3)
						++r.loaded[r.kind];
				}
			}
			// (a pass done)
			size_t stillLoaded = 0;
			for (const size_t n : r.loaded)
				stillLoaded += n;
			r.progress.loaded = stillLoaded;
			if (stillLoaded && !last)
			{
				r.kind = 0, r.at = 0, r.loaded = {};
				return r.progress;
			}
			// given back: every kind none of whose objects is loaded, unless a kind that holds it by slot keeps its own
			for (int k = 0; k < kKinds; ++k)
				r.give[k] = r.loaded[k] == 0;
			if (!r.give[kMap])
				r.give[kTypes] = false;
			if (!r.give[kTypes])
				r.give[kDrawable] = r.give[kDwd] = r.give[kFragment] = r.give[kTxd] = false;
			if (!r.give[kDrawable] || !r.give[kDwd] || !r.give[kFragment])
				r.give[kTxd] = false;
			r.givingBack = true, r.kind = 0, r.at = 0;
		}

		const auto* emptyBox = reinterpret_cast<const uint8_t*>(base + kEmptyBox);
		for (; r.kind < kKinds; ++r.kind, r.at = 0)
		{
			uint8_t* module = modules[r.kind];
			if (!module)
				continue;
			const uint32_t first = *reinterpret_cast<const uint32_t*>(module + 8);
			const auto removeSlot = *reinterpret_cast<RemoveSlotFn*>(*reinterpret_cast<uintptr_t*>(module) + 0x50);
			const auto& hashes = names.hashes[r.kind];
			for (; r.at < hashes.size(); ++r.at)
			{
				if (over())
					return r.progress;
				const uint32_t hash = hashes[r.at];
				const int32_t slot = Slot(module, hash);
				if (!Used(module, slot))
					continue;
				const uint32_t index = first + static_cast<uint32_t>(slot);
				uint32_t* entry = entries + static_cast<size_t>(index) * 2;
				if (Theirs(entry[0], land))
					continue;
				if (!r.give[r.kind] || (entry[1] & 3))
				{
					++r.progress.kept;
					continue;
				}
				if (r.kind == kBounds)
				{
					const auto* boxes = *reinterpret_cast<const uint8_t* const*>(module + kBoundsBoxes);
					const auto* flags = *reinterpret_cast<const uint16_t* const*>(module + kBoundsFlags);
					if (boxes && flags && std::memcmp(boxes + static_cast<size_t>(slot) * 32, emptyBox, 32) != 0)
					{
						auto& saved = names.bounds[hash];
						std::memcpy(saved.data(), boxes + static_cast<size_t>(slot) * 32, 32);
						std::memcpy(saved.data() + 32, flags + slot, 2);
					}
				}
				removeSlot(module, slot);
				entry[0] = entry[1] = 0;
				// (a map's and a collision file's slot off for the box streamers' searches - their trees still have its box
				// till they are made again, and a search that found a slot on took it for one wanted and missing: the
				// collision's check round the camera loaded everything there and then, 56 ms; the next file given the slot
				// has its flags set)
				if (r.kind == kMap)
				{
					for (const size_t at : {kMapBoxes, kMapBoxes2})
						if (auto* boxes = *reinterpret_cast<uint8_t**>(module + at))
							std::memcpy(boxes + static_cast<size_t>(slot) * 32, emptyBox, 32);
					if (auto* flags = *reinterpret_cast<uint16_t**>(module + kMapFlags))
						flags[slot] = 4;
				}
				else if (r.kind == kBounds)
				{
					if (auto* flags = *reinterpret_cast<uint16_t**>(module + kBoundsFlags))
						flags[slot] = 4;
				}
				++r.progress.released;
			}
		}
		Progress p = r.progress;
		p.done = true;
		names.slotsKnown = false;
		g_releasing.erase(land);
		log::Info("land files: {}: given back to the stores{}", land, p.kept ? std::format(", {} kept registered ({} still loaded)", p.kept, p.loaded) : std::string());
		return p;
	}

	Progress UnloadMaps(const std::string& land, const std::filesystem::path& pack, const float* box, double budget)
	{
		Progress p;
		if (!Supported())
		{
			p.done = true;
			return p;
		}
		using Clock = std::chrono::steady_clock;
		const auto start = Clock::now();
		Names& names = NamesOf(land, pack);
		const auto base = pattern::Module::Main().base;
		const uint32_t* entries = *reinterpret_cast<const uint32_t* const*>(base + kStreaming);
		uint8_t* module = ModuleOf(base, kKindInfo[kMap].vtable);
		const uint32_t first = *reinterpret_cast<const uint32_t*>(module + 8);
		const auto* boxes = *reinterpret_cast<const float* const*>(module + kMapBoxes);
		const auto* flags = *reinterpret_cast<const uint16_t* const*>(module + kMapFlags);
		// (the store's pool: 40 bytes a slot, +0x10 its LOD parent's - none, 0)
		const auto* pool = *reinterpret_cast<const uint8_t* const*>(module + 0x40);
		const int32_t stride = *reinterpret_cast<const int32_t*>(module + 0x54);
		if (!boxes || !flags || !pool)
		{
			p.done = true;
			return p;
		}
		for (const bool children : {true, false})
			for (const uint32_t hash : names.hashes[kMap])
			{
				const int32_t slot = Slot(module, hash);
				if (!Used(module, slot) || !(flags[slot] & 4))
					continue;
				const uint32_t index = first + static_cast<uint32_t>(slot);
				if (!(entries[static_cast<size_t>(index) * 2 + 1] & 3) || Theirs(entries[static_cast<size_t>(index) * 2], land))
					continue;
				if ((*reinterpret_cast<const uint64_t*>(pool + static_cast<size_t>(slot) * stride + 0x10) != 0) != children)
					continue;
				const float* b = boxes + static_cast<size_t>(slot) * 8;
				if (b[0] > box[2] || b[4] < box[0] || b[1] > box[3] || b[5] < box[1])
					continue;
				if (std::chrono::duration<double, std::milli>(Clock::now() - start).count() < budget && UnloadMap(module, slot, index))
					++p.released;
				else
					++p.loaded;
			}
		p.done = p.loaded == 0;
		return p;
	}

	namespace
	{
		// each of the dictionaries whose file is not the given one: its store, its slot, its streaming entry
		template <typename Each> void ForDictionaries(const std::unordered_map<uint32_t, uint32_t>& handles, Each each)
		{
			if (!Supported() || handles.empty())
				return;
			const auto base = pattern::Module::Main().base;
			uint32_t* entries = *reinterpret_cast<uint32_t* const*>(base + kStreaming);
			uint8_t* module = ModuleOf(base, kKindInfo[kDwd].vtable);
			const uint32_t first = *reinterpret_cast<const uint32_t*>(module + 8);
			for (const auto& [hash, handle] : handles)
			{
				const int32_t slot = Slot(module, hash);
				if (!Used(module, slot))
					continue;
				uint32_t* entry = entries + static_cast<size_t>(first + static_cast<uint32_t>(slot)) * 2;
				if (handle && entry[0] != handle)
					each(module, slot, first + static_cast<uint32_t>(slot), entry, handle);
			}
		}
	}

	size_t ForgetCaches(const std::string& land, const std::vector<uint32_t>& sets)
	{
		// (the list: an array of the paths' hashes, how many, room for how many - u16 each)
		struct Read
		{
			uint32_t* hashes;
			uint16_t count, capacity;
		};
		constexpr uintptr_t kCachesRead = 0x42F9BB8;
		auto* read = reinterpret_cast<Read*>(pattern::Module::Main().base + kCachesRead);
		if (!read->hashes || read->count > read->capacity)
			return 0;
		std::set<uint32_t> gone;
		for (const uint32_t set : sets)
			gone.insert(convert::Joaat(std::format("dlc_{}:/x64/data/cacheLoaderData_DLC/{}_{}_cache_y.dat", land, land, set)));
		uint16_t kept = 0;
		for (uint16_t i = 0; i < read->count; ++i)
			if (!gone.contains(read->hashes[i]))
				read->hashes[kept++] = read->hashes[i];
		const size_t removed = read->count - kept;
		read->count = kept;
		return removed;
	}

	size_t DictionariesToGive(const std::unordered_map<uint32_t, uint32_t>& handles)
	{
		size_t n = 0;
		ForDictionaries(handles, [&](uint8_t*, int32_t, uint32_t, uint32_t*, uint32_t) { ++n; });
		return n;
	}

	size_t GiveDictionaries(const std::unordered_map<uint32_t, uint32_t>& handles)
	{
		size_t waiting = 0;
		const auto base = pattern::Module::Main().base;
		auto* streaming = reinterpret_cast<uint8_t*>(base + kStreaming);
		ForDictionaries(handles, [&](uint8_t* module, int32_t slot, uint32_t index, uint32_t* entry, uint32_t handle) {
			if (entry[1] & 3)
			{
				// (loaded: unloaded once nothing holds it - not one the store has no object for, RemoveObject's int3)
				const auto vtable = *reinterpret_cast<uintptr_t*>(module);
				const bool object = !(entry[1] & 0x8000000) || (*reinterpret_cast<GetPtrFn*>(vtable + 0x78))(module, slot);
				if ((entry[1] & 3) == 1 && !(entry[1] & 0xFFFC) && object && (*reinterpret_cast<RefsFn*>(vtable + 0xC8))(module, slot) == 0)
					reinterpret_cast<RemoveObjectFn>(base + kRemoveObject)(streaming, index, 0);
				if (entry[1] & 3)
				{
					++waiting;
					return;
				}
			}
			// (the file the registration would have set: the low word kept, a resource's bit)
			entry[0] = handle;
			entry[1] = (entry[1] & 0xFFFF) | 0x08000000u;
		});
		return waiting;
	}

	void Prefetch(const std::string& land, const std::filesystem::path& pack)
	{
		if (g_names[land].read || g_prefetch.contains(land))
			return;
		auto job = std::make_unique<Prefetching>();
		Prefetching* p = job.get();
		p->worker = std::thread([land, pack, p] {
			Read(land, pack, p->names);
			p->done = true;
		});
		g_prefetch.emplace(land, std::move(job));
	}

	bool NamesReady(const std::string& land)
	{
		if (g_names[land].read)
			return true;
		const auto it = g_prefetch.find(land);
		return it != g_prefetch.end() && it->second->done;
	}
}
