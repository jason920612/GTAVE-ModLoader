// EXPERIMENT (experimentalPauseMenu): inject a test category and two settings items into the
// story-mode "Online" tab right after pausemenu.xml is parsed. See research/phase0.md §8–9.
#include "pausemenu.hpp"

#include <Windows.h>

#include <cstring>
#include <string_view>

#include <MinHook.h>

#include "../log.hpp"
#include "../pattern.hpp"
#include "text_override.hpp"
#include "../debug/watch.hpp"

#include <thread>

namespace loader::game::pausemenu
{
	namespace
	{
		// rage::atArray
		template<class T>
		struct AtArray
		{
			T* data;
			uint16_t count;
			uint16_t capacity;
		};

		// CMenuItem (0x28)
		struct MenuItem
		{
			int32_t target;     // menu screen id
			uint32_t label;     // text label hash
			uint8_t pad0[8];
			uint32_t* contexts;     // +0x10 visibility contexts (atArray of hashes); empty = always visible
			uint16_t contextCount;  // +0x18
			uint16_t contextCap;    // +0x1A
			uint32_t pad1;
			uint8_t pref;       // +0x20 menu preference id
			uint8_t optionType; // +0x21
			uint8_t action;     // +0x22
			uint8_t pad2[5];
		};
		static_assert(sizeof(MenuItem) == 0x28);

		// CMenuScreen (0x50)
		struct MenuScreen
		{
			uint64_t unk0;
			AtArray<MenuItem> items; // +0x08
			uint8_t pad[0x40 - 0x18];
			int32_t id;              // +0x40
			int32_t depth;           // +0x44
			uint8_t tail[0x50 - 0x48];
		};
		static_assert(sizeof(MenuScreen) == 0x50);

		struct MenuArray
		{
			uint8_t pad[0x10];
			AtArray<MenuScreen> screens; // +0x10
		};

		constexpr int32_t kSettings = 6, kSettingsAudio = 22, kSettingsControls = 24, kHeader = 28, kSettingsFeed = 93;
		constexpr int32_t kModsTab = 42;   // story-mode "Online" tab
		constexpr int32_t kTestPage = 132; // SETTINGS_SIXAXIS: research target
		constexpr uint8_t kPrefToggle = 187, kPrefSlider = 188; // unused by menus, code and the apply switch

		using LoadFn = bool (*)(int32_t mode);
		LoadFn g_origLoad = nullptr;

		// Scaleform -> game menu events (RVA 0x5EF9E0 on 0x6aa45f10). `args` is an array of GFx values
		// (0x18 bytes each, starting at +0x18: type at +8, double at +0x10).
		using EventFn = void (*)(const uint32_t* event, uint8_t* args, uintptr_t a3, uintptr_t a4);
		EventFn g_origEvent = nullptr;
		// "option changed" (args: pref id, new value). The game only stores the value when the
		// current header tab is SETTINGS, so for our Mods tab we store it ourselves.
		constexpr uint32_t kEventSetPref = 0x610E6168;

		// Header tab stack: entries of 16 bytes, screen id first; the last entry is the current tab.
		struct TabEntry
		{
			int32_t id;
			uint8_t pad[12];
		};
		TabEntry** g_tabStack = nullptr;
		uint16_t* g_tabCount = nullptr;
		MenuArray* g_menu = nullptr;
		int32_t* g_prefs = nullptr; // research: preference values (RVA 0x3DFC2A0 on 0x6aa45f10)

		constexpr uint32_t Joaat(std::string_view s)
		{
			uint32_t h = 0;
			for (char c : s)
			{
				h += static_cast<uint8_t>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
				h += h << 10;
				h ^= h >> 6;
			}
			h += h << 3;
			h ^= h >> 11;
			h += h << 15;
			return h;
		}

		MenuScreen* Find(int32_t id)
		{
			for (uint16_t i = 0; i < g_menu->screens.count; ++i)
				if (g_menu->screens.data[i].id == id)
					return &g_menu->screens.data[i];
			return nullptr;
		}

		template<class T>
		T* Alloc(size_t count)
		{
			// Never freed: the menu keeps pointing at it for the whole session.
			return static_cast<T*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(T) * count));
		}

		// Turns `dst` into a copy of `layout` (same page type, movie and depth) that shows `items`.
		void MakeScreen(MenuScreen* dst, const MenuScreen* layout, MenuItem* items, uint16_t count)
		{
			const int32_t id = dst->id;
			std::memcpy(dst, layout, sizeof(MenuScreen));
			dst->id = id;
			dst->items = {items, count, count};
		}

		void Inject()
		{
			MenuScreen* settings = Find(kSettings);
			MenuScreen* audio = Find(kSettingsAudio);
			MenuScreen* controls = Find(kSettingsControls);
			MenuScreen* header = Find(kHeader);
			MenuScreen* modsTab = Find(kModsTab);
			MenuScreen* page = Find(kTestPage);
			MenuScreen* feed = Find(kSettingsFeed);
			const MenuItem* saveCategory = nullptr; // template: the Audio category (no visibility contexts)
			if (settings)
				for (uint16_t i = 0; i < settings->items.count; ++i)
					if (settings->items.data[i].target == kSettingsAudio)
						saveCategory = &settings->items.data[i];
			if (!settings || !audio || !controls || !header || !modsTab || !page || !feed || !saveCategory || feed->items.count < 1 ||
			    audio->items.count < 1)
			{
				log::Error("pausemenu: expected screens not found, skipping injection (array {} count {}, found {} {} {} {} {} {} save {})",
				    static_cast<void*>(g_menu->screens.data), g_menu->screens.count, settings != nullptr, audio != nullptr, controls != nullptr,
				    header != nullptr, modsTab != nullptr, page != nullptr, saveCategory != nullptr);
				for (uint16_t i = 0; i < g_menu->screens.count && i < 8; ++i)
					log::Info("pausemenu:   screen[{}] id {} depth {} items {}", i, g_menu->screens.data[i].id, g_menu->screens.data[i].depth,
					    g_menu->screens.data[i].items.count);
				return;
			}

			// Research: our category appended to Settings, pointing at a page we fill.
			// Research: the story "Online" tab becomes a settings-style "Mods" tab with one category.
			for (uint16_t i = 0; i < header->items.count; ++i)
				if (header->items.data[i].target == kModsTab)
					header->items.data[i].label = Joaat("ML_TAB_MODS");
			auto* categories = Alloc<MenuItem>(1);
			categories[0] = *saveCategory;
			categories[0].target = kTestPage;
			categories[0].label = Joaat("ML_CAT_TEST");
			categories[0].contextCount = 0;
			MakeScreen(modsTab, settings, categories, 1);
			log::Info("pausemenu: research: mods tab built from the settings layout");

			auto* items = Alloc<MenuItem>(2);
			items[0] = feed->items.data[0]; // on/off toggle
			items[0].label = Joaat("ML_OPT_A");
			items[0].pref = kPrefToggle;
			items[0].contextCount = 0;
			items[1] = audio->items.data[0]; // 0..10 slider
			items[1].label = Joaat("ML_OPT_B");
			items[1].pref = kPrefSlider;
			items[1].contextCount = 0;
			MakeScreen(page, audio, items, 2);
			log::Info("pausemenu: test page injected (screens {}, {})", kModsTab, kTestPage);
		}

		int32_t CurrentTab()
		{
			const uint16_t n = *g_tabCount;
			return n && *g_tabStack ? (*g_tabStack)[n - 1].id : -1;
		}

		bool IsOurPref(int32_t pref)
		{
			return pref == kPrefToggle || pref == kPrefSlider;
		}

		void HookEvent(const uint32_t* event, uint8_t* args, uintptr_t a3, uintptr_t a4)
		{
			if (event && args && *event == kEventSetPref && CurrentTab() == kModsTab)
			{
				const auto isNumber = [&](int i) { return (*reinterpret_cast<uint32_t*>(args + 0x20 + 0x18 * i) & 0x8F) == 3; };
				if (isNumber(0) && isNumber(1))
				{
					const auto pref = static_cast<int32_t>(*reinterpret_cast<double*>(args + 0x28));
					const auto value = static_cast<int32_t>(*reinterpret_cast<double*>(args + 0x40));
					if (IsOurPref(pref))
						g_prefs[pref] = value;
				}
			}
			g_origEvent(event, args, a3, a4);
		}

		bool HookLoad(int32_t mode)
		{
			const bool ok = g_origLoad(mode);
			log::Info("pausemenu: load({}) -> {} (screens {})", mode, ok, g_menu->screens.count);

			return ok;
		}
	}

	bool InstallHooks()
	{
		const auto module = pattern::Module::Main();
		const auto at = pattern::Find(module.text,
		    pattern::Pattern::Parse("41 56 56 57 55 53 48 81 EC E0 00 00 00 48 C7 05 ? ? ? ? 00 00 00 00 83 F9 01 0F 85 ? ? ? ? 4C 8D 35"));
		if (!at)
		{
			log::Error("pausemenu: loader function not found");
			return false;
		}
		g_menu = reinterpret_cast<MenuArray*>(pattern::Rip(*at + 0x24));
		g_prefs = reinterpret_cast<int32_t*>(module.base + 0x3DFC2A0);
		if (MH_CreateHook(reinterpret_cast<void*>(*at), reinterpret_cast<void*>(&HookLoad), reinterpret_cast<void**>(&g_origLoad)) != MH_OK ||
		    MH_EnableHook(reinterpret_cast<void*>(*at)) != MH_OK)
		{
			log::Error("pausemenu: could not hook the loader");
			return false;
		}
		log::Info("pausemenu: loader hooked");

		const auto ev = pattern::Find(module.text, pattern::Pattern::Parse(
		    "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC 28 02 00 00 80 3D ? ? ? ? 00 0F 84 ? ? ? ? 45 89 C6 49 89 D4 49 89 CD"));
		const auto tabs = pattern::Find(module.text,
		    pattern::Pattern::Parse("0F B7 05 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? 48 8B 0D ? ? ? ? 48 C1 E0 04 83 7C 08 F0 06"));
		if (!ev || !tabs)
		{
			log::Error("pausemenu: menu event handler not found ({} {})", ev.has_value(), tabs.has_value());
			return false;
		}
		g_tabCount = reinterpret_cast<uint16_t*>(pattern::Rip(*tabs + 3));
		g_tabStack = reinterpret_cast<TabEntry**>(pattern::Rip(*tabs + 0x13));
		if (MH_CreateHook(reinterpret_cast<void*>(*ev), reinterpret_cast<void*>(&HookEvent), reinterpret_cast<void**>(&g_origEvent)) != MH_OK ||
		    MH_EnableHook(reinterpret_cast<void*>(*ev)) != MH_OK)
		{
			log::Error("pausemenu: could not hook the menu event handler");
			return false;
		}
		log::Info("pausemenu: menu event handler hooked");
		return true;
	}

	void Tick()
	{
		// Experiment: inject once the data is parsed, before the pause menu is ever opened.
		static bool injected = false;
		if (!injected && g_menu->screens.count)
		{
			injected = true;
			Inject();
		}
		static bool texts = false;
		if (!texts && text_override::Ready())
		{
			texts = text_override::Set(Joaat("ML_TAB_MODS"), "模組") && text_override::Set(Joaat("ML_CAT_TEST"), "測試模組") &&
			        text_override::Set(Joaat("ML_OPT_A"), "測試開關") && text_override::Set(Joaat("ML_OPT_B"), "測試數值");
			if (texts)
				log::Info("pausemenu: test labels registered");
		}
		static int32_t last[2] = {INT32_MIN, INT32_MIN};
		for (int i = 0; i < 2; ++i)
		{
			const int32_t v = g_prefs[kPrefToggle + i];
			if (v != last[i])
			{
				log::Info("pausemenu: pref {} = {}", kPrefToggle + i, v);
				last[i] = v;
			}
		}
	}
}
