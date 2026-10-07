// Price movement for both exchanges, one step per game hour: a random walk per company on top of a market mood that
// changes every day or few, and company news that pushes a price (a jump and a trend for some hours), some of it in two
// acts (a rumour, then how it turned out).
// - BAWSAQ: story mode leaves its prices empty (they come from GTA Online's community stats); the mod's prices are the
//   only ones.
// - LCN: the game moves these prices only on story events (missions), so the step starts from the game's current price
//   (story moves included) and writes the result back into the stock table and the save's LCN prices
//   (Global 114990+20573+103), where the game keeps them too.
// The mod's state is its save data ("market"), so it follows the game's save.
#include <algorithm>
#include <array>
#include <cmath>
#include <format>

#include "stocks.hpp"

namespace stocks::market
{
	namespace
	{
		constexpr size_t kHistory = 16;
		constexpr size_t kMaxNews = 40;
		constexpr int kMaxCatchUp = 72; // hours simulated at once after a long skip (sleeping, a mission, ...)

		// ---- random numbers (state saved with the rest) ----------------------------------------

		uint64_t g_rng = 0x9E3779B97F4A7C15ull;
		uint64_t Next()
		{
			g_rng ^= g_rng << 13;
			g_rng ^= g_rng >> 7;
			g_rng ^= g_rng << 17;
			return g_rng;
		}
		double Uniform() { return (Next() >> 11) * (1.0 / 9007199254740992.0); }
		double Range(double a, double b) { return a + (b - a) * Uniform(); }
		int RangeInt(int a, int b) { return a + static_cast<int>(Next() % static_cast<uint64_t>(b - a + 1)); }
		double Gauss()
		{
			const double u = std::max(Uniform(), 1e-12), v = Uniform();
			return std::sqrt(-2.0 * std::log(u)) * std::cos(6.283185307179586 * v);
		}

		// ---- events ----------------------------------------------------------------------------

		// What a piece of news does to a price: an immediate move, then a drift per hour for some hours.
		struct Effect
		{
			double jumpMin, jumpMax;   // fraction, e.g. 0.05 = +5 %
			double driftMin, driftMax; // per hour
			int hoursMin, hoursMax;
		};
		// {} in the texts = the company's name.
		struct Event
		{
			const char* title;
			const char* body;
			int mood;
			Effect effect;
			int followUp = -1; // index into kFollowUps: the second act
		};
		struct FollowUp
		{
			int hoursMin, hoursMax;  // until it happens
			double goodChance;
			Event good, bad;
		};

		const FollowUp kFollowUps[] = {
		    {8, 30, 0.6,
		        {"{} 確認收購案，股價跳空大漲", "雙方董事會通過合併協議，{} 股東每股可獲溢價收購。分析師認為這是今年最大的併購案之一。", 1,
		            {0.10, 0.22, 0.0005, 0.002, 6, 18}},
		        {"{} 收購談判破局，股價重挫", "知情人士指出，雙方在價格上無法取得共識，{} 宣布終止收購談判。先前追高的投資人損失慘重。", -1,
		            {-0.15, -0.07, -0.002, -0.0005, 6, 18}}},
		    {12, 48, 0.5,
		        {"{} 與主管機關達成和解", "{} 同意支付小額罰款並改善內部控管，調查正式結案。市場鬆了一口氣。", 1, {0.03, 0.08, 0.0003, 0.001, 8, 24}},
		        {"{} 遭重罰並被要求整頓", "主管機關認定 {} 違規情節重大，開出天價罰單，並要求更換經營團隊。", -1, {-0.18, -0.08, -0.002, -0.0006, 12, 36}}},
		    {6, 24, 0.85,
		        {"{} 勞資協商成功，工廠全面復工", "經過數日談判，{} 與工會達成協議，產線恢復運作，訂單交付可望追上進度。", 1, {0.03, 0.07, 0.0002, 0.0008, 6, 18}},
		        {"{} 罷工延長，產線停擺擴大", "工會拒絕 {} 的最新提案，罷工擴及其他廠區，第四季營收恐受重創。", -1, {-0.10, -0.04, -0.0015, -0.0004, 8, 24}}},
		    {10, 40, 0.55,
		        {"{} 專利訴訟勝訴", "法院駁回對 {} 的侵權指控，並判決原告支付訴訟費用。", 1, {0.04, 0.10, 0.0002, 0.0008, 6, 18}},
		        {"{} 專利訴訟敗訴，須支付鉅額賠償", "陪審團認定 {} 侵權成立，賠償金額遠超出市場預期。", -1, {-0.14, -0.06, -0.0015, -0.0004, 8, 24}}},
		    {12, 36, 0.5,
		        {"{} 新產品正式上市，首日即告售罄", "延宕多時的旗艦產品終於推出，{} 門市大排長龍，網路預購一度當機。", 1, {0.06, 0.14, 0.0005, 0.0015, 8, 24}},
		        {"{} 新產品再度延期，市場失望", "{} 表示品質測試未過，新產品上市日期無限期延後。", -1, {-0.12, -0.05, -0.0015, -0.0004, 8, 24}}},
		};

		const Event kEvents[] = {
		    {"{} 季度財報優於預期", "{} 本季營收與獲利雙雙創下新高，並上調全年財測。", 1, {0.03, 0.09, 0.0003, 0.0012, 12, 36}},
		    {"{} 季度財報不如預期", "{} 營收下滑、毛利率縮水，管理層對下一季展望保守。", -1, {-0.09, -0.03, -0.0012, -0.0003, 12, 36}},
		    {"分析師調升 {} 評等至「買進」", "多家券商同步調高 {} 的目標價，看好其市占率持續擴大。", 1, {0.01, 0.04, 0.0002, 0.0006, 6, 24}},
		    {"分析師調降 {} 評等至「賣出」", "券商報告指出 {} 估值過高、成長動能趨緩。", -1, {-0.04, -0.01, -0.0006, -0.0002, 6, 24}},
		    {"{} 執行長捲入醜聞", "八卦網站披露 {} 執行長的私生活爭議，董事會召開緊急會議。", -1, {-0.12, -0.05, -0.0012, -0.0003, 12, 30}},
		    {"{} 宣布大規模產品召回", "{} 坦承部分產品有安全瑕疵，將全面召回並免費更換。", -1, {-0.10, -0.04, -0.001, -0.0003, 12, 36}},
		    {"{} 宣布買回庫藏股", "{} 將動用現金買回自家股票，展現對公司前景的信心。", 1, {0.02, 0.06, 0.0002, 0.0008, 12, 24}},
		    {"{} 拿下大型政府合約", "{} 擊敗競爭對手，取得為期十年的聖安地列斯州政府合約。", 1, {0.04, 0.10, 0.0003, 0.001, 12, 36}},
		    {"{} 爆發資料外洩", "駭客竊取 {} 數百萬筆客戶資料，公司股價應聲下跌。", -1, {-0.08, -0.03, -0.0008, -0.0002, 8, 24}},
		    {"網紅大力推薦 {}，散戶湧入", "社群媒體掀起 {} 搶購潮，交易量暴增。專家提醒追高風險。", 1, {0.05, 0.15, -0.0015, -0.0005, 6, 18}},
		    // Two-act stories (the second act follows some hours later).
		    {"傳 {} 將被收購", "市場傳出 {} 正與大型集團洽談收購，雙方均未證實。", 1, {0.04, 0.10, 0.0, 0.0006, 6, 12}, 0},
		    {"{} 遭主管機關調查", "{} 涉嫌財報不實，聖安地列斯證券交易委員會已介入調查。", -1, {-0.10, -0.04, -0.0008, -0.0002, 8, 24}, 1},
		    {"{} 員工發動罷工", "{} 工會要求加薪與改善工時，主要工廠停工。", -1, {-0.06, -0.02, -0.0006, -0.0002, 6, 12}, 2},
		    {"{} 捲入專利訴訟", "競爭對手控告 {} 侵犯多項專利，求償金額驚人。", -1, {-0.06, -0.02, -0.0005, -0.0001, 6, 12}, 3},
		    {"{} 預告將發表旗艦新產品", "{} 宣布將於近日推出全新旗艦產品，外界期待甚高。", 1, {0.03, 0.08, 0.0002, 0.0008, 6, 12}, 4},
		};

		struct Mood
		{
			double drift;
			const char* title;
			const char* body;
			int mood;
		};
		// {} = the exchange.
		const Mood kMoods[] = {
		    {0.0012, "{} 多頭氣氛濃厚", "利率前景樂觀，資金大舉湧入股市，分析師預期漲勢將延續數日。", 1},
		    {0.0005, "{} 溫和走高", "經濟數據優於預期，投資人信心回升。", 1},
		    {0.0, "{} 盤整待變", "市場缺乏方向，投資人觀望即將公布的經濟數據。", 0},
		    {-0.0005, "{} 小幅走低", "通膨數據高於預期，投資人轉趨保守。", -1},
		    {-0.0012, "{} 賣壓沉重", "景氣衰退疑慮升溫，恐慌指數飆升，大盤連日下挫。", -1},
		};
		const char* const kExchanges[2] = {"LCN", "BAWSAQ"};

		// ---- state -----------------------------------------------------------------------------

		struct StockState
		{
			double price = 0, vol = 0.01, trend = 0;
			int trendLeft = 0;
			std::vector<float> history; // oldest first
		};
		struct Pending
		{
			int stock, followUp;
			int64_t due;
		};
		struct State
		{
			bool loaded = false;
			int64_t hour = 0;
			double marketDrift[2] = {}; // LCN, BAWSAQ
			int marketLeft[2] = {};
			std::array<StockState, kStockCount> stocks{};
			std::vector<Pending> pending;
			std::vector<NewsItem> news; // newest first
		} g;

		std::string Format(const char* text, const std::string& name)
		{
			std::string out = text;
			for (size_t at; (at = out.find("{}")) != std::string::npos;)
				out.replace(at, 2, name);
			return out;
		}
		std::string NameOf(int stock)
		{
			const std::string name = CompanyName(stock);
			return name.empty() ? Ticker(stock) : name;
		}

		// `stock` >= 0: company news; kMarketLcn / kMarketBawsaq: the whole exchange.
		void Publish(int64_t hour, int stock, const char* title, const char* body, int mood)
		{
			const std::string name = stock >= 0 ? NameOf(stock) : kExchanges[stock == kMarketBawsaq];
			g.news.insert(g.news.begin(), {hour, stock, Format(title, name), Format(body, name), mood});
			if (g.news.size() > kMaxNews)
				g.news.resize(kMaxNews);
		}

		void Apply(int stock, const Effect& e)
		{
			StockState& s = g.stocks[stock];
			s.price *= 1.0 + Range(e.jumpMin, e.jumpMax);
			s.trend = Range(e.driftMin, e.driftMax);
			s.trendLeft = RangeInt(e.hoursMin, e.hoursMax);
		}

		bool InStory(int stock)
		{
			return std::ranges::any_of(g.pending, [&](const Pending& p) { return p.stock == stock; });
		}

		void Fire(int64_t hour, int stock, const Event& e)
		{
			Apply(stock, e.effect);
			Publish(hour, stock, e.title, e.body, e.mood);
			if (e.followUp >= 0)
			{
				const FollowUp& f = kFollowUps[e.followUp];
				g.pending.push_back({stock, e.followUp, hour + RangeInt(f.hoursMin, f.hoursMax)});
			}
		}

		// One game hour.
		void Step(int64_t hour)
		{
			for (int x = 0; x < 2; ++x)
				if (--g.marketLeft[x] <= 0)
				{
					const Mood& m = kMoods[RangeInt(0, static_cast<int>(std::size(kMoods)) - 1)];
					g.marketDrift[x] = m.drift;
					g.marketLeft[x] = RangeInt(24, 96);
					Publish(hour, x ? kMarketBawsaq : kMarketLcn, m.title, m.body, m.mood);
				}
			for (int id = 0; id < kStockCount; ++id)
			{
				const bool bawsaq = IsBawsaq(id);
				StockState& s = g.stocks[id];
				// LCN: from the game's price, which story events may have moved since the last step.
				if (!bawsaq && Price(id) > 0)
					s.price = Price(id);
				if (s.price <= 0)
					continue;
				const double r = g.marketDrift[bawsaq] + (s.trendLeft > 0 ? s.trend : 0.0) + s.vol * Gauss();
				s.price = std::clamp(s.price * std::exp(r), 1.0, 5000.0);
				if (s.trendLeft > 0)
					--s.trendLeft;
				s.history.push_back(static_cast<float>(s.price));
				if (s.history.size() > kHistory)
					s.history.erase(s.history.begin());
			}
			// Second acts that are due.
			for (size_t i = 0; i < g.pending.size();)
				if (g.pending[i].due <= hour)
				{
					const Pending p = g.pending[i];
					g.pending.erase(g.pending.begin() + static_cast<std::ptrdiff_t>(i));
					const FollowUp& f = kFollowUps[p.followUp];
					Fire(hour, p.stock, Uniform() < f.goodChance ? f.good : f.bad);
				}
				else
					++i;
			// New company news: about one every eight hours on each exchange.
			for (int x = 0; x < 2; ++x)
				if (Uniform() < 0.12)
				{
					std::vector<int> candidates;
					for (int id = 0; id < kStockCount; ++id)
						if (IsBawsaq(id) == (x == 1) && g.stocks[id].price > 0 && !InStory(id))
							candidates.push_back(id);
					if (!candidates.empty())
						Fire(hour, candidates[Next() % candidates.size()], kEvents[Next() % std::size(kEvents)]);
				}
		}

		// A first state: prices spread out by company, with a made-up recent history.
		void Start(int64_t hour)
		{
			g = {};
			g.loaded = true;
			g.hour = hour;
			g_rng = 0x9E3779B97F4A7C15ull ^ static_cast<uint64_t>(hour) * 0x100000001B3ull;
			for (int id = 0; id < kStockCount; ++id)
			{
				StockState& s = g.stocks[id];
				if (IsBawsaq(id))
				{
					s.price = Range(12.0, 380.0);
					s.vol = Range(0.006, 0.02);
				}
				else
				{
					// LCN starts from the game's prices and history.
					s.price = Price(id);
					s.vol = Range(0.003, 0.01);
					for (const float v : History(id))
						if (v > 0)
							s.history.push_back(v);
				}
			}
			// A made-up recent history for BAWSAQ (LCN keeps the game's).
			for (int64_t h = hour - static_cast<int64_t>(kHistory); h < hour; ++h)
				for (int id = 0; id < kStockCount; ++id)
					if (IsBawsaq(id))
					{
						StockState& s = g.stocks[id];
						s.price = std::clamp(s.price * std::exp(s.vol * Gauss()), 1.0, 5000.0);
						s.history.push_back(static_cast<float>(s.price));
					}
			Publish(hour, kMarketBawsaq, "{} 恢復交易", "聖安地列斯證券交易所宣布 {} 系統維修完畢，即日起恢復正常交易。", 0);
		}

		// ---- saving ----------------------------------------------------------------------------

		void Save()
		{
			ml::Json stocks = ml::Json::Object();
			for (int id = 0; id < kStockCount; ++id)
				if (g.stocks[id].price > 0)
				{
					const StockState& s = g.stocks[id];
					stocks[std::to_string(id)] = {{"p", s.price}, {"vol", s.vol}, {"trend", s.trend}, {"left", s.trendLeft}, {"h", s.history}};
				}
			ml::Json pending = ml::Json::Array(), news = ml::Json::Array();
			for (const Pending& p : g.pending)
				pending.Push({{"stock", p.stock}, {"followUp", p.followUp}, {"due", p.due}});
			for (const NewsItem& n : g.news)
				news.Push({{"hour", n.hour}, {"stock", n.stock}, {"title", n.title}, {"body", n.body}, {"mood", n.mood}});
			ml::save::Set("market", {{"hour", g.hour}, {"rng", std::to_string(g_rng)},
			                            {"marketDrift", std::vector<double>{g.marketDrift[0], g.marketDrift[1]}},
			                            {"marketLeft", std::vector<int>{g.marketLeft[0], g.marketLeft[1]}}, {"stocks", stocks}, {"pending", pending},
			                            {"news", news}});
		}

		bool Load()
		{
			const ml::Json j = ml::save::Get("market");
			if (!j.IsObject())
				return false;
			g = {};
			g.loaded = true;
			g.hour = j["hour"].Int64();
			g_rng = std::stoull(j["rng"].Str("1"));
			for (int x = 0; x < 2; ++x)
			{
				g.marketDrift[x] = j["marketDrift"][x].Number();
				g.marketLeft[x] = j["marketLeft"][x].Int();
			}
			for (const auto& [id, s] : j["stocks"].Members())
				if (const int i = std::stoi(id); i >= 0 && i < kStockCount)
					g.stocks[i] = {s["p"].Number(), s["vol"].Number(0.01), s["trend"].Number(), s["left"].Int(), s["h"].Get<std::vector<float>>()};
			for (const ml::Json& p : j["pending"].Items())
				g.pending.push_back({p["stock"].Int(), p["followUp"].Int(), p["due"].Int64()});
			for (const ml::Json& n : j["news"].Items())
				g.news.push_back({n["hour"].Int64(), n["stock"].Int(), n["title"].Str(), n["body"].Str(), n["mood"].Int()});
			return true;
		}

		// BAWSAQ: every tick (the game empties it again now and then).
		void WriteBawsaq()
		{
			for (int id = 0; id < kStockCount; ++id)
				if (IsBawsaq(id) && !g.stocks[id].history.empty())
					SetQuote(id, static_cast<float>(g.stocks[id].price), g.stocks[id].history);
		}
		// LCN: after a step only, so story moves in between are kept. The save keeps LCN prices too (by the stock's place
		// on its exchange, +35), with the highest and lowest.
		void WriteLcn()
		{
			const ml::Global saved = ml::Global(114990) + 20573;
			for (int id = 0; id < kStockCount; ++id)
			{
				const StockState& s = g.stocks[id];
				if (IsBawsaq(id) || s.price <= 0)
					continue;
				SetQuote(id, static_cast<float>(s.price), s.history);
				const int k = Stock(id).Field(35).Int();
				if (k < 0 || k >= (saved + 103).Size())
					continue;
				const float p = static_cast<float>(s.price);
				(saved + 103).At(k).SetFloat(p);
				if ((saved + 146).At(k).Float() < p)
					(saved + 146).At(k).SetFloat(p);
				if (const float low = (saved + 189).At(k).Float(); low <= 0 || low > p)
					(saved + 189).At(k).SetFloat(p);
			}
		}

		int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d)
		{
			y -= m <= 2;
			const int64_t era = (y >= 0 ? y : y - 399) / 400;
			const unsigned yoe = static_cast<unsigned>(y - era * 400);
			const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
			const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
			return era * 146097 + static_cast<int64_t>(doe) - 719468;
		}
	}

	int64_t GameHour()
	{
		const int64_t days = DaysFromCivil(CLOCK::GET_CLOCK_YEAR(), static_cast<unsigned>(CLOCK::GET_CLOCK_MONTH() + 1),
		                         static_cast<unsigned>(CLOCK::GET_CLOCK_DAY_OF_MONTH())) - DaysFromCivil(2000, 1, 1);
		return days * 24 + CLOCK::GET_CLOCK_HOURS();
	}

	void Reload()
	{
		g.loaded = false;
	}

	void Tick()
	{
		const int64_t hour = GameHour();
		if (!g.loaded && !Load())
		{
			Start(hour);
			Save();
		}
		if (hour != g.hour)
		{
			// The clock went back (an older save, or the time was set): just carry on from there.
			int steps = static_cast<int>(std::clamp<int64_t>(hour - g.hour, 0, kMaxCatchUp));
			for (int64_t h = hour - steps + 1; h <= hour; ++h)
				Step(h);
			g.hour = hour;
			WriteLcn();
			Save();
		}
		WriteBawsaq();
	}

	std::vector<NewsItem> News(bool bawsaq)
	{
		std::vector<NewsItem> out;
		for (const NewsItem& n : g.news)
			if (n.stock >= 0 ? IsBawsaq(n.stock) == bawsaq : (n.stock == kMarketBawsaq) == bawsaq)
				out.push_back(n);
		return out;
	}
}
