// The officer's working life, part 1 (user's design 2026-10-22, research/phase0.md §56): the roster, the clock on duty
// and pay.
//
// The roster: day shift 07-15, evening shift 15-23, night shift 23-07 (a night shift belongs to the day it starts on),
// five days on and two off, counted from the day the officer was hired. New officers work nights while on probation
// (their first 10 shifts); after that the officer asks for a shift at the station and works it from their next week.
//
// A shift: the officer clocks in at the locker (the uniform, duty.cpp). On time (up to 15 minutes after the start) the
// sergeant holds roll call: the beat, the callsign, the BOLOs (a stolen car and a wanted person, who may turn up in the
// shift's calls) and the day's focus. Late, leaving early (more than 15 minutes before the end) and absent (the shift
// over and the officer, played, never came in) are recorded. Clocking out with reports not submitted is allowed after a
// warning and recorded too (all of it for the evaluation: promotion and discipline, later).
// While the user plays a protagonist the officer works their shifts by themselves: base pay, no score, seniority counts.
// Switched to during one, they are at the station on duty and the shift goes on, played.
//
// Pay (GTA-scaled, by rank, career.cpp): hourly; past 8 hours on duty, or on duty outside the roster (a day off, after a shift),
// time and a half; the hour before a shift is the officer's own; 10% more for hours between 23:00 and 07:00 (played
// hours: a shift worked by themselves is base pay). The payslip comes at clock-out, or at the end of a shift worked by
// themselves, into the officer's own account from their department (the game's bank: ml::game::Receive).
//
// The clock: while the officer is played on duty the game clock runs slower (a setting, 1/3 by default: an 8-hour shift
// is about 48 real minutes instead of 16). The mod pauses the clock and moves it on itself.
//
// Kept in the save: ml::save "officer_career" (rank, seniority, roster, the session under way, payslips, attendance).
#include "police.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <vector>

namespace police::shift
{
	namespace
	{
		// ---- the game's date and time -------------------------------------------------------------------------------
		int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d)
		{
			y -= m <= 2;
			const int64_t era = (y >= 0 ? y : y - 399) / 400;
			const unsigned yoe = static_cast<unsigned>(y - era * 400);
			const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
			const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
			return era * 146097 + static_cast<int64_t>(doe) - 719468;
		}
		void CivilFromDays(int64_t z, int& y, int& m, int& d)
		{
			z += 719468;
			const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
			const unsigned doe = static_cast<unsigned>(z - era * 146097);
			const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
			const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
			const unsigned mp = (5 * doy + 2) / 153;
			d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
			m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
			y = static_cast<int>(static_cast<int64_t>(yoe) + era * 400 + (m <= 2));
		}
		// Game minutes since 1970 (fractions from the seconds).
		double Now()
		{
			const int64_t day = DaysFromCivil(CLOCK::GET_CLOCK_YEAR(), static_cast<unsigned>(CLOCK::GET_CLOCK_MONTH() + 1),
			    static_cast<unsigned>(CLOCK::GET_CLOCK_DAY_OF_MONTH()));
			return static_cast<double>(day * 1440 + CLOCK::GET_CLOCK_HOURS() * 60 + CLOCK::GET_CLOCK_MINUTES()) + CLOCK::GET_CLOCK_SECONDS() / 60.0;
		}
		int64_t DayOf(double minute)
		{
			return static_cast<int64_t>(std::floor(minute / 1440.0));
		}
		int HourOf(double minute)
		{
			return static_cast<int>(std::floor(minute - static_cast<double>(DayOf(minute)) * 1440.0) / 60.0);
		}
		const char* Weekday(int64_t day)
		{
			static const char* const names[] = {"日", "一", "二", "三", "四", "五", "六"};
			return names[((day + 4) % 7 + 7) % 7]; // 1 Jan 1970 was a Thursday
		}
		std::string Date(int64_t day)
		{
			int y, m, d;
			CivilFromDays(day, y, m, d);
			return std::format("{}/{}（{}）", m, d, Weekday(day));
		}
		std::string Clock(double minute)
		{
			const int64_t m = static_cast<int64_t>(std::floor(minute)) % 1440;
			return std::format("{:02}:{:02}", m / 60, m % 60);
		}

		// ---- the department ---------------------------------------------------------------------------------------
		struct Kind
		{
			const char* name;
			int start;           // hour
			int sleep, wake;     // when the officer sleeps on this shift (the hours at home)
		};
		constexpr Kind kKinds[] = {{"日班", 7, 22, 6}, {"小夜班", 15, 1, 9}, {"大夜班", 23, 8, 15}};
		constexpr int kNight = 2;
		constexpr int kProbation = 10; // shifts on nights before the officer may ask for another
		constexpr int kGrace = 15;     // minutes late / early before it counts

		// Beats: an area of the map each (a circle), by department.
		struct Beat
		{
			const char* name;
			float x, y, r;
		};
		const std::vector<Beat> kBeats[4] = {
		    {{"市中心", 150.0f, -850.0f, 650.0f}, {"南洛聖都", 50.0f, -1700.0f, 650.0f}, {"小首爾、威斯普奇", -900.0f, -1100.0f, 700.0f},
		        {"好麥塢", 300.0f, 100.0f, 650.0f}, {"東洛聖都", 1000.0f, -1600.0f, 600.0f}},
		    {{"沙灘鎮", 1800.0f, 3700.0f, 900.0f}, {"葡萄籽", 2200.0f, 4900.0f, 800.0f}, {"佩立托灣", -200.0f, 6300.0f, 800.0f},
		        {"哈莫尼、68 號公路", 600.0f, 2800.0f, 900.0f}},
		    {{"市區高速公路", 300.0f, -1100.0f, 1700.0f}, {"塞諾拉高速公路", 2000.0f, 2800.0f, 1700.0f}, {"大洋公路", -2400.0f, 2200.0f, 1700.0f}},
		    {{"全郡", 0.0f, 2000.0f, 6000.0f}},
		};
		constexpr const char* kCallLetter[4] = {"L", "S", "H", "N"};
		// The department paying the salary (the game's bank payees: the police stations' accounts).
		constexpr int32_t kPayee[4] = {120, 122, 120, 120};

		struct Focus
		{
			const char* title;
			const char* detail;
		};
		constexpr Focus kFocus[] = {
		    {"竊車", "最近轄區竊車案增加，多注意車牌辨識的警示"},
		    {"交通安全", "注意危險駕駛、酒駕，該攔就攔"},
		    {"治安巡邏", "商店街和住宅區多繞幾圈，讓民眾看到警車"},
		    {"協尋", "協尋車輛和通緝犯可能在勤務區出沒"},
		};

		const char* const kBoloCars[] = {"sultan", "buffalo", "fugitive", "tailgater", "oracle", "felon", "baller", "dominator"};
		const char* const kBoloCrimes[] = {"持械搶劫", "竊盜", "傷害", "詐欺", "妨害自由"};

		int Department()
		{
			return std::clamp(look::Department(), 0, 3);
		}

		// ---- the state (kept in the save) -------------------------------------------------------------------------
		struct Session
		{
			bool active = false;
			double start = 0, last = 0;                                   // game minutes
			double regular = 0, overtime = 0, night = 0, own = 0, alone = 0; // minutes (alone: worked by themselves)
			bool locker = false;                                          // clocked in at the locker (not just found in uniform)
		};
		struct Planned // the roster's shift being followed: the one under way, or the next
		{
			int64_t start = 0, end = 0; // game minutes (0 = none planned)
			int kind = kNight;
			int beat = 0;
			int focus = 0;
			bool attended = false; // a session ran in it
			bool pending = false;  // it began with the officer played off duty: not in yet
			bool counted = false;  // counted towards seniority
			int late = 0;          // minutes
			bool rollCall = false;
			bool told60 = false, told15 = false, toldLate = false, toldEnd = false;
			std::string boloCar, boloPlate, boloName, boloCrime;
			bool boloCarUsed = false, boloPersonUsed = false;
		};
		struct Career
		{
			int64_t hired = 0; // day
			int shifts = 0;    // worked (seniority)
			int kind = kNight;
			int request = -1;
			int64_t requestFrom = 0; // day
			int late = 0, early = 0, absent = 0, overdue = 0;
			int suspended = 0;   // shifts of suspension left (discipline.cpp)
			bool fired = false;  // dismissed ...
			int64_t rehire = 0;  // ... may apply again from this day
			int unit = 0;
			std::string sergeant;
			Session session;
			Planned planned;
			std::vector<ml::Json> payslips, attendance; // the last 30 / 60
		};
		Career g;
		bool g_loaded = false;
		uint64_t g_nextStep = 0, g_nextStore = 0;
		double g_lastStep = 0; // the game minute of the last step
		Blip g_beatBlip = 0;
		int g_beatBlipOf = -1;
		bool g_confirmOut = false;
		uint64_t g_confirmUntil = 0;
		uint64_t g_nextBeatNag = 0;

		ml::Setting g_clockSetting;

		ml::Json ToJson(const Session& s)
		{
			return {{"active", s.active}, {"start", s.start}, {"last", s.last}, {"regular", s.regular}, {"overtime", s.overtime}, {"night", s.night},
			    {"own", s.own}, {"alone", s.alone}, {"locker", s.locker}};
		}
		Session SessionFrom(const ml::Json& j)
		{
			Session s;
			s.active = j["active"].Bool();
			s.start = j["start"].Number();
			s.last = j["last"].Number();
			s.regular = j["regular"].Number();
			s.overtime = j["overtime"].Number();
			s.night = j["night"].Number();
			s.own = j["own"].Number();
			s.alone = j["alone"].Number();
			s.locker = j["locker"].Bool();
			return s;
		}
		ml::Json ToJson(const Planned& p)
		{
			return {{"start", p.start}, {"end", p.end}, {"kind", p.kind}, {"beat", p.beat}, {"focus", p.focus}, {"attended", p.attended},
			    {"pending", p.pending}, {"counted", p.counted}, {"late", p.late}, {"rollCall", p.rollCall}, {"told60", p.told60}, {"told15", p.told15},
			    {"toldLate", p.toldLate}, {"toldEnd", p.toldEnd}, {"boloCar", p.boloCar}, {"boloPlate", p.boloPlate}, {"boloName", p.boloName},
			    {"boloCrime", p.boloCrime}, {"boloCarUsed", p.boloCarUsed}, {"boloPersonUsed", p.boloPersonUsed}};
		}
		Planned PlannedFrom(const ml::Json& j)
		{
			Planned p;
			p.start = j["start"].Int64();
			p.end = j["end"].Int64();
			p.kind = std::clamp(j["kind"].Int(kNight), 0, 2);
			p.beat = j["beat"].Int();
			p.focus = j["focus"].Int();
			p.attended = j["attended"].Bool();
			p.pending = j["pending"].Bool();
			p.counted = j["counted"].Bool();
			p.late = j["late"].Int();
			p.rollCall = j["rollCall"].Bool();
			p.told60 = j["told60"].Bool();
			p.told15 = j["told15"].Bool();
			p.toldLate = j["toldLate"].Bool();
			p.toldEnd = j["toldEnd"].Bool();
			p.boloCar = j["boloCar"].Str();
			p.boloPlate = j["boloPlate"].Str();
			p.boloName = j["boloName"].Str();
			p.boloCrime = j["boloCrime"].Str();
			p.boloCarUsed = j["boloCarUsed"].Bool();
			p.boloPersonUsed = j["boloPersonUsed"].Bool();
			return p;
		}

		void Store()
		{
			ml::save::Set("officer_career",
			    ml::Json{{"hired", g.hired}, {"shifts", g.shifts}, {"kind", g.kind}, {"request", g.request},
			        {"requestFrom", g.requestFrom}, {"late", g.late}, {"early", g.early}, {"absent", g.absent}, {"overdue", g.overdue},
			        {"suspended", g.suspended}, {"fired", g.fired}, {"rehire", g.rehire},
			        {"unit", g.unit}, {"sergeant", g.sergeant}, {"session", ToJson(g.session)}, {"planned", ToJson(g.planned)},
			        {"payslips", g.payslips}, {"attendance", g.attendance}});
			g_nextStore = ml::TickMs() + 10000;
		}

		int Rand(int lo, int hi)
		{
			return MISC::GET_RANDOM_INT_IN_RANGE(lo, hi + 1);
		}

		void Load(double now)
		{
			g_loaded = true;
			const ml::Json j = ml::save::Get("officer_career");
			g = Career{};
			if (!j["hired"].IsNumber())
			{
				// A new officer (or one from before the roster): hired today, on nights.
				g.hired = DayOf(now);
				g.unit = Rand(11, 89);
				g.sergeant = records::RandomName();
				ml::Log("shift: a new career, hired on day {}", g.hired);
				Store();
				return;
			}
			g.hired = j["hired"].Int64();
			g.shifts = j["shifts"].Int();
			g.kind = std::clamp(j["kind"].Int(kNight), 0, 2);
			g.request = j["request"].Int(-1);
			g.requestFrom = j["requestFrom"].Int64();
			g.late = j["late"].Int();
			g.early = j["early"].Int();
			g.absent = j["absent"].Int();
			g.suspended = j["suspended"].Int();
			g.fired = j["fired"].Bool();
			g.rehire = j["rehire"].Int64();
			g.overdue = j["overdue"].Int();
			g.unit = j["unit"].Int(42);
			g.sergeant = j["sergeant"].Str("Sgt. Reyes");
			g.session = SessionFrom(j["session"]);
			g.planned = PlannedFrom(j["planned"]);
			g.payslips = j["payslips"].Items();
			g.attendance = j["attendance"].Items();
		}

		// ---- the roster -------------------------------------------------------------------------------------------
		bool WorkDay(int64_t day)
		{
			return ((day - g.hired) % 7 + 7) % 7 < 5;
		}
		// The shift the officer works on a day (a request takes effect from its day).
		int KindOn(int64_t day)
		{
			if (g.request >= 0 && day >= g.requestFrom)
				return g.request;
			return g.kind;
		}
		// The roster's shift under way at `now`, or the next one.
		Planned Next(double now)
		{
			for (int64_t day = DayOf(now) - 1;; ++day)
			{
				if (!WorkDay(day))
					continue;
				const int kind = KindOn(day);
				const int64_t start = day * 1440 + kKinds[kind].start * 60;
				if (static_cast<double>(start + 480) <= now)
					continue;
				Planned p;
				p.start = start;
				p.end = start + 480;
				p.kind = kind;
				return p;
			}
		}
		// The first day of the officer's next week (after the coming days off).
		int64_t NextWeek(double now)
		{
			const int64_t today = DayOf(now);
			const int64_t cycle = ((today - g.hired) % 7 + 7) % 7;
			return today + (7 - cycle);
		}

		std::string Plate()
		{
			std::string s = std::to_string(Rand(10, 99));
			for (int i = 0; i < 3; ++i)
				s += static_cast<char>('A' + Rand(0, 25));
			return s + std::to_string(Rand(100, 999));
		}

		// The next shift planned: its beat, the focus, the BOLOs.
		void Plan(double now)
		{
			Planned p = Next(now);
			if (g.request >= 0 && DayOf(static_cast<double>(p.start)) >= g.requestFrom)
			{
				g.kind = g.request;
				g.request = -1;
			}
			const auto& beats = kBeats[Department()];
			p.beat = Rand(0, static_cast<int>(beats.size()) - 1);
			p.focus = Rand(0, static_cast<int>(std::size(kFocus)) - 1);
			p.boloCar = kBoloCars[Rand(0, static_cast<int>(std::size(kBoloCars)) - 1)];
			p.boloPlate = Plate();
			p.boloName = records::RandomName();
			p.boloCrime = kBoloCrimes[Rand(0, static_cast<int>(std::size(kBoloCrimes)) - 1)];
			g.planned = p;
			ml::Log("shift: next {} on {} {}-{}, beat {}", kKinds[p.kind].name, Date(DayOf(static_cast<double>(p.start))), Clock(static_cast<double>(p.start)),
			    Clock(static_cast<double>(p.end)), beats[p.beat].name);
			Store();
		}

		bool InWindow(double t)
		{
			return g.planned.start && t >= static_cast<double>(g.planned.start) && t < static_cast<double>(g.planned.end);
		}

		std::string CarName(const std::string& model)
		{
			const char* label = VEHICLE::GET_DISPLAY_NAME_FROM_VEHICLE_MODEL(MISC::GET_HASH_KEY(model.c_str()));
			const char* text = label ? HUD::GET_FILENAME_FOR_AUDIO_CONVERSATION(label) : nullptr;
			return text && *text && std::string(text) != "NULL" ? text : model;
		}

		std::string CallsignOf(const Planned& p)
		{
			return std::format("{}-{}-{:02}", p.beat + 1, kCallLetter[Department()], g.unit);
		}

		// ---- attendance and pay -------------------------------------------------------------------------------------
		// The attendance entry of the planned shift (made when first needed).
		ml::Json& Attendance()
		{
			if (g.attendance.empty() || g.attendance.back()["start"].Int64() != g.planned.start)
			{
				g.attendance.push_back(ml::Json{{"start", g.planned.start}, {"date", Date(DayOf(static_cast<double>(g.planned.start)))},
				    {"kind", kKinds[g.planned.kind].name}, {"status", ""}, {"notes", ml::Json::Array()}});
				if (g.attendance.size() > 60)
					g.attendance.erase(g.attendance.begin());
			}
			return g.attendance.back();
		}
		void Note(const std::string& status, const std::string& note = "")
		{
			ml::Json& a = Attendance();
			if (!status.empty())
				a["status"] = status;
			if (!note.empty())
			{
				ml::Json notes = a["notes"];
				notes.Push(note);
				a["notes"] = notes;
			}
		}

		int Rate()
		{
			return career::Rate();
		}

		// Counts the session up to `now`, a game minute at a time (time jumps, sleeping, are worked through).
		void Count(double now, bool played)
		{
			Session& s = g.session;
			if (!s.active)
				return;
			if (now < s.last || (played && now - s.last > 10.0))
			{
				// The clock went back, or jumped ahead while the officer was played (a trainer, a skip): not time worked.
				s.last = now;
				return;
			}
			for (double t = s.last; t < now;)
			{
				const double step = std::min(1.0, now - t);
				const bool window = InWindow(t);
				if (!played)
				{
					// Worked by themselves: within the roster's shift only (the session ends with it).
					if (!window)
						break;
					s.alone += step;
				}
				else if (g.planned.start && t >= static_cast<double>(g.planned.start - 60) && t < static_cast<double>(g.planned.start))
					s.own += step; // the hour before the shift
				else
				{
					const double worked = s.regular + s.overtime + s.alone;
					if (window && worked < 480.0)
						s.regular += step;
					else
						s.overtime += step;
					const int hour = HourOf(t);
					if (hour >= 23 || hour < 7)
						s.night += step;
				}
				t += step;
				s.last = t;
			}
			if (InWindow(now) || InWindow(s.last - 0.01))
				g.planned.attended = true;
			// Half a shift worked in it counts the shift (seniority).
			if (!g.planned.counted && s.regular + s.alone >= 240.0)
			{
				g.planned.counted = true;
				++g.shifts;
				ml::Log("shift: {} shifts worked", g.shifts);
			}
		}

		void Payslip(const Session& s)
		{
			const double rate = Rate();
			const double regular = (s.regular + s.alone) / 60.0, overtime = s.overtime / 60.0, night = s.night / 60.0;
			const int pay = static_cast<int>(std::lround(rate * regular + rate * 1.5 * overtime + rate * 0.1 * night));
			if (pay <= 0)
				return;
			const int64_t day = DayOf(s.start);
			g.payslips.push_back(ml::Json{{"date", Date(day)}, {"from", Clock(s.start)}, {"to", Clock(s.last)}, {"regular", std::round(regular * 100) / 100},
			    {"overtime", std::round(overtime * 100) / 100}, {"night", std::round(night * 100) / 100}, {"alone", std::round(s.alone / 0.6) / 100},
			    {"rate", Rate()}, {"rank", career::Title()}, {"pay", pay}});
			if (g.payslips.size() > 30)
				g.payslips.erase(g.payslips.begin());
			const bool received = ml::game::Receive(pay, kPayee[Department()], ml::game::Character::Fourth);
			std::string parts;
			for (const auto& [what, hours] : {std::pair{"正常", regular}, std::pair{"加班", overtime}, std::pair{"夜間加給", night}})
				if (hours >= 0.05)
					parts += std::format("{}{} {:.1f} 小時", parts.empty() ? "" : "、", what, hours);
			if (parts.empty())
				parts = "不到 0.1 小時";
			if (officer::IsPlayer())
				ml::Notify("薪資單：{}，共 ${}{}", parts, pay, received ? " 已存入帳戶" : "（入帳失敗）");
			else
				ml::Notify("{}（警察）下班了，薪資 ${}{}", look::Name(), pay, received ? " 已入帳" : "（入帳失敗）");
			ml::Log("shift: payslip ${} (regular {:.2f} h, overtime {:.2f} h, night {:.2f} h, alone {:.2f} h, own {:.2f} h, rate {}){}", pay, regular,
			    overtime, night, s.alone / 60.0, s.own / 60.0, Rate(), received ? "" : " NOT received");
		}

		void Begin(double at, bool locker)
		{
			g.session = Session{};
			g.session.active = true;
			g.session.start = g.session.last = at;
			g.session.locker = locker;
			ml::Log("shift: on duty at {} {} ({})", Date(DayOf(at)), Clock(at), locker ? "locker" : "found in uniform / by themselves");
			Store();
		}

		// `how`: "locker" (clocked out at the locker), "alone" (a shift worked by themselves ended), "" (the uniform came off
		// otherwise: a hospital, a switch away out of the shift).
		void End(const char* how)
		{
			Session s = g.session;
			g.session = Session{};
			if (!s.active)
				return;
			if (std::string(how) == "locker" && InWindow(s.last) && s.last < static_cast<double>(g.planned.end - kGrace))
			{
				++g.early;
				Note("", std::format("早退 {} 分鐘", static_cast<int>(static_cast<double>(g.planned.end) - s.last)));
			}
			Payslip(s);
			Store();
		}

		void Help(const std::string& text)
		{
			HUD::BEGIN_TEXT_COMMAND_DISPLAY_HELP("STRING");
			HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME(text.c_str());
			HUD::END_TEXT_COMMAND_DISPLAY_HELP(0, false, false, -1);
		}

		// ---- the clock on duty ------------------------------------------------------------------------------------
		bool g_slowing = false;
		int g_lastTimer = 0;
		double g_pendingSeconds = 0;

		int ClockRatio()
		{
			return g_clockSetting.Valid() ? g_clockSetting.Value() + 1 : 3;
		}

		void SlowClock(bool on)
		{
			if (!on || ClockRatio() <= 1)
			{
				if (g_slowing)
				{
					CLOCK::PAUSE_CLOCK(false);
					g_slowing = false;
				}
				return;
			}
			const int timer = MISC::GET_GAME_TIMER();
			if (!g_slowing)
			{
				g_slowing = true;
				g_lastTimer = timer;
				g_pendingSeconds = 0;
			}
			CLOCK::PAUSE_CLOCK(true);
			const int ms = std::max(1, CLOCK::GET_MILLISECONDS_PER_GAME_MINUTE());
			const int elapsed = std::clamp(timer - g_lastTimer, 0, 1000);
			g_lastTimer = timer;
			g_pendingSeconds += elapsed * 60.0 / ms / ClockRatio();
			if (g_pendingSeconds >= 1.0)
			{
				const int whole = static_cast<int>(g_pendingSeconds);
				g_pendingSeconds -= whole;
				CLOCK::ADD_TO_CLOCK_TIME(0, 0, whole);
			}
		}

		// ---- the beat on the map ----------------------------------------------------------------------------------
		void BeatBlip(int beat)
		{
			if (beat == g_beatBlipOf && (beat < 0 || HUD::DOES_BLIP_EXIST(g_beatBlip)))
				return;
			if (g_beatBlip)
				HUD::REMOVE_BLIP(&g_beatBlip);
			g_beatBlip = 0;
			g_beatBlipOf = beat;
			if (beat < 0)
				return;
			const Beat& b = kBeats[Department()][static_cast<size_t>(beat) % kBeats[Department()].size()];
			if (b.r >= 5000.0f)
				return;
			g_beatBlip = HUD::ADD_BLIP_FOR_RADIUS(b.x, b.y, 0.0f, b.r);
			HUD::SET_BLIP_COLOUR(g_beatBlip, 3);
			HUD::SET_BLIP_ALPHA(g_beatBlip, 40);
		}

		// ---- roll call --------------------------------------------------------------------------------------------
		menu::List RollCallMenu(bool held)
		{
			const Planned& p = g.planned;
			const Beat& beat = kBeats[Department()][static_cast<size_t>(p.beat) % kBeats[Department()].size()];
			menu::List m{held ? "勤前教育" : "今日勤務", {}};
			m.items.push_back({"日期", Date(DayOf(static_cast<double>(p.start))) + " " + kKinds[p.kind].name, std::format("{}–{}", Clock(static_cast<double>(p.start)), Clock(static_cast<double>(p.end)))});
			const std::string sergeant = colleagues::Sergeant(p.kind);
			m.items.push_back({"帶班", sergeant.empty() ? g.sergeant : sergeant, "今天的帶班巡佐"});
			m.items.push_back({"勤務區", beat.name, "調度會優先派這一區的案件給你，地圖上淡藍色的範圍"});
			m.items.push_back({"呼號", CallsignOf(p), "無線電上叫你的代號"});
			m.items.push_back({"協尋車輛", CarName(p.boloCar) + " " + p.boloPlate, "失竊車輛：看到就攔停（車牌辨識也會警示）"});
			m.items.push_back({"通緝", p.boloName, std::string("通緝罪名：") + p.boloCrime + "，可能在勤務區出沒"});
			m.items.push_back({"今日重點", kFocus[p.focus].title, kFocus[p.focus].detail});
			menu::Item ok{"知道了", "", ""};
			ok.action = [] {};
			m.items.push_back(ok);
			return m;
		}

		void ShiftRequestMenu()
		{
			menu::List m{"申請換班", {}};
			for (int k = 0; k < 3; ++k)
			{
				const bool now = k == (g.request >= 0 ? g.request : g.kind);
				menu::Item i{kKinds[k].name, now ? "目前" : "", std::format("{:02}:00 開始，8 小時", kKinds[k].start)};
				i.enabled = !now;
				i.action = [k] {
					g.request = k;
					g.requestFrom = NextWeek(Now());
					ml::Notify("已申請改上{}：從 {} 起生效", kKinds[k].name, Date(g.requestFrom));
					ml::Log("shift: asked for the {} from day {}", kKinds[k].name, g.requestFrom);
					// The planned shift follows when it is still ahead and on the new week.
					if (!g.planned.attended && !g.session.active && DayOf(static_cast<double>(g.planned.start)) >= g.requestFrom)
						Plan(Now());
					Store();
				};
				m.items.push_back(i);
			}
			menu::Open(m);
		}

		// ---- the steps ----------------------------------------------------------------------------------------------
		void Step(double now)
		{
			const bool played = officer::IsPlayer();
			const bool uniform = officer::InUniform() && officer::Ped();
			if (!g.planned.start || static_cast<double>(g.planned.end) <= g_lastStep - 1440.0 || static_cast<double>(g.planned.start) > now + 9.0 * 1440.0)
				Plan(now);

			Count(now, played);
			// Suspended or dismissed: no duty (a suspension counts down by the roster's shifts).
			const bool barred = g.suspended > 0 || g.fired;

			// The planned shift is over.
			if (static_cast<double>(g.planned.end) <= now)
			{
				if (g.session.active && !played)
					End("alone");
				if (g.suspended > 0 && !g.planned.attended)
				{
					--g.suspended;
					Note("停職");
					if (g.suspended == 0)
						ml::Notify("停職期滿，下一班可以回來上班");
				}
				else if (!barred && !g.planned.attended && g.planned.pending)
				{
					++g.absent;
					Note("缺勤");
					ml::Log("shift: absent");
					if (played)
						ml::Notify("你今天的{}缺勤了，會記錄在考評裡", kKinds[g.planned.kind].name);
					discipline::Absent();
				}
				Plan(now);
			}
			if (barred)
			{
				if (g.session.active)
				{
					End("");
					officer::Dress(false);
				}
				g_lastStep = now;
				if (ml::TickMs() >= g_nextStore)
					Store();
				return;
			}

			Planned& p = g.planned;
			const double start = static_cast<double>(p.start);
			const bool window = InWindow(now);
			if (played)
			{
				if (uniform && !g.session.active)
					Begin(now, false);
				else if (!uniform && g.session.active)
					End("");
				// Arriving: the officer was played off duty when the shift began.
				if (window && !g.session.active && !p.attended)
				{
					if (!p.pending)
					{
						p.pending = true;
						Store();
					}
					if (now > start + kGrace && !p.toldLate)
					{
						p.toldLate = true;
						ml::Notify("{}已經開始了，你遲到了：快到{}報到", kKinds[p.kind].name, "警局");
					}
				}
				if (!g.session.active && !window)
				{
					if (!p.told60 && now >= start - 60.0 && now < start - 15.0)
					{
						p.told60 = true;
						ml::Notify("一小時後上{}（{}）：記得到警局換制服報到", kKinds[p.kind].name, Clock(start));
					}
					if (!p.told15 && now >= start - 15.0 && now < start)
					{
						p.told15 = p.told60 = true;
						ml::Notify("15 分鐘後上{}", kKinds[p.kind].name);
					}
				}
				if (g.session.active && now >= static_cast<double>(p.end) - 1.0 && !p.toldEnd && p.attended && now < static_cast<double>(p.end) + 1.0)
				{
					p.toldEnd = true;
					ml::Notify("{}結束了：回警局換裝下班（之後算加班）", kKinds[p.kind].name);
				}
			}
			else
			{
				// Nobody plays the officer: they go to work by themselves (late when they were expected and had not come),
				// and home again at the shift's end.
				if (window && !g.session.active && !p.attended)
				{
					const double from = std::max(start, std::min(now, g_lastStep));
					if (p.pending && from > start + kGrace)
					{
						++g.late;
						p.late = static_cast<int>(from - start);
						Note(std::format("遲到 {} 分鐘", p.late));
					}
					else
						Note("自己上班");
					Begin(from, false);
					Count(now, false);
				}
				else if (g.session.active && !window)
				{
					End("alone");
					officer::Dress(false);
				}
			}
			g_lastStep = now;
			if (ml::TickMs() >= g_nextStore)
				Store();
		}
	}

	void Build()
	{
		g_clockSetting = ml::AddList("dutyClock", "上班時遊戲時鐘速度", {"原速（1 小時 = 2 分鐘）", "1/2", "1/3（8 小時班約 48 分鐘）", "1/4", "1/5", "1/6"}, 2);
	}

	void Loading()
	{
		g_loaded = false;
		g_lastStep = 0;
		g_slowing = false;
		g_beatBlip = 0;
		g_beatBlipOf = -1;
	}

	bool Ready()
	{
		return g_loaded;
	}

	void Tick()
	{
		// (only for a save with an officer: made in the creator, or played)
		static bool hasOfficer = false;
		static uint64_t checked = 0;
		if (ml::TickMs() >= checked)
		{
			Vector3 at{};
			float heading = 0;
			hasOfficer = officer::Ped() || look::Exists() || persist::Where(at, heading);
			checked = ml::TickMs() + 1000;
		}
		const bool ok = hasOfficer && persist::Settled() && !DLC::GET_IS_LOADING_SCREEN_ACTIVE();
		SlowClock(ok && g_loaded && officer::IsPlayer() && officer::InUniform() && g.session.active && !HUD::IS_PAUSE_MENU_ACTIVE());
		if (!ok)
			return;
		const double now = Now();
		if (!g_loaded)
		{
			Load(now);
			g_lastStep = now;
		}
		BeatBlip(officer::IsPlayer() && g.session.active ? g.planned.beat : -1);
		if (ml::TickMs() < g_nextStep)
			return;
		g_nextStep = ml::TickMs() + 500;
		Step(now);
		// Out of the beat for long: dispatch says so now and then.
		if (officer::IsPlayer() && g.session.active && InWindow(now) && !InBeat(ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), true)))
		{
			if (!g_nextBeatNag)
				g_nextBeatNag = ml::TickMs() + 180000;
			else if (ml::TickMs() >= g_nextBeatNag)
			{
				g_nextBeatNag = ml::TickMs() + 600000;
				ml::Notify("調度：{}，請回到你的勤務區（{}）", Callsign(), BeatName());
			}
		}
		else
			g_nextBeatNag = 0;
	}

	void ClockIn()
	{
		if (!g_loaded)
			return;
		const double now = Now();
		Count(now, true);
		if (!g.session.active)
			Begin(now, true);
		g.session.locker = true;
		Planned& p = g.planned;
		const double start = static_cast<double>(p.start);
		if (now >= start - 60.0 && now < static_cast<double>(p.end))
		{
			if (!p.attended && now > start + kGrace)
			{
				++g.late;
				p.late = static_cast<int>(now - start);
				Note(std::format("遲到 {} 分鐘", p.late));
				ml::Notify("遲到 {} 分鐘，錯過了勤前教育：勤務區{}，呼號 {}", p.late, BeatName(), Callsign());
			}
			else if (!p.rollCall)
			{
				if (!p.attended)
					Note("準時");
				p.rollCall = true;
				menu::Open(RollCallMenu(true));
			}
			p.attended = now >= start || p.attended;
		}
		else
			ml::Notify("排班以外的時間上班：算加班（勤務區{}）", BeatName());
		Store();
	}

	std::string BeforeClockOut()
	{
		const int drafts = records::Drafts();
		if (drafts > 0 && !(g_confirmOut && ml::TickMs() < g_confirmUntil))
		{
			g_confirmOut = true;
			g_confirmUntil = ml::TickMs() + 8000;
			return std::format("還有{}份報告沒提交：再按一次仍要下班（會記為報告逾期）", drafts);
		}
		g_confirmOut = false;
		return "";
	}

	void ClockOut()
	{
		partner::ShiftOver();
		if (!g_loaded)
			return;
		const double now = Now();
		Count(now, true);
		if (const int drafts = records::Drafts(); drafts > 0)
		{
			++g.overdue;
			if (g.planned.attended) // (a shift's: not a session out of the roster)
				Note("", std::format("報告逾期 {} 份", drafts));
		}
		End("locker");
	}

	void RequestShift()
	{
		if (!g_loaded)
			return;
		if (g.shifts < kProbation)
		{
			ml::Notify("試用期間只排大夜班（還要 {} 班）", kProbation - g.shifts);
			return;
		}
		ShiftRequestMenu();
	}

	bool MayWork(std::string& why)
	{
		if (!g_loaded)
			return true;
		if (g.fired)
		{
			why = Day() >= g.rehire ? "你已被免職：按 G「警局事務」可以重新申請入職" : std::format("你已被免職：{} 起可以重新申請", Date(g.rehire));
			return false;
		}
		if (g.suspended > 0)
		{
			why = std::format("停職中（還有{}班），不能上班", g.suspended);
			return false;
		}
		return true;
	}

	void Suspend(int shifts)
	{
		if (!g_loaded)
			return;
		// (the shifts from now on: a planned shift already over is not one of them)
		if (const double now = Now(); static_cast<double>(g.planned.end) <= now)
			Plan(now);
		g.suspended += shifts;
		Store();
		ml::Log("shift: suspended for {} shifts", shifts);
	}

	void Fire(int days)
	{
		if (!g_loaded)
			return;
		g.fired = true;
		g.suspended = 0;
		g.rehire = Day() + days;
		Store();
		ml::Log("shift: dismissed; may apply again on day {}", g.rehire);
	}

	bool MayRejoin()
	{
		return g_loaded && g.fired && Day() >= g.rehire;
	}

	void Rejoin()
	{
		if (!MayRejoin())
			return;
		const double now = Now();
		const std::vector<ml::Json> payslips = g.payslips, attendance = g.attendance;
		const std::string sergeant = g.sergeant;
		const int unit = g.unit;
		g = Career{};
		g.hired = DayOf(now);
		g.payslips = payslips;
		g.attendance = attendance;
		g.sergeant = sergeant;
		g.unit = unit;
		Plan(now);
		career::Rejoin();
		Store();
		ml::Notify("重新入職：從{}的試用期開始，先上大夜班", career::Title());
		ml::Log("shift: rejoined on day {}", g.hired);
	}

	bool OnProbation()
	{
		return g_loaded && g.shifts < kProbation;
	}

	int ShiftKind()
	{
		return g_loaded ? g.planned.kind : kNight;
	}

	bool OnShift()
	{
		if (!g_loaded || g.suspended > 0 || g.fired)
			return false;
		const double now = Now();
		return InWindow(now) && !(g.planned.pending && !g.session.active);
	}

	life::Scene Scene()
	{
		if (!g_loaded)
		{
			const int hour = CLOCK::GET_CLOCK_HOURS();
			return hour >= 23 || hour < 7 ? life::Scene::Home : life::Scene::Out;
		}
		if (OnShift() || g.session.active)
			return life::Scene::OnDuty;
		const Kind& k = kKinds[g.kind];
		const int hour = CLOCK::GET_CLOCK_HOURS();
		const bool asleep = k.sleep < k.wake ? hour >= k.sleep && hour < k.wake : hour >= k.sleep || hour < k.wake;
		return asleep ? life::Scene::Home : life::Scene::Out;
	}

	bool InBeat(const Vector3& at)
	{
		if (!g_loaded)
			return true;
		const auto& beats = kBeats[Department()];
		const Beat& b = beats[static_cast<size_t>(g.planned.beat) % beats.size()];
		return std::hypot(at.x - b.x, at.y - b.y) <= b.r;
	}

	std::string BeatName()
	{
		const auto& beats = kBeats[Department()];
		return beats[static_cast<size_t>(g.planned.beat) % beats.size()].name;
	}

	std::string Callsign()
	{
		return CallsignOf(g.planned);
	}

	bool TakeBoloCar(std::string& model, std::string& plate)
	{
		if (!g_loaded || g.planned.boloCarUsed || !InWindow(Now()) || ai::Random() > 0.5f)
			return false;
		g.planned.boloCarUsed = true;
		model = g.planned.boloCar;
		plate = g.planned.boloPlate;
		Store();
		return true;
	}

	bool TakeBoloPerson(std::string& name, std::string& crime)
	{
		if (!g_loaded || g.planned.boloPersonUsed || !InWindow(Now()) || ai::Random() > 0.4f)
			return false;
		g.planned.boloPersonUsed = true;
		name = g.planned.boloName;
		crime = g.planned.boloCrime;
		Store();
		return true;
	}

	Counts Totals()
	{
		return {g.shifts, g.late, g.early, g.absent, g.overdue};
	}

	std::string Today()
	{
		return Date(DayOf(Now()));
	}

	int MinutesToShift()
	{
		if (!g_loaded)
			return -1;
		const double now = Now();
		return static_cast<double>(g.planned.start) > now ? static_cast<int>(static_cast<double>(g.planned.start) - now) : -1;
	}

	int64_t Day()
	{
		return DayOf(Now());
	}

	std::string FocusText()
	{
		return kFocus[static_cast<size_t>(g.planned.focus) % std::size(kFocus)].detail;
	}

	std::string BoloCar()
	{
		return CarName(g.planned.boloCar) + "（" + g.planned.boloPlate + "）";
	}

	menu::Item MenuItem()
	{
		const double now = g_loaded ? Now() : 0;
		const bool window = g_loaded && InWindow(now);
		const int left = g_loaded ? static_cast<int>(static_cast<double>(g.planned.end) - now) : 0;
		menu::Item i{"勤務", g_loaded ? (window ? std::format("剩 {}:{:02}", left / 60, left % 60) : "加班") : "",
		    "今天的勤務區、呼號、協尋、重點"};
		i.enabled = g_loaded;
		i.sub = [] { return RollCallMenu(false); };
		return i;
	}

	ml::Json Json()
	{
		if (!g_loaded)
			return ml::Json{{"ready", false}};
		const double now = Now();
		ml::Json week = ml::Json::Array();
		for (int64_t day = DayOf(now), last = day + 7; day < last; ++day)
		{
			const bool work = WorkDay(day);
			const int kind = KindOn(day);
			week.Push(ml::Json{{"date", Date(day)}, {"work", work}, {"kind", work ? kKinds[kind].name : "休假"},
			    {"hours", work ? std::format("{:02}:00–{:02}:00", kKinds[kind].start, (kKinds[kind].start + 8) % 24) : ""}});
		}
		const Planned& p = g.planned;
		const Session& s = g.session;
		return ml::Json{{"ready", true}, {"name", look::Name()}, {"rank", career::Title()}, {"rate", Rate()}, {"shifts", g.shifts},
		    {"probation", std::max(0, kProbation - g.shifts)}, {"hired", Date(g.hired)}, {"kind", kKinds[g.kind].name},
		    {"suspended", g.suspended}, {"fired", g.fired},
		    {"request", g.request >= 0 ? std::string(kKinds[g.request].name) + "（" + Date(g.requestFrom) + " 起）" : ""},
		    {"late", g.late}, {"early", g.early}, {"absent", g.absent}, {"overdue", g.overdue},
		    {"now", Date(DayOf(now)) + " " + Clock(now)},
		    {"next", ml::Json{{"date", Date(DayOf(static_cast<double>(p.start)))}, {"kind", kKinds[p.kind].name},
		                 {"hours", Clock(static_cast<double>(p.start)) + "–" + Clock(static_cast<double>(p.end))}, {"beat", BeatName()},
		                 {"callsign", Callsign()}, {"boloCar", CarName(p.boloCar) + " " + p.boloPlate}, {"boloName", p.boloName},
		                 {"boloCrime", p.boloCrime}, {"focus", kFocus[p.focus].title}, {"underway", InWindow(now)}}},
		    {"session", s.active ? ml::Json{{"since", Clock(s.start)}, {"hours", std::round((s.regular + s.overtime + s.alone) / 6.0) / 10},
		                                 {"overtime", std::round(s.overtime / 6.0) / 10}}
		                         : ml::Json()},
		    {"week", week}, {"payslips", g.payslips}, {"attendance", g.attendance}};
	}
}
