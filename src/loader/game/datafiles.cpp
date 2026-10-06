#include "datafiles.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <mutex>
#include <string_view>
#include <unordered_map>

#include <MinHook.h>

#include "../log.hpp"
#include "../paths.hpp"
#include "../pattern.hpp"

namespace loader::game::datafiles
{
	namespace
	{
		// int Register(const char* path, uint32_t* size, int flags): resolves the path through the search paths and
		// returns a streaming handle for the file (-1 when it does not exist). Data files are loaded through it.
		using RegisterFn = int (*)(const char* path, uint32_t* size, int flags);
		// bool Resolve(searchPaths, char* out, int outSize, const char* path, const char* extension)
		using ResolveFn = bool (*)(void* searchPaths, char* out, int outSize, const char* path, const char* extension);
		using OpenFn = void* (*)(const char* path, bool readOnly);
		using ReadFn = int (*)(void* stream, void* buffer, int size);
		using CloseFn = void (*)(void* stream);

		RegisterFn g_origRegister = nullptr;
		ResolveFn g_resolve = nullptr;
		void* g_searchPaths = nullptr;
		const char* g_noExtension = nullptr;
		OpenFn g_open = nullptr;
		ReadFn g_read = nullptr;
		CloseFn g_close = nullptr;

		std::mutex g_mutex;
		convert::xmlmerge::Overrides g_overrides;
		std::unordered_map<std::string, std::string> g_redirects; // original path -> merged file (game path), "" = original
		std::vector<std::string> g_merged;

		bool ReadGameFile(const char* path, std::string& out)
		{
			void* stream = g_open(path, true);
			if (!stream)
				return false;
			out.clear();
			char buffer[64 * 1024];
			for (int n; (n = g_read(stream, buffer, sizeof(buffer))) > 0;)
				out.append(buffer, n);
			g_close(stream);
			return true;
		}

		std::string GamePath(const std::filesystem::path& p)
		{
			const auto u8 = p.generic_u8string();
			return std::string(u8.begin(), u8.end());
		}

		// One file per original path in ModLoader\cache\datafiles: device and path, with the separators flattened.
		std::string WriteCopy(std::string_view path, const std::string& text)
		{
			std::string name(path);
			for (auto& c : name)
				if (c == ':' || c == '/' || c == '\\')
					c = '_';
			const auto dir = paths::Get().root / L"cache" / L"datafiles";
			std::error_code ec;
			std::filesystem::create_directories(dir, ec);
			const auto file = dir / std::filesystem::path(std::u8string(name.begin(), name.end()));
			std::ofstream(file, std::ios::binary).write(text.data(), static_cast<std::streamsize>(text.size()));
			return GamePath(file);
		}

		// The game's own entries of the vehicles that replacement mods add under a new name.
		struct Captured
		{
			std::string vehicles, variation, txdParent;
			std::string handling; // the game's handling named like the model, before any override
		};
		std::vector<convert::VehicleClone> g_clones;
		std::unordered_map<std::string, Captured> g_captured; // by model name

		void Capture(const std::string& text)
		{
			const auto entries = convert::xmlmerge::Entries(text);
			for (const auto& clone : g_clones)
			{
				Captured& c = g_captured[clone.from];
				if (c.vehicles.empty())
					if (const auto it = entries.find("CVehicleModelInfo__InitDataList/InitDatas|modelName=" + clone.from); it != entries.end())
						c.vehicles = it->second;
				if (c.variation.empty())
					if (const auto it = entries.find("CVehicleModelInfoVariation/variationData|modelName=" + clone.from); it != entries.end())
						c.variation = it->second;
				if (c.handling.empty())
					if (const auto it = entries.find("CHandlingDataMgr/HandlingData|handlingName=" + clone.from); it != entries.end())
						c.handling = it->second;
				// <txdRelationships><Item><parent>vehshare</parent><child>adder</child></Item>: not keyed (parents repeat).
				if (c.txdParent.empty())
					for (size_t at = 0; (at = text.find("<child>", at)) != std::string::npos; at += 7)
					{
						const size_t end = text.find("</child>", at);
						const size_t item = text.rfind("<Item>", at);
						if (end == std::string::npos || item == std::string::npos)
							break;
						std::string child = text.substr(at + 7, end - at - 7);
						for (auto& ch : child)
							ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
						if (child == clone.from)
						{
							c.txdParent = convert::xmlmerge::ElementText(std::string_view(text).substr(item, end - item), "parent");
							break;
						}
					}
			}
		}

		std::string Lower(std::string s)
		{
			for (auto& c : s)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return s;
		}

		std::string Upper(std::string s)
		{
			for (auto& c : s)
				c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			return s;
		}

		std::string SetElement(std::string text, std::string_view tag, std::string_view value)
		{
			const std::string open = "<" + std::string(tag) + ">", close = "</" + std::string(tag) + ">";
			const size_t at = text.find(open);
			const size_t end = at == std::string::npos ? std::string::npos : text.find(close, at);
			if (end != std::string::npos)
				text.replace(at + open.size(), end - at - open.size(), value);
			return text;
		}

		// Contents of a placeholder of a replacement mod's pack (clones_*.meta), or "" when it has nothing.
		std::string Generate(std::string_view device, std::string_view file)
		{
			std::string items, relationships;
			int count = 0;
			for (const auto& clone : g_clones)
			{
				if (clone.device != device)
					continue;
				const Captured& c = g_captured[clone.from];
				// Without its own handling the new model gets a copy of the game's (unmodified) one, so another mod's
				// handling changes for the old model do not apply to it.
				std::string handlingId = clone.handlingId;
				const bool copyHandling = handlingId.empty() && !c.handling.empty() &&
				                          (c.vehicles.empty() || Lower(convert::xmlmerge::ElementText(c.vehicles, "handlingId")) == clone.from);
				if (copyHandling)
					handlingId = Upper(clone.to);
				if (file == "clones_handling.meta" && (!clone.handling.empty() || copyHandling))
					items += (clone.handling.empty() ? convert::xmlmerge::RenameValue(c.handling, "handlingName", clone.from, handlingId) : clone.handling) + "\n";
				else if (file == "clones_vehicles.meta")
				{
					std::string entry = clone.vehicles;
					if (entry.empty() && !c.vehicles.empty())
						entry = convert::xmlmerge::RenameValue(convert::xmlmerge::RenameValue(c.vehicles, "modelName", clone.from, clone.to), "txdName",
						    clone.from, clone.to);
					if (entry.empty())
					{
						log::Warn("datafiles: {} of mod {} is not a vehicle the game knows; only the first mod's {} is used", clone.from, clone.mod,
						    clone.from);
						continue;
					}
					if (!handlingId.empty())
						entry = SetElement(entry, "handlingId", handlingId);
					items += entry + "\n";
					if (!c.txdParent.empty())
						relationships += std::format("    <Item>\n      <parent>{}</parent>\n      <child>{}</child>\n    </Item>\n", c.txdParent, clone.to);
					log::Info("datafiles: mod {} adds its {} as {} (entry copied from {})", clone.mod, clone.from, clone.to,
					    clone.vehicles.empty() ? "the game's entry" : "the mod's entry");
				}
				else if (file == "clones_carvariations.meta")
				{
					std::string entry = clone.variation;
					if (entry.empty() && !c.variation.empty())
						entry = convert::xmlmerge::RenameValue(c.variation, "modelName", clone.from, clone.to);
					if (!entry.empty())
						items += entry + "\n";
				}
				else
					continue;
				++count;
			}
			if (count == 0)
				return {};
			if (file == "clones_handling.meta")
				return std::format("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<CHandlingDataMgr>\n  <HandlingData>\n{}  </HandlingData>\n</CHandlingDataMgr>\n", items);
			if (file == "clones_vehicles.meta")
				return std::format("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<CVehicleModelInfo__InitDataList>\n  <residentTxd>vehshare</residentTxd>\n"
				                   "  <residentAnims />\n  <InitDatas>\n{}  </InitDatas>\n  <txdRelationships>\n{}  </txdRelationships>\n"
				                   "</CVehicleModelInfo__InitDataList>\n",
				    items, relationships);
			return std::format("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<CVehicleModelInfoVariation>\n  <variationData>\n{}  </variationData>\n"
			                   "</CVehicleModelInfoVariation>\n",
			    items);
		}

		std::vector<convert::NamedFile> g_named;
		std::map<uint32_t, convert::TextEntry> g_text;
		std::string g_ownRoot; // ModLoader folder as a lower case game path
		std::set<uint32_t> g_textAdded; // labels already found in (or added to) one of the game's text files

		// GXT2 text with the mods' entries: replaced where the label exists, added to the file the mod's entry came from.
		// Layout: "2TXG", count, {hash, offset} * count sorted by hash, "2TXG", total size, NUL-terminated strings.
		std::string MergeText(const std::string& original, const std::string& file, int& replaced, int& added)
		{
			const auto u32 = [&](size_t at) {
				uint32_t v = 0;
				if (at + 4 <= original.size())
					memcpy(&v, original.data() + at, 4);
				return v;
			};
			if (original.size() < 8 || original.compare(0, 4, "2TXG") != 0)
				return {};
			const uint32_t count = u32(4);
			if (8 + uint64_t(count) * 8 > original.size())
				return {};
			std::map<uint32_t, std::string> entries;
			for (uint32_t i = 0; i < count; ++i)
			{
				const uint32_t offset = u32(12 + i * 8);
				if (offset >= original.size())
					return {};
				entries[u32(8 + i * 8)] = std::string(original.c_str() + offset, strnlen(original.c_str() + offset, original.size() - offset));
			}
			for (const auto& [hash, entry] : g_text)
			{
				const auto it = entries.find(hash);
				if (it != entries.end())
				{
					g_textAdded.insert(hash); // the game has it: replaced, never added elsewhere
					if (it->second != entry.text)
					{
						it->second = entry.text;
						++replaced;
					}
				}
				else if (entry.file == file && !g_textAdded.contains(hash))
				{
					g_textAdded.insert(hash);
					entries.emplace(hash, entry.text);
					++added;
				}
			}
			std::string out = "2TXG";
			const auto put = [&](uint32_t v) { out.append(reinterpret_cast<const char*>(&v), 4); };
			put(static_cast<uint32_t>(entries.size()));
			uint32_t offset = static_cast<uint32_t>(8 + entries.size() * 8 + 8);
			for (const auto& [hash, text] : entries)
			{
				put(hash);
				put(offset);
				offset += static_cast<uint32_t>(text.size() + 1);
			}
			out += "2TXG";
			put(offset);
			for (const auto& [hash, text] : entries)
				out.append(text.c_str(), text.size() + 1);
			return out;
		}

		// The merged (or generated) copy of `path`, or "" to load the original.
		std::string Redirect(const char* path)
		{
			const std::string lower = Lower(path);
			const std::string_view p = lower;
			const size_t cut = p.find_last_of(":/\\");
			const std::string_view name = cut == std::string_view::npos ? p : p.substr(cut + 1);
			const size_t dot = name.rfind('.');
			const std::string_view ext = dot == std::string_view::npos ? std::string_view() : name.substr(dot);
			const bool data = ext == ".meta" || ext == ".xml";
			if (!data && ext != ".gxt2" && ext != ".awc" && ext != ".gfx" && ext != ".dat")
				return {};
			// Our own copies (and generated packs' files read back through the game) are never redirected again.
			if (p.starts_with(g_ownRoot))
				return {};
			std::lock_guard lock(g_mutex);
			if (g_overrides.Empty() && g_clones.empty() && g_named.empty() && g_text.empty())
				return {};
			if (const auto it = g_redirects.find(lower); it != g_redirects.end())
				return it->second;
			std::string& redirect = g_redirects[lower];
			// Whole files replaced by name: the one meant for this folder, else one for any folder.
			if (!g_named.empty())
			{
				std::string_view parent = cut == std::string_view::npos ? std::string_view() : p.substr(0, cut);
				parent = parent.substr(parent.find_last_of(":/\\") == std::string_view::npos ? 0 : parent.find_last_of(":/\\") + 1);
				if (parent.ends_with(".rpf"))
					parent.remove_suffix(4);
				const convert::NamedFile* match = nullptr;
				for (const auto& f : g_named)
					if (f.name == name && (f.folder == parent || (f.folder.empty() && !match)))
					{
						match = &f;
						if (f.folder == parent)
							break;
					}
				if (match)
				{
					redirect = GamePath(match->file);
					log::Info("datafiles: {} -> {} of mod {}", path, match->name, match->mod);
					return redirect;
				}
			}
			if (ext == ".gxt2" && !g_text.empty())
			{
				char resolved[256] = {};
				std::string original;
				if (!g_resolve(g_searchPaths, resolved, sizeof(resolved), path, g_noExtension) || !ReadGameFile(resolved, original))
					return {};
				int replaced = 0, added = 0;
				const std::string merged = MergeText(original, std::string(name), replaced, added);
				if (replaced + added == 0)
					return {};
				redirect = WriteCopy(p, merged);
				log::Info("datafiles: {} -> {} text entr{} replaced, {} added", path, replaced, replaced == 1 ? "y" : "ies", added);
				return redirect;
			}
			if (!data || (g_overrides.Empty() && g_clones.empty()))
				return {};
			// The packs generated for replacement mods hold the overrides themselves; their clones_*.meta are generated.
			if (p.starts_with("dlc_mlr"))
			{
				const size_t colon = p.find(':'), slash = p.rfind('/');
				const std::string_view file = p.substr(slash + 1);
				if (colon == std::string_view::npos || !file.starts_with("clones_"))
					return {};
				const std::string text = Generate(p.substr(0, colon), file);
				if (!text.empty())
					redirect = WriteCopy(p, text);
				return redirect;
			}
			char resolved[256] = {};
			if (!g_resolve(g_searchPaths, resolved, sizeof(resolved), path, g_noExtension))
				return {};
			std::string original, merged;
			if (!ReadGameFile(resolved, original))
				return {};
			if (!g_clones.empty())
				Capture(original);
			std::vector<std::string> keys;
			const int replaced = g_overrides.Apply(original, merged, &keys);
			if (replaced == 0)
				return {};
			redirect = WriteCopy(p, merged);
			g_merged.push_back(std::format("{}: {} entr{}", p, replaced, replaced == 1 ? "y" : "ies"));
			log::Info("datafiles: {} -> {} entr{} overridden ({}{})", p, replaced, replaced == 1 ? "y" : "ies", keys.front(),
			    keys.size() > 1 ? ", ..." : "");
			return redirect;
		}

		int HookRegister(const char* path, uint32_t* size, int flags)
		{
			if (path)
				if (const std::string redirect = Redirect(path); !redirect.empty())
					return g_origRegister(redirect.c_str(), size, flags);
			return g_origRegister(path, size, flags);
		}

		OpenFn g_origOpen = nullptr;

		void* HookOpen(const char* path, bool readOnly)
		{
			if (path && readOnly)
				if (const std::string redirect = Redirect(path); !redirect.empty())
					return g_origOpen(redirect.c_str(), readOnly);
			return g_origOpen(path, readOnly);
		}
	}

	bool InstallHooks()
	{
		const auto module = pattern::Module::Main();
		// Register: ...; lea rax,[empty string]; mov [rsp+20h],rax; lea rcx,[search paths]; lea rdx,[rsp+30h];
		// mov r8d,100h; mov r9,rsi; call Resolve
		const auto reg = pattern::Find(module.text, pattern::Pattern::Parse(
		    "41 57 41 56 41 54 56 57 55 53 48 81 EC 30 02 00 00 45 89 C7 49 89 D6 48 89 CE 48 8D 0D ? ? ? ? E8 ? ? ? ? "
		    "48 8D 05 ? ? ? ? 48 89 44 24 20 48 8D 0D ? ? ? ? 48 8D 54 24 30 41 B8 00 01 00 00 49 89 F1 E8"));
		// Open(path, readOnly): mov ebx,edx; mov rdi,rcx; call GetDevice; test rax,rax; je; ...; call [rax+8]
		const auto open = pattern::Find(module.text, pattern::Pattern::Parse(
		    "41 56 56 57 53 48 83 EC 28 89 D3 48 89 CF E8 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? 48 89 C6 48 8B 00 48 89 F1 48 89 FA 41 89 D8 FF 50 08"));
		// A reader of a 4-byte magic: mov dl,1; call Open; test rax,rax; je; mov rsi,rax; mov [rsp+2Ch],0;
		// lea rdx,[rsp+2Ch]; mov rcx,rax; mov r8d,4; call Read
		const auto read = pattern::Find(module.text, pattern::Pattern::Parse(
		    "B2 01 E8 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? 48 89 C6 C7 44 24 2C 00 00 00 00 48 8D 54 24 2C 48 89 C1 41 B8 04 00 00 00 E8"));
		// mov dl,1; call Open; test rax,rax; je; mov rsi,rax; mov ecx,28h; call alloc; ...; mov rcx,rsi; call Close
		const auto close = pattern::Find(module.text, pattern::Pattern::Parse(
		    "B2 01 E8 ? ? ? ? 48 85 C0 74 48 48 89 C6 B9 28 00 00 00 E8 ? ? ? ? 48 89 C7 48 89 C1 E8 ? ? ? ? 48 8D 05 ? ? ? ? "
		    "48 89 07 0F 57 C0 0F 11 47 10 C7 47 20 00 00 00 00 48 89 F9 48 89 F2 45 31 C0 E8 ? ? ? ? 48 89 F1 E8"));
		if (!reg || !open || !read || !close)
		{
			log::Warn("datafiles: game functions not found ({} {} {} {}); data file overrides are not active", reg.has_value(),
			    open.has_value(), read.has_value(), close.has_value());
			return false;
		}
		g_noExtension = reinterpret_cast<const char*>(pattern::Rip(*reg + 0x29));
		g_searchPaths = reinterpret_cast<void*>(pattern::Rip(*reg + 0x35));
		g_resolve = reinterpret_cast<ResolveFn>(pattern::Rip(*reg + 0x48));
		g_open = reinterpret_cast<OpenFn>(*open);
		g_read = reinterpret_cast<ReadFn>(pattern::Rip(*read + 0x2A));
		g_close = reinterpret_cast<CloseFn>(pattern::Rip(*close + 0x4E));
		if (MH_CreateHook(reinterpret_cast<void*>(*reg), reinterpret_cast<void*>(&HookRegister), reinterpret_cast<void**>(&g_origRegister)) != MH_OK ||
		    MH_EnableHook(reinterpret_cast<void*>(*reg)) != MH_OK)
		{
			log::Warn("datafiles: could not hook the data file registration; data file overrides are not active");
			return false;
		}
		// Files opened as streams (Scaleform movies, audio game data, ...). Our own reads use the original.
		if (MH_CreateHook(reinterpret_cast<void*>(*open), reinterpret_cast<void*>(&HookOpen), reinterpret_cast<void**>(&g_origOpen)) == MH_OK &&
		    MH_EnableHook(reinterpret_cast<void*>(*open)) == MH_OK)
			g_open = g_origOpen;
		else
			log::Warn("datafiles: could not hook the stream open; files opened as streams are not replaced");
		g_ownRoot = Lower(GamePath(paths::Get().root));
		// Merged copies are written again whenever the game asks for them; drop those of earlier sessions.
		std::error_code ec;
		std::filesystem::remove_all(paths::Get().root / L"cache" / L"datafiles", ec);
		log::Info("datafiles: hooked (+{:#x}; resolve +{:#x}, open +{:#x}, read +{:#x}, close +{:#x})", *reg - module.base,
		    reinterpret_cast<uintptr_t>(g_resolve) - module.base, *open - module.base, reinterpret_cast<uintptr_t>(g_read) - module.base,
		    reinterpret_cast<uintptr_t>(g_close) - module.base);
		return true;
	}

	void SetOverrides(convert::xmlmerge::Overrides overrides)
	{
		std::lock_guard lock(g_mutex);
		g_overrides = std::move(overrides);
		g_redirects.clear();
	}

	void SetClones(std::vector<convert::VehicleClone> clones)
	{
		std::lock_guard lock(g_mutex);
		g_clones = std::move(clones);
		g_redirects.clear();
	}

	void SetFiles(std::vector<convert::NamedFile> files, std::map<uint32_t, convert::TextEntry> text)
	{
		std::lock_guard lock(g_mutex);
		g_named = std::move(files);
		g_text = std::move(text);
		g_redirects.clear();
	}

	std::vector<std::string> Merged()
	{
		std::lock_guard lock(g_mutex);
		return g_merged;
	}
}
