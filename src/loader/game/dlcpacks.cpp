#include "dlcpacks.hpp"

#include "../convert/replace.hpp"
#include "../convert/ypt.hpp"
#include "datafiles.hpp"

#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <functional>
#include <mutex>
#include <set>
#include <unordered_map>

#include <MinHook.h>
#include <thread>

#include "../config.hpp"
#include "../log.hpp"
#include "../paths.hpp"
#include "../pattern.hpp"
#include "../ui/overlay.hpp"

namespace loader::game::dlcpacks
{
	namespace
	{
		// RPF7 header: magic, entry count, names length (bits 28-30: name shift), encryption.
		constexpr uint32_t kMagic = 0x52504637;           // '7FPR'
		constexpr uint32_t kEncryptionOpen = 0x4E45504F;  // 'OPEN': unencrypted, written by tools/rpf_pack.py
		constexpr uint32_t kEncryptionNg = 0x0FEFFFFF;    // what the game's own archives use

		// Registers one DLC folder ("<path>dlc.rpf"); returns whether it was added.
		using RegisterFn = bool (*)(void* manager, const char* path);
		// Processes the DLC list after dlclist.xml was read (also scans platform:/dlcPacks/).
		using ProcessFn = void (*)(void* manager);
		// rpf.cache lookup used when an archive is opened. A hit carries the archive's table of
		// contents; when its "decrypted" flag is set the game uses it as is and never decrypts.
		// The game's decryption code rejects unencrypted archives (int3) and cannot be hooked
		// (it is restored by the executable's protection), so our archives come through here.
		using CacheLookupFn = void* (*)(void* device, const char* path);

		struct CacheEntry
		{
			uint8_t* toc;   // header + entries + names, plain
			uint8_t used;   // +0x08, set by the game
			uint8_t plain;  // +0x09, 1 = already decrypted
			uint8_t pad[0x40 - 10];
		};

		// NG decryption in place: (encryption, key index, data, size). Protected code: called, never hooked.
		using DecryptFn = void (*)(uint32_t encryption, uint32_t key, uint8_t* data, uint32_t size);

		// The game's hang watchdog crashes the game when the main thread stops for ~60 s, unless this counter is
		// non-zero (the game raises it around long operations itself). Raised while packs are converted.
		volatile long* g_watchdogPause = nullptr;

		struct WatchdogPause
		{
			WatchdogPause()
			{
				if (g_watchdogPause)
					_InterlockedIncrement(g_watchdogPause);
			}
			~WatchdogPause()
			{
				if (g_watchdogPause)
					_InterlockedDecrement(g_watchdogPause);
			}
		};

		RegisterFn g_register = nullptr;
		DecryptFn g_decrypt = nullptr;
		convert::Decryptor g_decryptor;
		std::unordered_map<std::string, convert::PackResult> g_prepared; // by pack name, first processing only
		ProcessFn g_origProcess = nullptr;
		CacheLookupFn g_origLookup = nullptr;

		std::mutex g_mutex;
		std::vector<Pack> g_packs;
		// Replacement mods: their data file entries, and the streaming files each one replaces.
		convert::xmlmerge::Overrides g_overrides;
		std::map<std::string, convert::ReplacementFiles> g_replacementFiles;
		std::map<std::string, std::vector<std::string>> g_conflicts; // pack name -> warnings
		std::map<std::string, std::map<std::string, std::string>> g_renames; // pack name -> vehicle renames
		std::vector<convert::VehicleClone> g_clones;
		std::unordered_map<std::string, CacheEntry*> g_entries; // normalized archive path as the game opens it

		std::string Utf8(const std::u8string& s)
		{
			return std::string(s.begin(), s.end());
		}

		std::string Normalize(std::string_view path)
		{
			std::string s(path);
			for (char& c : s)
				c = c == '\\' ? '/' : (c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
			return s;
		}

		// Reads the table of contents of the archive at `offset`. Only unencrypted archives get
		// an entry (encrypted ones are opened by the game as usual).
		CacheEntry* LoadToc(std::ifstream& in, uint64_t offset, std::string& error)
		{
			uint32_t header[4] = {};
			in.clear();
			in.seekg(static_cast<std::streamoff>(offset));
			if (!in.read(reinterpret_cast<char*>(header), sizeof(header)) || header[0] != kMagic)
			{
				error = "not an RPF7 archive";
				return nullptr;
			}
			if (header[3] != kEncryptionOpen)
			{
				error = "encrypted, used as it is";
				return nullptr;
			}
			const size_t size = 16 + static_cast<size_t>(header[1]) * 16 + (header[2] & 0x0FFFFFFF);
			if (header[1] == 0 || size > 64u << 20)
			{
				error = "corrupt header";
				return nullptr;
			}
			// Never freed: the game keeps using the table for the whole session. A cached table is
			// followed by a u16 parent index per entry, which the game fills itself (0x110060) only
			// when it decrypts a table; paths of files inside the archive are built from it.
			const uint32_t count = header[1];
			auto* toc = static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + count * sizeof(uint16_t)));
			std::memcpy(toc, header, 16);
			if (!in.read(reinterpret_cast<char*>(toc + 16), static_cast<std::streamsize>(size - 16)))
			{
				HeapFree(GetProcessHeap(), 0, toc);
				error = "truncated table of contents";
				return nullptr;
			}
			// No encryption: with NG here the game decrypts script resources (.ysc) as whole entries, which turns our
			// plain scripts into garbage (ERR_GEN_ZLIB_1). Other files and resources load the same either way.
			reinterpret_cast<uint32_t*>(toc)[3] = 0;
			auto* parents = reinterpret_cast<uint16_t*>(toc + size);
			for (uint32_t i = 0; i < count; ++i)
			{
				uint32_t marker, first, children;
				std::memcpy(&marker, toc + 16 + 16 * static_cast<size_t>(i) + 4, 4);
				std::memcpy(&first, toc + 16 + 16 * static_cast<size_t>(i) + 8, 4);
				std::memcpy(&children, toc + 16 + 16 * static_cast<size_t>(i) + 12, 4);
				if (marker != 0x7FFFFF00) // not a directory
					continue;
				for (uint32_t c = first; c < first + children && c < count; ++c)
					parents[c] = static_cast<uint16_t>(i);
			}
			auto* entry = static_cast<CacheEntry*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(CacheEntry)));
			entry->toc = toc;
			entry->plain = 1;
			return entry;
		}

		// A binary file inside an archive (entry layout: research/phase0.md §11).
		struct TocFile
		{
			std::string path; // inside the archive, '/' separated
			uint64_t offset;  // from the start of the archive
			uint32_t packed;  // deflated size, 0 = stored
			uint32_t size;
		};

		std::vector<TocFile> ListFiles(const CacheEntry& archive)
		{
			const auto* header = reinterpret_cast<const uint32_t*>(archive.toc);
			const uint32_t count = header[1], namesSize = header[2] & 0x0FFFFFFF, shift = (header[2] >> 28) & 7;
			const uint8_t* entries = archive.toc + 16;
			const char* names = reinterpret_cast<const char*>(entries + static_cast<size_t>(count) * 16);
			const auto name = [&](uint32_t offset) -> std::string {
				offset <<= shift;
				return offset < namesSize ? std::string(names + offset, strnlen(names + offset, namesSize - offset)) : std::string();
			};
			std::vector<TocFile> files;
			const auto walk = [&](auto&& self, uint32_t index, const std::string& prefix, int depth) -> void {
				if (index >= count || depth > 32)
					return;
				const uint8_t* e = entries + static_cast<size_t>(index) * 16;
				uint64_t q;
				uint32_t a, b;
				std::memcpy(&q, e, 8);
				std::memcpy(&a, e + 8, 4);
				std::memcpy(&b, e + 12, 4);
				if (static_cast<uint32_t>(q >> 32) == 0x7FFFFF00) // directory: first child, child count
				{
					const std::string dir = index == 0 ? std::string() : prefix + name(static_cast<uint32_t>(q)) + "/";
					for (uint32_t i = a; i < a + b && i < count; ++i)
						self(self, i, dir, depth + 1);
				}
				else if (!(q >> 63)) // binary file (resources are never archives or setup files)
					files.push_back({prefix + name(static_cast<uint32_t>(q & 0xFFFF)), ((q >> 40) & 0x7FFFFF) * 512,
					    static_cast<uint32_t>((q >> 16) & 0xFFFFFF), a});
			};
			walk(walk, 0, std::string(), 0);
			return files;
		}

		// Raw deflate, as used inside RPF archives, through the zlib1.dll that ships with the game.
		bool Inflate(const std::vector<uint8_t>& packed, std::vector<uint8_t>& out)
		{
			struct ZStream
			{
				const uint8_t* next_in;
				uint32_t avail_in;
				uint32_t total_in;
				uint8_t* next_out;
				uint32_t avail_out;
				uint32_t total_out;
				const char* msg;
				void* state;
				void* zalloc;
				void* zfree;
				void* opaque;
				int data_type;
				uint32_t adler;
				uint32_t reserved;
			};
			using InitFn = int(__cdecl*)(ZStream*, int, const char*, int);
			using InflateFn = int(__cdecl*)(ZStream*, int);
			using EndFn = int(__cdecl*)(ZStream*);
			static const HMODULE zlib = LoadLibraryW(L"zlib1.dll");
			const auto init = zlib ? reinterpret_cast<InitFn>(GetProcAddress(zlib, "inflateInit2_")) : nullptr;
			const auto inflate = zlib ? reinterpret_cast<InflateFn>(GetProcAddress(zlib, "inflate")) : nullptr;
			const auto end = zlib ? reinterpret_cast<EndFn>(GetProcAddress(zlib, "inflateEnd")) : nullptr;
			if (!init || !inflate || !end)
				return false;
			ZStream z{};
			z.next_in = packed.data();
			z.avail_in = static_cast<uint32_t>(packed.size());
			z.next_out = out.data();
			z.avail_out = static_cast<uint32_t>(out.size());
			if (init(&z, -15, "1.2.11", static_cast<int>(sizeof(ZStream))) != 0)
				return false;
			const int status = inflate(&z, 4 /* Z_FINISH */);
			end(&z);
			return status == 1 /* Z_STREAM_END */ && z.total_out == out.size();
		}

		std::string ReadText(std::ifstream& in, const TocFile& file)
		{
			std::vector<uint8_t> data(file.packed ? file.packed : file.size);
			in.clear();
			in.seekg(static_cast<std::streamoff>(file.offset));
			if (data.empty() || !in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())))
				return {};
			if (file.packed)
			{
				std::vector<uint8_t> plain(file.size);
				if (!Inflate(data, plain))
					return {};
				data = std::move(plain);
			}
			return std::string(data.begin(), data.end());
		}

		std::string XmlValue(const std::string& xml, std::string_view tag)
		{
			const std::string open = "<" + std::string(tag) + ">", close = "</" + std::string(tag) + ">";
			const size_t a = xml.find(open);
			const size_t b = a == xml.npos ? a : xml.find(close, a);
			return b == xml.npos ? std::string() : xml.substr(a + open.size(), b - a - open.size());
		}

		// Prepares an unencrypted pack: its dlc.rpf, and the archives inside it, which the game
		// opens as "<deviceName>:/<path>" (deviceName from the pack's setup2.xml).
		void IndexPack(const Pack& pack)
		{
			std::ifstream in(pack.dir / L"dlc.rpf", std::ios::binary);
			std::string error;
			CacheEntry* root = LoadToc(in, 0, error);
			if (!root)
			{
				log::Info("dlc pack {}: {}", pack.name, error);
				return;
			}
			std::unordered_map<std::string, CacheEntry*> found{{Normalize(pack.path + "dlc.rpf"), root}};
			const auto files = ListFiles(*root);
			const auto setup = std::find_if(files.begin(), files.end(), [](const TocFile& f) { return Normalize(f.path) == "setup2.xml"; });
			const std::string device = setup == files.end() ? std::string() : XmlValue(ReadText(in, *setup), "deviceName");
			if (device.empty())
				log::Warn("dlc pack {}: no deviceName in setup2.xml; archives inside it will not load", pack.name);
			else
				for (const TocFile& file : files)
				{
					const std::string path = Normalize(file.path);
					if (path.size() < 4 || path.compare(path.size() - 4, 4, ".rpf") != 0)
						continue;
					if (file.packed)
					{
						log::Warn("dlc pack {}: {} is compressed and cannot be opened", pack.name, file.path);
						continue;
					}
					std::string nestedError;
					if (CacheEntry* nested = LoadToc(in, file.offset, nestedError))
						found[Normalize(device) + ":/" + path] = nested;
				}
			log::Info("dlc pack {}: {} unencrypted archive(s), device {}", pack.name, found.size(), device);
			std::lock_guard lock(g_mutex);
			g_entries.merge(found);
		}

		std::vector<Pack> Discover()
		{
			std::vector<Pack> packs;
			const auto& disabled = config::Get().disabledAssets;
			std::error_code ec;
			for (const auto& entry : std::filesystem::directory_iterator(paths::Get().mods, ec))
			{
				if (!entry.is_directory())
					continue;
				Pack pack;
				if (!std::filesystem::is_regular_file(entry.path() / L"dlc.rpf", ec))
				{
					// A code mod's own folder (mods\<name>.dll next to it) is never a replacement mod.
					auto dll = entry.path();
					dll += L".dll";
					if (std::filesystem::exists(dll, ec) || !convert::LooksLikeReplacementMod(entry.path()))
						continue;
					pack.replacement = true;
					// DLC packs that .oiv packages install are packs of their own.
					std::vector<std::string> warnings;
					for (auto& oiv : convert::ExtractOivPacks(Utf8(entry.path().filename().u8string()), entry.path(), warnings))
					{
						Pack added;
						added.name = std::move(oiv.name);
						added.source = oiv.dir;
						added.dir = oiv.dir;
						added.enabled = !disabled.contains(added.name);
						packs.push_back(std::move(added));
					}
					for (const auto& w : warnings)
						log::Warn("dlc pack {}: {}", Utf8(entry.path().filename().u8string()), w);
				}
				pack.name = Utf8(entry.path().filename().u8string());
				pack.source = entry.path();
				pack.dir = entry.path();
				pack.enabled = !disabled.contains(pack.name);
				packs.push_back(std::move(pack));
			}
			std::sort(packs.begin(), packs.end(), [](const Pack& a, const Pack& b) { return a.name < b.name; });
			return packs;
		}

		void* HookLookup(void* device, const char* path)
		{
			if (path)
			{
				std::lock_guard lock(g_mutex);
				if (const auto it = g_entries.find(Normalize(path)); it != g_entries.end())
					return it->second;
			}
			return g_origLookup(device, path);
		}

		// Files that more than one enabled replacement mod replaces: the first mod (by name) wins. Shown as warnings of
		// the later mods.
		void FindConflicts(const std::vector<Pack>& packs)
		{
			std::map<std::string, std::string> owner;
			for (const Pack& pack : packs)
			{
				if (!pack.replacement || !pack.enabled)
					continue;
				for (const auto& file : g_replacementFiles[pack.name].streaming)
				{
					const auto [it, added] = owner.emplace(file, pack.name);
					if (!added)
					{
						g_conflicts[pack.name].push_back(std::format("{} 也被模組 {} 替換，目前使用 {} 的版本", file, it->second, it->second));
						log::Warn("replacement mod {}: {} is also replaced by {}; {} is used", pack.name, file, it->second, it->second);
					}
				}
			}
			for (const auto& c : g_overrides.Conflicts())
				log::Warn("replacement mods: data entry {} is overridden by more than one mod; the first one is used", c);
		}

		// Vehicles that more than one enabled replacement mod replaces: the first mod (by name) replaces the vehicle, each
		// later one adds its version as a new model with a suffix (adder -> adder_2) so they all coexist.
		void FindRenames(const std::vector<Pack>& packs)
		{
			std::map<std::string, std::string> owner; // model -> mod
			std::set<std::string> taken;              // every streaming file name of every mod
			std::map<std::string, std::vector<std::string>> names;
			for (const Pack& pack : packs)
				if (pack.replacement && pack.enabled)
				{
					names[pack.name] = convert::ListStreamingFiles(pack.source, g_decrypt ? &g_decryptor : nullptr);
					taken.insert(names[pack.name].begin(), names[pack.name].end());
				}
			for (const Pack& pack : packs)
			{
				if (!pack.replacement || !pack.enabled)
					continue;
				for (const auto& file : names[pack.name])
				{
					if (!file.ends_with(".yft") || file.ends_with("_hi.yft"))
						continue;
					const std::string model = file.substr(0, file.size() - 4);
					const auto [it, added] = owner.emplace(model, pack.name);
					if (added)
						continue;
					std::string renamed;
					for (int n = 2;; ++n)
					{
						renamed = std::format("{}_{}", model, n);
						if (!owner.contains(renamed) && !taken.contains(renamed + ".yft"))
							break;
					}
					owner.emplace(renamed, pack.name);
					g_renames[pack.name][model] = renamed;
					g_conflicts[pack.name].push_back(std::format("{} 也被模組 {} 替換；此模組的版本改成新增的車輛 {}", model, it->second, renamed));
					log::Warn("replacement mod {}: {} is also replaced by {}; this mod's {} is added as {}", pack.name, model, it->second, model, renamed);
				}
			}
		}

		// Converts the pack if it has legacy resources (once per session; later processing reuses the result).
		void Prepare(Pack& pack)
		{
			auto it = g_prepared.find(pack.name);
			if (it == g_prepared.end())
			{
				WatchdogPause pause;
				const auto showProgress = [] { std::thread([] { ui::StartOverlay(); }).detach(); };
				it = g_prepared.emplace(pack.name, pack.replacement
				        ? convert::PrepareReplacement(pack.name, pack.source, g_decrypt ? &g_decryptor : nullptr, showProgress, g_overrides,
				              g_replacementFiles[pack.name], g_renames[pack.name], g_clones)
				        : convert::PreparePack(pack.name, pack.source, g_decrypt ? &g_decryptor : nullptr, showProgress)).first;
			}
			const convert::PackResult& r = it->second;
			pack.state = r.state;
			pack.dir = r.dir;
			pack.convertedFiles = r.convertedFiles;
			pack.error = r.error;
			pack.warnings = r.warnings;
			pack.empty = r.empty;
		}

		// Walks an archive and its nested archives (research aid).
		void WalkArchive(const convert::Archive& archive, const std::string& prefix,
		    const std::function<void(const convert::Archive&, uint32_t, const std::string&)>& visit)
		{
			for (uint32_t i = 0; i < archive.Nodes().size(); ++i)
			{
				const auto& n = archive.Nodes()[i];
				if (n.directory)
					continue;
				const std::string path = prefix + n.name;
				if (!n.resource && !n.stored && path.ends_with(".rpf"))
				{
					convert::Archive nested;
					std::string error;
					if (archive.OpenNested(i, nested, error))
						WalkArchive(nested, path + "/", visit);
					continue;
				}
				visit(archive, i, path);
			}
		}

		// Research aid: ModLoader\debug_convert.txt, one command per line:
		//   <dlc.rpf path>                         convert into the cache without loading it
		//   list <archive>                         log resource versions per extension
		//   extract <archive>|<file name>|<out>    write one file (resources with their RSC7 header)
		//   find <archive>|<text>                  log paths containing the text
		//   extractall <archive>|<ext>|<dir>       write every file with that extension into the folder
		void DebugConvert()
		{
			WatchdogPause pause;
			std::ifstream list(paths::Get().root / L"debug_convert.txt");
			std::string line;
			for (int n = 0; list && std::getline(list, line); ++n)
			{
				if (line.empty())
					continue;
				if (line.starts_with("convertfile "))
				{
					// convertfile <legacy resource>|<output>[|<particle skip bits>]
					std::string rest = line.substr(12);
					const size_t bar = rest.find('|'), bar2 = rest.find('|', bar + 1);
					convert::g_particleDebugSkip = bar2 == std::string::npos ? 0 : std::atoi(rest.c_str() + bar2 + 1);
					if (bar2 != std::string::npos)
						rest.resize(bar2);
					const std::filesystem::path in(std::u8string(rest.begin(), rest.begin() + bar)), outPath(std::u8string(rest.begin() + bar + 1, rest.end()));
					std::ifstream f(in, std::ios::binary);
					convert::Bytes data((std::istreambuf_iterator<char>(f)), {}), converted;
					std::vector<std::string> warnings;
					std::string error;
					const bool ok = convert::ConvertResourceFile(Utf8(in.filename().u8string()), data, converted, warnings, error);
					if (ok)
						std::ofstream(outPath, std::ios::binary).write(reinterpret_cast<const char*>(converted.data()), static_cast<std::streamsize>(converted.size()));
					log::Info("debug convertfile {}: {} {} ({} warning(s))", in.filename().string(), ok ? "ok" : "failed", error, warnings.size());
					for (const auto& w : warnings)
						log::Info("  {}", w);
					continue;
				}
				if (line.starts_with("list ") || line.starts_with("extract ") || line.starts_with("find ") || line.starts_with("extractall "))
				{
					const std::string command = line.substr(0, line.find(' '));
					std::string rest = line.substr(command.size() + 1), archivePath = rest, wanted, out;
					if (command == "extractall")
					{
						const size_t a = rest.find('|'), b = rest.find('|', a + 1);
						archivePath = rest.substr(0, a);
						wanted = rest.substr(a + 1, b - a - 1); // extension, e.g. ".ydd"
						out = rest.substr(b + 1);
					}
					if (command == "find")
					{
						const size_t a = rest.find('|');
						archivePath = rest.substr(0, a);
						wanted = rest.substr(a + 1);
					}
					if (command == "extract")
					{
						const size_t a = rest.find('|'), b = rest.find('|', a + 1);
						archivePath = rest.substr(0, a);
						wanted = rest.substr(a + 1, b - a - 1);
						out = rest.substr(b + 1);
					}
					const std::filesystem::path file(std::u8string(archivePath.begin(), archivePath.end()));
					auto in = std::make_shared<std::ifstream>(file, std::ios::binary);
					std::error_code ec;
					convert::Archive archive;
					std::string error;
					if (!archive.Open(in, 0, std::filesystem::file_size(file, ec), Utf8(file.filename().u8string()), g_decrypt ? &g_decryptor : nullptr, error))
					{
						log::Warn("debug {}: {}", archivePath, error);
						continue;
					}
					std::map<std::string, std::string> versions; // ext:version -> count and example
					std::map<std::string, int> counts;
					WalkArchive(archive, "", [&](const convert::Archive& a, uint32_t i, const std::string& path) {
						const auto& node = a.Nodes()[i];
						if (command == "extractall")
						{
							// "<path fragment>*<extension>" limits it to paths containing the fragment.
							const size_t star = wanted.find('*');
							if (!node.name.ends_with(star == std::string::npos ? wanted : wanted.substr(star + 1)) ||
							    (star != std::string::npos && path.find(wanted.substr(0, star)) == std::string::npos))
								return;
							convert::Bytes data;
							std::string e;
							if (!a.ReadFile(i, data, e))
								log::Warn("debug extractall {}: {}", path, e);
							else
								std::ofstream(std::filesystem::path(std::u8string(out.begin(), out.end())) / std::filesystem::path(std::u8string(node.name.begin(), node.name.end())),
								    std::ios::binary)
								    .write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
							return;
						}
						if (command == "find")
						{
							if (path.find(wanted) != std::string::npos)
								log::Info("debug find {}: {}", file.filename().string(), path);
							return;
						}
						if (command == "extract")
						{
							if (node.name != wanted && !path.ends_with(wanted))
								return;
							convert::Bytes data;
							std::string e;
							if (a.ReadFile(i, data, e))
								std::ofstream(std::filesystem::path(std::u8string(out.begin(), out.end())), std::ios::binary)
								    .write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
							log::Info("debug extract {}: {} bytes {}", path, data.size(), e);
							return;
						}
						if (!node.resource)
							return;
						const auto dot = node.name.rfind('.');
						const std::string key = std::format("{} v{}", dot == std::string::npos ? node.name : node.name.substr(dot),
						    ((node.flags[0] >> 28) << 4) | (node.flags[1] >> 28));
						if (!counts[key]++)
							versions[key] = path;
					});
					for (const auto& [key, count] : counts)
						log::Info("debug list {}: {} x{} (e.g. {})", file.filename().string(), key, count, versions[key]);
					continue;
				}
				const std::filesystem::path file(std::u8string(line.begin(), line.end()));
				const auto r = convert::PreparePack(std::format("debug_{}", n), file.parent_path(), g_decrypt ? &g_decryptor : nullptr, {});
				log::Info("debug convert {}: state {}, {} converted, {} warning(s) {}", line, static_cast<int>(r.state), r.convertedFiles, r.warnings.size(), r.error);
			}
		}

		void HookProcess(void* manager)
		{
			static std::once_flag debug;
			std::call_once(debug, DebugConvert);
			auto packs = Discover();
			static std::once_flag renames;
			std::call_once(renames, [&] { FindRenames(packs); });
			for (Pack& pack : packs)
			{
				if (!pack.enabled)
				{
					log::Info("dlc pack {}: disabled", pack.name);
					continue;
				}
				Prepare(pack);
				if (pack.state == convert::PackState::Failed || pack.empty)
					continue;
				// RAGE paths are UTF-8, use forward slashes and end with a separator.
				pack.path = Utf8(pack.dir.generic_u8string()) + "/";
				// The game processes the list again when a session starts: reuse what we read.
				bool known;
				{
					std::lock_guard lock(g_mutex);
					known = g_entries.contains(Normalize(pack.path + "dlc.rpf"));
				}
				if (!known)
					IndexPack(pack);

				pack.registered = g_register(manager, pack.path.c_str());
				if (pack.registered)
					log::Info("dlc pack {}: added", pack.name);
				else
					log::Error("dlc pack {}: the game rejected {}dlc.rpf", pack.name, pack.path);
			}
			static std::once_flag overrides;
			std::call_once(overrides, [&] {
				FindConflicts(packs);
				if (!g_overrides.Empty())
				{
					log::Info("replacement mods: {} data file entr{} to override", g_overrides.Size(), g_overrides.Size() == 1 ? "y" : "ies");
					datafiles::SetOverrides(g_overrides);
				}
				if (!g_clones.empty())
					datafiles::SetClones(g_clones);
				// Files replaced by name and text entries: the first mod (by name) wins.
				std::vector<convert::NamedFile> named;
				std::map<uint32_t, convert::TextEntry> text;
				for (const Pack& pack : packs)
				{
					if (!pack.replacement || !pack.enabled)
						continue;
					const auto& files = g_replacementFiles[pack.name];
					for (const auto& f : files.named)
					{
						const auto other = std::find_if(named.begin(), named.end(), [&](const convert::NamedFile& n) { return n.name == f.name && n.folder == f.folder; });
						if (other == named.end() || f.merge) // merged files: every mod's entries are used
							named.push_back(f);
						else
						{
							g_conflicts[pack.name].push_back(std::format("{} 也被模組 {} 替換，目前使用 {} 的版本", f.name, other->mod, other->mod));
							log::Warn("replacement mod {}: {} is also replaced by {}; {} is used", pack.name, f.name, other->mod, other->mod);
						}
					}
					int clashes = 0;
					for (const auto& [hash, entry] : files.text)
						if (const auto [it, added] = text.emplace(hash, entry); !added && it->second.text != entry.text)
							++clashes;
					if (clashes)
						g_conflicts[pack.name].push_back(std::format("{} 條文字也被其他模組修改，目前使用名稱排在前面的模組的版本", clashes));
				}
				if (!named.empty() || !text.empty())
				{
					log::Info("replacement mods: {} file(s) replaced by name, {} text entr{}", named.size(), text.size(), text.size() == 1 ? "y" : "ies");
					datafiles::SetFiles(std::move(named), std::move(text));
				}
			});
			// Nothing at all to load (e.g. .oiv packages that only held DLC packs, listed on their own).
			std::erase_if(packs, [](const Pack& p) {
				const auto& files = g_replacementFiles[p.name];
				return p.empty && files.named.empty() && files.text.empty();
			});
			for (Pack& pack : packs)
				if (const auto it = g_conflicts.find(pack.name); it != g_conflicts.end())
					pack.warnings.insert(pack.warnings.end(), it->second.begin(), it->second.end());
			{
				std::lock_guard lock(g_mutex);
				g_packs = std::move(packs);
			}
			g_origProcess(manager);
		}

		bool Hook(uintptr_t at, void* detour, void** original)
		{
			return MH_CreateHook(reinterpret_cast<void*>(at), detour, original) == MH_OK && MH_EnableHook(reinterpret_cast<void*>(at)) == MH_OK;
		}
	}

	bool InstallHooks()
	{
		const auto module = pattern::Module::Main();
		const auto find = [&](const char* sig) { return pattern::Find(module.text, pattern::Pattern::Parse(sig)); };
		const auto reg = find("41 57 41 56 56 57 53 48 81 EC 40 02 00 00 48 89 D6 49 89 CE 0F 57 C0");
		const auto process = find("41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC 48 01 00 00 49 89 CF 48 8D 0D ? ? ? ? B2 01 E8");
		const auto lookup = find("41 57 41 56 41 55 41 54 56 57 53 48 81 EC 20 02 00 00 49 89 D6 48 89 CF 48 8B 01 FF 90 78 01 00 00");
		if (!reg || !process || !lookup)
		{
			log::Error("dlc packs: game code not found ({} {} {}); packs disabled", reg.has_value(), process.has_value(), lookup.has_value());
			return false;
		}
		g_register = reinterpret_cast<RegisterFn>(*reg);
		// Optional: without it, encrypted legacy packs cannot be converted.
		if (const auto decrypt = find("56 57 55 53 48 81 EC 28 02 00 00 44 89 CE 4C 89 C7 89 D5 89 CB 89 8C 24 10 02 00 00"))
		{
			g_decrypt = reinterpret_cast<DecryptFn>(*decrypt);
			g_decryptor = [](uint32_t key, uint8_t* data, uint32_t size) { g_decrypt(kEncryptionNg, key, data, size); };
		}
		else
			log::Warn("dlc packs: decryption not found; encrypted legacy packs cannot be converted");
		// Watchdog loop: mov eax, [pause counter]; test eax, eax; jnz ...; mov rax, [heartbeat]; cmp rsi, rax
		if (const auto watchdog = find("8B 05 ? ? ? ? 85 C0 75 E2 48 8B 05 ? ? ? ? 48 39 C6"))
			g_watchdogPause = reinterpret_cast<volatile long*>(pattern::Rip(*watchdog + 2));
		else
			log::Warn("dlc packs: hang watchdog not found; converting large packs may crash the game");
		if (!Hook(*lookup, reinterpret_cast<void*>(&HookLookup), reinterpret_cast<void**>(&g_origLookup)) ||
		    !Hook(*process, reinterpret_cast<void*>(&HookProcess), reinterpret_cast<void**>(&g_origProcess)))
		{
			log::Error("dlc packs: could not hook the game");
			return false;
		}
		log::Info("dlc packs: hooks installed");
		return true;
	}

	std::vector<Pack> Snapshot()
	{
		std::lock_guard lock(g_mutex);
		return g_packs;
	}

	const convert::Decryptor* Decryptor()
	{
		return g_decrypt ? &g_decryptor : nullptr;
	}
}
