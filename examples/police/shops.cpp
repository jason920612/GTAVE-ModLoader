// The officer's own clothes, hair and tattoo shops (research/char4-design.md M6). The game's shops are made for the
// protagonists' models and the loader does not start them for the officer (game/char4), so in those shops the officer
// gets this mod's instead, for the freemode model: the in-game list menu, a preview on the officer while browsing
// (←/→ for colours), and the price paid from the officer's own account through the game's bank (the shop's own payee,
// so the bank history shows the shop). What is bought is kept in the save (look.cpp) and worn off duty; tattoos show
// on duty too.
#include "police.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace police::shops
{
	namespace
	{
		enum class Kind
		{
			Hair,
			Clothes,
			Tattoo
		};
		struct Shop
		{
			Kind kind;
			int id; // shop_controller's shop number; the bank payee is 41 + id
			Vector3 at;
		};
		// shop_controller's shop places (its position table, by shop number).
		const Shop kShops[] = {
		    {Kind::Hair, 0, {-821.995f, -187.178f, 36.5689f}}, {Kind::Hair, 1, {133.57f, -1710.92f, 28.2916f}},
		    {Kind::Hair, 2, {-1287.08f, -1116.56f, 5.9901f}}, {Kind::Hair, 3, {1933.12f, 3726.08f, 31.8444f}},
		    {Kind::Hair, 4, {1208.33f, -470.917f, 65.208f}}, {Kind::Hair, 5, {-30.7448f, -148.492f, 56.0765f}},
		    {Kind::Hair, 6, {-280.816f, 6231.77f, 30.6955f}}, {Kind::Clothes, 7, {80.665f, -1391.67f, 28.3761f}},
		    {Kind::Clothes, 8, {1687.88f, 4820.55f, 41.0096f}}, {Kind::Clothes, 9, {419.531f, -807.579f, 28.4896f}},
		    {Kind::Clothes, 10, {-1094.05f, 2704.17f, 18.0873f}}, {Kind::Clothes, 11, {1197.97f, 2704.22f, 37.1572f}},
		    {Kind::Clothes, 12, {-818.622f, -1077.53f, 10.3282f}}, {Kind::Clothes, 13, {-0.2361f, 6516.05f, 30.8684f}},
		    {Kind::Clothes, 14, {-1199.81f, -776.689f, 16.3237f}}, {Kind::Clothes, 15, {618.186f, 2752.57f, 41.0881f}},
		    {Kind::Clothes, 16, {126.685f, -212.503f, 53.5578f}}, {Kind::Clothes, 17, {-3168.97f, 1055.29f, 19.8632f}},
		    {Kind::Clothes, 18, {-715.36f, -155.774f, 36.4105f}}, {Kind::Clothes, 19, {-158.22f, -304.966f, 38.735f}},
		    {Kind::Clothes, 20, {-1455.0f, -233.186f, 48.7936f}}, {Kind::Clothes, 21, {-1335.97f, -1278.55f, 3.8598f}},
		    {Kind::Tattoo, 22, {321.61f, 179.417f, 102.587f}}, {Kind::Tattoo, 23, {1861.69f, 3750.08f, 32.0318f}},
		    {Kind::Tattoo, 24, {-290.16f, 6199.09f, 30.4871f}}, {Kind::Tattoo, 25, {-1153.95f, -1425.02f, 3.9544f}},
		    {Kind::Tattoo, 26, {1322.45f, -1651.13f, 51.1885f}}, {Kind::Tattoo, 27, {-3169.42f, 1074.73f, 19.8343f}},
		};
		constexpr float kShopRadius = 9.0f;

		int Payee(int shop)
		{
			return 41 + shop;
		}

		std::string Money(int amount)
		{
			return std::format("${}", amount);
		}

		// The money, through the bank; a notice when there is not enough.
		bool Pay(int amount, int shop)
		{
			if (ml::game::Cash() < amount)
			{
				ml::Notify("錢不夠");
				return false;
			}
			if (!ml::game::Pay(amount, Payee(shop)))
			{
				ml::Notify("付款失敗");
				return false;
			}
			AUDIO::PLAY_SOUND_FRONTEND(-1, "PURCHASE", "HUD_LIQUOR_STORE_SOUNDSET", true);
			return true;
		}

		// The officer's own look again (after a preview).
		void Restore()
		{
			if (const ::Ped ped = officer::Ped())
				look::Apply(ped, officer::InUniform());
		}

		// ---- clothes ----

		struct Category
		{
			const char* name;
			bool prop;
			int slot;
			int price;
		};
		const Category kCategories[] = {
		    {"上衣", false, 11, 180}, {"內搭", false, 8, 60}, {"褲子", false, 4, 120}, {"鞋子", false, 6, 110}, {"項鍊與配件", false, 7, 90},
		    {"帽子", true, 0, 80}, {"眼鏡", true, 1, 70}, {"耳飾", true, 2, 60}, {"手錶", true, 6, 150}, {"手環", true, 7, 60},
		};

		struct Browse
		{
			int drawable = 0, texture = 0;
		} g_browse;

		// A top needs its own arms (and sometimes an undershirt): the shop data's forced components.
		std::vector<std::pair<int, look::Piece>> Forced(::Ped ped, int slot, int drawable, int texture)
		{
			std::vector<std::pair<int, look::Piece>> out;
			const Hash hash = EXTRAMETADATA::GET_HASH_NAME_FOR_COMPONENT(ped, slot, drawable, texture);
			if (!hash)
				return out;
			const int n = EXTRAMETADATA::GET_SHOP_PED_APPAREL_FORCED_COMPONENT_COUNT(hash);
			for (int i = 0; i < n && i < 8; ++i)
			{
				Hash name = 0;
				int value = 0, type = 0;
				EXTRAMETADATA::GET_FORCED_COMPONENT(hash, i, &name, &value, &type);
				if (type < 0 || type > 11)
					continue;
				if (name != 0 && name != static_cast<Hash>(-1))
				{
					alignas(8) int64_t data[24] = {};
					EXTRAMETADATA::GET_SHOP_PED_COMPONENT(name, reinterpret_cast<Any*>(data));
					out.push_back({type, {static_cast<int>(data[3]), static_cast<int>(data[4])}});
				}
				else
					out.push_back({type, {value, 0}});
			}
			return out;
		}

		void Show(const Category& c, int drawable, int texture)
		{
			const ::Ped ped = officer::Ped();
			if (!ped)
				return;
			if (c.prop)
			{
				if (drawable < 0)
					PED::CLEAR_PED_PROP(ped, c.slot, 0);
				else
					PED::SET_PED_PROP_INDEX(ped, c.slot, drawable, texture, true, 0);
				return;
			}
			PED::SET_PED_COMPONENT_VARIATION(ped, c.slot, drawable, texture, 0);
			for (const auto& [slot, p] : Forced(ped, c.slot, drawable, texture))
				PED::SET_PED_COMPONENT_VARIATION(ped, slot, p.drawable, p.texture, 0);
		}

		int Textures(const Category& c, int drawable)
		{
			const ::Ped ped = officer::Ped();
			if (!ped || drawable < 0)
				return 1;
			return std::max(1, c.prop ? PED::GET_NUMBER_OF_PED_PROP_TEXTURE_VARIATIONS(ped, c.slot, drawable)
			                          : PED::GET_NUMBER_OF_PED_TEXTURE_VARIATIONS(ped, c.slot, drawable));
		}

		void Buy(const Category& c, int shop)
		{
			const look::Piece worn = look::Worn(c.prop, c.slot);
			if (worn.drawable == g_browse.drawable && worn.texture == g_browse.texture)
			{
				ml::Notify("已經穿著這件");
				return;
			}
			const int price = g_browse.drawable < 0 ? 0 : c.price + 10 * std::min(g_browse.drawable, 40);
			if (price > 0 && !Pay(price, shop))
				return;
			if (!c.prop)
				if (const ::Ped ped = officer::Ped())
					for (const auto& [slot, p] : Forced(ped, c.slot, g_browse.drawable, g_browse.texture))
						look::Wear(false, slot, p);
			look::Wear(c.prop, c.slot, {g_browse.drawable, g_browse.texture});
			if (price > 0)
				ml::Notify("已購買（{}）", Money(price));
			else
				ml::Notify("已拿下");
			ml::Log("the officer bought {} {} / {} for ${}", c.name, g_browse.drawable, g_browse.texture, price);
		}

		menu::List CategoryMenu(const Category& c, int shop)
		{
			menu::List list{c.name, {}, Restore};
			const ::Ped ped = officer::Ped();
			const int count = !ped ? 0 : c.prop ? PED::GET_NUMBER_OF_PED_PROP_DRAWABLE_VARIATIONS(ped, c.slot) : PED::GET_NUMBER_OF_PED_DRAWABLE_VARIATIONS(ped, c.slot);
			const look::Piece worn = look::Worn(c.prop, c.slot);
			if (c.prop)
				list.items.push_back({"不戴", worn.drawable < 0 ? "戴著中" : "", "", true, true, [&c, shop] { Buy(c, shop); }, nullptr,
				    [&c] {
					    g_browse = {-1, 0};
					    Show(c, -1, 0);
				    }});
			for (int d = 0; d < count; ++d)
			{
				const int price = c.price + 10 * std::min(d, 40);
				const int textures = Textures(c, d);
				menu::Item item;
				item.label = std::format("款式 {}", d + 1);
				item.detail = worn.drawable == d ? "穿著中" : Money(price);
				item.help = textures > 1 ? std::format("←/→ 換顏色（{} 種）", textures) : "";
				item.keepOpen = true;
				item.action = [&c, shop] { Buy(c, shop); };
				item.hover = [&c, d, worn] {
					g_browse = {d, worn.drawable == d ? worn.texture : 0};
					Show(c, d, g_browse.texture);
				};
				item.step = [&c, d](int by) {
					const int n = Textures(c, d);
					g_browse = {d, (g_browse.texture + by + n) % n};
					Show(c, d, g_browse.texture);
				};
				list.items.push_back(std::move(item));
			}
			return list;
		}

		menu::List ClothesMenu(int shop)
		{
			menu::List list{"服飾店", {}, Restore};
			for (const Category& c : kCategories)
				list.items.push_back({c.name, "", "", true, false, nullptr, [&c, shop] { return CategoryMenu(c, shop); }});
			return list;
		}

		// ---- hair ----

		look::Hair g_hair{}; // being tried on

		void ShowHair()
		{
			const ::Ped ped = officer::Ped();
			if (!ped)
				return;
			const int hairs = PED::GET_NUMBER_OF_PED_DRAWABLE_VARIATIONS(ped, 2);
			PED::SET_PED_COMPONENT_VARIATION(ped, 2, hairs > 0 ? std::clamp(g_hair.hair, 0, hairs - 1) : 0, 0, 0);
			PED::SET_PED_HAIR_TINT(ped, g_hair.hairColor, g_hair.highlight);
			PED::SET_PED_HEAD_OVERLAY(ped, 2, g_hair.brows, 1.0f);
			PED::SET_PED_HEAD_OVERLAY_TINT(ped, 2, 1, g_hair.browColor, g_hair.browColor);
			PED::SET_PED_HEAD_OVERLAY(ped, 1, !look::Female() && g_hair.beard >= 0 ? g_hair.beard : 255, 1.0f);
			PED::SET_PED_HEAD_OVERLAY_TINT(ped, 1, 1, g_hair.beardColor, g_hair.beardColor);
		}

		void BuyHair(int price, int shop, const char* what)
		{
			const look::Hair now = look::GetHair();
			if (std::memcmp(&now, &g_hair, sizeof(now)) == 0)
			{
				ml::Notify("沒有改變");
				return;
			}
			if (!Pay(price, shop))
				return;
			look::SetHair(g_hair);
			ml::Notify("{}（{}）", what, Money(price));
		}

		// A list of choices for one part (hair styles, beards, brows), with colours on ←/→.
		menu::List PartMenu(const char* title, int count, int first, int look::Hair::*part, int look::Hair::*color, int price, int shop)
		{
			menu::List list{title, {}, Restore};
			const look::Hair now = look::GetHair();
			for (int i = first; i < count; ++i)
			{
				menu::Item item;
				item.label = i < 0 ? std::string("無") : std::format("款式 {}", i + 1);
				item.detail = now.*part == i ? "目前" : Money(price);
				item.help = color ? "←/→ 換顏色" : "";
				item.keepOpen = true;
				item.action = [price, shop] { BuyHair(price, shop, "已修剪"); };
				item.hover = [i, part] {
					g_hair = look::GetHair();
					g_hair.*part = i;
					ShowHair();
				};
				if (color)
					item.step = [color](int by) {
						g_hair.*color = (g_hair.*color + by + 64) % 64;
						ShowHair();
					};
				list.items.push_back(std::move(item));
			}
			return list;
		}

		menu::List HairMenu(int shop)
		{
			menu::List list{"理髮店", {}, Restore};
			const ::Ped ped = officer::Ped();
			const int hairs = ped ? PED::GET_NUMBER_OF_PED_DRAWABLE_VARIATIONS(ped, 2) : 0;
			list.items.push_back({"髮型", "", "", true, false, nullptr, [hairs, shop] {
				                      return PartMenu("髮型", hairs, 0, &look::Hair::hair, &look::Hair::hairColor, 120, shop);
			                      }});
			list.items.push_back({"挑染", "", "←/→ 換顏色，Enter 確定", true, true, [shop] { BuyHair(40, shop, "已染髮"); }, nullptr,
			    [] {
				    g_hair = look::GetHair();
				    ShowHair();
			    },
			    [](int by) {
				    g_hair.highlight = (g_hair.highlight + by + 64) % 64;
				    ShowHair();
			    }});
			list.items.push_back({"眉毛", "", "", true, false, nullptr, [shop] {
				                      return PartMenu("眉毛", 34, 0, &look::Hair::brows, &look::Hair::browColor, 30, shop);
			                      }});
			if (!look::Female())
				list.items.push_back({"鬍子", "", "", true, false, nullptr, [shop] {
					                      return PartMenu("鬍子", 29, -1, &look::Hair::beard, &look::Hair::beardColor, 50, shop);
				                      }});
			return list;
		}

		// ---- tattoos ----

		struct Tattoo
		{
			Hash collection, overlay;
			int cost, zone;
			std::string name;
		};

		// The freemode tattoos the game's tattoo shops sell (the shop DLC data of character type 3 male / 4 female).
		// The data: +0 lock, +1 id, +2 collection, +3 overlay, +4 cost, +5 zone, +6 group, +7 text label (TAT_*).
		std::vector<Tattoo> Tattoos()
		{
			std::vector<Tattoo> out;
			const int type = look::Female() ? 4 : 3;
			const int n = EXTRAMETADATA::GET_NUM_TATTOO_SHOP_DLC_ITEMS(type);
			for (int i = 0; i < n; ++i)
			{
				alignas(8) int64_t data[24] = {};
				if (!EXTRAMETADATA::GET_TATTOO_SHOP_DLC_ITEM_DATA(type, i, reinterpret_cast<Any*>(data)))
					continue;
				const auto collection = static_cast<Hash>(data[2]), overlay = static_cast<Hash>(data[3]);
				const char* label = reinterpret_cast<const char*>(&data[7]);
				// Tattoos are TAT_*; the list also holds T-shirt prints (CLO_*_DECL_*) and hidden ones without a price.
				if (!collection || !overlay || std::strncmp(label, "TAT_", 4) != 0 || data[4] <= 0)
					continue;
				const char* text = label[0] ? HUD::GET_FILENAME_FOR_AUDIO_CONVERSATION(label) : nullptr;
				out.push_back({collection, overlay, static_cast<int>(data[4]), static_cast<int>(data[5]),
				    text && *text && std::string(text) != "NULL" ? std::string(text) : std::format("刺青 {}", out.size() + 1)});
			}
			static bool logged = false;
			if (!std::exchange(logged, true))
				ml::Log("tattoo shop: {} tattoos for character type {}", out.size(), type);
			return out;
		}

		menu::List ZoneMenu(const char* title, int zone, int shop)
		{
			menu::List list{title, {}, Restore};
			for (const Tattoo& t : Tattoos())
			{
				if (t.zone != zone)
					continue;
				const bool has = look::HasTattoo(t.collection, t.overlay);
				menu::Item item;
				item.label = t.name;
				item.detail = has ? "已有" : Money(t.cost);
				item.help = has ? "Enter 去除（免費）" : "";
				item.keepOpen = true;
				item.action = [t, shop] {
					if (look::HasTattoo(t.collection, t.overlay))
					{
						look::RemoveTattoo(t.collection, t.overlay);
						ml::Notify("已去除");
					}
					else if (Pay(t.cost, shop))
					{
						look::AddTattoo(t.collection, t.overlay);
						ml::Notify("刺好了");
					}
				};
				item.hover = [t] {
					Restore();
					if (const ::Ped ped = officer::Ped())
						PED::ADD_PED_DECORATION_FROM_HASHES(ped, t.collection, t.overlay);
				};
				list.items.push_back(std::move(item));
			}
			return list;
		}

		menu::List TattooMenu(int shop)
		{
			// Zones: 0 torso, 1 head, 2 left arm, 3 right arm, 4 left leg, 5 right leg.
			static const char* const kZones[] = {"身體", "頭部", "左手", "右手", "左腳", "右腳"};
			menu::List list{"刺青店", {}, Restore};
			for (int z = 0; z < 6; ++z)
				list.items.push_back({kZones[z], "", "", true, false, nullptr, [z, shop] { return ZoneMenu(kZones[z], z, shop); }});
			return list;
		}

		const Shop* Near()
		{
			const ::Ped ped = PLAYER::PLAYER_PED_ID();
			const Vector3 p = ENTITY::GET_ENTITY_COORDS(ped, true);
			for (const Shop& s : kShops)
			{
				const float dx = p.x - s.at.x, dy = p.y - s.at.y, dz = p.z - s.at.z;
				if (dx * dx + dy * dy < kShopRadius * kShopRadius && std::fabs(dz) < 4.0f)
					return &s;
			}
			return nullptr;
		}
	}

	void Tick()
	{
		if (!officer::IsPlayer() || menu::IsOpen())
			return;
		const ::Ped ped = PLAYER::PLAYER_PED_ID();
		if (PED::IS_PED_IN_ANY_VEHICLE(ped, true) || PLAYER::GET_PLAYER_WANTED_LEVEL(PLAYER::PLAYER_ID()) > 0)
			return;
		const Shop* shop = Near();
		if (!shop)
			return;
		static const char* const kHelp[] = {"按 ~INPUT_CONTEXT~ 理髮", "按 ~INPUT_CONTEXT~ 逛服飾", "按 ~INPUT_CONTEXT~ 刺青"};
		HUD::BEGIN_TEXT_COMMAND_DISPLAY_HELP("STRING");
		HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME(kHelp[static_cast<int>(shop->kind)]);
		HUD::END_TEXT_COMMAND_DISPLAY_HELP(0, false, false, -1);
		if (!PAD::IS_CONTROL_JUST_PRESSED(0, 51))
			return;
		const int id = shop->id;
		switch (shop->kind)
		{
		case Kind::Hair: menu::Open(HairMenu(id)); break;
		case Kind::Clothes: menu::Open(ClothesMenu(id)); break;
		case Kind::Tattoo: menu::Open(TattooMenu(id)); break;
		}
	}
}
