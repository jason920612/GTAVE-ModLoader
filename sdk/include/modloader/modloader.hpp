// C++ convenience layer over modloader.h. Include this (and natives.hpp) in your mod.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <initializer_list>
#include <string>
#include <type_traits>
#include <vector>

#include "modloader.h"

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
