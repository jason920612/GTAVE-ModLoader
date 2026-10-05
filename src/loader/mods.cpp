#include "mods.hpp"

#include <intrin.h>

#include <algorithm>
#include <chrono>
#include <array>
#include <fstream>
#include <format>
#include <mutex>

#include "config.hpp"
#include "log.hpp"
#include "paths.hpp"

#include <nlohmann/json.hpp>

namespace loader::mods
{
	namespace
	{
		std::vector<std::unique_ptr<Mod>> g_mods;
		std::mutex g_modsMutex; // guards g_mods structure and each mod's state/error
		std::atomic_bool g_loaded = false;
		Mod* g_current = nullptr;     // mod whose fiber is running right now
		Mod* g_loading = nullptr;     // mod whose MLOnLoad is running right now
		// Handle = index. Fixed storage: GetSetting may run on any thread while other mods register.
		std::array<Setting, 4096> g_settings;
		std::atomic<int32_t> g_settingCount = 0;
		void* g_schedulerFiber = nullptr;
		DWORD g_gameThreadId = 0;
		const auto g_start = std::chrono::steady_clock::now();

		uint64_t NowMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_start).count();
		}

		Mod* ModFromAddress(void* address)
		{
			HMODULE module = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			    static_cast<LPCWSTR>(address), &module);
			for (auto& mod : g_mods)
				if (mod->module && mod->module == module)
					return mod.get();
			return nullptr;
		}

		void ModLog(Mod* mod, MLLogLevel level, std::string_view message)
		{
			static constexpr const char* kNames[] = {"DEBUG", "INFO ", "WARN ", "ERROR"};
			const auto lvl = static_cast<log::Level>(std::clamp<int>(level, ML_LOG_DEBUG, ML_LOG_ERROR));
			log::Write(lvl, std::format("[{}] {}", mod ? mod->name : "?", message));
			if (mod && mod->log)
			{
				const auto line = std::format("[{:>8}ms] [{}] {}\n", NowMs(), kNames[static_cast<int>(lvl)], message);
				fwrite(line.data(), 1, line.size(), mod->log);
				fflush(mod->log);
			}
		}

		// Parks a mod that must never run again and returns control to the scheduler.
		[[noreturn]] void AbandonCurrentFiber(Mod* mod, std::string error)
		{
			{
				std::lock_guard lock(g_modsMutex);
				mod->state = State::Faulted;
				mod->error = std::move(error);
			}
			log::Error("mod {} stopped: {}", mod->name, mod->error);
			for (;;)
				SwitchToFiber(g_schedulerFiber);
		}

		// ---- MLApi implementation -------------------------------------------------------------

		Mod* CallerOrCurrent(void* returnAddress)
		{
			if (GetCurrentThreadId() == g_gameThreadId && g_current)
				return g_current;
			return ModFromAddress(returnAddress);
		}

		bool InModFiber(const char* what, void* returnAddress)
		{
			if (GetCurrentThreadId() == g_gameThreadId && g_current)
				return true;
			ModLog(ModFromAddress(returnAddress), ML_LOG_ERROR, std::format("{} called outside MLMain; ignored", what));
			return false;
		}

		uint64_t g_dummyResult[4];

		void ApiNativeBegin(uint64_t hash)
		{
			if (InModFiber("NativeBegin", _ReturnAddress()))
				g_current->invocation.Begin(hash);
		}

		void ApiNativePush(uint64_t value)
		{
			if (GetCurrentThreadId() == g_gameThreadId && g_current)
				g_current->invocation.Push(value);
		}

		uint64_t* ApiNativeCall()
		{
			if (!InModFiber("NativeCall", _ReturnAddress()))
			{
				std::fill(std::begin(g_dummyResult), std::end(g_dummyResult), 0);
				return g_dummyResult;
			}

			Mod* mod = g_current;
			auto& inv = mod->invocation;
			switch (game::natives::Call(inv))
			{
			case game::natives::CallStatus::Ok:
				break;
			case game::natives::CallStatus::UnknownNative:
				ModLog(mod, ML_LOG_ERROR, std::format("native {:#018x} is not available in this game build", inv.hash));
				break;
			case game::natives::CallStatus::TooManyArgs:
				ModLog(mod, ML_LOG_ERROR, std::format("native {:#018x}: too many arguments", inv.hash));
				break;
			case game::natives::CallStatus::Crashed:
				AbandonCurrentFiber(mod, std::format("game crashed inside native {:#018x} (bad arguments?)", inv.hash));
			}
			return inv.result;
		}

		void ApiWait(uint32_t ms)
		{
			if (!InModFiber("Wait", _ReturnAddress()))
				return;
			g_current->wakeAt = NowMs() + ms;
			SwitchToFiber(g_schedulerFiber);
		}

		void ApiLog(MLLogLevel level, const char* message)
		{
			ModLog(CallerOrCurrent(_ReturnAddress()), level, message ? message : "");
		}

		uint64_t ApiGetTickMs()
		{
			return NowMs();
		}

		int32_t ApiAddSetting(MLSettingType type, const char* id, const char* label, int32_t defaultValue)
		{
			Mod* mod = g_loading;
			if (!mod || GetCurrentThreadId() != g_gameThreadId)
			{
				ModLog(ModFromAddress(_ReturnAddress()), ML_LOG_ERROR, "AddSetting called outside MLOnLoad; ignored");
				return -1;
			}
			if (type != ML_SETTING_TOGGLE && type != ML_SETTING_SLIDER)
			{
				ModLog(mod, ML_LOG_ERROR, std::format("AddSetting: unknown type {}", static_cast<int>(type)));
				return -1;
			}
			if (!id || !*id || !label || !*label)
			{
				ModLog(mod, ML_LOG_ERROR, "AddSetting: id and label are required");
				return -1;
			}
			if (mod->settings.size() >= ML_MAX_SETTINGS)
			{
				ModLog(mod, ML_LOG_ERROR, std::format("AddSetting: at most {} settings per mod; '{}' ignored", ML_MAX_SETTINGS, id));
				return -1;
			}
			for (const Setting* s : mod->settings)
				if (s->id == id)
				{
					ModLog(mod, ML_LOG_ERROR, std::format("AddSetting: duplicate id '{}'", id));
					return -1;
				}

			const int32_t handle = g_settingCount.load();
			if (handle >= static_cast<int32_t>(g_settings.size()))
			{
				ModLog(mod, ML_LOG_ERROR, "AddSetting: too many settings in total");
				return -1;
			}
			Setting& s = g_settings[handle];
			s.owner = mod;
			s.id = id;
			s.label = label;
			s.type = type;
			s.defaultValue = std::clamp(defaultValue, 0, s.Max());
			s.value = s.defaultValue;
			mod->settings.push_back(&s);
			g_settingCount = handle + 1; // publish after the entry is complete
			return handle;
		}

		int32_t ApiGetSetting(int32_t handle)
		{
			return handle >= 0 && handle < g_settingCount.load() ? g_settings[handle].value.load() : 0;
		}

		std::filesystem::path SettingsFile(const Mod& mod)
		{
			return mod.dir / L"settings.json";
		}

		void LoadSettings(Mod& mod)
		{
			if (mod.settings.empty())
				return;
			std::ifstream in(SettingsFile(mod));
			if (!in)
				return;
			const auto j = nlohmann::json::parse(in, nullptr, false);
			if (!j.is_object())
			{
				ModLog(&mod, ML_LOG_WARN, "settings.json is not valid JSON; using defaults");
				return;
			}
			for (Setting* s : mod.settings)
				if (const auto it = j.find(s->id); it != j.end() && it->is_number_integer())
					s->value = std::clamp(it->get<int32_t>(), 0, s->Max());
		}

		void SaveSettings(const Mod& mod)
		{
			nlohmann::json j = nlohmann::json::object();
			for (const Setting* s : mod.settings)
				j[s->id] = s->value.load();
			std::ofstream out(SettingsFile(mod), std::ios::trunc);
			if (out)
				out << j.dump(2) << "\n";
			else
				log::Warn("mod {}: could not write settings.json", mod.name);
		}

		const MLApi g_api{
			.apiVersion = ML_API_VERSION,
			.size = sizeof(MLApi),
			.NativeBegin = ApiNativeBegin,
			.NativePush = ApiNativePush,
			.NativeCall = ApiNativeCall,
			.Wait = ApiWait,
			.Log = ApiLog,
			.GetTickMs = ApiGetTickMs,
			.AddSetting = ApiAddSetting,
			.GetSetting = ApiGetSetting,
		};

		// ---- loading --------------------------------------------------------------------------

		// SEH wrappers: no C++ objects with destructors allowed in these frames.
		bool SafeOnLoad(MLOnLoadFn fn, const MLContext* ctx, int* result)
		{
			__try
			{
				*result = fn(&g_api, ctx);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool SafeMain(MLMainFn fn)
		{
			__try
			{
				fn();
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		void CALLBACK FiberProc(void* param)
		{
			auto* mod = static_cast<Mod*>(param);
			if (SafeMain(mod->main))
			{
				{
					std::lock_guard lock(g_modsMutex);
					mod->state = State::Finished;
				}
				ModLog(mod, ML_LOG_INFO, "MLMain returned");
				for (;;)
					SwitchToFiber(g_schedulerFiber);
			}
			AbandonCurrentFiber(mod, "crashed in MLMain");
		}

		void Load(Mod& mod)
		{
			std::error_code ec;
			std::filesystem::create_directories(mod.dir / L"data", ec);
			mod.dirStr = mod.dir.wstring() + L"\\";
			mod.configStr = (mod.dir / L"config.json").wstring();
			mod.dataStr = (mod.dir / L"data").wstring() + L"\\";
			mod.context = {mod.dirStr.c_str(), mod.configStr.c_str(), mod.dataStr.c_str()};
			mod.log = _wfsopen((mod.dir / L"log.txt").c_str(), L"w", _SH_DENYWR);

			// Let the mod's own DLL dependencies resolve from its folder too.
			mod.module = LoadLibraryExW(mod.file.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
			if (!mod.module)
			{
				mod.state = State::Failed;
				mod.error = std::format("LoadLibrary failed (error {})", GetLastError());
				return;
			}

			auto getInfo = reinterpret_cast<MLGetModInfoFn>(GetProcAddress(mod.module, "MLGetModInfo"));
			auto onLoad = reinterpret_cast<MLOnLoadFn>(GetProcAddress(mod.module, "MLOnLoad"));
			mod.main = reinterpret_cast<MLMainFn>(GetProcAddress(mod.module, "MLMain"));
			mod.onUnload = reinterpret_cast<MLOnUnloadFn>(GetProcAddress(mod.module, "MLOnUnload"));
			if (!getInfo || !onLoad)
			{
				mod.state = State::Failed;
				mod.error = "not a ModLoader mod (missing MLGetModInfo/MLOnLoad export)";
				FreeLibrary(mod.module);
				mod.module = nullptr;
				return;
			}

			const MLModInfo* info = getInfo();
			if (!info || info->apiVersion != ML_API_VERSION)
			{
				mod.state = State::Failed;
				mod.error = std::format("built for SDK API {}, loader provides {}", info ? info->apiVersion : 0, ML_API_VERSION);
				FreeLibrary(mod.module);
				mod.module = nullptr;
				return;
			}
			mod.name = info->name ? info->name : mod.file.stem().string();
			mod.version = info->version ? info->version : "";
			mod.author = info->author ? info->author : "";
			mod.description = info->description ? info->description : "";

			int accepted = 0;
			g_loading = &mod;
			const bool survived = SafeOnLoad(onLoad, &mod.context, &accepted);
			g_loading = nullptr;
			if (!survived)
			{
				mod.state = State::Faulted;
				mod.error = "crashed in MLOnLoad";
				return; // keep the module mapped: its code may still be referenced
			}
			if (!accepted)
			{
				mod.state = State::Failed;
				mod.error = "MLOnLoad returned 0";
				mod.settings.clear(); // a cancelled mod gets no page
				return;
			}
			LoadSettings(mod);

			if (mod.main)
			{
				mod.fiber = CreateFiber(256 * 1024, FiberProc, &mod);
				if (!mod.fiber)
				{
					mod.state = State::Failed;
					mod.error = "could not create fiber";
					return;
				}
			}
			mod.state = State::Loaded;
		}
	}

	void LoadAll()
	{
		g_gameThreadId = GetCurrentThreadId();
		g_schedulerFiber = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber(nullptr);

		std::vector<std::filesystem::path> files;
		std::error_code ec;
		for (const auto& entry : std::filesystem::directory_iterator(paths::Get().mods, ec))
		{
			auto ext = entry.path().extension().wstring();
			std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
			if (entry.is_regular_file() && ext == L".dll")
				files.push_back(entry.path());
		}
		std::sort(files.begin(), files.end());

		const auto disabled = config::Get().disabledMods;
		std::lock_guard lock(g_modsMutex);
		for (const auto& file : files)
		{
			// Registered before loading so logging from MLOnLoad can find the mod by address.
			Mod* mod = g_mods.emplace_back(std::make_unique<Mod>()).get();
			mod->file = file;
			mod->fileName = file.filename().string();
			mod->name = file.stem().string();
			mod->dir = file.parent_path() / file.stem();

			if (disabled.contains(mod->fileName))
				mod->state = State::Disabled;
			else
				Load(*mod);

			if (mod->state == State::Failed || mod->state == State::Faulted)
				log::Error("mod {}: {}", mod->fileName, mod->error);
			else
				log::Info("mod {}: {} {} by {} [{}]", mod->fileName, mod->name, mod->version, mod->author, ToString(mod->state));
		}
		log::Info("{} mod(s) found", g_mods.size());
		g_loaded = true;
	}

	void Tick()
	{
		const auto now = NowMs();
		for (auto& mod : g_mods)
		{
			if (!mod->fiber || (mod->state != State::Loaded && mod->state != State::Running) || now < mod->wakeAt)
				continue;
			if (mod->state == State::Loaded)
			{
				std::lock_guard lock(g_modsMutex);
				mod->state = State::Running;
			}
			g_current = mod.get();
			SwitchToFiber(mod->fiber);
			g_current = nullptr;
		}
	}

	const std::vector<std::unique_ptr<Mod>>& All()
	{
		return g_mods;
	}

	std::vector<ModView> Snapshot()
	{
		std::lock_guard lock(g_modsMutex);
		std::vector<ModView> out;
		out.reserve(g_mods.size());
		for (const auto& m : g_mods)
			out.push_back({m->fileName, m->name, m->version, m->author, m->description, m->error, m->dir, m->state});
		return out;
	}

	bool Loaded()
	{
		return g_loaded;
	}

	const char* ToString(State state)
	{
		switch (state)
		{
		case State::Disabled: return "disabled";
		case State::Failed: return "failed";
		case State::Loaded: return "loaded";
		case State::Running: return "running";
		case State::Finished: return "finished";
		default: return "faulted";
		}
	}

	void SetSettingValue(Setting& setting, int32_t value)
	{
		value = std::clamp(value, 0, setting.Max());
		if (setting.value.exchange(value) != value && setting.owner)
			SaveSettings(*setting.owner);
	}
}
