#include "models.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../convert/archive.hpp"
#include "../log.hpp"
#include "../paths.hpp"
#include "../pattern.hpp"
#include "dlcpacks.hpp"

namespace loader::game::models
{
	namespace
	{
		// Archetype hash map node: u32 hash, u16 index at +4, next at +8.
		struct Node
		{
			uint32_t hash;
			uint16_t index;
			uint16_t pad;
			Node* next;
		};
		constexpr size_t kTypeOffset = 0x9D; // model info: type in the low 5 bits

		using LookupFn = void* (*)(uint32_t hash, uint32_t* index);
		LookupFn g_lookup = nullptr;
		Node*** g_buckets = nullptr;
		uint16_t* g_bucketCount = nullptr;

		struct Name
		{
			std::string name, pack;
		};
		std::unordered_map<uint32_t, Name> g_names; // written by the scan thread before g_ready
		std::atomic_bool g_ready = false;
		std::once_flag g_scanStarted;

		// Bumped when the cache format or the scanned extensions change.
		constexpr int kCacheVersion = 3;

		std::string Utf8(const std::filesystem::path& p)
		{
			const auto s = p.u8string();
			return {s.begin(), s.end()};
		}

		bool IsModelFile(const std::string& name)
		{
			return name.ends_with(".yft") || name.ends_with(".ydd");
		}

		void Collect(const convert::Archive& archive, const std::string& pack, std::unordered_map<uint32_t, Name>& out, size_t& nested)
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
					{
						++nested;
						Collect(inner, pack, out, nested);
					}
					continue;
				}
				if (!n.resource || !IsModelFile(n.name))
					continue;
				std::string stem = n.name.substr(0, n.name.size() - 4);
				std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (stem.ends_with("_hi")) // high detail vehicle part of the same model
					continue;
				// Mod packs win over the game: a pack may replace a model.
				auto [it, added] = out.try_emplace(convert::Joaat(stem), Name{stem, pack});
				if (!added && !pack.empty())
					it->second = {stem, pack};
			}
		}

		// Archives to read, with the mod pack each one belongs to ("" = the game).
		std::vector<std::pair<std::filesystem::path, std::string>> Sources()
		{
			std::vector<std::pair<std::filesystem::path, std::string>> out;
			const auto game = paths::Get().gameDir;
			std::error_code ec;
			for (auto it = std::filesystem::recursive_directory_iterator(game, std::filesystem::directory_options::skip_permission_denied, ec);
			     it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
			{
				if (ec)
					break;
				const auto& path = it->path();
				if (it->is_directory(ec) && path == paths::Get().root)
				{
					it.disable_recursion_pending(); // ModLoader: mods are handled below, the cache holds copies
					continue;
				}
				if (it->is_regular_file(ec) && path.extension() == L".rpf")
					out.emplace_back(path, "");
			}
			for (const auto& pack : dlcpacks::Snapshot())
				if (pack.enabled)
					out.emplace_back(pack.source / L"dlc.rpf", pack.name);
			std::sort(out.begin(), out.end());
			return out;
		}

		// Size and time of every source: the cache is valid while these match.
		std::string Signature(const std::vector<std::pair<std::filesystem::path, std::string>>& sources)
		{
			uint64_t h = 1469598103934665603ull;
			const auto mix = [&](const void* p, size_t n) {
				for (size_t i = 0; i < n; ++i)
					h = (h ^ static_cast<const uint8_t*>(p)[i]) * 1099511628211ull;
			};
			for (const auto& [path, pack] : sources)
			{
				std::error_code ec;
				const auto size = std::filesystem::file_size(path, ec);
				const auto time = std::filesystem::last_write_time(path, ec).time_since_epoch().count();
				const auto s = Utf8(path) + "|" + pack;
				mix(s.data(), s.size());
				mix(&size, sizeof(size));
				mix(&time, sizeof(time));
			}
			return std::format("v{} {:016x}", kCacheVersion, h);
		}

		std::filesystem::path CacheFile()
		{
			return paths::Get().root / L"cache" / L"model_names.txt";
		}

		bool ReadCache(const std::string& signature, std::unordered_map<uint32_t, Name>& out)
		{
			std::ifstream in(CacheFile());
			std::string line;
			if (!in || !std::getline(in, line) || line != signature)
				return false;
			while (std::getline(in, line))
				if (const auto tab = line.find('\t'); tab != std::string::npos)
				{
					std::string name = line.substr(0, tab);
					const uint32_t hash = convert::Joaat(name); // before the move below
					out[hash] = {std::move(name), line.substr(tab + 1)};
				}
			return true;
		}

		void WriteCache(const std::string& signature, const std::unordered_map<uint32_t, Name>& names)
		{
			std::error_code ec;
			std::filesystem::create_directories(CacheFile().parent_path(), ec);
			std::ofstream out(CacheFile(), std::ios::trunc);
			out << signature << "\n";
			for (const auto& [hash, n] : names)
				out << n.name << "\t" << n.pack << "\n";
		}

		void Scan()
		{
			const auto start = std::chrono::steady_clock::now();
			const auto sources = Sources();
			const auto signature = Signature(sources);
			std::unordered_map<uint32_t, Name> names;
			if (ReadCache(signature, names))
			{
				g_names = std::move(names);
				g_ready = true;
				log::Info("models: {} names from the cache", g_names.size());
				return;
			}
			const convert::Decryptor* decrypt = dlcpacks::Decryptor();
			size_t archives = 0, nested = 0, failed = 0;
			for (const auto& [path, pack] : sources)
			{
				std::error_code ec;
				auto in = std::make_shared<std::ifstream>(path, std::ios::binary);
				convert::Archive archive;
				std::string error;
				if (!*in || !archive.Open(in, 0, std::filesystem::file_size(path, ec), Utf8(path.filename()), decrypt, error))
				{
					++failed;
					log::Debug("models: {} not read: {}", Utf8(path), error);
					continue;
				}
				++archives;
				Collect(archive, pack, names, nested);
			}
			WriteCache(signature, names);
			g_names = std::move(names);
			g_ready = true;
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
			log::Info("models: {} names from {} archives ({} nested, {} unreadable) in {} ms", g_names.size(), archives, nested, failed, ms);
		}
	}

	bool Install()
	{
		const auto module = pattern::Module::Main();
		// GetModelInfo(hash, &index): movzx r10d, word [bucket count]; ...; mov r11, [buckets]; ...; div r10d
		const auto at = pattern::Find(module.text,
		    pattern::Pattern::Parse("44 0F B7 15 ? ? ? ? 45 85 D2 74 ? 49 89 D0 4C 8B 1D ? ? ? ? 45 31 C9 89 C8 31 D2 41 F7 F2"));
		if (!at)
		{
			log::Error("models: model table not found; model lists unavailable");
			return false;
		}
		g_lookup = reinterpret_cast<LookupFn>(*at);
		g_bucketCount = reinterpret_cast<uint16_t*>(pattern::Rip(*at + 4));
		g_buckets = reinterpret_cast<Node***>(pattern::Rip(*at + 0x13));
		log::Info("models: table at +{:#x}", reinterpret_cast<uintptr_t>(g_buckets) - module.base);
		return true;
	}

	void StartNameScan()
	{
		std::call_once(g_scanStarted, [] { std::thread(Scan).detach(); });
	}

	bool NamesReady()
	{
		return g_ready;
	}

	int32_t Enumerate(Type type, const std::function<void(uint32_t, const std::string&, const std::string&)>& visit)
	{
		if (!g_lookup || !g_ready || !*g_buckets)
			return -1;
		static const Name kUnknown;
		int32_t count = 0;
		Node** buckets = *g_buckets;
		for (uint32_t b = 0; b < *g_bucketCount; ++b)
			for (const Node* n = buckets[b]; n; n = n->next)
			{
				const auto* info = static_cast<const uint8_t*>(g_lookup(n->hash, nullptr));
				if (!info || (info[kTypeOffset] & 0x1F) != static_cast<uint8_t>(type))
					continue;
				const auto it = g_names.find(n->hash);
				const Name& name = it != g_names.end() ? it->second : kUnknown;
				visit(n->hash, name.name, name.pack);
				++count;
			}
		return count;
	}
}
