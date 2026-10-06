#include "datafiles.hpp"

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
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

		// The merged copy of `path`, or "" to load the original.
		std::string Redirect(const char* path)
		{
			const std::string_view p = path;
			// The packs generated for replacement mods hold the overrides themselves.
			if ((!p.ends_with(".meta") && !p.ends_with(".xml")) || p.starts_with("dlc_mlr"))
				return {};
			std::lock_guard lock(g_mutex);
			if (g_overrides.Empty())
				return {};
			if (const auto it = g_redirects.find(path); it != g_redirects.end())
				return it->second;
			std::string& redirect = g_redirects[path];
			char resolved[256] = {};
			if (!g_resolve(g_searchPaths, resolved, sizeof(resolved), path, g_noExtension))
				return {};
			std::string original, merged;
			if (!ReadGameFile(resolved, original))
				return {};
			std::vector<std::string> keys;
			const int replaced = g_overrides.Apply(original, merged, &keys);
			if (replaced == 0)
				return {};
			// One file per original path: device and path, with the separators flattened.
			std::string name(p);
			for (auto& c : name)
				if (c == ':' || c == '/' || c == '\\')
					c = '_';
			const auto dir = paths::Get().root / L"cache" / L"datafiles";
			std::error_code ec;
			std::filesystem::create_directories(dir, ec);
			const auto file = dir / std::filesystem::path(std::u8string(name.begin(), name.end()));
			std::ofstream(file, std::ios::binary).write(merged.data(), static_cast<std::streamsize>(merged.size()));
			redirect = GamePath(file);
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

	std::vector<std::string> Merged()
	{
		std::lock_guard lock(g_mutex);
		return g_merged;
	}
}
