// The officer's look, made in a GTA Online-style creator (user's choice): sex, parents (face and skin blend), 20 face
// features, hair and colours, eyebrows, beard, eye colour, name and department. Kept in the game's save
// (ml::save "officer_look"), so it follows the saves like the rest of the officer.
//
// The creator is a page of the loader window (F4 > Police > 外觀): every change shows on the officer at once, and while
// it is being edited a camera looks at the officer's face (or the whole body). The freemode heads: mothers 21-41 and
// 45, fathers 0-20 and 42-44 (as Online's creator); SET_PED_MICRO_MORPH 0-19 are the face features.
#include "police.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace police::look
{
	namespace
	{
		struct Look
		{
			int sex = 0; // 0 male, 1 female
			int mother = 0, father = 0;
			float resemblance = 0.5f, skin = 0.5f; // 0 = mother, 1 = father
			std::array<float, 20> features{};
			int hair = 4, hairColor = 0, highlight = 0;
			int brows = 0, browColor = 0;
			int beard = -1, beardColor = 0; // -1 none
			int eyes = 0;
			std::string name;
			int department = 0; // 0 LSPD, 1 BCSO, 2 SAHP, 3 NOOSE
		} g;

		const int kMothers[] = {21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 45};
		const int kFathers[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 42, 43, 44};
		const char* const kFeatures[] = {"鼻子寬度", "鼻峰高度", "鼻子長度", "鼻樑", "鼻尖", "鼻樑歪斜", "眉毛高度", "眉毛深度", "顴骨高度", "顴骨寬度",
		    "臉頰", "眼睛", "嘴唇", "下顎寬度", "下顎長度", "下巴高度", "下巴長度", "下巴寬度", "下巴凹痕", "脖子粗細"};
		const std::vector<std::string> kDepartments = {"洛聖都警局 LSPD", "郡警 BCSO", "公路巡警 SAHP", "特勤 NOOSE（高階解鎖）"};

		// ---- menu items ----
		ml::Item g_sex, g_mother, g_father, g_resemblance, g_skin, g_hair, g_hairColor, g_highlight, g_brows, g_browColor, g_beard, g_beardColor,
		    g_eyes, g_department, g_nameLabel;
		std::array<ml::Item, 20> g_featureItems;
		bool g_headshotStale = false; // the wheel's picture shows an older look
		bool g_syncing = false; // menu values are being set from the look: not a change by the player

		ml::Json ToJson()
		{
			ml::Json features = ml::Json::Array();
			for (const float f : g.features)
				features.Push(f);
			return {{"sex", g.sex}, {"mother", g.mother}, {"father", g.father}, {"resemblance", g.resemblance}, {"skin", g.skin},
			    {"features", features}, {"hair", g.hair}, {"hairColor", g.hairColor}, {"highlight", g.highlight}, {"brows", g.brows},
			    {"browColor", g.browColor}, {"beard", g.beard}, {"beardColor", g.beardColor}, {"eyes", g.eyes}, {"name", g.name},
			    {"department", g.department}};
		}
		void FromJson(const ml::Json& j)
		{
			g = Look{};
			if (!j.IsObject())
				return;
			g.sex = j["sex"].Int(0);
			g.mother = j["mother"].Int(0);
			g.father = j["father"].Int(0);
			g.resemblance = j["resemblance"].Float(0.5f);
			g.skin = j["skin"].Float(0.5f);
			for (size_t i = 0; i < g.features.size() && i < j["features"].Size(); ++i)
				g.features[i] = j["features"][i].Float(0.0f);
			g.hair = j["hair"].Int(4);
			g.hairColor = j["hairColor"].Int(0);
			g.highlight = j["highlight"].Int(0);
			g.brows = j["brows"].Int(0);
			g.browColor = j["browColor"].Int(0);
			g.beard = j["beard"].Int(-1);
			g.beardColor = j["beardColor"].Int(0);
			g.eyes = j["eyes"].Int(0);
			g.name = j["name"].Str();
			g.department = j["department"].Int(0);
		}

		void Store()
		{
			ml::save::Set("officer_look", ToJson());
		}

		// Menu values from the look.
		void Sync()
		{
			g_syncing = true;
			g_sex.Set(static_cast<float>(g.sex));
			g_mother.Set(static_cast<float>(g.mother));
			g_father.Set(static_cast<float>(g.father));
			g_resemblance.Set(g.resemblance * 100.0f);
			g_skin.Set(g.skin * 100.0f);
			for (size_t i = 0; i < g.features.size(); ++i)
				g_featureItems[i].Set(g.features[i] * 100.0f);
			g_hair.Set(static_cast<float>(g.hair));
			g_hairColor.Set(static_cast<float>(g.hairColor));
			g_highlight.Set(static_cast<float>(g.highlight));
			g_brows.Set(static_cast<float>(g.brows));
			g_browColor.Set(static_cast<float>(g.browColor));
			g_beard.Set(static_cast<float>(g.beard + 1));
			g_beardColor.Set(static_cast<float>(g.beardColor));
			g_eyes.Set(static_cast<float>(g.eyes));
			g_department.Set(static_cast<float>(g.department));
			g_nameLabel.Label("姓名：" + (g.name.empty() ? std::string("（未設定）") : g.name));
			g_syncing = false;
		}

		// A change in the menu: into the look, onto the officer, into the save.
		void Changed(bool remodel)
		{
			if (g_syncing)
				return;
			const int sex = g_sex.Int();
			remodel = remodel || sex != g.sex;
			g.sex = sex;
			g.mother = g_mother.Int();
			g.father = g_father.Int();
			g.resemblance = g_resemblance.Value() / 100.0f;
			g.skin = g_skin.Value() / 100.0f;
			for (size_t i = 0; i < g.features.size(); ++i)
				g.features[i] = g_featureItems[i].Value() / 100.0f;
			g.hair = g_hair.Int();
			g.hairColor = g_hairColor.Int();
			g.highlight = g_highlight.Int();
			g.brows = g_brows.Int();
			g.browColor = g_browColor.Int();
			g.beard = g_beard.Int() - 1;
			g.beardColor = g_beardColor.Int();
			g.eyes = g_eyes.Int();
			if (g_department.Int() == 3)
			{
				ml::Notify("特勤要升到高階才能加入");
				g_syncing = true;
				g_department.Set(static_cast<float>(g.department));
				g_syncing = false;
			}
			else
				g.department = g_department.Int();
			Store();
			g_headshotStale = true;
			if (remodel)
				officer::Rebuild();
			else if (const ::Ped ped = officer::Ped())
				Apply(ped, officer::InUniform());
		}

		// ---- editing camera ----
		Cam g_cam = 0;
		bool g_face = true;

		void Camera()
		{
			const ::Ped ped = officer::Ped();
			if (!ped)
				return;
			if (!g_cam)
				g_cam = CAMERA::CREATE_CAM("DEFAULT_SCRIPTED_CAMERA", true);
			Vector3 fwd{}, right{}, up{}, pos{};
			ENTITY::GET_ENTITY_MATRIX(ped, &fwd, &right, &up, &pos);
			const Vector3 head = PED::GET_PED_BONE_COORDS(ped, 31086, 0, 0, 0); // SKEL_Head
			const float fov = g_face ? 30.0f : 50.0f;
			const Vector3 target = g_face ? head : Vector3{pos.x, pos.y, pos.z - 0.1f};
			const float d = g_face ? 0.9f : 2.8f;
			// The loader window covers the middle of the screen: the officer is framed on its right. Looking at a point
			// to their right (the camera's left) puts them right of centre.
			const float shift = d * std::tan(fov * 0.5f * 3.14159265f / 180.0f) * 1.78f * 0.72f;
			const Vector3 aim = {target.x + right.x * shift, target.y + right.y * shift, target.z};
			CAMERA::SET_CAM_COORD(g_cam, target.x + fwd.x * d + right.x * shift, target.y + fwd.y * d + right.y * shift, target.z + 0.02f);
			CAMERA::POINT_CAM_AT_COORD(g_cam, aim.x, aim.y, aim.z);
			CAMERA::SET_CAM_FOV(g_cam, fov);
			CAMERA::SET_CAM_ACTIVE(g_cam, true);
			CAMERA::RENDER_SCRIPT_CAMS(true, false, 0, true, false, 0);
		}

		bool g_editing = false;
	}

	Hash Model()
	{
		return MISC::GET_HASH_KEY(g.sex == 1 ? "mp_f_freemode_01" : "mp_m_freemode_01");
	}

	std::string Name()
	{
		return g.name;
	}

	int Department()
	{
		return g.department;
	}

	void Apply(::Ped ped, bool uniform)
	{
		const bool female = g.sex == 1;
		PED::SET_PED_DEFAULT_COMPONENT_VARIATION(ped);
		const int mother = kMothers[std::clamp(g.mother, 0, static_cast<int>(std::size(kMothers)) - 1)];
		const int father = kFathers[std::clamp(g.father, 0, static_cast<int>(std::size(kFathers)) - 1)];
		PED::SET_PED_HEAD_BLEND_DATA(ped, mother, father, 0, mother, father, 0, g.resemblance, g.skin, 0.0f, false);
		for (int i = 0; i < 20; ++i)
			PED::SET_PED_MICRO_MORPH(ped, i, g.features[static_cast<size_t>(i)]);
		const int hairs = PED::GET_NUMBER_OF_PED_DRAWABLE_VARIATIONS(ped, 2);
		PED::SET_PED_COMPONENT_VARIATION(ped, 2, hairs > 0 ? std::clamp(g.hair, 0, hairs - 1) : 0, 0, 0);
		PED::SET_PED_HAIR_TINT(ped, g.hairColor, g.highlight);
		PED::SET_PED_HEAD_OVERLAY(ped, 2, g.brows, 1.0f); // eyebrows
		PED::SET_PED_HEAD_OVERLAY_TINT(ped, 2, 1, g.browColor, g.browColor);
		PED::SET_PED_HEAD_OVERLAY(ped, 1, !female && g.beard >= 0 ? g.beard : 255, 1.0f); // beard
		PED::SET_PED_HEAD_OVERLAY_TINT(ped, 1, 1, g.beardColor, g.beardColor);
		PED::SET_HEAD_BLEND_EYE_COLOR(ped, g.eyes);
		// Clothes: the summer patrol uniform on duty (per department later; LSPD's for now), own clothes otherwise.
		if (uniform)
		{
			PED::SET_PED_COMPONENT_VARIATION(ped, 3, female ? 14 : 0, 0, 0);  // arms
			PED::SET_PED_COMPONENT_VARIATION(ped, 4, female ? 34 : 35, 0, 0); // trousers
			PED::SET_PED_COMPONENT_VARIATION(ped, 6, female ? 25 : 25, 0, 0); // shoes
			PED::SET_PED_COMPONENT_VARIATION(ped, 8, female ? 35 : 58, 0, 0); // undershirt and belt
			PED::SET_PED_COMPONENT_VARIATION(ped, 11, female ? 48 : 55, 0, 0); // shirt
			PED::CLEAR_PED_PROP(ped, 0, 0); // no cap: the freemode police caps (45/46) carry the sheriff's star
		}
		else
		{
			PED::SET_PED_COMPONENT_VARIATION(ped, 3, female ? 14 : 0, 0, 0);
			PED::SET_PED_COMPONENT_VARIATION(ped, 4, female ? 0 : 4, 0, 0);  // jeans
			PED::SET_PED_COMPONENT_VARIATION(ped, 6, female ? 3 : 1, 0, 0);  // trainers
			PED::SET_PED_COMPONENT_VARIATION(ped, 8, female ? 14 : 15, 0, 0); // nothing under
			PED::SET_PED_COMPONENT_VARIATION(ped, 11, female ? 0 : 0, 0, 0);  // T-shirt / top
			PED::CLEAR_PED_PROP(ped, 0, 0);
		}
	}

	void Loaded()
	{
		FromJson(ml::save::Get("officer_look"));
		Sync();
	}

	bool Exists()
	{
		return ml::save::Get("officer_look").IsObject();
	}

	void BuildMenu()
	{
		const ml::Page page = ml::Root().Sub("外觀（角色建立）");
		page.Action("開始編輯（鏡頭對準警察）", [] {
			if (!officer::IsPlayer())
			{
				ml::Notify("切到警察才能編輯外觀");
				return;
			}
			g_editing = true;
		});
		page.Action("切換鏡頭：臉／全身", [] { g_face = !g_face; });
		page.Action("完成編輯", [] { g_editing = false; });
		g_nameLabel = page.Text("姓名：（未設定）");
		page.Action("設定姓名（關掉 F4 視窗後輸入）", [] {
			MISC::DISPLAY_ONSCREEN_KEYBOARD(0, "FMMC_KEY_TIP8", "", g.name.c_str(), "", "", "", 30);
			int state = 0;
			while ((state = MISC::UPDATE_ONSCREEN_KEYBOARD()) == 0)
				ml::Wait(0);
			if (state == 1)
				if (const char* text = MISC::GET_ONSCREEN_KEYBOARD_RESULT(); text && *text)
				{
					g.name = text;
					Store();
					Sync();
				}
		});
		g_department = page.List(nullptr, "單位", kDepartments, 0);
		g_department.OnChange([] { Changed(false); });

		const ml::Page body = page.Sub("性別與父母");
		g_sex = body.List(nullptr, "性別", {"男", "女"}, 0);
		std::vector<std::string> mothers, fathers;
		for (size_t i = 0; i < std::size(kMothers); ++i)
			mothers.push_back("母親 " + std::to_string(i + 1));
		for (size_t i = 0; i < std::size(kFathers); ++i)
			fathers.push_back("父親 " + std::to_string(i + 1));
		g_mother = body.List(nullptr, "母親", mothers, 0);
		g_father = body.List(nullptr, "父親", fathers, 0);
		g_resemblance = body.Number(nullptr, "長相像父親（%）", 0, 100, 5, 50);
		g_skin = body.Number(nullptr, "膚色像父親（%）", 0, 100, 5, 50);
		for (const ml::Item* item : {&g_sex, &g_mother, &g_father, &g_resemblance, &g_skin})
			item->OnChange([] { Changed(false); });

		const ml::Page face = page.Sub("臉部特徵");
		for (size_t i = 0; i < g_featureItems.size(); ++i)
		{
			g_featureItems[i] = face.Number(nullptr, kFeatures[i], -100, 100, 5, 0);
			g_featureItems[i].OnChange([] { Changed(false); });
		}
		face.Action("全部歸零", [] {
			g.features.fill(0.0f);
			Store();
			Sync();
			if (const ::Ped ped = officer::Ped())
				Apply(ped, officer::InUniform());
		});

		const ml::Page hair = page.Sub("頭髮與五官");
		g_hair = hair.Number(nullptr, "髮型", 0, 80, 1, 4);
		g_hairColor = hair.Number(nullptr, "髮色", 0, 63, 1, 0);
		g_highlight = hair.Number(nullptr, "挑染", 0, 63, 1, 0);
		g_brows = hair.Number(nullptr, "眉毛", 0, 33, 1, 0);
		g_browColor = hair.Number(nullptr, "眉毛顏色", 0, 63, 1, 0);
		g_beard = hair.Number(nullptr, "鬍子（0 = 無，女性無效）", 0, 29, 1, 0);
		g_beardColor = hair.Number(nullptr, "鬍子顏色", 0, 63, 1, 0);
		g_eyes = hair.Number(nullptr, "眼睛顏色", 0, 31, 1, 0);
		for (const ml::Item* item : {&g_hair, &g_hairColor, &g_highlight, &g_brows, &g_browColor, &g_beard, &g_beardColor, &g_eyes})
			item->OnChange([] { Changed(false); });
		Sync();
	}

	void Tick()
	{
		if (g_editing && !officer::IsPlayer())
			g_editing = false;
		if (g_editing)
		{
			const ::Ped ped = PLAYER::PLAYER_PED_ID();
			TASK::CLEAR_PED_TASKS(ped);
			PAD::DISABLE_ALL_CONTROL_ACTIONS(0);
			HUD::HIDE_HUD_AND_RADAR_THIS_FRAME();
			Camera();
		}
		else if (g_cam || g_headshotStale)
		{
			if (g_headshotStale)
				officer::RefreshHeadshot();
			g_headshotStale = false;
			if (g_cam)
			{
				CAMERA::RENDER_SCRIPT_CAMS(false, false, 0, true, false, 0);
				CAMERA::DESTROY_CAM(g_cam, false);
				g_cam = 0;
			}
		}
	}
}
