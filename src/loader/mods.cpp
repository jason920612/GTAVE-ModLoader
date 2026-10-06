#include "mods.hpp"

#include <intrin.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <tuple>
#include <fstream>
#include <format>
#include <mutex>

#include "config.hpp"
#include "log.hpp"
#include "game/models.hpp"
#include "game/scripts.hpp"
#include "paths.hpp"
#include "ui/notify.hpp"

#include <nlohmann/json.hpp>

namespace loader::mods
{
	namespace
	{
		std::vector<std::unique_ptr<Mod>> g_mods;
		std::mutex g_modsMutex; // guards g_mods structure and each mod's state/error
		std::atomic_bool g_loaded = false;
		Mod* g_current = nullptr;     // mod whose fiber is running right now
		Task* g_currentTask = nullptr; // fiber of g_current that is running
		Mod* g_loading = nullptr;     // mod whose MLOnLoad is running right now
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
			g_currentTask->wakeAt = NowMs() + ms;
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

		// ---- menu items ------------------------------------------------------------------------

		std::recursive_mutex g_menuMutex; // items and each mod's menu fields; never held while mod code runs
		std::deque<Item> g_items;         // handle = index; deque keeps references stable
		int32_t g_itemCount = 0;          // menu mutex

		// Mod that may add items right now: inside MLOnLoad, MLMain or a callback (game thread).
		Mod* Registrar(const char* what, void* caller)
		{
			if (GetCurrentThreadId() == g_gameThreadId && (g_loading || g_current))
				return g_loading ? g_loading : g_current;
			ModLog(ModFromAddress(caller), ML_LOG_ERROR, std::format("{} called outside MLOnLoad/MLMain/a callback; ignored", what));
			return nullptr;
		}

		// Menu mutex held.
		Item* Get(int32_t handle)
		{
			if (handle < 0 || handle >= g_itemCount)
				return nullptr;
			Item& item = g_items[handle];
			return item.removed ? nullptr : &item;
		}

		float Normalize(const Item& item, float v)
		{
			if (v != v) // NaN
				v = item.min;
			v = std::clamp(v, item.min, item.max);
			if (item.kind == ItemKind::Number)
				v = std::min(item.max, item.min + std::round((v - item.min) / item.step) * item.step);
			else
				v = std::round(v);
			return v;
		}

		// Registers an item for `mod`. `item.value` holds the default; a saved value replaces it.
		int32_t AddItem(Mod* mod, Item& item)
		{
			std::lock_guard lock(g_menuMutex);
			if (item.parent != ML_ROOT_PAGE)
			{
				const Item* page = Get(item.parent);
				if (!page || page->owner != mod || page->kind != ItemKind::Page)
				{
					ModLog(mod, ML_LOG_ERROR, std::format("'{}': page {} is not a page of this mod", item.label, item.parent));
					return -1;
				}
			}
			if (!item.id.empty())
				for (int32_t h = 0; h < g_itemCount; ++h)
					if (const Item* other = Get(h); other && other->owner == mod && other->id == item.id)
					{
						ModLog(mod, ML_LOG_ERROR, std::format("duplicate id '{}'", item.id));
						return -1;
					}

			const int32_t handle = g_itemCount;
			Item& stored = g_items.emplace_back();
			stored.owner = mod;
			stored.parent = item.parent;
			stored.kind = item.kind;
			stored.pauseMenu = g_loading == mod && item.parent == ML_ROOT_PAGE &&
			                   (item.kind == ItemKind::Toggle || item.kind == ItemKind::List ||
			                       (item.kind == ItemKind::Number && item.min == 0 && item.max == 10 && item.step == 1));
			stored.id = std::move(item.id);
			stored.label = std::move(item.label);
			stored.min = item.min;
			stored.max = item.max;
			stored.step = item.step;
			stored.options = std::move(item.options);
			stored.fn = item.fn;
			stored.user = item.user;
			float value = Normalize(stored, item.value);
			if (!stored.id.empty())
				if (const auto it = mod->saved.find(stored.id); it != mod->saved.end() && it->is_number())
					value = Normalize(stored, it->get<float>());
			stored.value = value;
			if (stored.parent == ML_ROOT_PAGE)
				mod->rootItems.push_back(handle);
			else
				g_items[stored.parent].children.push_back(handle);
			g_itemCount = handle + 1;
			return handle;
		}

		std::string Text(const char* s)
		{
			return s ? s : "";
		}

		void MakeItem(Item& item, ItemKind kind, int32_t page, const char* id, const char* label)
		{
			item.kind = kind;
			item.parent = page;
			item.id = Text(id);
			item.label = Text(label);
		}

		int32_t ApiAddPage(int32_t page, const char* label)
		{
			Mod* mod = Registrar("AddPage", _ReturnAddress());
			if (!mod)
				return -1;
			Item item;
			MakeItem(item, ItemKind::Page, page, nullptr, label);
			return AddItem(mod, item);
		}

		int32_t ApiAddToggle(int32_t page, const char* id, const char* label, int32_t defaultValue)
		{
			Mod* mod = Registrar("AddToggle", _ReturnAddress());
			if (!mod)
				return -1;
			Item item;
			MakeItem(item, ItemKind::Toggle, page, id, label);
			item.value = defaultValue ? 1.0f : 0.0f;
			return AddItem(mod, item);
		}

		int32_t AddNumberItem(Mod* mod, int32_t page, const char* id, const char* label, float min, float max, float step, float defaultValue)
		{
			if (!(min <= max) || !(step > 0))
			{
				ModLog(mod, ML_LOG_ERROR, std::format("number '{}': needs min <= max and step > 0", Text(label)));
				return -1;
			}
			Item item;
			MakeItem(item, ItemKind::Number, page, id, label);
			item.min = min;
			item.max = max;
			item.step = step;
			item.value = defaultValue;
			return AddItem(mod, item);
		}

		int32_t ApiAddNumber(int32_t page, const char* id, const char* label, float min, float max, float step, float defaultValue)
		{
			Mod* mod = Registrar("AddNumber", _ReturnAddress());
			return mod ? AddNumberItem(mod, page, id, label, min, max, step, defaultValue) : -1;
		}

		int32_t AddListItem(Mod* mod, int32_t page, const char* id, const char* label, const char* const* options, int32_t count, int32_t defaultValue)
		{
			if (!options || count < 2 || count > ML_MAX_LIST_OPTIONS)
			{
				ModLog(mod, ML_LOG_ERROR, std::format("list '{}': needs 2..{} options", Text(label), ML_MAX_LIST_OPTIONS));
				return -1;
			}
			Item item;
			MakeItem(item, ItemKind::List, page, id, label);
			for (int32_t i = 0; i < count; ++i)
				item.options.emplace_back(options[i] && *options[i] ? options[i] : "?");
			item.max = static_cast<float>(count - 1);
			item.value = static_cast<float>(defaultValue);
			return AddItem(mod, item);
		}

		int32_t ApiAddList(int32_t page, const char* id, const char* label, const char* const* options, int32_t count, int32_t defaultValue)
		{
			Mod* mod = Registrar("AddList", _ReturnAddress());
			return mod ? AddListItem(mod, page, id, label, options, count, defaultValue) : -1;
		}

		int32_t ApiAddAction(int32_t page, const char* label, MLCallback fn, void* user)
		{
			Mod* mod = Registrar("AddAction", _ReturnAddress());
			if (!mod)
				return -1;
			Item item;
			MakeItem(item, ItemKind::Action, page, nullptr, label);
			item.fn = fn;
			item.user = user;
			return AddItem(mod, item);
		}

		int32_t ApiAddText(int32_t page, const char* label)
		{
			Mod* mod = Registrar("AddText", _ReturnAddress());
			if (!mod)
				return -1;
			Item item;
			MakeItem(item, ItemKind::Text, page, nullptr, label);
			return AddItem(mod, item);
		}

		int32_t ApiAddHotkey(const char* id, const char* label, uint32_t defaultKey, MLCallback fn, void* user)
		{
			Mod* mod = Registrar("AddHotkey", _ReturnAddress());
			if (!mod)
				return -1;
			Item item;
			MakeItem(item, ItemKind::Hotkey, ML_ROOT_PAGE, id, label);
			item.max = 255;
			item.value = static_cast<float>(defaultKey & 0xFF);
			item.fn = fn;
			item.user = user;
			return AddItem(mod, item);
		}

		// Item calls from mod code: only the owner may change an item. Menu mutex held.
		Item* Owned(int32_t handle, const char* what, void* caller)
		{
			Item* item = Get(handle);
			if (!item)
				return nullptr;
			if (Mod* mod = CallerOrCurrent(caller); item->owner != mod)
			{
				ModLog(mod, ML_LOG_ERROR, std::format("{}: item {} belongs to another mod", what, handle));
				return nullptr;
			}
			return item;
		}

		void ApiSetCallback(int32_t handle, MLCallback fn, void* user)
		{
			std::lock_guard lock(g_menuMutex);
			if (Item* item = Owned(handle, "SetCallback", _ReturnAddress()))
			{
				item->fn = fn;
				item->user = user;
			}
		}

		void ApiSetLabel(int32_t handle, const char* label)
		{
			std::lock_guard lock(g_menuMutex);
			if (Item* item = Owned(handle, "SetLabel", _ReturnAddress()))
				item->label = Text(label);
		}

		void ApiSetEnabled(int32_t handle, int32_t enabled)
		{
			std::lock_guard lock(g_menuMutex);
			if (Item* item = Owned(handle, "SetEnabled", _ReturnAddress()))
				item->enabled = enabled != 0;
		}

		// Menu mutex held.
		void Remove(std::vector<int32_t>& handles)
		{
			for (const int32_t h : handles)
			{
				Item& item = g_items[h];
				Remove(item.children);
				item.removed = true;
			}
			handles.clear();
		}

		void ApiClearPage(int32_t handle)
		{
			std::lock_guard lock(g_menuMutex);
			if (handle == ML_ROOT_PAGE)
			{
				// Settings shown in the pause menu and hotkeys stay: they are registered once.
				Mod* mod = CallerOrCurrent(_ReturnAddress());
				if (!mod)
					return;
				std::vector<int32_t> keep, drop;
				for (const int32_t h : mod->rootItems)
					(g_items[h].pauseMenu || g_items[h].kind == ItemKind::Hotkey ? keep : drop).push_back(h);
				Remove(drop);
				mod->rootItems = std::move(keep);
				return;
			}
			if (Item* page = Owned(handle, "ClearPage", _ReturnAddress()); page && page->kind == ItemKind::Page)
				Remove(page->children);
		}

		float ApiGetValue(int32_t handle)
		{
			std::lock_guard lock(g_menuMutex);
			const Item* item = Get(handle);
			return item ? item->value.load() : 0.0f;
		}

		void ApiSetValue(int32_t handle, float value)
		{
			std::lock_guard lock(g_menuMutex);
			if (Item* item = Owned(handle, "SetValue", _ReturnAddress()))
			{
				const float v = Normalize(*item, value);
				if (item->value.exchange(v) != v && !item->id.empty())
					item->owner->settingsDirty = true;
			}
		}

		void ApiNotify(const char* text)
		{
			const Mod* mod = CallerOrCurrent(_ReturnAddress());
			ui::Notify(mod ? mod->name : "", Text(text));
		}

		int32_t ApiEnumModels(MLModelType type, MLModelVisitor fn, void* user)
		{
			if (!InModFiber("EnumModels", _ReturnAddress()) || !fn)
				return -1;
			const auto t = type == ML_MODEL_PED ? game::models::Type::Ped : game::models::Type::Vehicle;
			// Copied first: the visitor is mod code and must not run while the table is walked.
			std::vector<std::tuple<uint32_t, std::string, std::string>> found;
			const int32_t n = game::models::Enumerate(t, [&](uint32_t hash, const std::string& name, const std::string& pack) {
				found.emplace_back(hash, name, pack);
			});
			for (const auto& [hash, name, pack] : found)
				fn(hash, name.c_str(), pack.c_str(), user);
			return n;
		}

		int64_t* ApiScriptGlobal(uint32_t index)
		{
			return InModFiber("ScriptGlobal", _ReturnAddress()) ? game::scripts::Global(index) : nullptr;
		}

		int32_t ApiEnumScripts(void (*fn)(int32_t, const char*, void*), void* user)
		{
			if (!InModFiber("EnumScripts", _ReturnAddress()) || !fn)
				return 0;
			const auto threads = game::scripts::Threads();
			for (const auto& t : threads)
				fn(t.id, t.name.c_str(), user);
			return static_cast<int32_t>(threads.size());
		}

		int32_t ApiGetScriptCode(int32_t id, uint8_t* buffer, int32_t size)
		{
			if (!InModFiber("GetScriptCode", _ReturnAddress()))
				return 0;
			const auto code = game::scripts::Code(id);
			if (buffer && size > 0)
				std::memcpy(buffer, code.data(), std::min<size_t>(code.size(), static_cast<size_t>(size)));
			return static_cast<int32_t>(code.size());
		}

		int32_t ApiRedirectScript(int32_t id, uint32_t address, const int64_t* args, int32_t count, int32_t mainFrame)
		{
			if (!InModFiber("RedirectScript", _ReturnAddress()) || count < 0 || (count && !args))
				return 0;
			std::string error;
			const bool ok = game::scripts::RedirectThread(id, address, {args, static_cast<size_t>(count)},
			    mainFrame ? game::scripts::Redirect::MainFrame : game::scripts::Redirect::Call, error);
			if (!ok)
				ModLog(g_current, ML_LOG_WARN, std::format("RedirectScript({}, {}): {}", id, address, error));
			return ok ? 1 : 0;
		}

		int32_t ApiScriptNativeIndex(int32_t id, uint64_t hash)
		{
			if (!InModFiber("ScriptNativeIndex", _ReturnAddress()))
				return -1;
			return game::scripts::NativeIndex(id, reinterpret_cast<const void*>(game::natives::FindHandler(hash)));
		}

		// ---- settings of the first release: items on the root page ------------------------------

		int32_t ApiAddSetting(MLSettingType type, const char* id, const char* label, int32_t defaultValue)
		{
			Mod* mod = g_loading;
			if (!mod || GetCurrentThreadId() != g_gameThreadId)
			{
				ModLog(ModFromAddress(_ReturnAddress()), ML_LOG_ERROR, "AddSetting called outside MLOnLoad; ignored");
				return -1;
			}
			if (!id || !*id || !label || !*label)
			{
				ModLog(mod, ML_LOG_ERROR, "AddSetting: id and label are required");
				return -1;
			}
			switch (type)
			{
			case ML_SETTING_TOGGLE:
			{
				Item item;
				MakeItem(item, ItemKind::Toggle, ML_ROOT_PAGE, id, label);
				item.value = defaultValue ? 1.0f : 0.0f;
				return AddItem(mod, item);
			}
			case ML_SETTING_SLIDER:
				return AddNumberItem(mod, ML_ROOT_PAGE, id, label, 0, 10, 1, static_cast<float>(defaultValue));
			case ML_SETTING_LIST:
				ModLog(mod, ML_LOG_ERROR, "AddSetting: use AddListSetting for lists");
				return -1;
			default:
				ModLog(mod, ML_LOG_ERROR, std::format("AddSetting: unknown type {}", static_cast<int>(type)));
				return -1;
			}
		}

		int32_t ApiAddListSetting(const char* id, const char* label, const char* const* options, int32_t count, int32_t defaultValue)
		{
			Mod* mod = g_loading;
			if (!mod || GetCurrentThreadId() != g_gameThreadId || !id || !*id || !label || !*label)
			{
				ModLog(CallerOrCurrent(_ReturnAddress()), ML_LOG_ERROR, "AddListSetting: MLOnLoad only, id and label required; ignored");
				return -1;
			}
			return AddListItem(mod, ML_ROOT_PAGE, id, label, options, count, defaultValue);
		}

		int32_t ApiGetSetting(int32_t handle)
		{
			return static_cast<int32_t>(ApiGetValue(handle));
		}

		std::filesystem::path SettingsFile(const Mod& mod)
		{
			return mod.dir / L"settings.json";
		}

		void ReadSettings(Mod& mod)
		{
			std::ifstream in(SettingsFile(mod));
			if (!in)
				return;
			auto j = nlohmann::json::parse(in, nullptr, false);
			if (!j.is_object())
			{
				ModLog(&mod, ML_LOG_WARN, "settings.json is not valid JSON; using defaults");
				return;
			}
			mod.saved = std::move(j);
		}

		// Values of items that are not registered (yet) are kept.
		void SaveSettings(Mod& mod)
		{
			nlohmann::json j;
			{
				std::lock_guard lock(g_menuMutex);
				mod.settingsDirty = false;
				for (int32_t h = 0; h < g_itemCount; ++h)
					if (const Item* item = Get(h); item && item->owner == &mod && !item->id.empty())
					{
						const float v = item->value;
						if (item->kind == ItemKind::Number && v != std::floor(v))
							mod.saved[item->id] = v;
						else
							mod.saved[item->id] = static_cast<int32_t>(v);
					}
				j = mod.saved;
			}
			std::ofstream out(SettingsFile(mod), std::ios::trunc);
			if (out)
				out << j.dump(2) << "\n";
			else
				log::Warn("mod {}: could not write settings.json", mod.name);
		}

		// The player changed a value or activated an item. Menu mutex held.
		void Changed(Item& item, float value)
		{
			if (item.kind == ItemKind::Action)
			{
				if (item.fn && item.enabled)
					item.owner->pending.emplace_back(item.fn, item.user);
				return;
			}
			const float v = Normalize(item, value);
			if (item.value.exchange(v) == v)
				return;
			if (!item.id.empty())
				item.owner->settingsDirty = true;
			if (item.fn && item.kind != ItemKind::Hotkey)
				item.owner->pending.emplace_back(item.fn, item.user);
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
			.AddListSetting = ApiAddListSetting,
			.AddPage = ApiAddPage,
			.AddToggle = ApiAddToggle,
			.AddNumber = ApiAddNumber,
			.AddList = ApiAddList,
			.AddAction = ApiAddAction,
			.AddText = ApiAddText,
			.AddHotkey = ApiAddHotkey,
			.SetCallback = ApiSetCallback,
			.SetLabel = ApiSetLabel,
			.SetEnabled = ApiSetEnabled,
			.ClearPage = ApiClearPage,
			.GetValue = ApiGetValue,
			.SetValue = ApiSetValue,
			.Notify = ApiNotify,
			.EnumModels = ApiEnumModels,
			.ScriptGlobal = ApiScriptGlobal,
			.EnumScripts = ApiEnumScripts,
			.GetScriptCode = ApiGetScriptCode,
			.RedirectScript = ApiRedirectScript,
			.ScriptNativeIndex = ApiScriptNativeIndex,
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

		bool SafeCallback(MLCallback fn, void* user)
		{
			__try
			{
				fn(user);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// Runs the mod's queued menu/hotkey callbacks one after another, then goes idle.
		void CALLBACK CallbackFiberProc(void* param)
		{
			auto* mod = static_cast<Mod*>(param);
			for (;;)
			{
				std::pair<MLCallback, void*> next{};
				{
					std::lock_guard lock(g_menuMutex);
					if (mod->pending.empty())
						mod->callbackTask.busy = false;
					else
					{
						next = mod->pending.front();
						mod->pending.erase(mod->pending.begin());
					}
				}
				if (!next.first)
				{
					SwitchToFiber(g_schedulerFiber);
					continue;
				}
				if (!SafeCallback(next.first, next.second))
					AbandonCurrentFiber(mod, "crashed in a menu or hotkey callback");
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

			ReadSettings(mod);
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
				std::lock_guard lock(g_menuMutex); // a cancelled mod gets no menu
				Remove(mod.rootItems);
				return;
			}
			{
				std::lock_guard lock(g_menuMutex);
				for (const int32_t h : mod.rootItems)
					if (g_items[h].pauseMenu && mod.pauseItems.size() < ML_MAX_SETTINGS)
						mod.pauseItems.push_back(&g_items[h]);
			}

			if (mod.main)
			{
				mod.mainTask.fiber = CreateFiber(256 * 1024, FiberProc, &mod);
				if (!mod.mainTask.fiber)
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
			if (mod->state != State::Loaded && mod->state != State::Running && mod->state != State::Finished)
				continue;
			if (mod->mainTask.fiber && mod->state != State::Finished && now >= mod->mainTask.wakeAt)
			{
				if (mod->state == State::Loaded)
				{
					std::lock_guard lock(g_modsMutex);
					mod->state = State::Running;
				}
				g_current = mod.get();
				g_currentTask = &mod->mainTask;
				SwitchToFiber(mod->mainTask.fiber);
			}

			// Callbacks run on their own fiber, so a callback that waits does not hold up MLMain (and the other way round).
			bool run = false;
			{
				std::lock_guard lock(g_menuMutex);
				if (!mod->callbackTask.busy && !mod->pending.empty())
					mod->callbackTask.busy = true;
				run = mod->callbackTask.busy && now >= mod->callbackTask.wakeAt;
			}
			if (run && mod->state != State::Faulted)
			{
				if (!mod->callbackTask.fiber)
					mod->callbackTask.fiber = CreateFiber(256 * 1024, CallbackFiberProc, mod.get());
				if (mod->callbackTask.fiber)
				{
					g_current = mod.get();
					g_currentTask = &mod->callbackTask;
					SwitchToFiber(mod->callbackTask.fiber);
				}
			}
			g_current = nullptr;
			g_currentTask = nullptr;

			// Saved at most once per tick, not on every change.
			bool dirty;
			{
				std::lock_guard lock(g_menuMutex);
				dirty = mod->settingsDirty;
			}
			if (dirty)
				SaveSettings(*mod);
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
		{
			std::lock_guard menuLock(g_menuMutex);
			out.push_back({m->fileName, m->name, m->version, m->author, m->description, m->error, m->dir, m->state, !m->rootItems.empty()});
		}
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

	void SetSettingValue(Item& item, int32_t value)
	{
		bool dirty;
		{
			std::lock_guard lock(g_menuMutex);
			Changed(item, static_cast<float>(value));
			dirty = item.owner && item.owner->settingsDirty;
		}
		if (dirty)
			SaveSettings(*item.owner);
	}

	std::vector<ItemView> PageItems(size_t mod, int32_t page)
	{
		std::lock_guard modsLock(g_modsMutex); // g_mods grows while LoadAll runs
		std::lock_guard lock(g_menuMutex);
		std::vector<ItemView> out;
		if (mod >= g_mods.size())
			return out;
		const std::vector<int32_t>* handles = &g_mods[mod]->rootItems;
		if (page != ML_ROOT_PAGE)
		{
			const Item* p = Get(page);
			if (!p || p->owner != g_mods[mod].get() || p->kind != ItemKind::Page)
				return out;
			handles = &p->children;
		}
		out.reserve(handles->size());
		for (const int32_t h : *handles)
		{
			const Item& i = g_items[h];
			out.push_back({h, i.kind, i.enabled, i.label, i.value.load(), i.min, i.max, i.step, i.options});
		}
		return out;
	}

	bool PageExists(size_t mod, int32_t page)
	{
		std::lock_guard modsLock(g_modsMutex); // g_mods grows while LoadAll runs
		std::lock_guard lock(g_menuMutex);
		const Item* p = Get(page);
		return mod < g_mods.size() && p && p->owner == g_mods[mod].get() && p->kind == ItemKind::Page;
	}

	void UiSetValue(int32_t handle, float value)
	{
		std::lock_guard lock(g_menuMutex);
		if (Item* item = Get(handle); item && item->enabled)
			Changed(*item, value);
	}

	void UiActivate(int32_t handle)
	{
		std::lock_guard lock(g_menuMutex);
		if (Item* item = Get(handle); item && item->kind == ItemKind::Action)
			Changed(*item, 0);
	}

	void OnKeyDown(uint32_t vk)
	{
		std::lock_guard lock(g_menuMutex);
		for (int32_t h = 0; h < g_itemCount; ++h)
			if (Item* item = Get(h); item && item->kind == ItemKind::Hotkey && item->enabled && item->fn && item->IntValue() == static_cast<int32_t>(vk))
				item->owner->pending.emplace_back(item->fn, item->user);
	}
}
