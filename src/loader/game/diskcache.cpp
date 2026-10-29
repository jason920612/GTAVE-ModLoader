#include "diskcache.hpp"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <format>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include <MinHook.h>

#include "../config.hpp"
#include "../log.hpp"
#include "../paths.hpp"

namespace loader::game::diskcache
{
	namespace
	{
		// dstorage.h 1.2: DStorageSetConfiguration1's settings (all 0: DirectStorage's defaults). Set before the first
		// DStorageGetFactory - the only one of its functions the game imports, so it sets none of its own.
		struct Configuration1
		{
			uint32_t numSubmitThreads;
			int32_t numBuiltInCpuDecompressionThreads;
			BOOL forceMappingLayer;
			BOOL disableBypassIO;
			BOOL disableTelemetry;
			BOOL disableGpuDecompressionMetacommand;
			BOOL disableGpuDecompression;
			BOOL forceFileBuffering;
		};
		using GetFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
		using SetConfiguration1Fn = HRESULT(WINAPI*)(const Configuration1*);
		GetFactoryFn g_getFactory = nullptr;
		SetConfiguration1Fn g_setConfiguration1 = nullptr;
		std::atomic<bool> g_configured = false;

		std::mutex g_mutex;
		std::string g_buffering = "not started"; // (DirectStorage and the file cache: what it is to do or did, and why)
		std::map<wchar_t, bool> g_drives; // (drive letter: seeks)

		HRESULT WINAPI GetFactory(REFIID riid, void** out)
		{
			if (!g_configured.exchange(true))
			{
				Configuration1 c{};
				c.disableBypassIO = TRUE; // (reads through the cache go down the file system's stack: no bypass)
				c.forceFileBuffering = TRUE;
				const HRESULT hr = g_setConfiguration1(&c);
				std::lock_guard lock(g_mutex);
				g_buffering = SUCCEEDED(hr) ? "reads through the file cache" : std::format("refused to read through the file cache ({:#x})", static_cast<uint32_t>(hr));
				log::Info("disk cache: the game uses DirectStorage: it {}", g_buffering);
			}
			return g_getFactory(riid, out);
		}

		// A pack held in memory: mapped whole, its pages read in a piece at a time and locked there.
		struct Held
		{
			std::filesystem::path file;
			HANDLE handle = INVALID_HANDLE_VALUE, mapping = nullptr;
			uint8_t* view = nullptr;
			uint64_t size = 0;
			std::atomic<uint64_t> read = 0, locked = 0; // (bytes in, bytes of them locked)
			std::atomic<double> seconds = 0;
			bool failed = false;
		};
		std::vector<std::unique_ptr<Held>> g_held; // (the worker's; changed and read under g_mutex)
		std::vector<std::filesystem::path> g_wanted;
		uint64_t g_version = 0; // (of g_wanted)
		std::condition_variable g_wake;
		uint64_t g_budget = 0; // bytes; 0: none
		bool g_lockRefused = false;
		SIZE_T g_baseMin = 0, g_baseMax = 0; // (the working set's limits before any was locked)

		std::string Name(const std::filesystem::path& pack)
		{
			return pack.parent_path().filename().string();
		}

		// the lands' cache: the folders to copy there, one at a time; what it holds (for the report)
		std::mutex g_fillMutex;
		std::condition_variable g_fillWake;
		std::deque<std::filesystem::path> g_fillQueue;
		std::set<std::wstring> g_fillAsked; // (once a run each)
		std::string g_fillState = "none";

		uint64_t FolderSize(const std::filesystem::path& folder)
		{
			uint64_t total = 0;
			std::error_code ec;
			for (const auto& e : std::filesystem::directory_iterator(folder, ec))
			{
				std::error_code e2;
				if (e.is_regular_file(e2))
					total += e.file_size(e2);
			}
			return total;
		}

		// Copies a land's folder into the cache: its files into <name>.part, the pack last, their times the land's; the
		// old copy gone, the new one named. Room made first: the least lately used copies let go (one in use stays).
		void CopyLand(const std::filesystem::path& folder)
		{
			const std::filesystem::path root = paths::CacheRoot();
			std::error_code ec;
			if (root.empty() || paths::Cached(folder) != folder || std::filesystem::exists(folder / L"dlc.rpf.new", ec))
				return;
			const std::string name = folder.filename().string();
			const uint64_t need = FolderSize(folder), budget = static_cast<uint64_t>(std::max(0, config::Get().landCacheGB)) << 30;
			std::filesystem::create_directories(root, ec);
			// (room: the copies by their last use, oldest first)
			std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> copies;
			uint64_t used = 0;
			for (const auto& e : std::filesystem::directory_iterator(root, ec))
			{
				std::error_code e2;
				if (!e.is_directory(e2) || e.path().filename() == folder.filename())
					continue;
				used += FolderSize(e.path());
				auto when = std::filesystem::last_write_time(e.path() / L"used", e2);
				if (e2)
					when = std::filesystem::last_write_time(e.path(), e2);
				copies.push_back({when, e.path()});
			}
			std::sort(copies.begin(), copies.end());
			for (const auto& [when, path] : copies)
			{
				if (used + need <= budget)
					break;
				const uint64_t size = FolderSize(path);
				std::error_code e2;
				std::filesystem::remove_all(path, e2);
				if (!e2)
				{
					used -= std::min(used, size);
					log::Info("disk cache: {} let go from the lands' cache ({} MB)", path.filename().string(), size >> 20);
				}
			}
			if (used + need > budget)
			{
				log::Warn("disk cache: no room in the lands' cache for {} ({} MB; {} of {} MB used)", name, need >> 20, used >> 20, budget >> 20);
				return;
			}
			const auto t0 = std::chrono::steady_clock::now();
			const std::filesystem::path part = root / (folder.filename().wstring() + L".part"), copy = root / folder.filename();
			std::filesystem::remove_all(part, ec);
			std::filesystem::create_directories(part, ec);
			std::vector<std::filesystem::path> files;
			for (const auto& e : std::filesystem::directory_iterator(folder, ec))
			{
				std::error_code e2;
				if (e.is_regular_file(e2) && e.path().filename() != L"dlc.rpf")
					files.push_back(e.path());
			}
			files.push_back(folder / L"dlc.rpf");
			for (const auto& file : files)
			{
				const std::filesystem::path to = part / file.filename();
				std::error_code e2;
				if (!CopyFileExW(file.c_str(), to.c_str(), nullptr, nullptr, nullptr, 0))
				{
					log::Warn("disk cache: {} not copied into the lands' cache (error {})", (folder.filename() / file.filename()).string(), GetLastError());
					std::filesystem::remove_all(part, e2);
					return;
				}
				std::filesystem::last_write_time(to, std::filesystem::last_write_time(file, e2), e2);
			}
			// (a pack rebuilt meanwhile: this copy is of the old one)
			for (const auto& file : files)
			{
				std::error_code e2, e3;
				if (std::filesystem::last_write_time(file, e2) != std::filesystem::last_write_time(part / file.filename(), e3) || e2 || e3)
				{
					std::filesystem::remove_all(part, e2);
					return;
				}
			}
			std::filesystem::remove_all(copy, ec);
			std::filesystem::rename(part, copy, ec);
			if (ec)
			{
				log::Warn("disk cache: {}'s copy not named in the lands' cache ({})", name, ec.message());
				std::filesystem::remove_all(part, ec);
				return;
			}
			const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			log::Info("disk cache: {} ({} MB) copied into the lands' cache in {:.1f} s ({:.0f} MB/s): read from there at the next start", name, need >> 20,
			    seconds, seconds > 0 ? (need >> 20) / seconds : 0.0);
		}

		void Fill()
		{
			// (a copy left half-made by an earlier run: gone)
			{
				const std::filesystem::path root = paths::CacheRoot();
				std::error_code ec;
				for (const auto& e : std::filesystem::directory_iterator(root, ec))
					if (e.path().extension() == L".part")
					{
						std::error_code e2;
						std::filesystem::remove_all(e.path(), e2);
					}
			}
			SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
			for (;;)
			{
				std::filesystem::path folder;
				{
					std::unique_lock lock(g_fillMutex);
					g_fillWake.wait(lock, [] { return !g_fillQueue.empty(); });
					folder = g_fillQueue.front();
					g_fillQueue.pop_front();
					g_fillState = "copying " + folder.filename().string();
				}
				CopyLand(folder);
				std::lock_guard lock(g_fillMutex);
				g_fillState = "idle";
			}
		}

		// a land whose pack is all in memory: to the cache (once a run)
		void ToCache(const std::filesystem::path& pack)
		{
			if (paths::CacheRoot().empty())
				return;
			static std::once_flag started;
			std::call_once(started, [] { std::thread(Fill).detach(); });
			std::lock_guard lock(g_fillMutex);
			if (!g_fillAsked.insert(pack.parent_path().wstring()).second)
				return;
			g_fillQueue.push_back(pack.parent_path());
			g_fillWake.notify_all();
		}

		// The working set's minimum kept over what is locked (VirtualLock counts against it), else nothing more is locked.
		bool Room(uint64_t locked)
		{
			if (!g_baseMin)
			{
				DWORD flags = 0;
				GetProcessWorkingSetSizeEx(GetCurrentProcess(), &g_baseMin, &g_baseMax, &flags);
			}
			const SIZE_T min = g_baseMin + static_cast<SIZE_T>(locked) + (64u << 20), max = std::max<SIZE_T>(g_baseMax, min + (256u << 20));
			return SetProcessWorkingSetSizeEx(GetCurrentProcess(), min, max, QUOTA_LIMITS_HARDWS_MIN_DISABLE | QUOTA_LIMITS_HARDWS_MAX_DISABLE) != 0;
		}

		uint64_t LockedTotal()
		{
			uint64_t total = 0;
			for (const auto& h : g_held)
				total += h->locked;
			return total;
		}

		void LetGo(Held& h)
		{
			if (h.view)
			{
				if (h.locked)
					VirtualUnlock(h.view, static_cast<SIZE_T>(h.locked));
				UnmapViewOfFile(h.view);
			}
			if (h.mapping)
				CloseHandle(h.mapping);
			if (h.handle != INVALID_HANDLE_VALUE)
				CloseHandle(h.handle);
			h.view = nullptr, h.mapping = nullptr, h.handle = INVALID_HANDLE_VALUE, h.locked = 0;
		}

		bool Changed(uint64_t seen)
		{
			std::lock_guard lock(g_mutex);
			return g_version != seen;
		}

		// Reads a held pack in from where it got to, a piece at a time (each prefetched in one go - the disk reads on in a
		// row - then locked), till it is all in or what is wanted changes. The first wanted at the game's own priority, the
		// others behind its reads (the thread's background mode: its I/O the lowest).
		void ReadIn(Held& h, bool first, uint64_t seen)
		{
			constexpr uint64_t kPiece = 32ull << 20;
			if (!first)
				SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
			const auto t0 = std::chrono::steady_clock::now();
			const double before = h.seconds;
			while (h.read < h.size && !Changed(seen))
			{
				const uint64_t at = h.read, n = std::min(kPiece, h.size - at);
				WIN32_MEMORY_RANGE_ENTRY range{h.view + at, static_cast<SIZE_T>(n)};
				PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);
				bool locked = false;
				if (!g_lockRefused)
				{
					std::lock_guard lock(g_mutex);
					locked = Room(LockedTotal() + n) && VirtualLock(h.view + at, static_cast<SIZE_T>(n));
					if (!locked)
					{
						g_lockRefused = true;
						log::Warn("disk cache: memory for {} could not be locked (error {}); its pages read in, Windows' to keep or not", Name(h.file), GetLastError());
					}
				}
				if (locked)
					h.locked += n;
				else
					for (uint64_t k = 0; k < n; k += 4096) // (each page touched: read in, in the working set)
						static_cast<void>(*reinterpret_cast<volatile const uint8_t*>(h.view + at + k));
				h.read += n;
				h.seconds = before + std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			}
			if (!first)
				SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
			if (h.read == h.size)
				log::Info("disk cache: {} ({} MB) read in in {:.1f} s ({:.0f} MB/s){}", Name(h.file), h.size >> 20, h.seconds.load(),
				    h.seconds > 0 ? (h.size >> 20) / h.seconds : 0.0, h.locked ? ", held in memory" : "");
		}

		bool Open(Held& h)
		{
			h.handle = CreateFileW(h.file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h.handle == INVALID_HANDLE_VALUE)
				return false;
			h.mapping = CreateFileMappingW(h.handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
			h.view = h.mapping ? static_cast<uint8_t*>(MapViewOfFile(h.mapping, FILE_MAP_READ, 0, 0, 0)) : nullptr;
			return h.view != nullptr;
		}

		void Work()
		{
			uint64_t seen = ~0ull;
			for (;;)
			{
				std::vector<std::filesystem::path> wanted;
				{
					std::unique_lock lock(g_mutex);
					g_wake.wait(lock, [&] { return g_version != seen; });
					seen = g_version;
					wanted = g_wanted;
				}
				// the packs to hold: the wanted ones on a drive that seeks, the most wanted first, as many as the budget
				// takes (one too big for what is left passed over: a later one may fit)
				std::vector<std::filesystem::path> plan;
				uint64_t total = 0;
				for (const auto& file : wanted)
				{
					std::error_code ec;
					const uint64_t size = std::filesystem::file_size(file, ec);
					if (ec || !Seeks(file) || total + size > g_budget)
						continue;
					total += size;
					plan.push_back(file);
				}
				{
					std::lock_guard lock(g_mutex);
					for (auto it = g_held.begin(); it != g_held.end();)
						if (std::find(plan.begin(), plan.end(), (*it)->file) == plan.end())
						{
							log::Info("disk cache: {} let go ({} MB)", Name((*it)->file), (*it)->read >> 20);
							LetGo(**it);
							it = g_held.erase(it);
						}
						else
							++it;
					Room(LockedTotal());
				}
				for (size_t k = 0; k < plan.size() && !Changed(seen); ++k)
				{
					Held* h = nullptr;
					{
						std::lock_guard lock(g_mutex);
						for (const auto& held : g_held)
							if (held->file == plan[k])
								h = held.get();
						if (!h)
						{
							auto added = std::make_unique<Held>();
							added->file = plan[k];
							std::error_code ec;
							added->size = std::filesystem::file_size(plan[k], ec);
							h = added.get();
							g_held.push_back(std::move(added));
							if (!Open(*h))
							{
								h->failed = true;
								log::Warn("disk cache: {} cannot be mapped (error {})", plan[k].string(), GetLastError());
								LetGo(*h);
							}
						}
					}
					if (!h->failed && h->read < h->size)
						ReadIn(*h, k == 0, seen);
					if (!h->failed && h->read == h->size)
						ToCache(h->file);
				}
			}
		}
	}

	bool Seeks(const std::filesystem::path& path)
	{
		std::error_code ec;
		const std::wstring root = std::filesystem::absolute(path, ec).root_name().wstring();
		if (root.size() != 2 || root[1] != L':')
			return false;
		const wchar_t letter = static_cast<wchar_t>(std::towupper(root[0]));
		{
			std::lock_guard lock(g_mutex);
			if (const auto it = g_drives.find(letter); it != g_drives.end())
				return it->second;
		}
		bool seeks = false;
		const std::wstring volume = std::wstring(L"\\\\.\\") + letter + L":";
		if (HANDLE h = CreateFileW(volume.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr); h != INVALID_HANDLE_VALUE)
		{
			STORAGE_PROPERTY_QUERY query{StorageDeviceSeekPenaltyProperty, PropertyStandardQuery, {0}};
			DEVICE_SEEK_PENALTY_DESCRIPTOR seek{};
			DWORD got = 0;
			seeks = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &seek, sizeof(seek), &got, nullptr) && got >= sizeof(seek) &&
			        seek.IncursSeekPenalty;
			CloseHandle(h);
		}
		std::lock_guard lock(g_mutex);
		g_drives[letter] = seeks;
		log::Info("disk cache: drive {}: {}", static_cast<char>(letter), seeks ? "seeks (a hard disk)" : "does not seek");
		return seeks;
	}

	void Start()
	{
		const auto& c = config::Get();
		// the budget: diskCacheGB, or (-1) an eighth of the memory, at most 32 GB
		MEMORYSTATUSEX memory{sizeof(memory)};
		GlobalMemoryStatusEx(&memory);
		g_budget = c.diskCacheGB < 0 ? std::min<uint64_t>(memory.ullTotalPhys / 8, 32ull << 30) : static_cast<uint64_t>(c.diskCacheGB) << 30;
		// whether a drive the game reads from seeks
		std::string slow;
		std::vector<std::filesystem::path> places{paths::Get().gameDir, paths::Get().mods};
		for (const auto& dir : paths::LandRoots())
			places.push_back(dir);
		for (const auto& place : places)
			if (Seeks(place))
				slow += (slow.empty() ? "" : ", ") + place.string();
		const bool buffered = c.bufferedIo == "on" || (c.bufferedIo == "auto" && !slow.empty());
		{
			std::lock_guard lock(g_mutex);
			g_buffering = std::string(buffered ? "to read through the file cache" : "as the game has it") + " (" +
			              (c.bufferedIo == "on" ? std::string("bufferedIo on") : !slow.empty() ? "on a hard disk: " + slow : "no hard disk") + ")";
		}
		log::Info("disk cache: {} MB of memory for the lands' packs on a hard disk; DirectStorage {}", g_budget >> 20, g_buffering);
		if (!buffered)
			return;
		HMODULE ds = GetModuleHandleW(L"dstorage.dll");
		if (!ds)
			ds = LoadLibraryW((paths::Get().gameDir / L"dstorage.dll").c_str());
		auto* getFactory = ds ? reinterpret_cast<void*>(GetProcAddress(ds, "DStorageGetFactory")) : nullptr;
		g_setConfiguration1 = ds ? reinterpret_cast<SetConfiguration1Fn>(GetProcAddress(ds, "DStorageSetConfiguration1")) : nullptr;
		if (!getFactory || !g_setConfiguration1 ||
		    MH_CreateHook(getFactory, reinterpret_cast<void*>(&GetFactory), reinterpret_cast<void**>(&g_getFactory)) != MH_OK || MH_EnableHook(getFactory) != MH_OK)
		{
			log::Warn("disk cache: DirectStorage {} - should the game use it, its reads would go past the file cache",
			    ds ? "has no DStorageSetConfiguration1 (before 1.2)" : "not found");
			std::lock_guard lock(g_mutex);
			g_buffering = "past the cache, not to be changed";
		}
	}

	void Want(const std::vector<std::filesystem::path>& packs)
	{
		if (!g_budget)
			return;
		static std::once_flag started;
		std::call_once(started, [] { std::thread(Work).detach(); });
		std::lock_guard lock(g_mutex);
		if (packs == g_wanted)
			return;
		g_wanted = packs;
		++g_version;
		g_wake.notify_all();
	}

	std::string Report()
	{
		std::lock_guard lock(g_mutex);
		std::string out = std::format("ok DirectStorage {}: {} | budget {} MB, locked {} MB{}", g_configured ? "used by the game" : "not used by the game (yet)",
		    g_buffering, g_budget >> 20, LockedTotal() >> 20, g_lockRefused ? " (locking refused: read in only)" : "");
		for (const auto& [letter, seeks] : g_drives)
			out += std::format(" | drive {}: {}", static_cast<char>(letter), seeks ? "seeks" : "no seek");
		for (const auto& file : g_wanted)
			out += std::format(" | wanted {}", Name(file));
		for (const auto& h : g_held)
			out += std::format(" | held {}: {}/{} MB, {} MB locked, {:.1f} s{}", Name(h->file), h->read >> 20, h->size >> 20, h->locked >> 20, h->seconds.load(),
			    h->failed ? " (failed)" : "");
		if (const auto root = paths::CacheRoot(); !root.empty())
		{
			std::lock_guard fill(g_fillMutex);
			out += std::format(" | lands' cache {}: {}, {} waiting", root.string(), g_fillState, g_fillQueue.size());
		}
		return out;
	}
}
