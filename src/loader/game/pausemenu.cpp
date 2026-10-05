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
			AtArray<void>* contextsPad; // +0x10 visibility contexts (atArray, 12 bytes incl. counts)
			uint8_t pad1[0x8];
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

		constexpr int32_t kSettings = 6, kSettingsAudio = 22, kSettingsControls = 24, kHeader = 28;
		constexpr int32_t kModsTab = 42;   // story-mode "Online" tab
		constexpr int32_t kTestPage = 93; // SETTINGS_FEED: a plain list page; test items are appended to it
		constexpr uint8_t kPrefToggle = 217, kPrefSlider = 218;

		using LoadFn = bool (*)(int32_t mode);
		LoadFn g_origLoad = nullptr;
		MenuArray* g_menu = nullptr;
		int32_t* g_prefs = nullptr; // research: preference values (RVA 0x3DFC2A4 on 0x6aa45f10)

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
			const MenuItem* saveCategory = nullptr;
			if (settings)
				for (uint16_t i = 0; i < settings->items.count; ++i)
					if (settings->items.data[i].target == 32) // SETTINGS_SAVEGAME, a plain selectable category
						saveCategory = &settings->items.data[i];
			if (!settings || !audio || !controls || !header || !modsTab || !page || !saveCategory || controls->items.count < 4 ||
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

			// Append a toggle and a slider (unused preference ids) to the Feed settings page.
			const uint16_t n = page->items.count;
			auto* items = Alloc<MenuItem>(n + 2u);
			std::memcpy(items, page->items.data, sizeof(MenuItem) * n);
			items[n] = controls->items.data[3]; // vibration toggle
			items[n].label = Joaat("ML_OPT_A");
			items[n].pref = kPrefToggle;
			items[n + 1] = audio->items.data[0]; // volume slider
			items[n + 1].label = Joaat("ML_OPT_B");
			items[n + 1].pref = kPrefSlider;
			page->items = {items, static_cast<uint16_t>(n + 2), static_cast<uint16_t>(n + 2)};
			log::Info("pausemenu: test page injected (screens {}, {})", kModsTab, kTestPage);
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
		g_prefs = reinterpret_cast<int32_t*>(module.base + 0x3DFC2A4);
		if (MH_CreateHook(reinterpret_cast<void*>(*at), reinterpret_cast<void*>(&HookLoad), reinterpret_cast<void**>(&g_origLoad)) != MH_OK ||
		    MH_EnableHook(reinterpret_cast<void*>(*at)) != MH_OK)
		{
			log::Error("pausemenu: could not hook the loader");
			return false;
		}
		log::Info("pausemenu: loader hooked");
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
