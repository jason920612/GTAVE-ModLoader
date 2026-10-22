#include "bootscript.hpp"

#include <filesystem>
#include <fstream>
#include <string>

#include "../game/natives.hpp"
#include "../game/script.hpp"
#include "../log.hpp"
#include "../paths.hpp"

namespace loader::debug::bootscript
{
	namespace
	{
		constexpr uint64_t kRequestScript = 0x6EB5F71AA68F2E8E;
		constexpr uint64_t kHasScriptLoaded = 0xE6CC9F3BA0FB9EF1;
		constexpr uint64_t kStartNewScript = 0xE81651AD79516E48;
		constexpr uint64_t kSetScriptAsNoLongerNeeded = 0xC90D2DCACD56184C;

		bool g_loaded = false;
		std::string g_name;
		bool g_started = false;
		game::natives::Invocation g_call;

		uint64_t Call(uint64_t hash, uint64_t arg0 = 0, uint64_t arg1 = 0, int args = 1)
		{
			g_call.Begin(hash);
			g_call.Push(arg0);
			if (args > 1)
				g_call.Push(arg1);
			return game::natives::Call(g_call) == game::natives::CallStatus::Ok ? g_call.result[0] : 0;
		}
	}

	void Tick()
	{
		if (!g_loaded)
		{
			g_loaded = true;
			std::ifstream in(paths::Get().root / L"debug_bootscript.txt");
			std::getline(in, g_name);
			if (!g_name.empty())
				log::Info("bootscript: {} starts when startup runs", g_name);
		}
		if (g_name.empty())
			return;
		game::scrThread* startup = game::script::FindThread(game::script::Joaat("startup"));
		if (!startup)
		{
			g_started = false; // again at the next session start
			return;
		}
		if (g_started)
			return;
		game::script::ScopedThread scope(startup);
		const auto name = reinterpret_cast<uint64_t>(g_name.c_str());
		Call(kRequestScript, name);
		if (!Call(kHasScriptLoaded, name))
			return;
		const uint64_t id = Call(kStartNewScript, name, 1424, 2);
		Call(kSetScriptAsNoLongerNeeded, name);
		g_started = true;
		log::Info("bootscript: started {} (thread {}) while startup runs", g_name, id);
	}
}
