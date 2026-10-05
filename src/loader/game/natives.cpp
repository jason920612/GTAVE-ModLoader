#include "natives.hpp"

#include <Windows.h>

#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "../log.hpp"
#include "pointers.hpp"

namespace loader::game::natives
{
	namespace
	{
		std::unordered_map<uint64_t, uint64_t> g_crossmap;  // public -> runtime
		std::unordered_map<uint64_t, Handler> g_handlers;   // public -> handler
		bool g_resolved = false;

		// Only the fields InitNativeTables touches; the rest stays zero.
		struct FakeProgram
		{
			uint8_t pad0[0x2C];
			uint32_t nativeCount;  // 0x2C
			uint8_t pad1[0x10];
			uint64_t* entrypoints; // 0x40 hashes in, handlers out
			uint8_t pad2[0x38];
		};
		static_assert(offsetof(FakeProgram, nativeCount) == 0x2C);
		static_assert(offsetof(FakeProgram, entrypoints) == 0x40);

		uint64_t Runtime(uint64_t publicHash)
		{
			const auto it = g_crossmap.find(publicHash);
			return it != g_crossmap.end() ? it->second : publicHash;
		}

		// Kept free of C++ objects so __try can be used.
		bool SafeInvoke(Handler handler, CallContext* ctx)
		{
			__try
			{
				handler(ctx);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		void FixVectors(CallContext& ctx)
		{
			for (int i = 0; i < ctx.vectorRefCount && i < 4; ++i)
			{
				ctx.vectorRefs[i]->x = ctx.vectorSources[i].x;
				ctx.vectorRefs[i]->y = ctx.vectorSources[i].y;
				ctx.vectorRefs[i]->z = ctx.vectorSources[i].z;
			}
			ctx.vectorRefCount = 0;
		}
	}

	bool LoadCrossmap(const std::filesystem::path& file)
	{
		std::ifstream in(file);
		if (!in)
		{
			log::Warn("crossmap {} not found: only natives with unchanged hashes will work", file.string());
			return false;
		}
		std::string line;
		while (std::getline(in, line))
		{
			const auto comma = line.find(',');
			if (comma == std::string::npos)
				continue;
			try
			{
				g_crossmap[std::stoull(line.substr(0, comma), nullptr, 16)] = std::stoull(line.substr(comma + 1), nullptr, 16);
			}
			catch (const std::exception&)
			{
				log::Warn("crossmap: ignoring bad line '{}'", line);
			}
		}
		log::Info("crossmap: {} entries", g_crossmap.size());
		return true;
	}

	bool ResolveHandlers()
	{
		if (g_resolved)
			return true;

		// Slot 0 is a hash that is never registered, so it yields the game's "unknown native" handler.
		std::vector<uint64_t> publicHashes{0};
		std::vector<uint64_t> table{0};
		for (const auto& [pub, runtime] : g_crossmap)
		{
			publicHashes.push_back(pub);
			table.push_back(runtime);
		}

		FakeProgram program{};
		program.nativeCount = static_cast<uint32_t>(table.size());
		program.entrypoints = table.data();
		g_pointers.InitNativeTables(reinterpret_cast<scrProgram*>(&program));

		const auto unknown = table[0];
		size_t missing = 0;
		for (size_t i = 1; i < table.size(); ++i)
		{
			if (table[i] == unknown)
				++missing;
			else
				g_handlers[publicHashes[i]] = reinterpret_cast<Handler>(table[i]);
		}
		log::Info("natives: {} resolved, {} not registered in this build", g_handlers.size(), missing);
		if (g_crossmap.empty())
			log::Error("natives: no crossmap loaded; most natives will be unavailable to mods");
		else if (missing * 20 > g_crossmap.size())
			log::Warn("natives: over 5% of the crossmap did not resolve; it may be outdated for this game build");
		g_resolved = true;
		return true;
	}

	Handler FindHandler(uint64_t publicHash)
	{
		if (const auto it = g_handlers.find(publicHash); it != g_handlers.end())
			return it->second;
		if (g_crossmap.contains(publicHash) || !g_resolved)
			return nullptr;

		// Not in the crossmap: the hash may be unchanged in Enhanced. Resolve and cache it.
		uint64_t probe[2] = {0, publicHash};
		FakeProgram program{};
		program.nativeCount = 2;
		program.entrypoints = probe;
		g_pointers.InitNativeTables(reinterpret_cast<scrProgram*>(&program));
		const auto handler = probe[1] != probe[0] ? reinterpret_cast<Handler>(probe[1]) : nullptr;
		g_handlers[publicHash] = handler;
		return handler;
	}

	void Invocation::Begin(uint64_t publicHash)
	{
		hash = publicHash;
		handler = FindHandler(publicHash);
		overflow = false;
		ctx = {};
		ctx.returnValue = result;
		ctx.args = args;
		result[0] = result[1] = result[2] = result[3] = 0;
	}

	void Invocation::Push(uint64_t value)
	{
		if (ctx.argCount >= kMaxArgs)
		{
			overflow = true;
			return;
		}
		args[ctx.argCount++] = value;
	}

	CallStatus Call(Invocation& inv)
	{
		if (!inv.handler)
			return CallStatus::UnknownNative;
		if (inv.overflow)
			return CallStatus::TooManyArgs;
		if (!SafeInvoke(inv.handler, &inv.ctx))
			return CallStatus::Crashed;
		FixVectors(inv.ctx);
		return CallStatus::Ok;
	}
}
