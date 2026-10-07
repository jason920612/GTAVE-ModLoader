// C++ convenience layer over modloader.h. Include this (and natives.hpp) in your mod.
// High-level parts: ml::StartTask / WaitUntil, ml::save (data that follows the game's save), ml::Global,
// ml::scripts::RunFunction, typed ml::web::Function, ml::Json (json.hpp); game.hpp adds ml::game and streaming.
// The ml::scripts calls that take raw addresses (Code, Redirect, CallFunction, Static, NativeIndex) are advanced.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <format>
#include <functional>
#include <initializer_list>
#include <memory>
#include <tuple>
#include <utility>
#include <string>
#include <type_traits>
#include <vector>

#include "modloader.h"
#include "json.hpp"

namespace ml
{
	// Layout matches the game's scrVector (each component padded to 8 bytes).
	struct alignas(8) Vector3
	{
		float x;
		uint32_t _padX;
		float y;
		uint32_t _padY;
		float z;
		uint32_t _padZ;

		constexpr Vector3() : x(0), _padX(0), y(0), _padY(0), z(0), _padZ(0) {}
		constexpr Vector3(float x_, float y_, float z_) : x(x_), _padX(0), y(y_), _padY(0), z(z_), _padZ(0) {}
	};
	static_assert(sizeof(Vector3) == 24);

	namespace detail
	{
		inline const MLApi* g_api = nullptr;
		inline MLContext g_ctx{};

		template<class T>
		uint64_t ToSlot(T value)
		{
			static_assert(sizeof(T) <= sizeof(uint64_t), "native arguments must fit in 8 bytes");
			uint64_t slot = 0;
			std::memcpy(&slot, &value, sizeof(T));
			return slot;
		}

		template<class T>
		void Push(const T& value)
		{
			g_api->NativePush(ToSlot(value));
		}

		// Natives take a vector argument as three consecutive float slots.
		inline void Push(const Vector3& v)
		{
			g_api->NativePush(ToSlot(v.x));
			g_api->NativePush(ToSlot(v.y));
			g_api->NativePush(ToSlot(v.z));
		}
	}

	inline const MLApi& Api() { return *detail::g_api; }
	inline const MLContext& Context() { return detail::g_ctx; }

	// Call this first in MLOnLoad.
	inline void Init(const MLApi* api, const MLContext* ctx)
	{
		detail::g_api = api;
		detail::g_ctx = *ctx;
	}

	template<class Ret = void, class... Args>
	Ret Invoke(uint64_t hash, Args... args)
	{
		const auto& api = Api();
		api.NativeBegin(hash);
		(detail::Push(args), ...);
		uint64_t* result = api.NativeCall();
		if constexpr (!std::is_void_v<Ret>)
		{
			Ret value;
			std::memcpy(&value, result, sizeof(Ret));
			return value;
		}
	}

	inline void Wait(uint32_t ms) { Api().Wait(ms); }

	// True when the loader supports settings (AddSetting/GetSetting).
	inline bool HasSettings() { return Api().size >= offsetof(MLApi, GetSetting) + sizeof(void*); }

	// A value the player changes in the pause menu ("Mods" tab). Cheap to read every frame.
	class Setting
	{
	public:
		Setting() = default;
		explicit Setting(int32_t handle) : m_handle(handle) {}
		bool Valid() const { return m_handle >= 0; }
		int32_t Value() const { return Valid() ? Api().GetSetting(m_handle) : 0; }
		explicit operator bool() const { return Value() != 0; }

	private:
		int32_t m_handle = -1;
	};

	// Call in MLOnLoad (after Init). On/off switch.
	inline Setting AddToggle(const char* id, const char* label, bool defaultValue = false)
	{
		return Setting(HasSettings() ? Api().AddSetting(ML_SETTING_TOGGLE, id, label, defaultValue ? 1 : 0) : -1);
	}
	// True when the loader supports list settings (AddListSetting).
	inline bool HasListSettings() { return Api().size >= offsetof(MLApi, AddListSetting) + sizeof(void*); }

	// Call in MLOnLoad (after Init). The player picks one of `options`; Value() is its index.
	//   auto difficulty = ml::AddList("difficulty", "難度", {"簡單", "普通", "困難"}, 1);
	inline Setting AddList(const char* id, const char* label, std::initializer_list<const char*> options, int32_t defaultValue = 0)
	{
		if (!HasListSettings())
			return Setting();
		return Setting(Api().AddListSetting(id, label, options.begin(), static_cast<int32_t>(options.size()), defaultValue));
	}

	// Call in MLOnLoad (after Init). Slider with values 0..10.
	inline Setting AddSlider(const char* id, const char* label, int32_t defaultValue = 5)
	{
		return Setting(HasSettings() ? Api().AddSetting(ML_SETTING_SLIDER, id, label, defaultValue) : -1);
	}
	inline uint64_t TickMs() { return Api().GetTickMs(); }

	// ---- menus (loader window "模組功能" tab) ----------------------------------------------------

	// True when the loader supports menus (pages, actions, hotkeys, notifications).
	inline bool HasMenus() { return Api().size >= offsetof(MLApi, Notify) + sizeof(void*); }

	namespace detail
	{
		// Callbacks stay alive for the whole session (items may be removed while one is queued).
		inline void* Keep(std::function<void()> fn) { return new std::function<void()>(std::move(fn)); }
		inline void Run(void* user) { (*static_cast<std::function<void()>*>(user))(); }
	}

	// A menu entry. Value() is cheap and works from any thread.
	class Item
	{
	public:
		Item() = default;
		explicit Item(int32_t handle) : m_handle(handle) {}
		bool Valid() const { return m_handle >= 0; }
		int32_t Handle() const { return m_handle; }
		float Value() const { return Valid() ? Api().GetValue(m_handle) : 0.0f; }
		int32_t Int() const { return static_cast<int32_t>(Value()); }
		explicit operator bool() const { return Value() != 0; }
		// Sets the value (no OnChange callback).
		void Set(float value) const { if (Valid()) Api().SetValue(m_handle, value); }
		void Label(const std::string& label) const { if (Valid()) Api().SetLabel(m_handle, label.c_str()); }
		void Enabled(bool enabled) const { if (Valid()) Api().SetEnabled(m_handle, enabled ? 1 : 0); }
		// Runs `fn` on the mod's script fiber when the player changes the value.
		const Item& OnChange(std::function<void()> fn) const
		{
			if (Valid())
				Api().SetCallback(m_handle, detail::Run, detail::Keep(std::move(fn)));
			return *this;
		}

	private:
		int32_t m_handle = -1;
	};

	// A menu page. Root() is the mod's top-level page; Sub() adds a page that opens from this one.
	//   auto vehicles = ml::Root().Sub("載具");
	//   vehicles.Action("修理", [] { ... });   // runs on the mod's script fiber: natives and Wait are fine
	class Page
	{
	public:
		Page() = default;
		explicit Page(int32_t handle) : m_handle(handle), m_valid(true) {}
		bool Valid() const { return m_valid && HasMenus(); }
		int32_t Handle() const { return m_handle; }

		Page Sub(const char* label) const
		{
			const int32_t h = Valid() ? Api().AddPage(m_handle, label) : -1;
			return h >= 0 ? Page(h) : Page();
		}
		Item Toggle(const char* id, const char* label, bool defaultValue = false) const
		{
			return Item(Valid() ? Api().AddToggle(m_handle, id, label, defaultValue ? 1 : 0) : -1);
		}
		Item Number(const char* id, const char* label, float min, float max, float step, float defaultValue) const
		{
			return Item(Valid() ? Api().AddNumber(m_handle, id, label, min, max, step, defaultValue) : -1);
		}
		Item List(const char* id, const char* label, const std::vector<std::string>& options, int32_t defaultValue = 0) const
		{
			if (!Valid())
				return Item();
			std::vector<const char*> texts;
			for (const auto& o : options)
				texts.push_back(o.c_str());
			return Item(Api().AddList(m_handle, id, label, texts.data(), static_cast<int32_t>(texts.size()), defaultValue));
		}
		Item Action(const char* label, std::function<void()> fn) const
		{
			return Item(Valid() ? Api().AddAction(m_handle, label, detail::Run, detail::Keep(std::move(fn))) : -1);
		}
		Item Text(const char* label) const { return Item(Valid() ? Api().AddText(m_handle, label) : -1); }
		// Removes every item on the page (for lists that change). On Root() settings and hotkeys stay.
		void Clear() const
		{
			if (Valid())
				Api().ClearPage(m_handle);
		}

	private:
		int32_t m_handle = ML_ROOT_PAGE;
		bool m_valid = false;
	};

	inline Page Root() { return Page(ML_ROOT_PAGE); }

	// A key the player can rebind in the loader window; `fn` runs on the mod's script fiber when it is pressed.
	// `defaultKey` is a Windows virtual-key code, e.g. VK_F6 (0x75); 0 = unbound.
	inline Item Hotkey(const char* id, const char* label, uint32_t defaultKey, std::function<void()> fn)
	{
		return Item(HasMenus() ? Api().AddHotkey(id, label, defaultKey, detail::Run, detail::Keep(std::move(fn))) : -1);
	}

	// True when the loader can list models (EnumModels).
	inline bool HasModels() { return Api().size >= offsetof(MLApi, EnumModels) + sizeof(void*); }

	struct Model
	{
		uint32_t hash;
		std::string name; // empty when unknown
		std::string pack; // ModLoader\mods pack folder, empty for the game's own
	};

	// Every vehicle or ped model, add-on packs included. MLMain or a callback only. Empty while the
	// loader is still reading the model names (`ready` = false): try again a bit later.
	inline std::vector<Model> Models(MLModelType type, bool* ready = nullptr)
	{
		std::vector<Model> out;
		int32_t n = HasModels() ? Api().EnumModels(type, [](uint32_t hash, const char* name, const char* pack, void* user) {
			static_cast<std::vector<Model>*>(user)->push_back({hash, name ? name : "", pack ? pack : ""});
		}, &out) : -1;
		if (ready)
			*ready = n >= 0;
		return out;
	}

	// ---- game scripts (advanced) ----
	namespace scripts
	{
		inline bool Available() { return Api().size >= offsetof(MLApi, ScriptNativeIndex) + sizeof(void*); }

		// Script global `index`, or nullptr. MLMain or a callback only.
		inline int64_t* Global(uint32_t index) { return Available() ? Api().ScriptGlobal(index) : nullptr; }

		struct Thread
		{
			int32_t id;
			std::string name;
		};
		inline std::vector<Thread> Threads()
		{
			std::vector<Thread> out;
			if (Available())
				Api().EnumScripts([](int32_t id, const char* name, void* user) { static_cast<std::vector<Thread>*>(user)->push_back({id, name}); }, &out);
			return out;
		}
		inline std::vector<uint8_t> Code(int32_t id)
		{
			std::vector<uint8_t> code;
			if (!Available())
				return code;
			code.resize(static_cast<size_t>((std::max)(0, Api().GetScriptCode(id, nullptr, 0))));
			if (!code.empty())
				Api().GetScriptCode(id, code.data(), static_cast<int32_t>(code.size()));
			return code;
		}
		// See MLApi::RedirectScript.
		inline bool Redirect(int32_t id, uint32_t address, std::initializer_list<int64_t> args = {}, bool mainFrame = false)
		{
			return Available() && Api().RedirectScript(id, address, args.begin(), static_cast<int32_t>(args.size()), mainFrame ? 1 : 0) != 0;
		}
	}

	namespace scripts
	{
		// Index of a native in the thread's program (see MLApi::ScriptNativeIndex).
		inline int32_t NativeIndex(int32_t id, uint64_t hash) { return Available() ? Api().ScriptNativeIndex(id, hash) : -1; }

		// Runs `address` (a function's ENTER) in thread `id` from its next update, returning to `returnTo`
		// (see MLApi::CallScriptFunction).
		inline bool CallFunction(int32_t id, uint32_t address, std::initializer_list<int64_t> args, uint32_t returnTo)
		{
			if (Api().size < offsetof(MLApi, CallScriptFunction) + sizeof(void*))
				return false;
			const std::vector<int64_t> a(args);
			return Api().CallScriptFunction(id, address, a.data(), static_cast<int32_t>(a.size()), returnTo) != 0;
		}

		// Static variable of a running script thread (see MLApi::ScriptStatic), or nullptr.
		inline int64_t* Static(int32_t id, uint32_t index)
		{
			return Api().size >= offsetof(MLApi, ScriptStatic) + sizeof(void*) ? Api().ScriptStatic(id, index) : nullptr;
		}

		inline bool OverridesAvailable() { return Api().size >= offsetof(MLApi, RemoveScriptNativeOverride) + sizeof(void*); }

		// One call of an overridden native: arguments in, result out, and the game's own handler.
		struct NativeCall
		{
			MLNativeCall* raw;
			template<class T = int64_t>
			T Arg(uint32_t i) const
			{
				T v{};
				if (i < raw->argCount)
					std::memcpy(&v, &raw->args[i], sizeof(T));
				return v;
			}
			template<class T>
			void Return(T value)
			{
				std::memcpy(raw->result, &value, sizeof(T) < 8 ? sizeof(T) : 8);
			}
			void CallOriginal() { Api().CallOriginalNative(raw); }
		};

		// Replaces `hash` for the game script `script` (see MLApi::OverrideScriptNative). The function lives as long as
		// the mod. Returns the override id, or 0.
		inline int32_t OverrideNative(const char* script, uint64_t hash, std::function<void(NativeCall&)> fn)
		{
			if (!OverridesAvailable())
				return 0;
			auto* stored = new std::function<void(NativeCall&)>(std::move(fn));
			return Api().OverrideScriptNative(script, hash, [](MLNativeCall* call, void* user) {
				NativeCall c{call};
				(*static_cast<std::function<void(NativeCall&)>*>(user))(c);
			}, stored);
		}
		inline void RemoveOverride(int32_t id)
		{
			if (OverridesAvailable())
				Api().RemoveScriptNativeOverride(id);
		}
	}

	// In-game web browser: functions pages can call, events pages can listen to (see MLApi::RegisterWebFunction).
	namespace web
	{
		inline bool Available() { return Api().size >= offsetof(MLApi, OpenBrowser) + sizeof(void*); }

		// Makes `name` callable from pages as game.call(name, ...args). `fn` gets the arguments as JSON text (an array)
		// and returns the result as JSON text ("" = null). Runs on the mod's callback fiber (natives and Wait allowed).
		inline bool Function(const char* name, std::function<std::string(const std::string& args)> fn)
		{
			if (!Available())
				return false;
			struct Holder
			{
				std::function<std::string(const std::string&)> fn;
				std::string result;
			};
			auto* holder = new Holder{std::move(fn), {}};
			return Api().RegisterWebFunction(name, [](const char* args, void* user) -> const char* {
				auto* h = static_cast<Holder*>(user);
				h->result = h->fn(args ? args : "[]");
				return h->result.empty() ? nullptr : h->result.c_str();
			}, holder) != 0;
		}
		// Sends `event` with `json` data ("" = null) to the open page.
		inline void Emit(const char* event, const std::string& json = {})
		{
			if (Available())
				Api().WebEmit(event, json.empty() ? nullptr : json.c_str());
		}
		inline void Open(const char* url)
		{
			if (Available())
				Api().OpenBrowser(url);
		}
	}

	// ======== high-level API ==================================================================
	// Prefer these over the advanced script calls above; see also modloader/game.hpp (needs natives.hpp).

	namespace detail
	{
		template<class F>
		bool Has(F MLApi::*field)
		{
			return Api().size >= reinterpret_cast<size_t>(&(static_cast<const MLApi*>(nullptr)->*field)) + sizeof(void*);
		}
	}

	// A piece of work running on its own script fiber (see StartTask).
	class Task
	{
	public:
		Task() = default;
		explicit Task(int32_t id) : m_id(id) {}
		bool Valid() const { return m_id > 0; }
		bool Running() const { return Valid() && Api().TaskRunning(m_id) != 0; }
		// Waits (on the calling fiber) until the task has finished.
		void Join() const
		{
			while (Running())
				Wait(0);
		}

	private:
		int32_t m_id = 0;
	};

	// Runs `fn` on a new script fiber next to MLMain: it may call natives and Wait without holding anything else up.
	//   ml::StartTask([] { auto model = ml::LoadModel(hash); ... });
	inline Task StartTask(std::function<void()> fn)
	{
		if (!detail::Has(&MLApi::StartTask))
			return Task();
		auto* stored = new std::function<void()>(std::move(fn));
		const int32_t id = Api().StartTask([](void* user) {
			std::unique_ptr<std::function<void()>> f(static_cast<std::function<void()>*>(user));
			(*f)();
		}, stored);
		if (!id)
			delete stored;
		return Task(id);
	}

	// Waits (MLMain, a callback or a task) until `done()` is true; false when `timeoutMs` passed first.
	inline bool WaitUntil(const std::function<bool()>& done, uint32_t timeoutMs, uint32_t stepMs = 0)
	{
		const uint64_t end = TickMs() + timeoutMs;
		while (!done())
		{
			if (TickMs() >= end)
				return false;
			Wait(stepMs);
		}
		return true;
	}

	// A data file in the mod's data folder: ml::DataPath("cache.txt").
	inline std::wstring DataPath(std::wstring_view name)
	{
		return std::wstring(Context().dataDir ? Context().dataDir : L"") + std::wstring(name);
	}

	// ---- data that follows the game's save -----------------------------------------------------
	// Values written to data\save.json when the game saves and dropped when a save is loaded, so a mod's progress stays in
	// step with the game's (e.g. a bought property disappears again when the player reloads an older save).
	//   ml::save::Set("owned", owned, character);        ml::Json owned = ml::save::Get("owned", character);
	namespace save
	{
		inline constexpr int32_t Shared = ML_SAVE_SHARED; // one value for all characters; or 0 Michael, 1 Franklin, 2 Trevor

		inline bool Available() { return detail::Has(&MLApi::SaveDataSet); }

		// Null when unset.
		inline Json Get(const char* key, int32_t slot = Shared)
		{
			if (!Available())
				return Json();
			const int32_t n = Api().SaveDataGet(slot, key, nullptr, 0);
			if (n < 0)
				return Json();
			std::string text(static_cast<size_t>(n) + 1, '\0');
			Api().SaveDataGet(slot, key, text.data(), n + 1);
			text.resize(static_cast<size_t>(n));
			return Json::Parse(text);
		}
		inline bool Set(const char* key, const Json& value, int32_t slot = Shared)
		{
			return Available() && Api().SaveDataSet(slot, key, value.Dump().c_str()) != 0;
		}
		inline void Remove(const char* key, int32_t slot = Shared)
		{
			if (Available())
				Api().SaveDataSet(slot, key, nullptr);
		}
	}

	// ---- script globals ------------------------------------------------------------------------
	// A script global by index, with offsets and arrays instead of raw pointer arithmetic:
	//   ml::Global gens = ml::Global(114990) + 32759;   // a field 32759 slots in
	//   bool owned = gens.At(13).Bit(5);                  // element 13 of the array there (size slot first)
	//   std::string name = ml::Global(1312440).At(id, 1951).Field(16).Text();
	// Reads give 0 / false / "" when the global's block is not allocated. MLMain, a callback or a task.
	class Global
	{
	public:
		explicit Global(uint32_t index) : m_index(index) {}
		uint32_t Index() const { return m_index; }
		Global Field(int32_t offset) const { return Global(m_index + offset); }
		Global operator+(int32_t offset) const { return Field(offset); }
		// Element `i` of a script array starting here (one size slot, then `elementSize` slots per element).
		Global At(int32_t i, int32_t elementSize = 1) const { return Global(m_index + 1 + i * elementSize); }
		// Element count of a script array starting here.
		int32_t Size() const { return Int(); }

		int64_t* Ptr() const { return scripts::Global(m_index); }
		bool Valid() const { return Ptr() != nullptr; }
		int32_t Int() const { return Read<int32_t>(); }
		int64_t Int64() const { return Read<int64_t>(); }
		float Float() const { return Read<float>(); }
		bool Bool() const { return Int() != 0; }
		bool Bit(int bit) const { return (Int() >> bit & 1) != 0; }
		// Text stored in the global's slots (a script string buffer).
		std::string Text() const
		{
			const auto* p = reinterpret_cast<const char*>(Ptr());
			return p ? std::string(p, strnlen(p, 64)) : std::string();
		}
		void Set(int32_t v) const { Write(static_cast<int64_t>(v)); }
		void SetFloat(float v) const { Write(v); }
		void SetBit(int bit, bool on) const { Set(on ? Int() | 1 << bit : Int() & ~(1 << bit)); }

	private:
		template<class T>
		T Read() const
		{
			T v{};
			if (const int64_t* p = Ptr())
				std::memcpy(&v, p, sizeof(T));
			return v;
		}
		template<class T>
		void Write(T v) const
		{
			if (int64_t* p = Ptr())
			{
				*p = 0;
				std::memcpy(p, &v, sizeof(T));
			}
		}
		uint32_t m_index;
	};

	namespace scripts
	{
		enum class RunResult
		{
			Ran = 1,
			NotFound = 0, // no such script, or the pattern matched nothing (game update?)
			Timeout = -1,
		};
		// Runs one function of a game script by itself, found by a byte pattern of its start ("2d 04 6f ?? 00", ?? = any
		// byte): a new thread of `script` calls it with `args` and ends when it returns. Waits for that.
		// Arguments that are references take addresses, e.g. reinterpret_cast<int64_t>(ml::Global(77590).Ptr()).
		//   ml::scripts::RunFunction("appinternet", "2d 04 6f 00 00 38 03", {item, character, ref, -1}, 4000);
		inline RunResult RunFunction(const char* script, const char* pattern, std::initializer_list<int64_t> args, int32_t stackSize = 1024,
		    uint32_t timeoutMs = 5000)
		{
			if (!detail::Has(&MLApi::RunScriptFunction))
				return RunResult::NotFound;
			const std::vector<int64_t> a(args);
			return static_cast<RunResult>(Api().RunScriptFunction(script, pattern, stackSize, a.data(), static_cast<int32_t>(a.size()), timeoutMs));
		}
	}

	// ---- typed web functions -------------------------------------------------------------------
	namespace detail
	{
		template<class F>
		struct Signature : Signature<decltype(&F::operator())>
		{
		};
		template<class C, class R, class... A>
		struct Signature<R (C::*)(A...) const>
		{
			using Ret = R;
			using Args = std::tuple<std::decay_t<A>...>;
		};
		template<class C, class R, class... A>
		struct Signature<R (C::*)(A...)>
		{
			using Ret = R;
			using Args = std::tuple<std::decay_t<A>...>;
		};
		template<class R, class... A>
		struct Signature<R (*)(A...)>
		{
			using Ret = R;
			using Args = std::tuple<std::decay_t<A>...>;
		};

		template<class Args, class F, size_t... I>
		auto CallWithJson(F& fn, const Json& args, std::index_sequence<I...>)
		{
			return fn(args[I].template Get<std::tuple_element_t<I, Args>>()...);
		}
	}

	namespace web
	{
		// Typed form: the page's arguments become the function's parameters (int, double, bool, std::string, ml::Json,
		// std::vector<...>; missing ones are 0 / "" / null) and the result is sent back as JSON.
		//   ml::web::Function("shop.buy", [](int item, int garage) -> ml::Json { return {{"ok", true}}; });
		//   game.call('shop.buy', 12, 31).then(r => r.ok)
		template<class F>
		    requires(!std::is_convertible_v<F, std::function<std::string(const std::string&)>>)
		bool Function(const char* name, F fn)
		{
			using Sig = detail::Signature<F>;
			using Args = typename Sig::Args;
			return Function(name, std::function<std::string(const std::string&)>([fn = std::move(fn)](const std::string& text) mutable {
				const Json args = Json::Parse(text);
				constexpr auto indices = std::make_index_sequence<std::tuple_size_v<Args>>();
				if constexpr (std::is_void_v<typename Sig::Ret>)
				{
					detail::CallWithJson<Args>(fn, args, indices);
					return std::string();
				}
				else
					return Json(detail::CallWithJson<Args>(fn, args, indices)).Dump();
			}));
		}
		// Sends `event` with `data` to the open page.
		inline void Emit(const char* event, const Json& data)
		{
			if (Available())
				Api().WebEmit(event, data.Dump().c_str());
		}
	}

	// Short on-screen message.
	template<class... Args>
	void Notify(std::format_string<Args...> fmt, Args&&... args)
	{
		if (HasMenus())
			Api().Notify(std::format(fmt, std::forward<Args>(args)...).c_str());
	}

	template<class... Args>
	void Log(std::format_string<Args...> fmt, Args&&... args)
	{
		Api().Log(ML_LOG_INFO, std::format(fmt, std::forward<Args>(args)...).c_str());
	}
	template<class... Args>
	void LogError(std::format_string<Args...> fmt, Args&&... args)
	{
		Api().Log(ML_LOG_ERROR, std::format(fmt, std::forward<Args>(args)...).c_str());
	}
}

// Declares MLGetModInfo for you: ML_MOD_INFO("My Mod", "1.0.0", "me", "What it does")
#define ML_MOD_INFO(name, version, author, description)                                           \
	extern "C" __declspec(dllexport) const MLModInfo* MLGetModInfo()                               \
	{                                                                                              \
		static const MLModInfo info{ML_API_VERSION, name, version, author, description};          \
		return &info;                                                                              \
	}
