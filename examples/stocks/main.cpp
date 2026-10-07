// Stock market mod: the LCN and BAWSAQ websites of the loader's browser (web\www.lcn-exchange.com, web\www.bawsaq.com;
// research/phase0.md §35).
//
// The stock table, the portfolios and the trades are the game's own: LCN prices come from the game's stock_controller
// (in-game events move them), a trade runs appinternet's own buy / sell function (money, cost basis, stats), and the
// portfolio is part of the game's save. Prices move every game hour on both exchanges (market.cpp): BAWSAQ, which story
// mode leaves empty offline, is simulated entirely; LCN moves on top of the game's own (story) price changes.
//
// Pages call:
//   stocks.list(exchange)      -> { character, cash, tradable, stocks: [...], portfolio: [...], slots, news: [...] }
//   stocks.buy(id, shares)     -> { ok, error, cash }
//   stocks.sell(id, shares)    -> { ok, error, cash }
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include "stocks.hpp"

ML_MOD_INFO("Stocks", "0.1.0", "ModLoader", "The LCN and BAWSAQ stock market websites")

namespace stocks
{
	// ---- the game's stock table --------------------------------------------------------------

	const ml::Global kStocks{57379};

	ml::Global Stock(int id) { return kStocks.At(id, 36); }
	bool IsBawsaq(int id) { return Stock(id).Field(8).Bool(); }
	float Price(int id) { return Stock(id).Field(9).Float(); }
	std::vector<float> History(int id)
	{
		std::vector<float> out;
		for (int k = 15; k <= 30; ++k)
			out.push_back(Stock(id).Field(k).Float());
		return out;
	}

	std::string Text(const char* label)
	{
		if (!label || !*label || !HUD::DOES_TEXT_LABEL_EXIST(label))
			return {};
		std::string s = HUD::GET_FILENAME_FOR_AUDIO_CONVERSATION(label);
		for (size_t at; (at = s.find("\xC2\xB5")) != std::string::npos;) // the game's non-breaking space
			s.replace(at, 2, " ");
		return s;
	}
	std::string CompanyName(int id) { return Text(Stock(id).Text().c_str()); }
	std::string Ticker(int id) { return Text(Stock(id).Field(4).Text().c_str()); }

	void SetQuote(int id, float price, const std::vector<float>& history)
	{
		const ml::Global e = Stock(id);
		e.Field(9).SetFloat(price);
		float high = price, low = price;
		for (size_t k = 0; k < 16; ++k)
		{
			const float v = k < history.size() ? history[history.size() < 16 ? k : history.size() - 16 + k] : price;
			e.Field(15 + static_cast<int>(k)).SetFloat(v);
			high = std::max(high, v);
			low = std::min(low, v);
		}
		const float first = history.empty() ? price : history[history.size() > 16 ? history.size() - 16 : 0];
		e.Field(31).SetFloat(high);
		e.Field(32).SetFloat(low);
		e.Field(33).SetFloat(price - first);
		e.Field(34).SetFloat(first > 0 ? (price - first) / first * 100.0f : 0.0f);
	}

	namespace
	{
		// ---- portfolios: Global 114990+20573, per character 33 slots: stock ids, cost, shares (arrays of 10) ----

		constexpr int kSlots = 10;
		ml::Global Portfolio(int c) { return ml::Global(114990) + (20573 + 33 * c); }
		int SlotStock(int c, int slot) { return Portfolio(c).At(slot).Int(); }
		float SlotCost(int c, int slot) { return (Portfolio(c) + 11).At(slot).Float(); }
		int SlotShares(int c, int slot) { return (Portfolio(c) + 22).At(slot).Int(); }
		int Held(int c, int stock)
		{
			for (int slot = 0; slot < kSlots; ++slot)
				if (SlotShares(c, slot) > 0 && SlotStock(c, slot) == stock)
					return SlotShares(c, slot);
			return 0;
		}
		int SlotsUsed(int c)
		{
			int used = 0;
			for (int slot = 0; slot < kSlots; ++slot)
				used += SlotShares(c, slot) > 0;
			return used;
		}

		// ---- trades: appinternet's own functions, (character, character, stock, shares, total as a float) ----

		constexpr const char* kBuyPattern = "2d 05 0b 00 00 38 01 38 02 5d ?? ?? ?? 39 07 38";  // @25312
		constexpr const char* kSellPattern = "2d 05 0f 00 00 70 39 07 71 39 08 71 39 08 38 08"; // @24363

		int64_t FloatArg(float f)
		{
			uint32_t bits;
			std::memcpy(&bits, &f, 4);
			return bits;
		}

		ml::Json Result(const char* error)
		{
			return {{"ok", !*error}, {"error", error}, {"cash", ml::game::Cash()}};
		}

		ml::Json Trade(bool buy, int id, int shares)
		{
			const int c = ml::game::CharacterIndex();
			if (c < 0)
				return Result("character");
			if (id < 0 || id >= kStockCount || shares <= 0)
				return Result("unknown");
			const float price = Price(id);
			if (price <= 0)
				return Result("closed");
			const int before = Held(c, id);
			const float total = price * static_cast<float>(shares);
			if (buy)
			{
				if (ml::game::Cash() < static_cast<int>(std::ceil(total)))
					return Result("money");
				if (before == 0 && SlotsUsed(c) >= kSlots)
					return Result("slots");
			}
			else if (before < shares)
				return Result("shares");
			const auto ran = ml::scripts::RunFunction("appinternet", buy ? kBuyPattern : kSellPattern, {c, c, id, shares, FloatArg(total)}, 4000);
			if (ran != ml::scripts::RunResult::Ran)
			{
				ml::LogError("the game's stock {} function was not found (game update?)", buy ? "buy" : "sell");
				return Result("unsupported");
			}
			const int after = Held(c, id);
			if (after != before + (buy ? shares : -shares))
			{
				ml::LogError("{} {} x{}: holding {} -> {}", buy ? "buy" : "sell", id, shares, before, after);
				return Result("failed");
			}
			ml::Log("character {} {} {} shares of {} at ${:.2f} (holding {} -> {})", c, buy ? "bought" : "sold", shares, Ticker(id), price, before, after);
			return Result("");
		}

		// ---- LCN news: the game's own (Global 64926: 4 groups of 3 items, 11 slots: +1 title, +5 text, +9 stock) ----

		ml::Json LcnNews()
		{
			ml::Json out = ml::Json::Array();
			std::vector<std::string> seen;
			const ml::Global news{64926};
			for (int group = 0; group < news.Size(); ++group)
			{
				const ml::Global items = news.At(group, 34);
				for (int i = 0; i < items.Size(); ++i)
				{
					const ml::Global item = items.At(i, 11);
					const std::string title = item.Field(1).Text();
					if (title.empty() || std::ranges::find(seen, title) != seen.end())
						continue;
					seen.push_back(title);
					const std::string text = Text(title.c_str());
					if (text.empty())
						continue;
					const int stock = item.Field(9).Int();
					const bool rise = title.find("_SR_") != std::string::npos || title.find("_USR_") != std::string::npos;
					const bool fall = title.find("_SF_") != std::string::npos || title.find("_USF_") != std::string::npos;
					out.Push({{"title", text}, {"body", Text(item.Field(5).Text().c_str())}, {"stock", stock >= 0 && stock < kStockCount ? stock : -1},
					    {"mood", rise ? 1 : fall ? -1 : 0}});
				}
			}
			return out;
		}

		ml::Json List(const std::string& exchange)
		{
			const bool bawsaq = exchange == "bawsaq";
			const int c = ml::game::CharacterIndex();
			ml::Json list = ml::Json::Array(), portfolio = ml::Json::Array();
			bool tradable = false;
			for (int id = 0; id < kStockCount; ++id)
			{
				if (IsBawsaq(id) != bawsaq)
					continue;
				const float price = Price(id);
				tradable |= price > 0;
				list.Push({{"id", id}, {"ticker", Ticker(id)}, {"name", CompanyName(id)}, {"price", price}, {"history", History(id)},
				    {"change", Stock(id).Field(33).Float()}, {"changePct", Stock(id).Field(34).Float()},
				    {"held", c >= 0 ? Held(c, id) : 0}});
			}
			if (c >= 0)
				for (int slot = 0; slot < kSlots; ++slot)
					if (const int shares = SlotShares(c, slot); shares > 0)
					{
						const int id = SlotStock(c, slot);
						portfolio.Push({{"id", id}, {"ticker", Ticker(id)}, {"name", CompanyName(id)}, {"exchange", IsBawsaq(id) ? "bawsaq" : "lcn"},
						    {"shares", shares}, {"cost", SlotCost(c, slot)}, {"price", Price(id)}});
					}
			// The mod's news (newest first), then for LCN the game's own.
			ml::Json news = ml::Json::Array();
			for (const auto& n : market::News(bawsaq))
				news.Push({{"title", n.title}, {"body", n.body}, {"stock", n.stock}, {"mood", n.mood}, {"hour", n.hour}});
			if (!bawsaq)
			{
				const ml::Json game = LcnNews();
				for (const ml::Json& n : game.Items())
					news.Push(n);
			}
			return {{"character", c}, {"cash", ml::game::Cash()}, {"tradable", tradable}, {"stocks", list}, {"portfolio", portfolio},
			    {"slots", kSlots}, {"slotsUsed", c >= 0 ? SlotsUsed(c) : 0}, {"news", news}, {"hour", market::GameHour()}};
		}

		std::atomic_bool g_saveLoading = false;
	}
}

using namespace stocks;

extern "C" __declspec(dllexport) int MLOnLoad(const MLApi* api, const MLContext* ctx)
{
	ml::Init(api, ctx);
	if (!ml::web::Available() || !ml::save::Available())
	{
		ml::LogError("this loader has no web browser or save data; the stocks mod needs them");
		return 0;
	}
	ml::web::Function("stocks.list", [](std::string exchange) { return List(exchange); });
	ml::web::Function("stocks.buy", [](int id, int shares) { return Trade(true, id, shares); });
	ml::web::Function("stocks.sell", [](int id, int shares) { return Trade(false, id, shares); });
	ml::game::On(ml::game::Event::SaveLoading, [] { g_saveLoading = true; });
	return 1;
}

extern "C" __declspec(dllexport) void MLMain()
{
	for (;;)
	{
		if (g_saveLoading.exchange(false))
			market::Reload();
		if (ml::game::CharacterIndex() >= 0 && !ml::game::IsLoadingScreen())
			market::Tick();
		ml::Wait(250);
	}
}
