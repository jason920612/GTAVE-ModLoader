#include "dlss.hpp"

#include <Windows.h>
#include <ShlObj.h>
#include <Shlwapi.h>
#include <SoftPub.h>
#include <WinTrust.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <MinHook.h>

#include "../config.hpp"
#include "../graphics/streamline.hpp"
#include "../log.hpp"
#include "../paths.hpp"

namespace loader::game::dlss
{
	namespace
	{
		bool g_trace = false; // loader.json "debugDlssTrace": log NGX's file calls and the DLL loads (research §51)

		std::wstring Lower(std::wstring_view s)
		{
			std::wstring out(s);
			for (auto& c : out)
				c = static_cast<wchar_t>(std::towlower(c));
			return out;
		}

		std::string Utf8(std::wstring_view w)
		{
			if (w.empty())
				return {};
			const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
			std::string out(n, '\0');
			WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
			return out;
		}

		std::wstring FromAnsi(const char* s)
		{
			const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
			std::wstring out(n > 1 ? n - 1 : 0, L'\0');
			if (n > 1)
				MultiByteToWideChar(CP_ACP, 0, s, -1, out.data(), n);
			return out;
		}

		std::string ToAnsi(const std::wstring& w)
		{
			const int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
			std::string out(n > 1 ? n - 1 : 0, '\0');
			if (n > 1)
				WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
			return out;
		}

		// Lower case with backslashes, no repeated separators (NGX asks for "<game folder>\/nvngx_dlss.dll").
		std::wstring Normalize(std::wstring_view path)
		{
			std::wstring out;
			out.reserve(path.size());
			for (size_t i = 0; i < path.size(); ++i)
			{
				const wchar_t c = path[i] == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(path[i]));
				if (c == L'\\' && i > 1 && !out.empty() && out.back() == L'\\')
					continue;
				out += c;
			}
			return out;
		}

		std::wstring_view FileName(std::wstring_view path)
		{
			const auto slash = path.find_last_of(L"\\/");
			return slash == std::wstring_view::npos ? path : path.substr(slash + 1);
		}

		struct Version
		{
			uint16_t major = 0, minor = 0, build = 0, revision = 0;
			auto operator<=>(const Version&) const = default;
			bool Valid() const { return major != 0; }
			uint64_t Packed() const { return uint64_t{major} << 48 | uint64_t{minor} << 32 | uint64_t{build} << 16 | revision; }
			static Version Unpack(uint64_t v)
			{
				return {static_cast<uint16_t>(v >> 48), static_cast<uint16_t>(v >> 32), static_cast<uint16_t>(v >> 16), static_cast<uint16_t>(v)};
			}
			std::string Text() const { return Valid() ? std::format("{}.{}.{}", major, minor, build) : std::string(); }
		};
		constexpr Version kModelK{310, 2, 0, 0};  // DLSS 4 transformer model (preset K)
		constexpr Version kModelsLM{310, 5, 0, 0}; // DLSS 4.5 second-generation transformer models (presets L, M)

		Version FileVersion(const std::wstring& path)
		{
			DWORD handle = 0;
			const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
			if (!size)
				return {};
			std::vector<uint8_t> data(size);
			VS_FIXEDFILEINFO* info = nullptr;
			UINT length = 0;
			if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) || !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info)
				return {};
			return {HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS)};
		}

		// Valid Authenticode signature by NVIDIA Corporation (every user may write to NVIDIA's store folder). Local
		// check only: no revocation or network lookups.
		bool SignedByNvidia(const std::wstring& path)
		{
			WINTRUST_FILE_INFO file{};
			file.cbStruct = sizeof(file);
			file.pcwszFilePath = path.c_str();
			WINTRUST_DATA data{};
			data.cbStruct = sizeof(data);
			data.dwUIChoice = WTD_UI_NONE;
			data.fdwRevocationChecks = WTD_REVOKE_NONE;
			data.dwUnionChoice = WTD_CHOICE_FILE;
			data.pFile = &file;
			data.dwStateAction = WTD_STATEACTION_VERIFY;
			data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
			GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
			const auto window = static_cast<HWND>(INVALID_HANDLE_VALUE);
			bool nvidia = false;
			if (WinVerifyTrust(window, &action, &data) == ERROR_SUCCESS)
			{
				CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(data.hWVTStateData);
				CRYPT_PROVIDER_SGNR* signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
				const CRYPT_PROVIDER_CERT* cert = signer ? WTHelperGetProvCertFromChain(signer, 0) : nullptr;
				if (cert && cert->pCert)
				{
					wchar_t name[256] = {};
					CertGetNameStringW(cert->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 256);
					nvidia = std::wstring_view(name) == L"NVIDIA Corporation";
				}
			}
			data.dwStateAction = WTD_STATEACTION_CLOSE;
			WinVerifyTrust(window, &action, &data);
			return nvidia;
		}

		// ---- the game's DLSS DLLs and NVIDIA's newest copies --------------------------------------------

		struct Dll
		{
			const wchar_t* storeFolder; // ProgramData\NVIDIA\NGX\models\<storeFolder>
			const wchar_t* name;        // in the game folder, lower case
			const char* label;
			// Set once by Discover, before g_checked.
			std::wstring gamePath, gamePathLower;
			Version game;
			std::wstring storePath, storePathLower; // newest NVIDIA-signed file ("" = none)
			std::string storePathAnsi;
			Version store;
			bool redirect = false; // NGX gets storePath when it asks for the game's copy
			// Runtime.
			std::atomic<HMODULE> module = nullptr; // storePath, loaded through the redirect
			std::atomic<uint64_t> loaded = 0;      // packed version of the copy NGX loaded
			std::atomic_bool loadedFromStore = false;
			std::atomic_bool announced = false;
		};
		Dll g_dlls[] = {
			{L"dlss", L"nvngx_dlss.dll", "DLSS Super Resolution"},
			{L"dlssg", L"nvngx_dlssg.dll", "DLSS Frame Generation"},
		};
		Dll& g_sr = g_dlls[0];
		Dll& g_fg = g_dlls[1];
		std::atomic_bool g_checked = false;
		bool g_upgrade = false; // dlssUpgrade at start

		// The newest NVIDIA-signed file in models\<folder>\versions\<n>\files (folders are named
		// (major << 16) | (minor << 8) | build, so the newest is the largest number).
		void FindNewest(Dll& d, const std::filesystem::path& models)
		{
			std::error_code ec;
			std::vector<std::pair<uint64_t, std::filesystem::path>> folders;
			for (std::filesystem::directory_iterator it(models / d.storeFolder / L"versions", ec), end; !ec && it != end; it.increment(ec))
			{
				const std::wstring name = it->path().filename().wstring();
				if (!name.empty() && name.size() < 16 && std::all_of(name.begin(), name.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
					folders.emplace_back(std::wcstoull(name.c_str(), nullptr, 10), it->path());
			}
			std::sort(folders.begin(), folders.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
			for (const auto& [number, folder] : folders)
			{
				for (std::filesystem::directory_iterator it(folder / L"files", ec), end; !ec && it != end; it.increment(ec))
				{
					const std::wstring path = it->path().wstring();
					if (Lower(it->path().extension().wstring()) != L".bin")
						continue;
					const Version v = FileVersion(path);
					if (!v.Valid() || v <= d.store)
						continue;
					if (!SignedByNvidia(path))
					{
						log::Warn("dlss: {} is not signed by NVIDIA; not used", Utf8(path));
						continue;
					}
					d.store = v;
					d.storePath = path;
				}
				if (d.store.Valid())
					return;
			}
		}

		// On a worker at startup; NGX loads DLSS ~12 s later.
		void Discover()
		{
			try
			{
				std::filesystem::path models;
				PWSTR programData = nullptr;
				if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &programData)))
					models = std::filesystem::path(programData) / L"NVIDIA" / L"NGX" / L"models";
				CoTaskMemFree(programData);
				for (auto& d : g_dlls)
				{
					d.gamePath = (paths::Get().gameDir / d.name).wstring();
					d.gamePathLower = Normalize(d.gamePath);
					d.game = FileVersion(d.gamePath);
					if (!models.empty())
						FindNewest(d, models);
					d.storePathLower = Normalize(d.storePath);
					d.storePathAnsi = ToAnsi(d.storePath);
					d.redirect = g_upgrade && d.game.Valid() && d.store > d.game;
					log::Info("dlss: {}: game {}, NVIDIA store {}{}", d.label, d.game.Valid() ? d.game.Text() : "none",
					    d.store.Valid() ? std::format("{} ({})", d.store.Text(), Utf8(d.storePath)) : "none",
					    d.redirect ? " -> NGX gets the store copy" : g_upgrade ? "" : " (dlssUpgrade off)");
				}
			}
			catch (const std::exception& e)
			{
				log::Error("dlss: checking NVIDIA's DLSS store failed: {}", e.what());
				for (auto& d : g_dlls)
					d.redirect = false;
			}
			g_checked.store(true, std::memory_order_release);
		}

		// ---- the NGX core's file calls (its import table points here) ---------------------------------

		// The DLL whose store copy NGX gets instead of `path` (the game's copy, by path or bare name), or nullptr.
		// Other copies, such as the driver's own nvngx_dlssg.dll, stay as they are.
		Dll* Target(std::wstring_view path)
		{
			if (path.empty() || !g_checked.load(std::memory_order_acquire))
				return nullptr;
			const std::wstring lower = Normalize(path);
			const std::wstring_view file = FileName(lower);
			for (auto& d : g_dlls)
				if (d.redirect && file == d.name && (lower == d.gamePathLower || file.size() == lower.size()))
					return &d;
			return nullptr;
		}

		// The DLL loaded through the redirect that `name` (a bare file name or the game's path) asks for.
		Dll* Redirected(const wchar_t* name)
		{
			if (!name || !g_checked.load(std::memory_order_acquire))
				return nullptr;
			const std::wstring lower = Normalize(name);
			const std::wstring_view file = FileName(lower);
			for (auto& d : g_dlls)
			{
				const std::wstring_view dll = d.name;
				if (d.module.load() && (lower == d.gamePathLower || (file.size() == lower.size() && (file == dll || file == dll.substr(0, dll.size() - 4)))))
					return &d;
			}
			return nullptr;
		}

		std::string ModuleAt(const void* address)
		{
			HMODULE m = nullptr;
			wchar_t path[MAX_PATH] = {};
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(address), &m) ||
			    !GetModuleFileNameW(m, path, MAX_PATH))
				return "?";
			return std::format("{}+{:#x}", Utf8(FileName(path)), reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(m));
		}

		// With "debugDlssTrace": NGX's calls that name an NGX / DLSS file, and every DLL it loads.
		void Trace(const char* api, std::wstring_view path, const void* caller, const Dll* to)
		{
			if (!g_trace)
				return;
			const std::wstring lower = Lower(path);
			if (!to && lower.find(L"nvngx") == std::wstring::npos && lower.find(L"\\ngx\\") == std::wstring::npos && std::strncmp(api, "LoadLibrary", 11) != 0)
				return;
			log::Info("dlss: {} {}({}){}", ModuleAt(caller), api, Utf8(path), to ? " -> " + Utf8(to->storePath) : "");
		}

		void LoadedThroughRedirect(Dll& d, HMODULE module, DWORD flags)
		{
			if (flags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE))
				return;
			d.module = module;
			if (!d.announced.exchange(true))
				log::Info("dlss: NGX loads {} {} from NVIDIA's store instead of the game's {}", d.label, d.store.Text(), d.game.Text());
		}

		HMODULE WINAPI HookLoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags)
		{
			Dll* to = name ? Target(name) : nullptr;
			Trace("LoadLibraryExW", name ? name : L"", _ReturnAddress(), to);
			if (to)
			{
				if (HMODULE module = LoadLibraryExW(to->storePath.c_str(), file, flags))
				{
					LoadedThroughRedirect(*to, module, flags);
					return module;
				}
				const DWORD error = GetLastError();
				log::Warn("dlss: could not load {} (error {}); NGX gets the game's copy", Utf8(to->storePath), error);
			}
			return LoadLibraryExW(name, file, flags);
		}

		HMODULE WINAPI HookLoadLibraryW(LPCWSTR name)
		{
			Dll* to = name ? Target(name) : nullptr;
			Trace("LoadLibraryW", name ? name : L"", _ReturnAddress(), to);
			if (to)
			{
				if (HMODULE module = LoadLibraryW(to->storePath.c_str()))
				{
					LoadedThroughRedirect(*to, module, 0);
					return module;
				}
				const DWORD error = GetLastError();
				log::Warn("dlss: could not load {} (error {}); NGX gets the game's copy", Utf8(to->storePath), error);
			}
			return LoadLibraryW(name);
		}

		HANDLE WINAPI HookCreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE templateFile)
		{
			// Reads only: NVIDIA's store files are never opened for writing on NGX's behalf.
			constexpr DWORD kWrites = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | DELETE | WRITE_DAC | WRITE_OWNER;
			Dll* to = name && disposition == OPEN_EXISTING && !(access & kWrites) ? Target(name) : nullptr;
			Trace("CreateFileW", name ? name : L"", _ReturnAddress(), to);
			return CreateFileW(to ? to->storePath.c_str() : name, access, share, security, disposition, flags, templateFile);
		}

		DWORD WINAPI HookGetFileAttributesW(LPCWSTR name)
		{
			Dll* to = name ? Target(name) : nullptr;
			Trace("GetFileAttributesW", name ? name : L"", _ReturnAddress(), to);
			return GetFileAttributesW(to ? to->storePath.c_str() : name);
		}

		DWORD APIENTRY HookGetFileVersionInfoSizeExW(DWORD flags, LPCWSTR name, LPDWORD handle)
		{
			Dll* to = name ? Target(name) : nullptr;
			Trace("GetFileVersionInfoSizeExW", name ? name : L"", _ReturnAddress(), to);
			return GetFileVersionInfoSizeExW(flags, to ? to->storePath.c_str() : name, handle);
		}

		BOOL APIENTRY HookGetFileVersionInfoExW(DWORD flags, LPCWSTR name, DWORD handle, DWORD size, LPVOID data)
		{
			Dll* to = name ? Target(name) : nullptr;
			Trace("GetFileVersionInfoExW", name ? name : L"", _ReturnAddress(), to);
			return GetFileVersionInfoExW(flags, to ? to->storePath.c_str() : name, handle, size, data);
		}

		DWORD APIENTRY HookGetFileVersionInfoSizeA(LPCSTR name, LPDWORD handle)
		{
			const std::wstring wide = name ? FromAnsi(name) : std::wstring();
			Dll* to = Target(wide);
			Trace("GetFileVersionInfoSizeA", wide, _ReturnAddress(), to);
			return GetFileVersionInfoSizeA(to ? to->storePathAnsi.c_str() : name, handle);
		}

		BOOL APIENTRY HookGetFileVersionInfoA(LPCSTR name, DWORD handle, DWORD size, LPVOID data)
		{
			const std::wstring wide = name ? FromAnsi(name) : std::wstring();
			Dll* to = Target(wide);
			Trace("GetFileVersionInfoA", wide, _ReturnAddress(), to);
			return GetFileVersionInfoA(to ? to->storePathAnsi.c_str() : name, handle, size, data);
		}

		// NGX asked for the game's copy: the store copy it got reports the game's path, as the calls above do.
		DWORD WINAPI HookGetModuleFileNameW(HMODULE module, LPWSTR buffer, DWORD size)
		{
			if (module && g_checked.load(std::memory_order_acquire))
			{
				for (auto& d : g_dlls)
				{
					if (d.module.load() != module)
						continue;
					Trace("GetModuleFileNameW", d.storePath, _ReturnAddress(), nullptr);
					if (!size)
					{
						SetLastError(ERROR_INSUFFICIENT_BUFFER);
						return 0;
					}
					const auto n = static_cast<DWORD>(std::min<size_t>(d.gamePath.size(), size - 1));
					std::wmemcpy(buffer, d.gamePath.data(), n);
					buffer[n] = L'\0';
					if (n < d.gamePath.size())
					{
						SetLastError(ERROR_INSUFFICIENT_BUFFER);
						return size;
					}
					SetLastError(ERROR_SUCCESS);
					return n;
				}
			}
			return GetModuleFileNameW(module, buffer, size);
		}

		HMODULE WINAPI HookGetModuleHandleW(LPCWSTR name)
		{
			if (Dll* d = Redirected(name))
			{
				Trace("GetModuleHandleW", name, _ReturnAddress(), d);
				return d->module.load();
			}
			return GetModuleHandleW(name);
		}

		BOOL WINAPI HookGetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE* module)
		{
			if (!(flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS))
			{
				if (Dll* d = Redirected(name))
				{
					Trace("GetModuleHandleExW", name, _ReturnAddress(), d);
					return GetModuleHandleExW(flags, d->storePath.c_str(), module);
				}
			}
			return GetModuleHandleExW(flags, name, module);
		}

		// Research only (debugDlssTrace).
		HANDLE WINAPI HookFindFirstFileExW(LPCWSTR name, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags)
		{
			Trace("FindFirstFileExW", name ? name : L"", _ReturnAddress(), nullptr);
			return FindFirstFileExW(name, level, data, op, filter, flags);
		}

		HANDLE WINAPI HookFindFirstFileA(LPCSTR name, LPWIN32_FIND_DATAA data)
		{
			Trace("FindFirstFileA", name ? FromAnsi(name) : std::wstring(), _ReturnAddress(), nullptr);
			return FindFirstFileA(name, data);
		}

		BOOL WINAPI HookPathFileExistsW(LPCWSTR path)
		{
			Trace("PathFileExistsW", path ? path : L"", _ReturnAddress(), nullptr);
			return PathFileExistsW(path);
		}

		BOOL WINAPI HookPathFileExistsA(LPCSTR path)
		{
			Trace("PathFileExistsA", path ? FromAnsi(path) : std::wstring(), _ReturnAddress(), nullptr);
			return PathFileExistsA(path);
		}

		struct Hook
		{
			const char* function;
			void* replacement;
			bool traceOnly;
		};
		const Hook kHooks[] = {
			{"LoadLibraryExW", reinterpret_cast<void*>(&HookLoadLibraryExW), false},
			{"LoadLibraryW", reinterpret_cast<void*>(&HookLoadLibraryW), false},
			{"CreateFileW", reinterpret_cast<void*>(&HookCreateFileW), false},
			{"GetFileAttributesW", reinterpret_cast<void*>(&HookGetFileAttributesW), false},
			{"GetFileVersionInfoSizeExW", reinterpret_cast<void*>(&HookGetFileVersionInfoSizeExW), false},
			{"GetFileVersionInfoExW", reinterpret_cast<void*>(&HookGetFileVersionInfoExW), false},
			{"GetFileVersionInfoSizeA", reinterpret_cast<void*>(&HookGetFileVersionInfoSizeA), false},
			{"GetFileVersionInfoA", reinterpret_cast<void*>(&HookGetFileVersionInfoA), false},
			{"GetModuleFileNameW", reinterpret_cast<void*>(&HookGetModuleFileNameW), false},
			{"GetModuleHandleW", reinterpret_cast<void*>(&HookGetModuleHandleW), false},
			{"GetModuleHandleExW", reinterpret_cast<void*>(&HookGetModuleHandleExW), false},
			{"FindFirstFileExW", reinterpret_cast<void*>(&HookFindFirstFileExW), true},
			{"FindFirstFileA", reinterpret_cast<void*>(&HookFindFirstFileA), true},
			{"PathFileExistsW", reinterpret_cast<void*>(&HookPathFileExistsW), true},
			{"PathFileExistsA", reinterpret_cast<void*>(&HookPathFileExistsA), true},
		};

		// Points `module`'s imports of the functions above at the hooks (by name, whichever DLL they come from);
		// returns how many slots changed.
		int PatchImports(HMODULE module)
		{
			auto* base = reinterpret_cast<uint8_t*>(module);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
			const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
			if (!dir.VirtualAddress)
				return 0;
			int changed = 0;
			for (auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name; ++desc)
			{
				if (!desc->OriginalFirstThunk || !desc->FirstThunk)
					continue;
				const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
				auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
				for (; names->u1.AddressOfData; ++names, ++slots)
				{
					if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
						continue;
					const char* function = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name;
					for (const auto& hook : kHooks)
					{
						if (std::strcmp(function, hook.function) != 0)
							continue;
						const auto target = reinterpret_cast<ULONGLONG>(hook.replacement);
						DWORD old = 0;
						if ((!hook.traceOnly || g_trace) && slots->u1.Function != target &&
						    VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &old))
						{
							slots->u1.Function = target;
							VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), old, &old);
							++changed;
						}
						break;
					}
				}
			}
			return changed;
		}

		// ---- Super Resolution presets (Streamline slDLSSSetOptions) ---------------------------------------

		// Streamline 2.9 include/sl_dlss.h, sl::DLSSOptions (kStructVersion3, 88 bytes): after the 32-byte
		// BaseStructure (next, 16-byte type GUID, version) come u32 mode, outputWidth, outputHeight; f32 sharpness,
		// preExposure, exposureScale; 3 x Boolean; u32 presets for DLAA, Quality, Balanced, Performance, Ultra
		// Performance, Ultra Quality; 2 x Boolean. sl::DLSSPreset: 0 default, 1 A ... 11 K, 12 L, 13 M.
		// sl::DLSSMode: 0 off, 1 Performance, 2 Balanced, 3 Quality, 4 Ultra Performance, 5 Ultra Quality, 6 DLAA.
		// sl.dlss passes the presets to NGX as they are and recreates the DLSS feature when they change.
		constexpr uint64_t kOptionsVersion = 3;
		constexpr size_t kOptionsSize = 88, kVersionOffset = 24, kModeOffset = 32, kPresetsOffset = 60;
		constexpr uint32_t kPresetK = 11, kPresetL = 12, kPresetM = 13;
		constexpr int kChoiceGame = -1, kChoiceAuto = 0; // else the preset itself
		using Presets = std::array<uint32_t, 6>;         // DLAA, Quality, Balanced, Performance, Ultra Performance, Ultra Quality

		// Streamline (include/sl_core_api.h): extern "C" sl::Result slGetFeatureFunction(sl::Feature, const char*, void*&).
		using GetFeatureFunctionFn = int (*)(uint32_t, const char*, void*&);
		// slDLSSSetOptions(const sl::ViewportHandle&, const sl::DLSSOptions&).
		using DlssSetOptionsFn = int (*)(const void*, const void*);
		GetFeatureFunctionFn g_getFeatureFunction = nullptr;
		DlssSetOptionsFn g_dlssSetOptions = nullptr;
		std::atomic_bool g_interposerHooked = false;
		std::atomic_int g_choice = kChoiceAuto;
		std::atomic_int g_mode = -1;
		std::atomic<uint32_t> g_gamePreset = 0, g_sentPreset = 0;
		std::mutex g_reportMutex;
		uint32_t g_lastMode = ~0u;
		Presets g_lastGame{}, g_lastSent{};
		std::atomic_bool g_warnedVersion = false;
		std::atomic_bool g_warnedModels = false; // the chosen model needs a newer DLSS (once per setting)

		int ParseChoice(const std::string& s)
		{
			if (s == "game")
				return kChoiceGame;
			if (s.size() == 1 && std::toupper(static_cast<unsigned char>(s[0])) >= 'A' && std::toupper(static_cast<unsigned char>(s[0])) <= 'O')
				return std::toupper(static_cast<unsigned char>(s[0])) - 'A' + 1;
			if (s != "auto")
				log::Warn("dlss: unknown dlssPreset \"{}\"; using auto", s);
			return kChoiceAuto;
		}

		// The presets to send: the game's, or the chosen ones when the loaded DLSS has those models.
		Presets Choose(const Presets& game)
		{
			const int choice = g_choice.load(std::memory_order_relaxed);
			const Version dll = Version::Unpack(g_sr.loaded.load(std::memory_order_relaxed));
			if (choice == kChoiceGame || !dll.Valid())
				return game;
			const auto preset = static_cast<uint32_t>(choice);
			const Version needed = choice == kChoiceAuto || preset >= kPresetL ? kModelsLM : kModelK;
			if (dll < needed)
			{
				if (!g_warnedModels.exchange(true))
					log::Warn("dlss: the {} models need DLSS {} or newer, the game uses {}; presets left as the game sets them",
					    choice == kChoiceAuto ? std::string("DLSS 4.5") : "preset " + PresetLetter(preset), needed.Text(), dll.Text());
				return game;
			}
			if (choice == kChoiceAuto)
				return {kPresetK, kPresetK, kPresetK, kPresetM, kPresetL, kPresetK};
			return {preset, preset, preset, preset, preset, preset};
		}

		int Slot(uint32_t mode)
		{
			constexpr int kSlots[] = {-1, 3, 2, 1, 4, 5, 0};
			return mode < 7 ? kSlots[mode] : -1;
		}

		std::string Letters(const Presets& p)
		{
			std::string out;
			for (const uint32_t v : p)
				out += (out.empty() ? "" : "/") + (v ? PresetLetter(v) : std::string("-"));
			return out;
		}

		void Report(uint32_t mode, const Presets& game, const Presets& sent)
		{
			const int slot = Slot(mode);
			g_mode = static_cast<int>(mode);
			g_gamePreset = slot >= 0 ? game[slot] : 0;
			g_sentPreset = slot >= 0 ? sent[slot] : 0;
			std::lock_guard lock(g_reportMutex);
			if (mode == g_lastMode && game == g_lastGame && sent == g_lastSent)
				return;
			g_lastMode = mode;
			g_lastGame = game;
			g_lastSent = sent;
			static const char* kModes[] = {"off", "Performance", "Balanced", "Quality", "Ultra Performance", "Ultra Quality", "DLAA"};
			log::Info("dlss: Super Resolution mode {} ({}), presets DLAA/Q/B/P/UP/UQ {} -> {} (DLSS {})", mode, mode < 7 ? kModes[mode] : "?", Letters(game),
			    Letters(sent), Version::Unpack(g_sr.loaded.load()).Text());
		}

		// The DLSS mode the game renders for while UE5 draws the world (config ue5GameResolution), or 0: the game's.
		uint32_t RendererMode()
		{
			const auto& c = config::Get();
			if (!c.ue5Renderer)
				return 0;
			const std::string& r = c.ue5GameResolution;
			return r == "performance" ? 1 : r == "balanced" ? 2 : r == "quality" ? 3 : 0;
		}

		// slDLSSGetOptimalSettings(const sl::DLSSOptions&, sl::DLSSOptimalSettings&): the render size for a mode; asked
		// with the renderer's mode, the game renders at that size.
		using DlssGetOptimalSettingsFn = int (*)(const void* options, void* settings);
		DlssGetOptimalSettingsFn g_dlssGetOptimalSettings = nullptr;

		int DlssGetOptimalSettings(const void* options, void* settings)
		{
			const uint32_t mode = RendererMode();
			uint64_t version = 0;
			std::memcpy(&version, static_cast<const uint8_t*>(options) + kVersionOffset, sizeof(version));
			if (!mode || version != kOptionsVersion)
				return g_dlssGetOptimalSettings(options, settings);
			alignas(8) uint8_t copy[kOptionsSize];
			std::memcpy(copy, options, kOptionsSize);
			std::memcpy(copy + kModeOffset, &mode, sizeof(mode));
			const int result = g_dlssGetOptimalSettings(copy, settings);
			uint32_t size[2] = {};
			std::memcpy(size, static_cast<const uint8_t*>(settings) + 32, sizeof(size));
			log::Info("dlss: render size for the UE5 renderer's mode {}: {}x{}", mode, size[0], size[1]);
			return result;
		}

		int DlssSetOptions(const void* viewport, const void* options)
		{
			const auto* o = static_cast<const uint8_t*>(options);
			uint64_t version = 0;
			std::memcpy(&version, o + kVersionOffset, sizeof(version));
			if (version != kOptionsVersion)
			{
				if (!g_warnedVersion.exchange(true))
					log::Warn("dlss: sl::DLSSOptions version {} (expected {}); presets left as the game sets them", version, kOptionsVersion);
				return g_dlssSetOptions(viewport, options);
			}
			uint32_t mode = 0;
			Presets game{};
			std::memcpy(&mode, o + kModeOffset, sizeof(mode));
			std::memcpy(game.data(), o + kPresetsOffset, sizeof(game));
			const uint32_t rendererMode = mode ? RendererMode() : 0; // (DLSS off stays off)
			Presets sent = Choose(game);
			// (while UE5 draws the world the game's DLSS picture only carries peds and vehicles: the lighter model K)
			if (rendererMode && !(Version::Unpack(g_sr.loaded.load(std::memory_order_relaxed)) < kModelK))
				sent.fill(kPresetK);
			Report(rendererMode ? rendererMode : mode, game, sent);
			if (sent == game && !rendererMode)
				return g_dlssSetOptions(viewport, options);
			// Streamline copies the options; the game's own stay untouched.
			alignas(8) uint8_t copy[kOptionsSize];
			std::memcpy(copy, options, kOptionsSize);
			std::memcpy(copy + kPresetsOffset, sent.data(), sizeof(sent));
			if (rendererMode)
				std::memcpy(copy + kModeOffset, &rendererMode, sizeof(rendererMode));
			return g_dlssSetOptions(viewport, copy);
		}

		// Research (debugDlssTrace): DLSS Frame Generation as the game sets it and as Streamline reports it.
		// Streamline 2.9 include/sl_dlss_g.h, after the 32-byte BaseStructure: sl::DLSSGOptions u32 mode (0 off, 1 on,
		// 2 auto), u32 numFramesToGenerate; sl::DLSSGState u64 estimatedVRAMUsageInBytes, u32 status (0 ok, else
		// failure bits), u32 minWidthOrHeight, u32 numFramesActuallyPresented, u32 numFramesToGenerateMax.
		using DlssgSetOptionsFn = int (*)(const void* viewport, const void* options);
		using DlssgGetStateFn = int (*)(const void* viewport, void* state, const void* options);
		DlssgSetOptionsFn g_dlssgSetOptions = nullptr;
		DlssgGetStateFn g_dlssgGetState = nullptr;
		std::mutex g_fgMutex;
		std::string g_lastFgOptions, g_lastFgState;

		void TraceOnChange(std::string& last, std::string line)
		{
			std::lock_guard lock(g_fgMutex);
			if (line != last)
				log::Info("dlss: {}", last = std::move(line));
		}

		int DlssgSetOptions(const void* viewport, const void* options)
		{
			uint32_t v[2];
			std::memcpy(v, static_cast<const uint8_t*>(options) + 32, sizeof(v));
			TraceOnChange(g_lastFgOptions, std::format("Frame Generation options: mode {}, frames to generate {}", v[0], v[1]));
			return g_dlssgSetOptions(viewport, options);
		}

		int DlssgGetState(const void* viewport, void* state, const void* options)
		{
			const int result = g_dlssgGetState(viewport, state, options);
			uint32_t v[4];
			std::memcpy(v, static_cast<const uint8_t*>(state) + 40, sizeof(v));
			TraceOnChange(g_lastFgState, std::format("Frame Generation state: result {}, status {:#x}, frames presented {}, max frames to generate {}", result,
			                                 v[0], v[2], v[3]));
			return result;
		}

		int GetFeatureFunction(uint32_t feature, const char* name, void*& function)
		{
			const int result = g_getFeatureFunction(feature, name, function);
			if (g_trace)
				log::Info("dlss: slGetFeatureFunction(feature {}, {}) -> {} {}", feature, name ? name : "?", result, function);
			if (result != 0 || !function || !name)
				return result;
			if (std::strcmp(name, "slDLSSSetOptions") == 0)
			{
				g_dlssSetOptions = reinterpret_cast<DlssSetOptionsFn>(function);
				function = reinterpret_cast<void*>(&DlssSetOptions);
			}
			else if (std::strcmp(name, "slDLSSGetOptimalSettings") == 0)
			{
				g_dlssGetOptimalSettings = reinterpret_cast<DlssGetOptimalSettingsFn>(function);
				function = reinterpret_cast<void*>(&DlssGetOptimalSettings);
			}
			else if (g_trace && std::strcmp(name, "slDLSSGSetOptions") == 0)
			{
				g_dlssgSetOptions = reinterpret_cast<DlssgSetOptionsFn>(function);
				function = reinterpret_cast<void*>(&DlssgSetOptions);
			}
			else if (g_trace && std::strcmp(name, "slDLSSGGetState") == 0)
			{
				g_dlssgGetState = reinterpret_cast<DlssgGetStateFn>(function);
				function = reinterpret_cast<void*>(&DlssgGetState);
			}
			return result;
		}

		void HookInterposer(HMODULE interposer)
		{
			if (!interposer || g_interposerHooked.exchange(true))
				return;
			graphics::streamline::Hook(interposer, g_trace);
			void* target = reinterpret_cast<void*>(GetProcAddress(interposer, "slGetFeatureFunction"));
			if (!target || MH_CreateHook(target, reinterpret_cast<void*>(&GetFeatureFunction), reinterpret_cast<void**>(&g_getFeatureFunction)) != MH_OK ||
			    MH_EnableHook(target) != MH_OK)
			{
				log::Warn("dlss: could not hook Streamline's slGetFeatureFunction; DLSS presets left as the game sets them");
				return;
			}
			wchar_t path[MAX_PATH] = {};
			GetModuleFileNameW(interposer, path, MAX_PATH);
			log::Info("dlss: Streamline {} hooked (DLSS presets)", FileVersion(path).Text());
		}

		// ---- DLL loads -------------------------------------------------------------------------------

		// ntdll's DLL load notification (LdrRegisterDllNotification); hooking kernelbase!LoadLibraryExW instead made
		// the game crash at start (research §51).
		struct UnicodeString
		{
			USHORT length, maximumLength;
			PWSTR buffer;
		};
		struct DllNotificationData
		{
			ULONG flags;
			const UnicodeString* fullName;
			const UnicodeString* baseName;
			PVOID base;
			ULONG size;
		};
		using DllNotificationFn = VOID(CALLBACK*)(ULONG reason, const DllNotificationData* data, PVOID context);
		using LdrRegisterDllNotificationFn = LONG(NTAPI*)(ULONG flags, DllNotificationFn callback, PVOID context, PVOID* cookie);
		constexpr ULONG kDllLoaded = 1;
		PVOID g_cookie = nullptr;

		bool NeedNgxHooks()
		{
			if (g_trace)
				return true;
			if (!g_upgrade)
				return false;
			if (!g_checked.load(std::memory_order_acquire))
				return true; // the hooks leave calls alone until the store is checked
			return std::any_of(std::begin(g_dlls), std::end(g_dlls), [](const Dll& d) { return d.redirect; });
		}

		// Research (debugDlssTrace): the parameters NGX creates DLSS Super Resolution with, to see the presets
		// arrive. NVSDK_NGX_Parameter's vtable has each overload set in reverse order (MSVC): sl.dlss calls
		// Set(name, unsigned int) at slot 4 and Get(name, int*) at slot 11, so Get(name, unsigned int*) is slot 12.
		using CreateFeatureFn = int (*)(void* commandList, int feature, void* parameters, void** handle);
		using GetParameterFn = int (*)(void* parameters, const char* name, uint64_t* value);
		CreateFeatureFn g_createFeature = nullptr;
		std::atomic_bool g_createFeatureHooked = false;

		std::string Parameter(void* parameters, int slot, const char* name)
		{
			uint64_t value = 0; // Get writes 4 bytes; 8 in case the slot were another overload
			const auto get = reinterpret_cast<GetParameterFn>((*reinterpret_cast<void***>(parameters))[slot]);
			const auto result = static_cast<uint32_t>(get(parameters, name, &value));
			return (result & 0xfff00000) != 0xbad00000 ? std::to_string(static_cast<int32_t>(value)) : "-"; // NVSDK_NGX_Result_Fail
		}

		int CreateFeature(void* commandList, int feature, void* parameters, void** handle)
		{
			constexpr int kSuperSampling = 1; // NVSDK_NGX_Feature_SuperSampling
			if (feature == kSuperSampling && parameters)
			{
				const auto u = [&](const char* name) { return Parameter(parameters, 12, name); };
				log::Info("dlss: NGX creates Super Resolution: PerfQualityValue {}, presets DLAA {} Quality {} Balanced {} Performance {} "
				          "UltraPerformance {}, {}x{} -> {}x{}",
				    Parameter(parameters, 11, "PerfQualityValue"), u("DLSS.Hint.Render.Preset.DLAA"), u("DLSS.Hint.Render.Preset.Quality"),
				    u("DLSS.Hint.Render.Preset.Balanced"), u("DLSS.Hint.Render.Preset.Performance"), u("DLSS.Hint.Render.Preset.UltraPerformance"), u("Width"),
				    u("Height"), u("OutWidth"), u("OutHeight"));
			}
			const int result = g_createFeature(commandList, feature, parameters, handle);
			if (feature == kSuperSampling)
				log::Info("dlss: NGX CreateFeature(Super Resolution) -> {:#x}", static_cast<uint32_t>(result));
			return result;
		}

		void HookCreateFeature(HMODULE ngx)
		{
			if (g_createFeatureHooked.exchange(true))
				return;
			void* target = reinterpret_cast<void*>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_CreateFeature"));
			if (!target || MH_CreateHook(target, reinterpret_cast<void*>(&CreateFeature), reinterpret_cast<void**>(&g_createFeature)) != MH_OK ||
			    MH_EnableHook(target) != MH_OK)
				log::Warn("dlss: could not hook NVSDK_NGX_D3D12_CreateFeature");
		}

		void NgxLoaded(HMODULE module, const std::wstring& path)
		{
			const int patched = PatchImports(module);
			std::thread([module, path, patched] {
				Sleep(100);
				// In case the loader resolved the imports after the notification.
				const int again = PatchImports(module);
				log::Info("dlss: NGX core {} {}: {} imports hooked{}", Utf8(path), FileVersion(path).Text(), patched + again,
				    again ? std::format(" ({} after the loader resolved them)", again) : "");
				if (g_trace)
					HookCreateFeature(module);
			}).detach();
		}

		std::string Stack(const std::vector<void*>& frames)
		{
			std::string out;
			std::string last;
			for (void* frame : frames)
			{
				std::string m = ModuleAt(frame);
				m = m.substr(0, m.find('+'));
				if (m == last || m == "ntdll.dll" || m == "KERNELBASE.dll" || m == "KERNEL32.DLL")
					continue;
				last = m;
				out += (out.empty() ? "" : " < ") + m;
			}
			return out;
		}

		// Under the loader lock: only patch and record; logging and version reads happen on a worker.
		VOID CALLBACK OnDll(ULONG reason, const DllNotificationData* data, PVOID)
		{
			if (reason != kDllLoaded || !data || !data->fullName || !data->fullName->buffer)
				return;
			const std::wstring path(data->fullName->buffer, data->fullName->length / sizeof(wchar_t));
			const std::wstring lower = Normalize(path);
			const std::wstring_view base = FileName(lower);
			const auto module = static_cast<HMODULE>(data->base);

			if (base == L"_nvngx.dll" && NeedNgxHooks())
				NgxLoaded(module, path);
			if (base == L"sl.interposer.dll")
				std::thread([module] {
					Sleep(50); // let the loader finish
					HookInterposer(module);
				}).detach();

			// The game's copy or the store's (other copies, e.g. the driver's, only show in the trace).
			Dll* dll = nullptr;
			if (g_checked.load(std::memory_order_acquire))
				for (auto& d : g_dlls)
					if (lower == d.gamePathLower || (!d.storePathLower.empty() && lower == d.storePathLower))
						dll = &d;
			if (dll)
			{
				dll->loadedFromStore = lower != dll->gamePathLower;
				dll->loaded = (dll->loadedFromStore ? dll->store : dll->game).Packed();
			}

			std::vector<void*> stack;
			if (g_trace && (dll || base.find(L"nvngx") != std::wstring_view::npos || base.starts_with(L"sl.")))
			{
				void* frames[32];
				stack.assign(frames, frames + RtlCaptureStackBackTrace(1, 32, frames, nullptr));
			}
			if (dll || !stack.empty())
				std::thread([dll, path, module, stack] {
					Sleep(50);
					const Version v = FileVersion(path);
					if (dll)
					{
						dll->loaded = v.Packed();
						log::Info("dlss: {} {} loaded from {}", dll->label, v.Text(), Utf8(path));
					}
					if (!stack.empty())
						log::Info("dlss: loaded {} {} at {} by {}", Utf8(path), v.Text(), static_cast<void*>(module), Stack(stack));
				}).detach();
		}
	}

	void Start()
	{
		const auto& cfg = config::Get();
		g_trace = cfg.debugDlssTrace;
		g_upgrade = cfg.dlssUpgrade;
		g_choice = ParseChoice(cfg.dlssPreset);
		std::thread(&Discover).detach();

		const auto reg = reinterpret_cast<LdrRegisterDllNotificationFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification"));
		if (!reg || reg(0, &OnDll, nullptr, &g_cookie) != 0)
		{
			log::Warn("dlss: could not watch DLL loads; DLSS left as the game ships it");
			return;
		}
		// Normally both load ~12 s after start.
		if (HMODULE ngx = GetModuleHandleW(L"_nvngx.dll"); ngx && NeedNgxHooks())
		{
			wchar_t path[MAX_PATH] = {};
			GetModuleFileNameW(ngx, path, MAX_PATH);
			NgxLoaded(ngx, path);
		}
		if (HMODULE interposer = GetModuleHandleW(L"sl.interposer.dll"))
			std::thread([interposer] { HookInterposer(interposer); }).detach();
	}

	void PresetChanged()
	{
		g_choice = ParseChoice(config::Get().dlssPreset);
		g_warnedModels = false;
		log::Info("dlss: preset setting now \"{}\"", config::Get().dlssPreset);
	}

	Status GetStatus()
	{
		Status s;
		s.upgradeAtStart = g_upgrade;
		s.checked = g_checked.load(std::memory_order_acquire);
		if (s.checked)
		{
			s.gameSr = g_sr.game.Text();
			s.gameFg = g_fg.game.Text();
			s.storeSr = g_sr.store.Text();
			s.storeFg = g_fg.store.Text();
		}
		s.loadedSr = Version::Unpack(g_sr.loaded.load()).Text();
		s.loadedFg = Version::Unpack(g_fg.loaded.load()).Text();
		s.loadedSrFromStore = g_sr.loadedFromStore;
		s.loadedFgFromStore = g_fg.loadedFromStore;
		s.mode = g_mode;
		s.gamePreset = g_gamePreset;
		s.sentPreset = g_sentPreset;
		return s;
	}

	std::string PresetLetter(uint32_t preset)
	{
		return preset >= 1 && preset <= 26 ? std::string(1, static_cast<char>('A' + preset - 1)) : std::string();
	}
}
