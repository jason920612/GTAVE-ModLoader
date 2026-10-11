#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <modloader/game.hpp>
#include <modloader/json.hpp>

namespace social
{
	// Something the player did (main.cpp): chase, buy, hospital, arrest, stock.
	struct Event
	{
		int64_t t;
		std::string type;
		int c; // character, -1 none
		std::string a, b;
		int64_t n;
	};
	// Stable id of an event (also used by the pages).
	inline std::string EventKey(const Event& e) { return std::format("{}.{}.{}", e.t, e.type, e.c); }

	int64_t GameMinute();
	const char* Weather();
	const std::vector<Event>& Events();

	// Posts and replies written by the loader's local text model (gen.cpp).
	namespace gen
	{
		void Reload();                 // a save was loaded: drop the queue, read what the save has
		void Tick();                   // queue background work and send the next request (MLMain)
		void OnEvent(const Event& e);  // a new event: posts about it
		// Pages: posts that should get replies [{ key, who, text, site, pool: [ids] }]; the player's own post.
		void Want(const ml::Json& list);
		ml::Json Post(const std::string& site, const std::string& text);
		ml::Json ToJson();
	}
}
