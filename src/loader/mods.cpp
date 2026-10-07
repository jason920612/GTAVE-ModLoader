#include "web/browser.hpp"
#include "mods.hpp"

#include <intrin.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <tuple>
#include <fstream>
#include <format>
#include <mutex>
#include <set>

#include "config.hpp"
#include "log.hpp"
#include "game/models.hpp"
#include "convert/resource.hpp"
#include "game/scripts.hpp"
#include "game/story.hpp"
#include <modloader/script.hpp>
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

		int32_t ApiOverrideScriptNative(const char* script, uint64_t hash, void (*fn)(MLNativeCall*, void*), void* user)
		{
			Mod* mod = g_loading ? g_loading : g_current;
			if (!mod || GetCurrentThreadId() != g_gameThreadId || !script || !fn)
			{
				ModLog(ModFromAddress(_ReturnAddress()), ML_LOG_ERROR, "OverrideScriptNative called outside MLOnLoad / MLMain; ignored");
				return 0;
			}
			auto* handler = reinterpret_cast<void*>(game::natives::FindHandler(hash));
			if (!handler)
			{
				ModLog(mod, ML_LOG_ERROR, std::format("OverrideScriptNative: native {:016X} is not known", hash));
				return 0;
			}
			auto id = std::make_shared<int32_t>(0);
			*id = game::scripts::AddNativeOverride(convert::Joaat(script), handler, [mod, fn, user, id](void* context, void*) {
				auto* ctx = static_cast<game::natives::CallContext*>(context);
				MLNativeCall call{ctx->returnValue, ctx->argCount, ctx->args, context, *id};
				Mod* previous = g_current;
				g_current = mod; // natives called from the override run as this mod's
				fn(&call, user);
				g_current = previous;
			});
			if (!*id)
				ModLog(mod, ML_LOG_ERROR, "OverrideScriptNative: too many overrides");
			else
				ModLog(mod, ML_LOG_INFO, std::format("native {:016X} overridden for script {}", hash, script));
			return *id;
		}

		void ApiCallOriginalNative(MLNativeCall* call)
		{
			if (!call)
				return;
			game::scripts::CallOriginal(call->override, call->context);
		}

		void ApiRemoveScriptNativeOverride(int32_t id)
		{
			game::scripts::RemoveNativeOverride(id);
		}

		// ---- in-game web browser ---------------------------------------------------------------

		struct WebFunction
		{
			Mod* mod;
			MLWebFunction fn;
			void* user;
		};
		std::map<std::string, WebFunction> g_webFunctions; // menu mutex

		// One page call waiting on its mod's callback fiber.
		struct WebCall
		{
			WebFunction function;
			std::string args;
			int64_t id;
		};
		void RunWebCall(void* param)
		{
			std::unique_ptr<WebCall> call(static_cast<WebCall*>(param));
			const char* result = call->function.fn(call->args.c_str(), call->function.user);
			web::Respond(call->id, true, result);
		}

		int32_t ApiRegisterWebFunction(const char* name, MLWebFunction fn, void* user)
		{
			Mod* mod = g_loading ? g_loading : g_current;
			if (!mod || GetCurrentThreadId() != g_gameThreadId || !name || !*name || !fn)
			{
				ModLog(ModFromAddress(_ReturnAddress()), ML_LOG_ERROR, "RegisterWebFunction called outside MLOnLoad / MLMain; ignored");
				return 0;
			}
			std::lock_guard lock(g_menuMutex);
			if (!g_webFunctions.emplace(name, WebFunction{mod, fn, user}).second)
			{
				ModLog(mod, ML_LOG_ERROR, std::format("RegisterWebFunction: \"{}\" is already registered", name));
				return 0;
			}
			return 1;
		}

		void ApiWebEmit(const char* event, const char* json)
		{
			if (event)
				web::Emit(event, json);
		}

		void ApiOpenBrowser(const char* url)
		{
			web::Open(url ? url : "");
		}

		int32_t ApiCallScriptFunction(int32_t id, uint32_t address, const int64_t* args, int32_t count, uint32_t returnTo)
		{
			if (!InModFiber("CallScriptFunction", _ReturnAddress()) || count < 0 || (count && !args))
				return 0;
			std::string error;
			const bool ok = game::scripts::RedirectThread(id, address, {args, static_cast<size_t>(count)}, game::scripts::Redirect::Call, error, returnTo);
			if (!ok)
				ModLog(g_current, ML_LOG_WARN, std::format("CallScriptFunction({}, {}): {}", id, address, error));
			return ok ? 1 : 0;
		}

		int64_t* ApiScriptStatic(int32_t id, uint32_t index)
		{
			if (!InModFiber("ScriptStatic", _ReturnAddress()))
				return nullptr;
			return game::scripts::Static(id, index);
		}

		// ---- high-level API: tasks ------------------------------------------------------------

		bool SafeCallback(MLCallback fn, void* user);

		std::mutex g_taskMutex;
		std::set<int32_t> g_runningTasks; // task mutex
		int32_t g_nextTaskId = 1;         // game thread

		void CALLBACK TaskFiberProc(void* param)
		{
			auto* task = static_cast<Task*>(param);
			if (!SafeCallback(task->fn, task->user))
				AbandonCurrentFiber(task->mod, "crashed in a task");
			task->done = true;
			{
				std::lock_guard lock(g_taskMutex);
				g_runningTasks.erase(task->id);
			}
			for (;;)
				SwitchToFiber(g_schedulerFiber);
		}

		int32_t ApiStartTask(MLCallback fn, void* user)
		{
			Mod* mod = Registrar("StartTask", _ReturnAddress());
			if (!mod || !fn)
				return 0;
			auto task = std::make_unique<Task>();
			task->mod = mod;
			task->fn = fn;
			task->user = user;
			task->id = g_nextTaskId++;
			task->fiber = CreateFiber(256 * 1024, TaskFiberProc, task.get());
			if (!task->fiber)
			{
				ModLog(mod, ML_LOG_ERROR, "StartTask: could not create a fiber");
				return 0;
			}
			{
				std::lock_guard lock(g_taskMutex);
				g_runningTasks.insert(task->id);
			}
			const int32_t id = task->id;
			mod->tasks.push_back(std::move(task));
			return id;
		}

		int32_t ApiTaskRunning(int32_t id)
		{
			std::lock_guard lock(g_taskMutex);
			return g_runningTasks.contains(id) ? 1 : 0;
		}

		// Runs the mod's tasks that are due, then forgets the finished ones. Game thread.
		void RunTasks(Mod& mod, uint64_t now)
		{
			// By index: a task may start another one.
			for (size_t i = 0; i < mod.tasks.size() && mod.state != State::Faulted; ++i)
			{
				Task* task = mod.tasks[i].get();
				if (task->done || now < task->wakeAt)
					continue;
				g_current = &mod;
				g_currentTask = task;
				SwitchToFiber(task->fiber);
			}
			g_current = nullptr;
			g_currentTask = nullptr;
			if (mod.state == State::Faulted)
			{
				std::lock_guard lock(g_taskMutex);
				for (const auto& task : mod.tasks)
					g_runningTasks.erase(task->id);
				return;
			}
			std::erase_if(mod.tasks, [](const std::unique_ptr<Task>& task) {
				if (!task->done)
					return false;
				DeleteFiber(task->fiber);
				return true;
			});
		}

		// Parks the calling mod fiber for at least `ms`.
		void FiberSleep(uint32_t ms)
		{
			g_currentTask->wakeAt = NowMs() + ms;
			SwitchToFiber(g_schedulerFiber);
		}

		// ---- high-level API: the story game ---------------------------------------------------

		int32_t ApiRequestAutosave()
		{
			if (!InModFiber("RequestAutosave", _ReturnAddress()))
				return 0;
			return game::story::RequestAutosave() ? 1 : 0;
		}

		int32_t ApiOnGameEvent(int32_t event, MLCallback fn, void* user)
		{
			Mod* mod = g_loading ? g_loading : g_current;
			if (!mod || GetCurrentThreadId() != g_gameThreadId || !fn)
			{
				ModLog(ModFromAddress(_ReturnAddress()), ML_LOG_ERROR, "OnGameEvent called outside MLOnLoad / MLMain; ignored");
				return 0;
			}
			if (event < ML_EVENT_CHARACTER_CHANGED || event > ML_EVENT_SAVE_LOADING)
			{
				ModLog(mod, ML_LOG_ERROR, std::format("OnGameEvent: unknown event {}", event));
				return 0;
			}
			std::lock_guard lock(g_menuMutex);
			mod->events.push_back({event, {fn, user}});
			return 1;
		}

		// Queues the mods' handlers of `event` on their callback fibers. Game thread.
		void Dispatch(int32_t event)
		{
			std::lock_guard lock(g_menuMutex);
			for (auto& mod : g_mods)
				if (mod->state != State::Faulted)
					for (const auto& [e, handler] : mod->events)
						if (e == event)
							mod->pending.push_back(handler);
		}

		// ---- high-level API: data that follows the game's save -----------------------------

		std::mutex g_saveMutex;

		std::filesystem::path SaveFile(const Mod& mod)
		{
			return mod.dir / L"data" / L"save.json";
		}

		// Save mutex held.
		void ReadSaveData(Mod& mod)
		{
			if (mod.saveRead)
				return;
			mod.saveRead = true;
			if (std::ifstream in(SaveFile(mod)); in)
			{
				auto j = nlohmann::json::parse(in, nullptr, false);
				if (j.is_object())
					mod.saveCommitted = std::move(j);
				else
					ModLog(&mod, ML_LOG_WARN, "data\\save.json is not valid JSON; starting empty");
			}
			if (!mod.saveCommitted.is_object())
				mod.saveCommitted = nlohmann::json::object();
			mod.saveWorking = mod.saveCommitted;
		}

		std::string SlotKey(int32_t slot)
		{
			return slot < 0 ? "shared" : std::to_string(slot);
		}

		int32_t ApiSaveDataGet(int32_t slot, const char* key, char* buffer, int32_t size)
		{
			Mod* mod = CallerOrCurrent(_ReturnAddress());
			if (!mod || !key || slot < ML_SAVE_SHARED || slot > 2)
				return -1;
			std::string text;
			{
				std::lock_guard lock(g_saveMutex);
				ReadSaveData(*mod);
				const auto group = mod->saveWorking.find(SlotKey(slot));
				if (group == mod->saveWorking.end() || !group->is_object())
					return -1;
				const auto value = group->find(key);
				if (value == group->end())
					return -1;
				text = value->dump();
			}
			if (buffer && size > 0)
			{
				const size_t n = std::min(text.size(), static_cast<size_t>(size - 1));
				std::memcpy(buffer, text.data(), n);
				buffer[n] = 0;
			}
			return static_cast<int32_t>(text.size());
		}

		int32_t ApiSaveDataSet(int32_t slot, const char* key, const char* json)
		{
			Mod* mod = CallerOrCurrent(_ReturnAddress());
			if (!mod || !key || slot < ML_SAVE_SHARED || slot > 2)
				return 0;
			nlohmann::json value;
			if (json)
			{
				value = nlohmann::json::parse(json, nullptr, false);
				if (value.is_discarded())
				{
					ModLog(mod, ML_LOG_ERROR, std::format("SaveDataSet(\"{}\"): not valid JSON", key));
					return 0;
				}
			}
			std::lock_guard lock(g_saveMutex);
			ReadSaveData(*mod);
			auto& group = mod->saveWorking[SlotKey(slot)];
			if (!group.is_object())
				group = nlohmann::json::object();
			if (json)
				group[key] = std::move(value);
			else
				group.erase(key);
			mod->saveDirty = true;
			return 1;
		}

		// The game saved: the mods' save data is written. Game thread.
		void CommitSaveData()
		{
			for (auto& mod : g_mods)
			{
				std::lock_guard lock(g_saveMutex);
				if (!mod->saveDirty)
					continue;
				std::ofstream out(SaveFile(*mod), std::ios::trunc);
				if (!out)
				{
					ModLog(mod.get(), ML_LOG_ERROR, "could not write data\\save.json");
					continue;
				}
				out << mod->saveWorking.dump(1) << "\n";
				mod->saveCommitted = mod->saveWorking;
				mod->saveDirty = false;
				ModLog(mod.get(), ML_LOG_INFO, "game saved: save data written");
			}
		}

		// A save is being loaded: unsaved changes are dropped. Game thread.
		void DropSaveData()
		{
			for (auto& mod : g_mods)
			{
				std::lock_guard lock(g_saveMutex);
				if (!mod->saveDirty)
					continue;
				mod->saveWorking = mod->saveCommitted;
				mod->saveDirty = false;
				ModLog(mod.get(), ML_LOG_INFO, "save loading: unsaved save data dropped");
			}
		}

		// ---- high-level API: game script functions -----------------------------------------

		constexpr uint64_t kDoesScriptExist = 0xFC04745FBE67C19A;
		constexpr uint64_t kRequestScript = 0x6EB5F71AA68F2E8E;
		constexpr uint64_t kHasScriptLoaded = 0xE6CC9F3BA0FB9EF1;
		constexpr uint64_t kStartNewScript = 0xE81651AD79516E48;
		constexpr uint64_t kSetScriptAsNoLongerNeeded = 0xC90D2DCACD56184C;
		constexpr uint64_t kIsThreadActive = 0x46E9AE36D8FA6417;
		constexpr uint64_t kTerminateThread = 0xC8B189ED9138BCD4;
		constexpr uint64_t kTerminateThisThread = 0x1090044AD1DA76FA;

		// "2d 04 ?? 00" -> bytes, -1 = any. Empty when malformed.
		std::vector<int> ParsePattern(std::string_view text)
		{
			std::vector<int> out;
			for (size_t i = 0; i < text.size();)
			{
				if (text[i] == ' ')
				{
					++i;
					continue;
				}
				if (i + 1 >= text.size())
					return {};
				const std::string_view byte = text.substr(i, 2);
				if (byte == "??")
					out.push_back(-1);
				else
				{
					int v = 0;
					if (std::from_chars(byte.data(), byte.data() + 2, v, 16).ptr != byte.data() + 2)
						return {};
					out.push_back(v);
				}
				i += 2;
			}
			return out;
		}

		int64_t FindPattern(const std::vector<uint8_t>& code, const std::vector<int>& pattern)
		{
			if (pattern.empty() || code.size() < pattern.size())
				return -1;
			for (size_t a = 0; a + pattern.size() <= code.size(); ++a)
			{
				size_t k = 0;
				while (k < pattern.size() && (pattern[k] < 0 || code[a + k] == pattern[k]))
					++k;
				if (k == pattern.size())
					return static_cast<int64_t>(a);
			}
			return -1;
		}

		// Address of a NATIVE TERMINATE_THIS_THREAD instruction in the thread's program, or -1.
		int64_t FindTerminate(int32_t thread, const std::vector<uint8_t>& code)
		{
			const int32_t index = game::scripts::NativeIndex(thread, reinterpret_cast<const void*>(game::natives::FindHandler(kTerminateThisThread)));
			if (index < 0)
				return -1;
			for (uint32_t a = 0; a < code.size();)
			{
				const auto in = ml::script::Decode(code, a);
				if (!in.length)
				{
					++a;
					continue;
				}
				if (in.op == ml::script::NATIVE && in.operand == index && code[a + 1] == 0)
					return a;
				a += in.length;
			}
			return -1;
		}

		int32_t ApiRunScriptFunction(const char* script, const char* pattern, int32_t stackSize, const int64_t* args, int32_t count, uint32_t timeoutMs)
		{
			if (!InModFiber("RunScriptFunction", _ReturnAddress()) || !script || !pattern || count < 0 || (count && !args))
				return 0;
			Mod* mod = g_current;
			const auto bytes = ParsePattern(pattern);
			if (bytes.empty())
			{
				ModLog(mod, ML_LOG_ERROR, std::format("RunScriptFunction: bad pattern \"{}\"", pattern));
				return 0;
			}
			if (!game::natives::Invoke<int32_t>(kDoesScriptExist, script))
			{
				ModLog(mod, ML_LOG_WARN, std::format("RunScriptFunction: no script \"{}\"", script));
				return 0;
			}
			const uint64_t deadline = NowMs() + timeoutMs;
			game::natives::Invoke<void>(kRequestScript, script);
			while (!game::natives::Invoke<int32_t>(kHasScriptLoaded, script))
			{
				if (NowMs() >= deadline)
				{
					ModLog(mod, ML_LOG_WARN, std::format("RunScriptFunction: {} did not load in time", script));
					return -1;
				}
				FiberSleep(0);
			}
			const int32_t thread = game::natives::Invoke<int32_t>(kStartNewScript, script, stackSize > 0 ? stackSize : 1024);
			game::natives::Invoke<void>(kSetScriptAsNoLongerNeeded, script);
			if (!thread)
			{
				ModLog(mod, ML_LOG_WARN, std::format("RunScriptFunction: could not start {}", script));
				return 0;
			}
			const auto code = game::scripts::Code(thread);
			const int64_t address = FindPattern(code, bytes);
			const int64_t terminate = address >= 0 ? FindTerminate(thread, code) : -1;
			std::string error = address < 0 ? "pattern not found" : terminate < 0 ? "no TERMINATE_THIS_THREAD" : "";
			if (error.empty() &&
			    !game::scripts::RedirectThread(thread, static_cast<uint32_t>(address), {args, static_cast<size_t>(count)}, game::scripts::Redirect::Call,
			        error, static_cast<uint32_t>(terminate)))
				error = "redirect: " + error;
			if (!error.empty())
			{
				game::natives::Invoke<void>(kTerminateThread, thread);
				ModLog(mod, ML_LOG_WARN, std::format("RunScriptFunction({}, \"{}\"): {}", script, pattern, error));
				return 0;
			}
			while (game::natives::Invoke<int32_t>(kIsThreadActive, thread))
			{
				if (NowMs() >= deadline)
				{
					game::natives::Invoke<void>(kTerminateThread, thread);
					ModLog(mod, ML_LOG_WARN, std::format("RunScriptFunction({}): the function did not return in time", script));
					return -1;
				}
				FiberSleep(0);
			}
			return 1;
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
			.OverrideScriptNative = ApiOverrideScriptNative,
			.CallOriginalNative = ApiCallOriginalNative,
			.RemoveScriptNativeOverride = ApiRemoveScriptNativeOverride,
			.ScriptStatic = ApiScriptStatic,
			.RegisterWebFunction = ApiRegisterWebFunction,
			.WebEmit = ApiWebEmit,
			.OpenBrowser = ApiOpenBrowser,
			.CallScriptFunction = ApiCallScriptFunction,
			.StartTask = ApiStartTask,
			.TaskRunning = ApiTaskRunning,
			.RequestAutosave = ApiRequestAutosave,
			.OnGameEvent = ApiOnGameEvent,
			.SaveDataGet = ApiSaveDataGet,
			.SaveDataSet = ApiSaveDataSet,
			.RunScriptFunction = ApiRunScriptFunction,
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
		// Story state for the high-level API: save data follows the game's saves, events go to the callback fibers.
		const auto changes = game::story::Update();
		if (changes.saved)
		{
			CommitSaveData();
			Dispatch(ML_EVENT_GAME_SAVED);
		}
		if (changes.loading)
		{
			DropSaveData();
			Dispatch(ML_EVENT_SAVE_LOADING);
		}
		if (changes.character)
			Dispatch(ML_EVENT_CHARACTER_CHANGED);

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
			if (mod->state != State::Faulted)
				RunTasks(*mod, now);

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

	bool CallWebFunction(const std::string& name, const std::string& args, int64_t id)
	{
		std::lock_guard lock(g_menuMutex);
		const auto it = g_webFunctions.find(name);
		if (it == g_webFunctions.end() || it->second.mod->state == State::Faulted)
			return false;
		it->second.mod->pending.emplace_back(&RunWebCall, new WebCall{it->second, args, id});
		return true;
	}
}
