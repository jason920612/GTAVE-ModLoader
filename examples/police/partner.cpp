// A colleague riding with the officer for the shift (police life part 3, the user's design): the field training officer
// (FTO) during probation, and, from Officer III, a rookie the officer trains.
// - FTO: on every probation shift a training officer of the officer's shift (Officer III or above, colleagues.cpp)
//   comes along: in the patrol car's passenger seat, on foot with the officer, armed (the game's group: they follow,
//   get in and out with the officer, fight with them; they count as police on scene for the suspects). They comment
//   on what the officer does: speeding without lights, the siren without a call, shooting at someone who is not a
//   threat (bad); a case closed, a good report (good), tips now and then. At clock-out their rating of the shift (75,
//   +5 a good point, -8 a bad one) goes to the evaluation (career.cpp: the first promotion counts it).
// - Rookie: from Officer III the officer may take a rookie along for the shift (the station's menu). Now and then,
//   when nothing is going on, the rookie asks a question from the exam (career.cpp), answered in the menu; a shift
//   of training (4 hours or more) counts for the evaluation.
#include "police.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <vector>

namespace police::partner
{
	namespace
	{
		using ai::Distance;

		enum class Role
		{
			None,
			Trainer, // the officer's FTO
			Rookie,  // the officer trains them
		};
		Role g_role = Role::None;
		Role g_wanted = Role::None; // a rookie asked for at the station (this shift)
		int g_colleague = -1;
		::Ped g_ped = 0;
		uint64_t g_since = 0;
		int g_good = 0, g_bad = 0, g_answered = 0;
		uint64_t g_nextSpeed = 0, g_nextSiren = 0, g_nextShot = 0, g_nextTip = 0, g_nextQuestion = 0;
		int g_lastReport = 0, g_lastScores = 0;
		bool g_spawnFailed = false;

		const char* const kTips[] = {
		    "巡邏的時候多看看路邊停的車，車牌辨識會幫你，但眼睛更重要。",
		    "攔停之前先想好要停在哪裡，別讓自己站在車流裡。",
		    "盤查的時候站在駕駛座車門後面一點，對方開門也撞不到你。",
		    "嫌犯上銬之後一定要搜身，很多人就是在這裡出事。",
		    "報告趁記憶還新的時候寫，下班前交，不然考評很難看。",
		    "支援叫得早不丟臉，等到出事才叫就來不及了。",
		    "夜班的時候注意路口，酒駕的多半這時候出來。",
		};
		const char* const kRookieThanks[] = {"原來如此，謝謝前輩！", "我記下來了！", "難怪教官上課一直強調這個。"};

		bool Exists(::Ped p)
		{
			return p && ENTITY::DOES_ENTITY_EXIST(p) && !ENTITY::IS_ENTITY_DEAD(p, false);
		}

		void Say(const std::string& line)
		{
			if (g_colleague < 0)
				return;
			HUD::BEGIN_TEXT_COMMAND_PRINT("STRING");
			HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME((colleagues::Name(g_colleague) + "：" + line).c_str());
			HUD::END_TEXT_COMMAND_PRINT(6000, true);
		}

		void Remove()
		{
			if (g_ped && ENTITY::DOES_ENTITY_EXIST(g_ped))
			{
				PED::REMOVE_PED_FROM_GROUP(g_ped);
				ENTITY::SET_ENTITY_AS_MISSION_ENTITY(g_ped, true, true);
				PED::DELETE_PED(&g_ped);
			}
			g_ped = 0;
			if (g_colleague >= 0)
				colleagues::SetBusy(g_colleague, false);
		}

		bool Spawn(Role role)
		{
			const int who = role == Role::Trainer ? colleagues::PickTrainer() : colleagues::PickRookie();
			if (who < 0)
				return false;
			const ::Ped pl = PLAYER::PLAYER_PED_ID();
			const Vector3 behind = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(pl, -1.2f, -1.5f, 0.0f);
			Vector3 at = behind;
			if (Vector3 snapped{}; ai::OnFoot(behind.x, behind.y, behind.z, snapped) && Distance(snapped, behind) < 4.0f)
				at = snapped;
			const Hash model = colleagues::Model(who);
			const auto loaded = ml::LoadModel(model);
			if (!loaded)
				return false;
			const ::Ped p = PED::CREATE_PED(6, model, at.x, at.y, at.z, ENTITY::GET_ENTITY_HEADING(pl), false, false);
			if (!p)
				return false;
			ENTITY::SET_ENTITY_AS_MISSION_ENTITY(p, true, true);
			colleagues::Dress(p, who);
			PED::SET_PED_RELATIONSHIP_GROUP_HASH(p, MISC::GET_HASH_KEY("COP"));
			WEAPON::GIVE_WEAPON_TO_PED(p, MISC::GET_HASH_KEY("WEAPON_COMBATPISTOL"), 200, false, true);
			PED::SET_PED_ARMOUR(p, 50);
			PED::SET_PED_CAN_BE_TARGETTED_BY_PLAYER(p, PLAYER::PLAYER_ID(), false);
			PED::SET_PED_AS_GROUP_MEMBER(p, PLAYER::GET_PLAYER_GROUP(PLAYER::PLAYER_ID()));
			PED::SET_PED_NEVER_LEAVES_GROUP(p, true);
			PED::SET_PED_COMBAT_ABILITY(p, 2);
			g_ped = p;
			g_colleague = who;
			g_role = role;
			g_since = ml::TickMs();
			g_good = g_bad = g_answered = 0;
			g_lastReport = records::LastId();
			g_lastScores = static_cast<int>(records::Scores(0).size());
			g_nextTip = ml::TickMs() + 240000;
			g_nextQuestion = ml::TickMs() + 180000;
			colleagues::SetBusy(who, true);
			if (role == Role::Trainer)
				Say(std::format("我是你的野外訓練官{}。試用期這幾班我跟你一起出勤，照我教的做。", colleagues::Name(who)));
			else
				Say("前輩好！今天請多指教！");
			ml::Log("partner: {} rides along ({})", colleagues::Name(who), role == Role::Trainer ? "FTO" : "rookie");
			return true;
		}

		void Bad(const char* line)
		{
			++g_bad;
			Say(line);
			ml::Log("partner: FTO noted a mistake ({})", line);
		}

		void Good(const char* line)
		{
			++g_good;
			Say(line);
		}

		// What the training officer sees.
		void Watch()
		{
			const uint64_t now = ml::TickMs();
			const ::Ped pl = PLAYER::PLAYER_PED_ID();
			const Vehicle car = PED::IS_PED_IN_ANY_VEHICLE(pl, false) ? PED::GET_VEHICLE_PED_IS_IN(pl, false) : 0;
			suspect::Target t;
			const bool pursuit = suspect::Pursued(t);
			const bool busy = !dispatch::Idle() || assist::Busy() || pursuit || custody::Busy();
			if (car && VEHICLE::GET_VEHICLE_CLASS(car) == 18)
			{
				const int stage = lights::Stage(car);
				if (now >= g_nextSpeed && ENTITY::GET_ENTITY_SPEED(car) > 33.0f && stage < 2)
				{
					g_nextSpeed = now + 90000;
					Bad("沒開警燈就別開那麼快，出事是你要負責。");
				}
				if (now >= g_nextSiren && stage == 3 && !busy)
				{
					g_nextSiren = now + 120000;
					Bad("沒有案子不要亂開警笛。");
				}
			}
			if (now >= g_nextShot && PED::IS_PED_SHOOTING(pl) && !(pursuit && t.mode == suspect::Target::Mode::Armed))
			{
				g_nextShot = now + 60000;
				Bad("你在幹嘛？！對方沒有威脅，為什麼開槍？");
			}
			// A case closed (a report drafted), a report submitted.
			if (const int last = records::LastId(); last > g_lastReport)
			{
				g_lastReport = last;
				Good("案子處理得不錯。記得下班前把報告交了。");
			}
			if (const std::vector<int> scores = records::Scores(0); static_cast<int>(scores.size()) > g_lastScores)
			{
				g_lastScores = static_cast<int>(scores.size());
				if (scores.back() >= 70)
					Good("報告寫得很完整，這樣就對了。");
				else
					Bad("報告要寫詳細一點，罪名要跟證據對得上。");
			}
			if (now >= g_nextTip && !busy)
			{
				g_nextTip = now + 300000 + static_cast<uint64_t>(ai::Random() * 180000.0f);
				Say(kTips[static_cast<size_t>(ai::Random() * std::size(kTips)) % std::size(kTips)]);
			}
		}

		// The rookie asks something (an exam question), answered in the menu.
		void Ask()
		{
			const uint64_t now = ml::TickMs();
			suspect::Target t;
			if (now < g_nextQuestion || menu::IsOpen() || !dispatch::Idle() || assist::Busy() || suspect::Pursued(t) || custody::Busy())
				return;
			g_nextQuestion = now + 300000 + static_cast<uint64_t>(ai::Random() * 120000.0f);
			std::string question;
			std::vector<std::string> options;
			int right = 0;
			if (!career::Quiz(question, options, right))
				return;
			Say("前輩，請問一下：" + question);
			menu::List m{"新人問你", {}};
			for (size_t i = 0; i < options.size(); ++i)
			{
				menu::Item o{options[i], "", question};
				const bool correct = static_cast<int>(i) == right;
				const std::string answer = options[static_cast<size_t>(right)];
				o.action = [correct, answer] {
					if (correct)
					{
						++g_answered;
						Say(kRookieThanks[static_cast<size_t>(ai::Random() * std::size(kRookieThanks)) % std::size(kRookieThanks)]);
					}
					else
						Say("咦？可是訓練教材上寫的是「" + answer + "」耶……");
				};
				m.items.push_back(o);
			}
			menu::Open(m);
		}
	}

	::Ped Ped()
	{
		return Exists(g_ped) ? g_ped : 0;
	}

	bool MayTrain()
	{
		return career::May(career::Unlock::FieldTraining) && !shift::OnProbation();
	}

	void WantRookie(bool want)
	{
		g_wanted = want ? Role::Rookie : Role::None;
		if (!want && g_role == Role::Rookie)
		{
			Say("好的，那我先回去了。");
			Remove();
			g_role = Role::None;
		}
	}

	bool RookieWanted()
	{
		return g_wanted == Role::Rookie;
	}

	void ShiftOver()
	{
		if (g_role == Role::None)
			return;
		const uint64_t minutes = (ml::TickMs() - g_since) / 60000; // real minutes
		if (g_role == Role::Trainer)
		{
			const int rating = std::clamp(75 + g_good * 5 - g_bad * 8, 0, 100);
			const char* word = rating >= 85 ? "表現很好，繼續保持" : rating >= 70 ? "還可以，有幾個地方要注意" : "今天要檢討的地方不少";
			Say(std::format("今天{}。", word));
			career::AddTrainerRating(rating);
			ml::Notify("野外訓練官{}的評分：{}（好的表現 {}、缺失 {}）", colleagues::Name(g_colleague), rating, g_good, g_bad);
		}
		else if (minutes >= 30) // about four game hours at the duty clock's 1/3
		{
			career::AddBonus(2, "帶訓新人");
			ml::Notify("帶新人一班：考評 +2（新人答對 {} 題）", g_answered);
		}
		Remove();
		g_role = Role::None;
		g_wanted = Role::None;
	}

	void Forget()
	{
		g_ped = 0;
		g_role = Role::None;
		g_wanted = Role::None;
		g_colleague = -1;
	}

	void Tick()
	{
		const bool working = officer::IsPlayer() && duty::OnDuty();
		if (!working)
		{
			if (g_role != Role::None)
			{
				Remove();
				g_role = Role::None;
			}
			g_spawnFailed = false;
			return;
		}
		const Role want = shift::OnProbation() ? Role::Trainer : g_wanted;
		if (g_role != want && want != Role::None && !g_spawnFailed)
		{
			if (g_role != Role::None)
				Remove();
			g_role = Role::None;
			if (!Spawn(want))
				g_spawnFailed = true; // (nobody to come: not asked again this time on duty)
		}
		if (g_role == Role::None)
			return;
		if (!Exists(g_ped))
		{
			// Hurt or lost: the ride-along is over for this shift.
			ml::Log("partner: the partner is gone");
			if (g_colleague >= 0)
				colleagues::SetBusy(g_colleague, false);
			g_ped = 0;
			g_role = Role::None;
			g_spawnFailed = true;
			return;
		}
		// Back with the officer when left behind far (a teleport, a fast drive while they were out).
		const ::Ped pl = PLAYER::PLAYER_PED_ID();
		if (Distance(ENTITY::GET_ENTITY_COORDS(g_ped, true), ENTITY::GET_ENTITY_COORDS(pl, true)) > 150.0f && !PED::IS_PED_IN_ANY_VEHICLE(g_ped, false))
		{
			const Vehicle car = PED::GET_VEHICLE_PED_IS_IN(pl, false);
			if (car && VEHICLE::IS_VEHICLE_SEAT_FREE(car, 0, false))
				PED::SET_PED_INTO_VEHICLE(g_ped, car, 0);
		}
		if (!PED::IS_PED_GROUP_MEMBER(g_ped, PLAYER::GET_PLAYER_GROUP(PLAYER::PLAYER_ID())))
		{
			PED::SET_PED_AS_GROUP_MEMBER(g_ped, PLAYER::GET_PLAYER_GROUP(PLAYER::PLAYER_ID()));
			PED::SET_PED_NEVER_LEAVES_GROUP(g_ped, true);
		}
		if (g_role == Role::Trainer)
			Watch();
		else
			Ask();
	}
}
