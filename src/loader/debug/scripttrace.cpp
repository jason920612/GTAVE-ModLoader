#include "scripttrace.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../game/natives.hpp"
#include "../game/pointers.hpp"
#include "../game/script.hpp"
#include "../game/scripts.hpp"
#include "../log.hpp"
#include "../paths.hpp"

namespace loader::debug::scripttrace
{
	namespace
	{
		// The interpreter may pass more than the call context: rdx, r8 and r9 are handed on untouched.
		using Handler = void (*)(void* context, void* a, void* b, void* c);

		struct Program
		{
			uint32_t hash = 0;
			uint8_t* patched = nullptr;           // program object whose table holds our stubs
			std::vector<Handler> originals;       // by native index
			uint8_t* stubs = nullptr;             // executable stubs, one per index
			uint32_t stubCount = 0;
			uint32_t marker = 0; // an entry that holds our stub, to see whether the table is still ours
		};
		std::vector<Program> g_programs;
		std::vector<uint64_t> g_skip; // natives (public hashes) left alone: "-<hash>" lines
		std::vector<uint64_t> g_only; // when not empty, only these natives are traced: "+<hash>" lines
		bool g_loaded = false;

		struct Entry
		{
			uint32_t ip;
			uint16_t index;
			uint16_t program;
			uint64_t result; // first return slot after the call
		};
		constexpr size_t kEntries = 1 << 20;
		std::vector<Entry> g_ring(kEntries);
		size_t g_next = 0;
		uint64_t g_total = 0;

		// stub: saves rcx/rdx/r8/r9, calls Record(id), restores them and jumps to the game's handler, so the handler runs
		// with exactly the stack and return address the interpreter gave it (protected handlers look at their caller).
		constexpr size_t kStubSize = 64;

		void Record(uint64_t id)
		{
			const uint16_t program = static_cast<uint16_t>(id >> 16), index = static_cast<uint16_t>(id);
			uint32_t ip = 0;
			if (auto** active = game::g_pointers.ActiveThread; active && *active)
				std::memcpy(&ip, reinterpret_cast<uint8_t*>(*active) + 0x1C, 4); // the NATIVE instruction
			g_ring[g_next] = {ip, index, program, 0};
			g_next = (g_next + 1) % kEntries;
			++g_total;
		}

		void Patch(Program& p, uint16_t number)
		{
			uint8_t* program = game::scripts::ProgramByHash(p.hash);
			if (!program)
			{
				p.patched = nullptr;
				return;
			}
			auto** natives = *reinterpret_cast<Handler**>(program + 0x40);
			const uint32_t count = *reinterpret_cast<uint32_t*>(program + 0x2C);
			if (!natives || !count)
				return;
			if (p.patched == program && p.marker < count && natives[p.marker] == reinterpret_cast<Handler>(p.stubs + kStubSize * p.marker))
				return; // still ours
			p.originals.assign(natives, natives + count);
			if (p.stubCount < count)
			{
				p.stubs = static_cast<uint8_t*>(VirtualAlloc(nullptr, kStubSize * count, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
				p.stubCount = count;
			}
			// WAIT is left alone: the interpreter recognises it by its handler (a replaced WAIT never yields).
			static const auto skip = [] {
				std::vector<Handler> handlers{reinterpret_cast<Handler>(game::natives::FindHandler(0x4EDE34FBADD967A6ULL))};
				for (const uint64_t h : g_skip)
					handlers.push_back(reinterpret_cast<Handler>(game::natives::FindHandler(h)));
				return handlers;
			}();
			static const auto only = [] {
				std::vector<Handler> handlers;
				for (const uint64_t h : g_only)
					handlers.push_back(reinterpret_cast<Handler>(game::natives::FindHandler(h)));
				return handlers;
			}();
			for (uint32_t i = 0; i < count; ++i)
			{
				if (std::find(skip.begin(), skip.end(), natives[i]) != skip.end())
					continue;
				if (!only.empty() && std::find(only.begin(), only.end(), natives[i]) == only.end())
					continue;
				uint8_t* s = p.stubs + kStubSize * i;
				const uint64_t id = (uint64_t(number) << 16) | i;
				const uint64_t record = reinterpret_cast<uint64_t>(&Record);
				const uint64_t original = reinterpret_cast<uint64_t>(p.originals[i]);
				const uint8_t code[] = {
				    0x51, 0x52, 0x41, 0x50, 0x41, 0x51,             // push rcx, rdx, r8, r9
				    0x48, 0x83, 0xEC, 0x28,                         // sub rsp, 28h (keeps the call aligned)
				    0x48, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0,             // mov rcx, id
				    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,             // mov rax, Record
				    0xFF, 0xD0,                                     // call rax
				    0x48, 0x83, 0xC4, 0x28,                         // add rsp, 28h
				    0x41, 0x59, 0x41, 0x58, 0x5A, 0x59,             // pop r9, r8, rdx, rcx
				    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,             // mov rax, original
				    0xFF, 0xE0,                                     // jmp rax
				};
				static_assert(sizeof(code) <= kStubSize);
				std::memcpy(s, code, sizeof(code));
				std::memcpy(s + 12, &id, 8);
				std::memcpy(s + 22, &record, 8);
				std::memcpy(s + 44, &original, 8);
				natives[i] = reinterpret_cast<Handler>(s);
				p.marker = i;
			}
			p.patched = program;
			if (const auto probe = reinterpret_cast<uintptr_t>(game::natives::FindHandler(0x85F01B8D5B90570EULL)))
				log::Info("scripttrace: HAS_SCALEFORM_MOVIE_LOADED handler at +{:#x}", probe - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
			log::Info("scripttrace: tracing {} natives of program {:08X}", count, p.hash);
		}
	}

	bool Load()
	{
		g_loaded = true;
		std::ifstream in(paths::Get().root / L"debug_trace.txt");
		for (std::string line; std::getline(in, line);)
			if (!line.empty() && line[0] == '-')
				g_skip.push_back(std::stoull(line.substr(1), nullptr, 16));
			else if (!line.empty() && line[0] == '+')
				g_only.push_back(std::stoull(line.substr(1), nullptr, 16));
			else if (!line.empty() && line[0] != '#')
				g_programs.push_back({game::script::Joaat(line.c_str())});
		if (!g_programs.empty())
			log::Info("scripttrace: {} script(s) to trace", g_programs.size());
		return !g_programs.empty();
	}

	void Tick()
	{
		if (!g_loaded)
			Load();
		if (g_programs.empty())
			return;
		for (size_t i = 0; i < g_programs.size(); ++i)
			Patch(g_programs[i], static_cast<uint16_t>(i));
		std::error_code ec;
		if (std::filesystem::remove(paths::Get().root / L"trace_dump.txt", ec))
		{
			std::ofstream out(paths::Get().root / L"trace.log", std::ios::trunc);
			const size_t n = std::min<uint64_t>(g_total, kEntries);
			for (size_t k = 0; k < n; ++k)
			{
				const Entry& e = g_ring[(g_next + kEntries - n + k) % kEntries];
				out << e.program << ' ' << e.ip << ' ' << e.index << ' ' << static_cast<int64_t>(e.result) << '\n';
			}
			log::Info("scripttrace: wrote {} calls to trace.log", n);
			g_total = 0;
		}
	}
}
