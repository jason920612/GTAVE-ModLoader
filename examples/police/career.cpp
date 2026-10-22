// The officer's working life, part 2 (user's design 2026-10-22, research/phase0.md §56): rank and promotion.
//
// Ranks: Officer I-III, Sergeant I-II, Lieutenant (the sheriff's and the highway patrol's own titles). Promotion to the
// next rank takes seniority (shifts worked at the rank), an evaluation score (the reports' scores, less late, leaving
// early, absent, reports overdue and, later, discipline, all since the last promotion) and enough reports, and then an
// exam of 15 multiple-choice questions on the station computer (the in-car computer's page opened at the station: G at
// the locker); 70% passes, a failed exam can be taken again after 3 more shifts.
// A rank brings its pay (shift.cpp) and unlocks: patrol cars (duty.cpp), long guns in the equipment (Officer II a
// shotgun, Officer III a carbine), one more backup unit for sergeants, NOOSE from Sergeant II (the creator's
// department), and the colleagues' roles (field training officer, supervising) when those come.
//
// Kept in the save: ml::save "officer_promotion".
#include "police.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <vector>

namespace police::career
{
	namespace
	{
		struct RankInfo
		{
			const char* title[4]; // LSPD, BCSO, SAHP, NOOSE (look::Department())
			int rate;              // pay per hour
		};
		constexpr RankInfo kRanks[] = {
		    {{"警員一級", "副警長一級", "公路警員一級", "特勤隊員一級"}, 200},
		    {{"警員二級", "副警長二級", "公路警員二級", "特勤隊員二級"}, 225},
		    {{"警員三級", "副警長三級", "公路警員三級", "特勤隊員三級"}, 250},
		    {{"巡佐一級", "巡佐一級", "巡佐一級", "小隊長一級"}, 300},
		    {{"巡佐二級", "巡佐二級", "巡佐二級", "小隊長二級"}, 330},
		    {{"警督", "警督", "警督", "中隊長"}, 400},
		};
		constexpr int kTop = static_cast<int>(std::size(kRanks)) - 1;

		// What promotion to the next rank takes (by the current rank): shifts at the rank, evaluation, reports submitted.
		struct Need
		{
			int shifts, eval, reports;
		};
		constexpr Need kNeed[] = {{10, 60, 3}, {15, 65, 5}, {20, 70, 8}, {25, 75, 10}, {30, 80, 12}};
		constexpr int kQuestions = 15, kPass = 11, kRetryShifts = 3;

		// What each rank unlocks (shown on the computer; the effects are in duty.cpp, units.cpp, look.cpp).
		const char* const kUnlocks[] = {
		    "基本巡邏車、手槍、電擊槍、警棍、手電筒",
		    "更多巡邏車、霰彈槍",
		    "未塗裝警車與攔截車、卡賓槍、可擔任野外訓練官",
		    "全部巡邏車、多一組支援、督導同仁",
		    "可加入特勤 NOOSE",
		    "指揮全時段勤務",
		};

		// ---- the exam's questions (three levels: officers, sergeants, lieutenants; options shuffled when asked) -------
		struct Question
		{
			const char* text;
			std::array<const char*, 4> options; // the first is right
		};
		const Question kOfficer[] = {
		    {"攔停車輛時，應該先開哪一段警燈？", {"第二段：全部警燈，不開警笛", "第一段：只有後方黃燈", "第三段：警燈加警笛", "不開燈，按喇叭就好"}},
		    {"嫌犯上銬之後，下一步應該做什麼？", {"搜身，確認身上沒有武器或違禁品", "立刻放進警車", "先開罰單", "解開手銬讓他休息"}},
		    {"案件報告應該在什麼時候提交？", {"下班前", "隔天上班時", "月底", "有空再交"}},
		    {"看到勤前教育交代的協尋車輛，應該怎麼做？", {"攔停並查證車牌", "不用理會", "只拍照", "跟到它停下來，不要攔停"}},
		    {"盤查證件時查到對方有通緝，應該怎麼做？", {"依法逮捕並上銬", "讓他離開", "罰款了事", "只做紀錄"}},
		    {"車上電腦顯示前車「已報失竊」，駕駛還在車上，應該怎麼做？", {"保持距離、呼叫支援，依程序命令駕駛下車", "立刻開槍", "直接撞停", "放行"}},
		    {"嫌犯徒步逃跑時，應該怎麼做？", {"追捕並用無線電回報位置和方向", "對逃跑的人開槍", "放棄追捕", "開車撞他"}},
		    {"使用致命武力的前提是什麼？", {"對員警或他人構成立即的生命威脅", "嫌犯逃跑", "嫌犯辱罵員警", "嫌犯拒絕出示證件"}},
		    {"嫌犯舉手投降、願意配合時，應該怎麼做？", {"收起武力，依程序上銬", "繼續用槍指著並開槍", "放他走", "叫他快跑"}},
		    {"搜車時發現管制藥物，應該怎麼做？", {"當作證物登錄，並寫進報告", "自己保管", "丟掉", "還給駕駛"}},
		    {"在勤務區外巡邏太久，調度會怎麼做？", {"提醒你回到勤務區，案件改派其他單位", "給你加班費", "把你停職", "沒有任何影響"}},
		    {"上班遲到超過多久會被記錄？", {"15 分鐘", "1 分鐘", "1 小時", "不會記錄"}},
		    {"遇到酒駕嫌疑人，應該怎麼做？", {"帶回警局處理，不讓他繼續駕駛", "讓他自己開回家", "罰款就好", "沒收鑰匙後離開"}},
		    {"押送嫌犯回警局時，嫌犯應該坐在哪裡？", {"警車後座", "前座", "後車廂", "自己開車跟著"}},
		    {"對已上銬、沒有反抗的嫌犯施暴，會怎樣？", {"列入違紀，可能受懲處", "考評加分", "沒有影響", "加薪"}},
		    {"高速追逐時最重要的是什麼？", {"注意其他用路人的安全，必要時放棄追逐", "不計代價撞停", "闖紅燈加速", "關閉警燈避免嚇到嫌犯"}},
		    {"民眾報案車輛失竊時，調度會提供什麼？", {"最後目擊的大概區域", "嫌犯住址", "嫌犯照片", "什麼都不提供"}},
		    {"在無線電上，員警用什麼稱呼？", {"呼號", "本名", "綽號", "車牌號碼"}},
		    {"交通攔停時，應該要求駕駛做什麼？", {"熄火，留在車上或依指示下車", "下車跑步", "把車開走", "打電話找律師"}},
		    {"在報告裡，證物應該怎麼寫？", {"詳實記錄證物與發現經過", "省略不寫", "只寫結論", "交給嫌犯簽名就好"}},
		    {"從嫌犯身上搜出未申報的槍枝，可能的罪名是？", {"非法持有武器", "妨害安寧", "違規停車", "無照駕駛"}},
		    {"呼叫支援時應該說明什麼？", {"位置和狀況", "只說「快來」", "什麼都不用說", "只報呼號"}},
		    {"大夜班的時間是？", {"23:00–07:00", "07:00–15:00", "15:00–23:00", "19:00–03:00"}},
		    {"新進員警試用期間排的是哪一班？", {"大夜班", "日班", "小夜班", "自己選"}},
		    {"一次上班超過 8 小時，多出的時數怎麼算？", {"加班，按 1.5 倍計薪", "不計薪", "按 2 倍計薪", "扣薪"}},
		    {"還有報告沒提交就下班，會怎樣？", {"記為報告逾期", "沒事", "立刻停職", "加薪"}},
		    {"面對可能持刀的嫌犯，至少應保持多遠的距離？", {"約 6 到 7 公尺以上", "1 公尺", "貼身控制", "50 公尺"}},
		    {"電擊槍適合用在什麼情況？", {"需要制伏攻擊者，但不需要致命武力時", "已經上銬的人", "逃跑的路人", "開車中的駕駛"}},
		    {"失竊車輛停在路邊、駕駛不在車上時，應該怎麼做？", {"注意周邊、搜尋駕駛，並把車輛列為證物", "立刻離開", "把那台車開走自用", "對空鳴槍"}},
		    {"執勤時開槍之後會有什麼？", {"使用武力的審查", "什麼都沒有", "自動升遷", "放假一天"}},
		};
		const Question kSergeant[] = {
		    {"巡佐在現場的首要職責是什麼？", {"掌握全局、分配人力，確保員警與民眾安全", "親自追捕每一個嫌犯", "寫罰單", "回警局待命"}},
		    {"部屬開槍之後，巡佐應該怎麼做？", {"確保現場安全、通知上級並啟動使用武力審查", "不用管", "銷毀證據", "讓員警先下班"}},
		    {"民眾投訴員警態度不佳，巡佐應該怎麼做？", {"受理並記錄，依程序轉內部調查", "置之不理", "當場處罰員警", "叫民眾離開"}},
		    {"追逐已經危及大量民眾時，巡佐可以怎麼做？", {"命令終止追逐", "要求加速", "叫直升機開槍", "不能干涉"}},
		    {"部屬多次遲到，巡佐應該怎麼做？", {"面談並記錄，必要時懲處", "視而不見", "直接開除", "幫他打卡"}},
		    {"「員警需要協助」的呼叫代表什麼？", {"員警有危險，附近所有單位立即支援", "有空再去", "下班時間到了", "勤前教育開始"}},
		    {"帶班巡佐在勤前教育要交代什麼？", {"勤務區、協尋與今日重點", "個人私事", "午餐地點", "什麼都不用"}},
		    {"現場有多名嫌犯時，應該怎麼做？", {"請求足夠支援，分開控制嫌犯", "一個人處理全部", "放走一半", "等嫌犯自己投降"}},
		    {"新進員警由誰帶領實習？", {"野外訓練官（FTO）", "自己摸索", "民眾", "調度員"}},
		    {"野外訓練官的評語會影響新人的什麼？", {"試用期考核", "薪水以外的一切", "什麼都不影響", "巡邏車的顏色"}},
		    {"員警受傷時，巡佐首先要做什麼？", {"確保送醫和現場安全", "寫報告", "追究責任", "繼續巡邏"}},
		    {"證物保管的原則是什麼？", {"完整記錄經手過程（保管鏈）", "誰撿到誰保管", "隨意放在車上", "交給民眾保管"}},
		    {"審核報告時發現罪名和證據不符，應該怎麼做？", {"退回要求修正", "照樣送出", "自己隨便改", "銷毀報告"}},
		    {"封鎖現場的目的是什麼？", {"保全證據、保護民眾", "方便拍照", "讓交通更順暢", "讓員警休息"}},
		    {"員警下班開私車時遇到犯罪，應該怎麼做？", {"確保自身安全並報案，必要時以下班員警身分處置", "不管", "開槍示警", "叫路人幫忙"}},
		    {"多名員警在場時，武力的使用應該？", {"與威脅程度相稱", "越多越好", "一律開槍", "一律不用"}},
		    {"停職期間的員警可以做什麼？", {"不能執勤，也不支薪", "照常上班", "只能加班", "照常升遷"}},
		    {"員警下班時觸法被通緝，會怎樣？", {"列入違紀調查", "沒事", "考評加分", "升遷"}},
		    {"支援警力到場後，主辦員警應該怎麼做？", {"說明狀況並分派任務", "離開現場", "把案件交給第一個到的人", "回家"}},
		    {"調度把案件派給其他勤務區的單位，通常是因為？", {"你不在自己的勤務區", "你太認真", "隨機決定", "天氣不好"}},
		};
		const Question kLieutenant[] = {
		    {"警督負責什麼？", {"一個時段或單位的整體勤務與人員管理", "單一巡邏車", "只寫報告", "只管交通"}},
		    {"內部調查的結果可能有哪些？", {"不成立、警告、停職、降級或免職", "只有記功", "只有加薪", "沒有結果"}},
		    {"排班時要考量什麼？", {"人力、年資與勤務需求", "只看個人喜好", "抽籤決定", "不用排班"}},
		    {"發生槍擊等重大事件時，警督應該怎麼做？", {"建立指揮、調度足夠資源並與其他單位協調", "等天亮再說", "只派一台車", "關閉無線電"}},
		    {"對外發布案件資訊時應注意什麼？", {"不影響偵查與當事人權益", "公開嫌犯所有隱私", "越快越好、不管內容", "交給民眾發布"}},
		    {"部屬考評的依據是什麼？", {"報告品質、出勤、執勤表現與紀律", "外表", "年齡", "私交"}},
		    {"什麼時候申請特勤（NOOSE）支援？", {"重武裝嫌犯、人質等高風險事件", "交通違規", "寫報告時", "任何時候"}},
		    {"員警加班過多時，主管應該怎麼做？", {"注意疲勞，調整勤務", "鼓勵繼續加班", "不用管", "扣薪"}},
		    {"警局、郡警與公路巡警的轄區關係是？", {"各有轄區，必要時互相支援", "互不往來", "全部由警局管", "沒有轄區之分"}},
		    {"發現部屬有收賄嫌疑，應該怎麼做？", {"依法移送並啟動內部調查", "私下解決", "分一杯羹", "調到別區了事"}},
		    {"審查使用武力的案件時，重點是什麼？", {"當時的威脅是否足以使用那樣的武力", "嫌犯的外表", "員警的年資", "媒體反應"}},
		    {"提升轄區治安最有效的做法是？", {"依犯罪型態調整巡邏重點與資源", "只靠追逐", "減少巡邏", "只開罰單"}},
		    {"員警違紀被免職後會怎樣？", {"交回制服與裝備，不再是警察", "繼續上班", "升遷", "調到別區"}},
		    {"社區警政的核心是什麼？", {"與社區合作、預防犯罪", "只抓人", "不跟民眾說話", "只在警局待命"}},
		    {"指揮多個單位時，無線電應該怎麼用？", {"清楚、簡短，依呼號溝通", "大家同時說話", "用自創暗號", "關掉"}},
		};

		struct Asked
		{
			const Question* q;
			std::array<int, 4> order; // shown option i = q->options[order[i]]
		};

		// ---- the state (kept in the save) ---------------------------------------------------------------------------
		struct State
		{
			int rank = 0;
			shift::Counts since{}; // the counters when the rank began
			int reportsAfter = 0;  // reports with a higher id count for this rank
			int retryAt = -1;      // shifts worked before the exam may be taken again (-1 = any time)
			std::vector<ml::Json> history, exams;
			std::vector<int> trainer; // the training officer's ratings of probation shifts
			int bonus = 0;            // training a rookie, supervising (this rank)
		};
		constexpr int kMaxBonus = 15;
		State g;
		bool g_loaded = false;
		std::vector<Asked> g_exam;

		ml::Json CountsJson(const shift::Counts& c)
		{
			return {{"shifts", c.shifts}, {"late", c.late}, {"early", c.early}, {"absent", c.absent}, {"overdue", c.overdue}};
		}
		shift::Counts CountsFrom(const ml::Json& j)
		{
			return {j["shifts"].Int(), j["late"].Int(), j["early"].Int(), j["absent"].Int(), j["overdue"].Int()};
		}

		void Store()
		{
			ml::save::Set("officer_promotion", ml::Json{{"rank", g.rank}, {"since", CountsJson(g.since)}, {"reportsAfter", g.reportsAfter},
			                                       {"retryAt", g.retryAt}, {"history", g.history}, {"exams", g.exams}, {"trainer", g.trainer},
			                                       {"bonus", g.bonus}});
		}

		void Load()
		{
			if (g_loaded)
				return;
			g_loaded = true;
			g = State{};
			g_exam.clear();
			const ml::Json j = ml::save::Get("officer_promotion");
			if (!j["rank"].IsNumber())
				return; // a new officer: Officer I from the start of the career
			g.rank = std::clamp(j["rank"].Int(), 0, kTop);
			g.since = CountsFrom(j["since"]);
			g.reportsAfter = j["reportsAfter"].Int();
			g.retryAt = j["retryAt"].Int(-1);
			g.history = j["history"].Items();
			g.exams = j["exams"].Items();
			for (const ml::Json& r : j["trainer"].Items())
				g.trainer.push_back(r.Int());
			g.bonus = j["bonus"].Int();
		}

		int Department()
		{
			return std::clamp(look::Department(), 0, 3);
		}

		// Since the rank began.
		shift::Counts Since()
		{
			const shift::Counts now = shift::Totals();
			return {now.shifts - g.since.shifts, now.late - g.since.late, now.early - g.since.early, now.absent - g.since.absent,
			    now.overdue - g.since.overdue};
		}

		struct Evaluation
		{
			int score = 60, reportsAvg = 60, reports = 0, penalty = 0, trainer = -1, bonus = 0;
		};
		Evaluation Evaluate()
		{
			Evaluation e;
			const std::vector<int> scores = records::Scores(g.reportsAfter);
			e.reports = static_cast<int>(scores.size());
			if (!scores.empty())
			{
				int sum = 0;
				for (const int s : scores)
					sum += s;
				e.reportsAvg = sum / e.reports;
			}
			const shift::Counts c = Since();
			e.penalty = c.late * 3 + c.early * 4 + c.absent * 10 + c.overdue * 5;
			int base = e.reportsAvg;
			if (g.rank == 0 && !g.trainer.empty())
			{
				int sum = 0;
				for (const int r : g.trainer)
					sum += r;
				e.trainer = sum / static_cast<int>(g.trainer.size());
				base = (base + e.trainer) / 2; // the FTO's word counts half on probation
			}
			e.bonus = std::min(g.bonus, kMaxBonus);
			e.score = std::clamp(base - e.penalty + e.bonus, 0, 100);
			return e;
		}

		// Why the exam may not be taken now ("" = it may).
		std::string NotReady()
		{
			if (g.rank >= kTop)
				return "已經是最高職級";
			const Need& n = kNeed[g.rank];
			const shift::Counts c = Since();
			const Evaluation e = Evaluate();
			if (c.shifts < n.shifts)
				return std::format("年資還差 {} 班", n.shifts - c.shifts);
			if (e.reports < n.reports)
				return std::format("還要再提交 {} 份報告", n.reports - e.reports);
			if (e.score < n.eval)
				return std::format("考評 {} 分，要 {} 分", e.score, n.eval);
			if (g.retryAt >= 0 && shift::Totals().shifts < g.retryAt)
				return std::format("上次考試沒過，再上 {} 班才能重考", g.retryAt - shift::Totals().shifts);
			return "";
		}

		const Question* Pick(const Question* pool, size_t size, std::vector<const Question*>& taken)
		{
			for (int tries = 0; tries < 50; ++tries)
			{
				const Question* q = &pool[static_cast<size_t>(MISC::GET_RANDOM_INT_IN_RANGE(0, static_cast<int>(size)))];
				if (std::find(taken.begin(), taken.end(), q) == taken.end())
				{
					taken.push_back(q);
					return q;
				}
			}
			return nullptr;
		}

		void Promote()
		{
			const int from = g.rank;
			g.rank = std::min(kTop, g.rank + 1);
			g.since = shift::Totals();
			g.reportsAfter = records::LastId();
			g.retryAt = -1;
			g.bonus = 0;
			g.history.push_back(ml::Json{{"date", shift::Today()}, {"from", kRanks[from].title[Department()]}, {"to", kRanks[g.rank].title[Department()]}});
			Store();
			AUDIO::PLAY_SOUND_FRONTEND(-1, "RANK_UP", "HUD_AWARDS", true);
			ml::Notify("恭喜升任{}！時薪 ${}。解鎖：{}", kRanks[g.rank].title[Department()], kRanks[g.rank].rate, kUnlocks[g.rank]);
			ml::Log("career: promoted to rank {} ({})", g.rank, kRanks[g.rank].title[0]);
		}
	}

	int Rank()
	{
		Load();
		return g.rank;
	}

	int Rate()
	{
		Load();
		return kRanks[g.rank].rate;
	}

	std::string Title(int rank)
	{
		Load();
		return kRanks[std::clamp(rank < 0 ? g.rank : rank, 0, kTop)].title[Department()];
	}

	bool AtStation()
	{
		return officer::IsPlayer() && duty::AtStation(ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), true));
	}

	void Loading()
	{
		g_loaded = false;
	}

	ml::Json Json()
	{
		Load();
		const Evaluation e = Evaluate();
		const shift::Counts c = Since();
		ml::Json unlocks = ml::Json::Array();
		for (int r = 0; r <= kTop; ++r)
			unlocks.Push(ml::Json{{"title", kRanks[r].title[Department()]}, {"rate", kRanks[r].rate}, {"items", kUnlocks[r]}, {"have", r <= g.rank}});
		ml::Json next;
		if (g.rank < kTop)
		{
			const Need& n = kNeed[g.rank];
			next = ml::Json{{"title", kRanks[g.rank + 1].title[Department()]}, {"rate", kRanks[g.rank + 1].rate},
			    {"shifts", ml::Json::Array({c.shifts, n.shifts})}, {"eval", ml::Json::Array({e.score, n.eval})},
			    {"reports", ml::Json::Array({e.reports, n.reports})}, {"why", NotReady()}, {"atStation", AtStation()}};
		}
		return ml::Json{{"title", Title()}, {"rate", Rate()}, {"rank", g.rank}, {"next", next},
		    {"eval", ml::Json{{"score", e.score}, {"reportsAvg", e.reportsAvg}, {"reports", e.reports}, {"penalty", e.penalty}, {"trainer", e.trainer},
		                 {"bonus", e.bonus}, {"late", c.late},
		                 {"early", c.early}, {"absent", c.absent}, {"overdue", c.overdue}}},
		    {"unlocks", unlocks}, {"history", g.history}, {"exams", g.exams}, {"questions", kQuestions}, {"pass", kPass}};
	}

	ml::Json StartExam()
	{
		Load();
		if (const std::string why = NotReady(); !why.empty())
			return ml::Json{{"ok", false}, {"error", why}};
		if (!AtStation())
			return ml::Json{{"ok", false}, {"error", "要在自己的警局才能考試（警局置物櫃按 G →「警局電腦」）"}};
		// Officer exams from the officers' questions; a sergeant's mostly the sergeants' (and some of the officers');
		// the lieutenant's from all three.
		std::vector<const Question*> taken;
		const int next = g.rank + 1;
		const int fromLieutenant = next == kTop ? 8 : 0;
		const int fromSergeant = next == kTop ? 4 : next >= 3 ? 10 : 0;
		const int fromOfficer = kQuestions - fromLieutenant - fromSergeant;
		for (int i = 0; i < fromLieutenant; ++i)
			Pick(kLieutenant, std::size(kLieutenant), taken);
		for (int i = 0; i < fromSergeant; ++i)
			Pick(kSergeant, std::size(kSergeant), taken);
		for (int i = 0; i < fromOfficer; ++i)
			Pick(kOfficer, std::size(kOfficer), taken);
		g_exam.clear();
		ml::Json questions = ml::Json::Array();
		for (const Question* q : taken)
		{
			if (!q)
				continue;
			Asked a{q, {0, 1, 2, 3}};
			for (int i = 3; i > 0; --i)
				std::swap(a.order[static_cast<size_t>(i)], a.order[static_cast<size_t>(MISC::GET_RANDOM_INT_IN_RANGE(0, i + 1))]);
			g_exam.push_back(a);
			ml::Json options = ml::Json::Array();
			for (const int o : a.order)
				options.Push(q->options[static_cast<size_t>(o)]);
			questions.Push(ml::Json{{"text", q->text}, {"options", options}});
		}
		ml::Log("career: exam for rank {} started ({} questions)", next, g_exam.size());
		return ml::Json{{"ok", true}, {"title", kRanks[next].title[Department()]}, {"questions", questions}, {"pass", kPass}};
	}

	ml::Json SubmitExam(const std::vector<int>& answers)
	{
		Load();
		if (g_exam.empty())
			return ml::Json{{"ok", false}, {"error", "沒有進行中的考試"}};
		int correct = 0;
		ml::Json results = ml::Json::Array();
		for (size_t i = 0; i < g_exam.size(); ++i)
		{
			const Asked& a = g_exam[i];
			const int picked = i < answers.size() ? answers[i] : -1;
			const bool right = picked >= 0 && picked < 4 && a.order[static_cast<size_t>(picked)] == 0;
			correct += right;
			results.Push(ml::Json{{"right", right}, {"answer", a.q->options[0]}});
		}
		const int total = static_cast<int>(g_exam.size());
		g_exam.clear();
		const bool pass = correct >= kPass;
		const int next = g.rank + 1;
		g.exams.push_back(ml::Json{{"date", shift::Today()}, {"title", kRanks[std::min(next, kTop)].title[Department()]}, {"correct", correct},
		    {"total", total}, {"pass", pass}});
		if (g.exams.size() > 20)
			g.exams.erase(g.exams.begin());
		ml::Log("career: exam {} / {} ({})", correct, total, pass ? "passed" : "failed");
		if (pass)
			Promote();
		else
		{
			g.retryAt = shift::Totals().shifts + kRetryShifts;
			Store();
			ml::Notify("升等考試答對 {} 題，沒有通過（要 {} 題）。再上 {} 班可以重考", correct, kPass, kRetryShifts);
		}
		return ml::Json{{"ok", true}, {"correct", correct}, {"total", total}, {"pass", pass}, {"results", results}, {"title", Title()}};
	}

	void AddTrainerRating(int rating)
	{
		Load();
		g.trainer.push_back(std::clamp(rating, 0, 100));
		if (g.trainer.size() > 20)
			g.trainer.erase(g.trainer.begin());
		Store();
		ml::Log("career: FTO rating {}", rating);
	}

	void AddBonus(int points, const char* why)
	{
		Load();
		if (g.bonus >= kMaxBonus)
			return;
		g.bonus = std::min(kMaxBonus, g.bonus + points);
		Store();
		ml::Log("career: evaluation +{} ({}), bonus {}", points, why, g.bonus);
	}

	bool Quiz(std::string& text, std::vector<std::string>& options, int& right)
	{
		const Question& q = kOfficer[static_cast<size_t>(MISC::GET_RANDOM_INT_IN_RANGE(0, static_cast<int>(std::size(kOfficer))))];
		std::array<int, 4> order{0, 1, 2, 3};
		for (int i = 3; i > 0; --i)
			std::swap(order[static_cast<size_t>(i)], order[static_cast<size_t>(MISC::GET_RANDOM_INT_IN_RANGE(0, i + 1))]);
		text = q.text;
		options.clear();
		for (int i = 0; i < 4; ++i)
		{
			options.push_back(q.options[static_cast<size_t>(order[static_cast<size_t>(i)])]);
			if (order[static_cast<size_t>(i)] == 0)
				right = i;
		}
		return true;
	}

	bool May(Unlock what)
	{
		const int rank = Rank();
		switch (what)
		{
		case Unlock::Shotgun: return rank >= 1;
		case Unlock::Carbine: return rank >= 2;
		case Unlock::FieldTraining: return rank >= 2;
		case Unlock::Supervise: return rank >= 3;
		case Unlock::Noose: return rank >= 4;
		}
		return false;
	}
}
