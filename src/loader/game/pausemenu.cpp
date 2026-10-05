// Native "Mods" tab in the story-mode pause menu. See research/phase0.md §8–10.
//
// The menu is data driven (pausemenu.xml -> CMenuArray). Right after the data is parsed and
// before the menu is first opened, we:
//   - turn header tab 42 (the story-mode "Online" tab) into a copy of the SETTINGS tab whose
//     categories are the mods with settings;
//   - append one page screen per mod (copies of the audio settings page) with toggle/slider items.
// Item values live in the game's menu preference array. We only use preference slots nothing
// else reads or writes, and share them between pages: right before the game builds a mod's
// page we copy that mod's values into the slots; when the player changes an option on the Mods
// tab we store it back into the mod's setting.
#include "pausemenu.hpp"

#include <Windows.h>

#include <array>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <MinHook.h>

#include "../log.hpp"
#include "../mods.hpp"
#include "../pattern.hpp"
#include "text_override.hpp"

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
			int32_t target;        // menu screen id
			uint32_t label;        // text label hash
			uint8_t pad0[8];
			uint32_t* contexts;    // +0x10 visibility contexts (atArray of hashes); empty = always visible
			uint16_t contextCount; // +0x18
			uint16_t contextCap;   // +0x1A
			uint32_t pad1;
			uint8_t pref;          // +0x20 menu preference id
			uint8_t optionType;    // +0x21
			uint8_t action;        // +0x22
			uint8_t pad2[5];
		};
		static_assert(sizeof(MenuItem) == 0x28);

		// CMenuScreen (0x50). +0x00 is the C++ handler (created on first open from the type at +0x28).
		struct MenuScreen
		{
			uint64_t handler;
			AtArray<MenuItem> items; // +0x08
			uint8_t pad[0x40 - 0x18];
			int32_t id;              // +0x40 (the array is sorted by id: the game binary searches it)
			int32_t depth;           // +0x44
			uint8_t tail[0x50 - 0x48];
		};
		static_assert(sizeof(MenuScreen) == 0x50);

		struct MenuArray
		{
			uint8_t pad[0x10];
			AtArray<MenuScreen> screens; // +0x10
		};

		// Header tab stack: 16-byte entries, screen id first; the last entry is the current tab.
		struct TabEntry
		{
			int32_t id;
			uint8_t pad[12];
		};

		constexpr int32_t kSettings = 6, kSettingsAudio = 22, kHeader = 28, kSettingsFeed = 93;
		constexpr int32_t kModsTab = 42; // story-mode "Online" tab

		// Preference slots that no menu, no code path and no apply handler uses (build 0x6aa45f10).
		constexpr std::array<uint8_t, ML_MAX_SETTINGS> kSlots = {16, 63, 64, 65, 74, 82, 83, 102, 104, 155, 171, 187, 188, 189, 190};

		// "option changed" event from the menu movie (args: preference id, new value).
		constexpr uint32_t kEventSetPref = 0x610E6168;

		using LoadFn = bool (*)(int32_t mode);
		using BuildFn = uintptr_t (*)(uintptr_t screen, uintptr_t a2, uintptr_t a3, uintptr_t a4);
		// `args` is an array of GFx values (0x18 bytes each from +0x18: type at +8, double at +0x10).
		using EventFn = void (*)(const uint32_t* event, uint8_t* args, uintptr_t a3, uintptr_t a4);
		LoadFn g_origLoad = nullptr;
		BuildFn g_origBuild = nullptr;
		EventFn g_origEvent = nullptr;

		MenuArray* g_menu = nullptr;
		int32_t* g_prefs = nullptr;      // preference values (int[217])
		int32_t* g_savedPrefs = nullptr; // values as last saved; the menu shows differing ones in italics
		TabEntry** g_tabStack = nullptr;
		uint16_t* g_tabCount = nullptr;
		bool g_hooked = false;

		struct Page
		{
			int32_t screen = 0;
			uint32_t labelHash = 0;
			std::string name;
			std::vector<mods::Setting*> settings;
		};
		std::vector<Page> g_pages;      // fixed after Inject (game thread)
		const Page* g_active = nullptr; // page whose values are in the slots
		bool g_owned = false;           // the Mods tab shows our pages

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
		constexpr uint32_t kTabLabel = Joaat("ML_TAB_MODS");

		uint32_t SettingLabel(size_t page, size_t index)
		{
			return Joaat(std::format("ML_SET_{}_{}", page, index));
		}

		MenuScreen* Find(int32_t id)
		{
			for (uint16_t i = 0; i < g_menu->screens.count; ++i)
				if (g_menu->screens.data[i].id == id)
					return &g_menu->screens.data[i];
			return nullptr;
		}

		const MenuItem* FindItem(const MenuScreen* screen, int32_t target)
		{
			for (uint16_t i = 0; i < screen->items.count; ++i)
				if (screen->items.data[i].target == target)
					return &screen->items.data[i];
			return nullptr;
		}

		template<class T>
		T* Alloc(size_t count)
		{
			// Never freed: the menu keeps pointing at it for the whole session.
			return static_cast<T*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(T) * count));
		}

		int32_t CurrentTab()
		{
			const uint16_t n = *g_tabCount;
			return n && *g_tabStack ? (*g_tabStack)[n - 1].id : -1;
		}

		bool Inject()
		{
			for (const auto& mod : mods::All())
				if (!mod->settings.empty())
					g_pages.push_back({0, Joaat(std::format("ML_MOD_{}", g_pages.size())), mod->name, mod->settings});
			if (g_pages.empty())
				return false;

			const MenuScreen* settings = Find(kSettings);
			const MenuScreen* audio = Find(kSettingsAudio);
			const MenuScreen* feed = Find(kSettingsFeed);
			MenuScreen* header = Find(kHeader);
			const MenuItem* categoryTemplate = settings ? FindItem(settings, kSettingsAudio) : nullptr;     // no visibility contexts
			const MenuItem* toggleTemplate = feed && feed->items.count ? &feed->items.data[0] : nullptr;    // on/off
			const MenuItem* sliderTemplate = audio && audio->items.count ? &audio->items.data[0] : nullptr; // 0..10
			if (!settings || !audio || !feed || !header || !Find(kModsTab) || !categoryTemplate || !toggleTemplate || !sliderTemplate)
			{
				log::Error("pausemenu: unexpected menu data ({} screens), Mods tab not added", g_menu->screens.count);
				g_pages.clear();
				return false;
			}

			// New page screens get ids after the last one so the array stays sorted.
			const uint16_t oldCount = g_menu->screens.count;
			const size_t newCount = oldCount + g_pages.size();
			if (newCount > UINT16_MAX)
			{
				g_pages.clear();
				return false;
			}
			const int32_t firstId = g_menu->screens.data[oldCount - 1].id + 1;
			auto* screens = Alloc<MenuScreen>(newCount);
			auto* categories = Alloc<MenuItem>(g_pages.size());
			std::memcpy(screens, g_menu->screens.data, sizeof(MenuScreen) * oldCount);
			for (size_t p = 0; p < g_pages.size(); ++p)
			{
				Page& page = g_pages[p];
				page.screen = firstId + static_cast<int32_t>(p);
				const auto count = static_cast<uint16_t>(page.settings.size());

				auto* items = Alloc<MenuItem>(count);
				for (size_t i = 0; i < count; ++i)
				{
					items[i] = page.settings[i]->type == ML_SETTING_SLIDER ? *sliderTemplate : *toggleTemplate;
					items[i].label = SettingLabel(p, i);
					items[i].pref = kSlots[i];
					items[i].contextCount = 0;
				}
				MenuScreen& screen = screens[oldCount + p];
				screen = *audio;
				screen.handler = 0;
				screen.id = page.screen;
				screen.items = {items, count, count};

				categories[p] = *categoryTemplate;
				categories[p].target = page.screen;
				categories[p].label = page.labelHash;
				categories[p].contextCount = 0;
			}

			// The Mods tab: same layout, movie and handler type as SETTINGS.
			for (uint16_t i = 0; i < oldCount; ++i)
				if (screens[i].id == kModsTab)
				{
					screens[i] = *settings;
					screens[i].handler = 0;
					screens[i].id = kModsTab;
					screens[i].items = {categories, static_cast<uint16_t>(g_pages.size()), static_cast<uint16_t>(g_pages.size())};
				}
			for (uint16_t i = 0; i < header->items.count; ++i)
				if (header->items.data[i].target == kModsTab)
					header->items.data[i].label = kTabLabel;

			// The old array is left alone: nothing references it before the menu is first opened.
			g_menu->screens = {screens, static_cast<uint16_t>(newCount), static_cast<uint16_t>(newCount)};
			log::Info("pausemenu: Mods tab ready ({} mod page(s), screens {}..{})", g_pages.size(), firstId,
			    firstId + static_cast<int32_t>(g_pages.size()) - 1);
			return true;
		}

		bool RegisterTexts()
		{
			if (!text_override::Set(kTabLabel, "模組"))
				return false;
			for (size_t p = 0; p < g_pages.size(); ++p)
			{
				if (!text_override::Set(g_pages[p].labelHash, g_pages[p].name))
					return false;
				for (size_t i = 0; i < g_pages[p].settings.size(); ++i)
					if (!text_override::Set(SettingLabel(p, i), g_pages[p].settings[i]->label))
						return false;
			}
			return true;
		}

		// ---- hooks (menu code runs on the game thread) ----------------------------------------

		bool HookLoad(int32_t mode)
		{
			const bool ok = g_origLoad(mode);
			log::Debug("pausemenu: load({}) -> {}", mode, ok);
			return ok;
		}

		// Builds a menu column for a screen (also the preview of a hovered category).
		uintptr_t HookBuild(uintptr_t screen, uintptr_t a2, uintptr_t a3, uintptr_t a4)
		{
			for (const Page& page : g_pages)
				if (page.screen == static_cast<int32_t>(screen))
				{
					for (size_t i = 0; i < page.settings.size(); ++i)
						g_prefs[kSlots[i]] = g_savedPrefs[kSlots[i]] = page.settings[i]->value;
					g_active = &page;
					break;
				}
			return g_origBuild(screen, a2, a3, a4);
		}

		// The game only stores option changes while the current tab is SETTINGS.
		void HookEvent(const uint32_t* event, uint8_t* args, uintptr_t a3, uintptr_t a4)
		{
			if (g_active && event && args && *event == kEventSetPref && CurrentTab() == kModsTab)
			{
				const auto isNumber = [&](int i) { return (*reinterpret_cast<uint32_t*>(args + 0x20 + 0x18 * i) & 0x8F) == 3; };
				if (isNumber(0) && isNumber(1))
				{
					const auto pref = static_cast<int32_t>(*reinterpret_cast<double*>(args + 0x28));
					const auto value = static_cast<int32_t>(*reinterpret_cast<double*>(args + 0x40));
					for (size_t i = 0; i < g_active->settings.size(); ++i)
						if (kSlots[i] == pref)
						{
							mods::SetSettingValue(*g_active->settings[i], value);
							g_prefs[pref] = g_savedPrefs[pref] = g_active->settings[i]->value; // saved right away
							break;
						}
				}
			}
			g_origEvent(event, args, a3, a4);
		}

		bool Hook(uintptr_t at, void* detour, void** original)
		{
			return MH_CreateHook(reinterpret_cast<void*>(at), detour, original) == MH_OK && MH_EnableHook(reinterpret_cast<void*>(at)) == MH_OK;
		}
	}

	bool InstallHooks()
	{
		const auto module = pattern::Module::Main();
		const auto find = [&](const char* sig) { return pattern::Find(module.text, pattern::Pattern::Parse(sig)); };
		// LoadPauseMenuData(mode): mov qword [CMenuArray + x], 0; cmp ecx, 1; ...
		const auto load = find("41 56 56 57 55 53 48 81 EC E0 00 00 00 48 C7 05 ? ? ? ? 00 00 00 00 83 F9 01 0F 85 ? ? ? ? 4C 8D 35");
		// Column builder(screen id, flag): stores the flag, then binary searches the screens.
		const auto build = find("41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC 68 01 00 00 88 54 24 78 0F B7 05");
		// Menu event handler(event hash*, args*, ...).
		const auto ev = find("41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC 28 02 00 00 80 3D ? ? ? ? 00 0F 84 ? ? ? ? 45 89 C6 49 89 D4 49 89 CD");
		// movzx eax, word [tab count]; ...; mov rcx, [tab stack]; ...; cmp dword [rax+rcx-0x10], 6
		const auto tabs = find("0F B7 05 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? 48 8B 0D ? ? ? ? 48 C1 E0 04 83 7C 08 F0 06");
		// Column builder, toggle display: lea rcx, [prefs]; mov ecx, [rcx+r9*4]; ...; lea rbx, [saved prefs]; cmp ecx, [rbx+r9*4]
		const auto prefs = find("48 8D 0D ? ? ? ? 42 8B 0C 89 31 D2 48 8D 1D ? ? ? ? 42 3B 0C 8B");
		if (!load || !build || !ev || !tabs || !prefs)
		{
			log::Error("pausemenu: menu code not found ({} {} {} {} {}); Mods tab disabled", load.has_value(), build.has_value(), ev.has_value(),
			    tabs.has_value(), prefs.has_value());
			return false;
		}
		g_menu = reinterpret_cast<MenuArray*>(pattern::Rip(*load + 0x24));
		g_tabCount = reinterpret_cast<uint16_t*>(pattern::Rip(*tabs + 3));
		g_tabStack = reinterpret_cast<TabEntry**>(pattern::Rip(*tabs + 0x13));
		g_prefs = reinterpret_cast<int32_t*>(pattern::Rip(*prefs + 3));
		g_savedPrefs = reinterpret_cast<int32_t*>(pattern::Rip(*prefs + 0x10));
		if (!Hook(*load, reinterpret_cast<void*>(&HookLoad), reinterpret_cast<void**>(&g_origLoad)) ||
		    !Hook(*build, reinterpret_cast<void*>(&HookBuild), reinterpret_cast<void**>(&g_origBuild)) ||
		    !Hook(*ev, reinterpret_cast<void*>(&HookEvent), reinterpret_cast<void**>(&g_origEvent)))
		{
			log::Error("pausemenu: could not hook the menu");
			return false;
		}
		log::Info("pausemenu: hooks installed (prefs at +{:#x}, saved at +{:#x})", reinterpret_cast<uintptr_t>(g_prefs) - module.base,
		    reinterpret_cast<uintptr_t>(g_savedPrefs) - module.base);
		g_hooked = true;
		return true;
	}

	bool Tick()
	{
		static bool injected = false, texts = false;
		if (!g_hooked)
			return false;
		if (!injected && mods::Loaded() && g_menu->screens.count)
		{
			injected = true;
			g_owned = Inject();
		}
		if (g_owned && !texts && text_override::Ready())
		{
			texts = true; // once: a full text map will not empty itself
			if (!RegisterTexts())
				log::Warn("pausemenu: could not register all labels");
		}
		return g_owned;
	}
}
