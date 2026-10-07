#pragma once
// Stock market mod: the LCN and BAWSAQ websites (research/phase0.md §35).
#include <cstdint>
#include <string>
#include <vector>

#include <modloader/game.hpp>

namespace stocks
{
	// ---- the game's stock table (Global 57379: 80 entries of 36 slots) ----------------------------
	constexpr int kStockCount = 80;
	ml::Global Stock(int id);                // the entry
	bool IsBawsaq(int id);                   // +8: 0 LCN, 1 BAWSAQ
	float Price(int id);                     // +9
	std::vector<float> History(int id);      // +15..+30, oldest first (16 points)
	std::string CompanyName(int id);         // +0 label, in the game's language
	std::string Ticker(int id);              // +4 label
	// Writes a price with its history (BAWSAQ, which the game leaves empty offline).
	void SetQuote(int id, float price, const std::vector<float>& history);

	// Text of a game label ("" when there is none).
	std::string Text(const char* label);
}

// Price movement for both exchanges, with news events (market.cpp).
namespace stocks::market
{
	constexpr int kMarketLcn = -2, kMarketBawsaq = -1; // NewsItem::stock for news about a whole exchange
	struct NewsItem
	{
		int64_t hour; // game hours since the year 2000
		int stock;    // a company, or kMarketLcn / kMarketBawsaq
		std::string title, body;
		int mood;     // +1 good, -1 bad, 0 neutral
	};
	// MLMain: advances the prices with the game clock and keeps the stock table filled.
	void Tick();
	// A save is being loaded: the state is read again.
	void Reload();
	// The mod's news of one exchange, newest first.
	std::vector<NewsItem> News(bool bawsaq);
	// Game hours since the year 2000 (the in-game clock).
	int64_t GameHour();
}
