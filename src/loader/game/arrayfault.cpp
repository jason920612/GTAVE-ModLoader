#include "arrayfault.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../log.hpp"
#include "../paths.hpp"
#include "../pattern.hpp"
#include "pointers.hpp"
#include "script.hpp"
#include "scripts.hpp"

namespace loader::game::arrayfault
{
	namespace
	{
		// scrThread (build 0x6aa45f10), as in scripts.cpp.
		constexpr size_t kProgram = 0x10, kStackSize = 0x60, kStack = 0xB8, kName = 0x154;
		// ARRAY_U8, ARRAY_U8_LOAD, ARRAY_U8_STORE (element size u8 at +1); ARRAY_U16* (u16 at +1).
		constexpr uint8_t kArrayU8 = 52, kArrayU8Store = 54, kArrayU16 = 73, kArrayU16Store = 75;

		struct Fault
		{
			uint32_t program;
			uint32_t ip;
			uint8_t op;
			uint32_t index;
			uint32_t count;
			uint32_t elementSize;
			uintptr_t base;
			uintptr_t stack;
			uint32_t stackSize;
			bool served;
			char name[64];
		};
		constexpr uint32_t kRing = 256;
		std::array<Fault, kRing> g_ring{};
		std::atomic<uint32_t> g_written = 0;
		uint32_t g_logged = 0;
		uint64_t g_total = 0;

		// Serving (experiment U7, development only): ModLoader\debug_arrayfault.txt lines "serve <script>" name scripts
		// whose access to the element just past the end (index == count: the fourth character's element of a
		// three-element array) gets a shadow element instead of the thread being killed. A shadow starts as a copy of
		// element 0, or with the values of a "shadow <global index> <value> ..." line, and lives for the session.
		std::vector<uint32_t> g_serve;                                  // program hashes
		std::vector<std::pair<uint32_t, std::vector<int64_t>>> g_shadowInit; // global index of an array -> first values
		std::unordered_map<uintptr_t, std::vector<int64_t>> g_shadows; // array (its count slot) -> shadow element
		std::array<uintptr_t, 76> g_continuation{};                     // by opcode: the handler after its bounds check
		uintptr_t* g_jumpTo = nullptr;                                  // the thunk's target for a served access
		std::unordered_set<uint64_t> g_servedLogged;                    // program << 32 | ip, logged once

		// While watching: the first access violations (and bad instructions) are logged with the running script, to
		// tell a crash in rewritten script code from one elsewhere.
		uintptr_t g_moduleBase = 0;
		LONG CALLBACK OnException(EXCEPTION_POINTERS* e)
		{
			const DWORD code = e->ExceptionRecord->ExceptionCode;
			if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_PRIV_INSTRUCTION)
				return EXCEPTION_CONTINUE_SEARCH;
			static std::atomic<int> logged = 0;
			if (logged++ >= 20)
				return EXCEPTION_CONTINUE_SEARCH;
			const CONTEXT& c = *e->ContextRecord;
			char script[64] = "-";
			uint32_t ip = 0;
			if (g_pointers.ActiveThread && *g_pointers.ActiveThread)
			{
				const auto* t = reinterpret_cast<const uint8_t*>(*g_pointers.ActiveThread);
				strncpy_s(script, reinterpret_cast<const char*>(t + kName), _TRUNCATE);
				ip = *reinterpret_cast<const uint32_t*>(t + 0x1C);
			}
			log::Warn("exception {:#x} at rva {:#x} (address {:#x}); script {} (saved ip {}), r13-r14 {:#x}", code, c.Rip - g_moduleBase,
			    e->ExceptionRecord->NumberParameters > 1 ? e->ExceptionRecord->ExceptionInformation[1] : 0, script, ip, c.R13 - c.R14);
			return EXCEPTION_CONTINUE_SEARCH;
		}

		bool IsArray(uint8_t op)
		{
			return (op >= kArrayU8 && op <= kArrayU8Store) || (op >= kArrayU16 && op <= kArrayU16Store);
		}

		// Called by the thunk with the failing instruction, and for ARRAY opcodes the array (its count slot) and the
		// index. Script thread, inside the interpreter. Returns 0 (the game's failure code runs), or for a served access
		// the array base that makes the handler's own arithmetic land on the shadow (and sets the thunk's target).
		uintptr_t OnFault(const uint8_t* ip, int64_t* base, uint32_t index, uint64_t ipOffset)
		{
			const uint32_t n = g_written.load(std::memory_order_relaxed);
			Fault& f = g_ring[n % kRing];
			f = {};
			f.ip = static_cast<uint32_t>(ipOffset);
			f.op = *ip;
			if (IsArray(f.op))
			{
				f.index = index;
				f.count = *reinterpret_cast<const uint32_t*>(base);
				f.elementSize = f.op <= kArrayU8Store ? ip[1] : *reinterpret_cast<const uint16_t*>(ip + 1);
				f.base = reinterpret_cast<uintptr_t>(base);
			}
			if (g_pointers.ActiveThread && *g_pointers.ActiveThread)
			{
				const auto* t = reinterpret_cast<const uint8_t*>(*g_pointers.ActiveThread);
				f.program = *reinterpret_cast<const uint32_t*>(t + kProgram);
				f.stack = *reinterpret_cast<const uintptr_t*>(t + kStack);
				f.stackSize = *reinterpret_cast<const uint32_t*>(t + kStackSize);
				strncpy_s(f.name, reinterpret_cast<const char*>(t + kName), _TRUNCATE);
			}
			uintptr_t served = 0;
			if (IsArray(f.op) && index == f.count && f.elementSize && g_continuation[f.op] &&
			    std::find(g_serve.begin(), g_serve.end(), f.program) != g_serve.end())
			{
				auto& shadow = g_shadows[f.base];
				if (shadow.empty())
				{
					shadow.assign(base + 1, base + 1 + f.elementSize);
					for (const auto& [global, values] : g_shadowInit)
						if (scripts::Global(global) == base)
							std::copy_n(values.begin(), std::min<size_t>(values.size(), shadow.size()), shadow.begin());
				}
				*g_jumpTo = g_continuation[f.op];
				served = reinterpret_cast<uintptr_t>(shadow.data()) - (uintptr_t{index} * f.elementSize + 1) * 8;
				f.served = true;
			}
			g_written.store(n + 1, std::memory_order_release);
			return served;
		}

		// The handler's opcode from the code after its bounds check (address, load or store; u8 or u16 element size).
		uint8_t HandlerOp(const uint8_t* next)
		{
			const auto has = [next](std::initializer_list<uint8_t> bytes) { return std::search(next, next + 32, bytes.begin(), bytes.end()) != next + 32; };
			const bool u16 = has({0x0F, 0xB7, 0x55, 0x01}); // movzx edx, word ptr [r13+1]
			if (!u16 && !has({0x0F, 0xB6, 0x55, 0x01}))     // movzx edx, byte ptr [r13+1]
				return 0;
			const uint8_t first = u16 ? kArrayU16 : kArrayU8;
			if (has({0x48, 0x8D, 0x04, 0xC8})) // lea rax,[rax+rcx*8]: the element's address
				return first;
			if (has({0x48, 0x8B, 0x04, 0xC8})) // mov rax,[rax+rcx*8]: its value
				return first + 1;
			return first + 2;
		}

		// Where an array lives: a global index, a slot of the thread's stack (statics first, then frames), or an address.
		std::string Where(const Fault& f)
		{
			for (uint32_t block = 0; block < 64; ++block)
				if (const int64_t* first = scripts::Global(block << 18))
				{
					const auto start = reinterpret_cast<uintptr_t>(first);
					if (f.base >= start && f.base < start + (size_t{1} << 18) * 8)
						return std::format("G{}", (block << 18) + (f.base - start) / 8);
				}
			if (f.stack && f.base >= f.stack && f.base < f.stack + size_t{f.stackSize} * 8)
				return std::format("stack slot {}", (f.base - f.stack) / 8);
			return std::format("{:#x}", f.base);
		}

		const char* OpName(uint8_t op)
		{
			switch (op)
			{
			case 52: return "ARRAY_U8";
			case 53: return "ARRAY_U8_LOAD";
			case 54: return "ARRAY_U8_STORE";
			case 73: return "ARRAY_U16";
			case 74: return "ARRAY_U16_LOAD";
			case 75: return "ARRAY_U16_STORE";
			default: return "not an array op";
			}
		}

		// Executable memory within 2 GB of `target` (a rel32 jump reaches it).
		uint8_t* AllocateNear(uintptr_t target, size_t size)
		{
			SYSTEM_INFO info{};
			GetSystemInfo(&info);
			const uintptr_t granularity = info.dwAllocationGranularity;
			const uintptr_t start = target & ~(granularity - 1);
			for (uintptr_t step = granularity; step < 0x7FF00000; step += granularity)
				for (const uintptr_t at : {start - step, start + step})
					if (void* p = VirtualAlloc(reinterpret_cast<void*>(at), size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))
						return static_cast<uint8_t*>(p);
			return nullptr;
		}

		struct Emitter
		{
			uint8_t* at;
			void Bytes(std::initializer_list<uint8_t> bytes)
			{
				for (const uint8_t b : bytes)
					*at++ = b;
			}
			void U64(uint64_t v)
			{
				std::memcpy(at, &v, 8);
				at += 8;
			}
		};
	}

	void Install()
	{
		std::error_code ec;
		const auto config = paths::Get().root / L"debug_arrayfault.txt";
		if (!std::filesystem::exists(config, ec))
			return;
		std::ifstream in(config);
		for (std::string line; std::getline(in, line);)
			if (line.starts_with("serve "))
				g_serve.push_back(script::Joaat(line.substr(6).c_str()));
			else if (line.starts_with("shadow "))
			{
				std::istringstream words(line.substr(7));
				uint32_t global = 0;
				words >> global;
				std::vector<int64_t> values;
				for (int64_t v; words >> v;)
					values.push_back(v);
				g_shadowInit.emplace_back(global, std::move(values));
			}
		if (!g_pointers.TlsIndex)
		{
			log::Warn("array faults: TLS index not resolved, not watching");
			return;
		}
		// mov rax,[r15]; mov ecx,[r15-8]; cmp ecx,[rax]; jnb <failure block> (short or near): the six ARRAY handlers.
		const auto module = pattern::Module::Main();
		g_moduleBase = module.base;
		AddVectoredExceptionHandler(1, OnException);
		const auto handler = pattern::Pattern::Parse("49 8B 07 41 8B 4F F8 3B 08");
		uintptr_t block = 0;
		size_t handlers = 0;
		for (size_t offset = 0; offset < module.text.size();)
		{
			const auto hit = pattern::Find(module.text.subspan(offset), handler);
			if (!hit)
				break;
			const auto* jnb = reinterpret_cast<const uint8_t*>(*hit + 9);
			const uintptr_t target = jnb[0] == 0x73 ? *hit + 11 + static_cast<int8_t>(jnb[1])
			                         : jnb[0] == 0x0F && jnb[1] == 0x83 ? pattern::Rip(*hit + 11)
			                                                            : 0;
			if (!target || (block && target != block))
			{
				log::Warn("array faults: the ARRAY handlers jump to different blocks, not watching");
				return;
			}
			block = target;
			++handlers;
			if (const uint8_t op = HandlerOp(jnb + (jnb[0] == 0x73 ? 2 : 6)))
				g_continuation[op] = reinterpret_cast<uintptr_t>(jnb + (jnb[0] == 0x73 ? 2 : 6));
			offset = *hit + 1 - reinterpret_cast<uintptr_t>(module.text.data());
		}
		// The block starts with mov eax,[_tls_index]; mov rcx,gs:[58h] (6 + 9 bytes).
		const auto* code = reinterpret_cast<const uint8_t*>(block);
		static constexpr uint8_t kGs[] = {0x65, 0x48, 0x8B, 0x0C, 0x25, 0x58, 0x00, 0x00, 0x00};
		if (handlers != 6 || code[0] != 0x8B || code[1] != 0x05 || pattern::Rip(block + 2) != reinterpret_cast<uintptr_t>(g_pointers.TlsIndex) ||
		    std::memcmp(code + 6, kGs, sizeof(kGs)) != 0)
		{
			log::Warn("array faults: failure block not recognised ({} handlers), not watching", handlers);
			return;
		}

		uint8_t* thunk = AllocateNear(block, 0x1000);
		if (!thunk)
		{
			log::Warn("array faults: no memory near the interpreter, not watching");
			return;
		}
		Emitter e{thunk};
		// Keep everything the interpreter has live; the C++ side gets (ip, array, index, ip offset).
		e.Bytes({0x9C, 0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53}); // pushfq; rax rcx rdx r8-r11
		e.Bytes({0x55, 0x48, 0x89, 0xE5});                                                   // push rbp; mov rbp,rsp
		e.Bytes({0x48, 0x83, 0xE4, 0xF0, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00});         // and rsp,-16; sub rsp,80h
		for (uint8_t r = 0; r < 6; ++r)                                                      // movdqu [rsp+20h+16r],xmm r
			e.Bytes({0xF3, 0x0F, 0x7F, static_cast<uint8_t>(0x44 | r << 3), 0x24, static_cast<uint8_t>(0x20 + 0x10 * r)});
		e.Bytes({0x48, 0x89, 0xC2});       // mov rdx,rax   (array)
		e.Bytes({0x41, 0x89, 0xC8});       // mov r8d,ecx   (index)
		e.Bytes({0x4D, 0x89, 0xE9});       // mov r9,r13
		e.Bytes({0x4D, 0x29, 0xF1});       // sub r9,r14    (ip offset)
		e.Bytes({0x4C, 0x89, 0xE9});       // mov rcx,r13   (instruction)
		e.Bytes({0x48, 0xB8});             // mov rax,OnFault
		e.U64(reinterpret_cast<uint64_t>(&OnFault));
		e.Bytes({0xFF, 0xD0});             // call rax
		const auto restore = [&] {
			for (uint8_t r = 0; r < 6; ++r) // movdqu xmm r,[rsp+20h+16r]
				e.Bytes({0xF3, 0x0F, 0x6F, static_cast<uint8_t>(0x44 | r << 3), 0x24, static_cast<uint8_t>(0x20 + 0x10 * r)});
			e.Bytes({0x48, 0x89, 0xEC, 0x5D}); // mov rsp,rbp; pop rbp
			e.Bytes({0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58, 0x9D}); // r11-r8 rdx rcx rax; popfq
		};
		// Served: the returned base replaces rax, and the handler goes on after its bounds check.
		e.Bytes({0x48, 0x85, 0xC0, 0x0F, 0x84}); // test rax,rax; jz not served
		uint8_t* notServed = e.at;
		e.Bytes({0, 0, 0, 0});
		e.Bytes({0x48, 0x89, 0x45, 0x38}); // mov [rbp+38h],rax (the saved rax)
		restore();
		e.Bytes({0xFF, 0x25});             // jmp [g_jumpTo]
		uint8_t* jumpSlot = e.at;
		e.Bytes({0, 0, 0, 0});
		const int32_t skip = static_cast<int32_t>(e.at - (notServed + 4));
		std::memcpy(notServed, &skip, 4);
		restore();
		// The instruction the jump replaced, then back to the block.
		e.Bytes({0x48, 0xB8}); // mov rax,&_tls_index
		e.U64(reinterpret_cast<uint64_t>(g_pointers.TlsIndex));
		e.Bytes({0x8B, 0x00});                         // mov eax,[rax]
		e.Bytes({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip]
		e.U64(block + 6);
		g_jumpTo = reinterpret_cast<uintptr_t*>(e.at);
		e.U64(0);
		const int32_t slot = static_cast<int32_t>(reinterpret_cast<uint8_t*>(g_jumpTo) - (jumpSlot + 4));
		std::memcpy(jumpSlot, &slot, 4);
		FlushInstructionCache(GetCurrentProcess(), thunk, static_cast<size_t>(e.at - thunk));

		uint8_t jump[6] = {0xE9, 0, 0, 0, 0, 0x90};
		const int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(thunk) - static_cast<intptr_t>(block + 5));
		std::memcpy(jump + 1, &rel, 4);
		DWORD old = 0;
		if (!VirtualProtect(reinterpret_cast<void*>(block), sizeof(jump), PAGE_EXECUTE_READWRITE, &old))
		{
			log::Warn("array faults: cannot write the failure block, not watching");
			return;
		}
		std::memcpy(reinterpret_cast<void*>(block), jump, sizeof(jump));
		VirtualProtect(reinterpret_cast<void*>(block), sizeof(jump), old, &old);
		FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(block), sizeof(jump));
		const size_t continuations = std::count_if(g_continuation.begin(), g_continuation.end(), [](uintptr_t c) { return c != 0; });
		if (continuations != 6)
		{
			g_serve.clear();
			log::Warn("array faults: {} of 6 handlers recognised, nothing is served", continuations);
		}
		log::Info("array faults: watching the failure block at rva {:#x} (6 handlers), serving {} script(s)", block - module.base, g_serve.size());
	}

	void Tick()
	{
		const uint32_t written = g_written.load(std::memory_order_acquire);
		if (written - g_logged > kRing)
		{
			log::Warn("array faults: {} not logged (too many at once)", written - g_logged - kRing);
			g_logged = written - kRing;
		}
		for (; g_logged != written; ++g_logged)
		{
			const Fault& f = g_ring[g_logged % kRing];
			++g_total;
			if (f.served)
			{
				if (g_servedLogged.insert(uint64_t{f.program} << 32 | f.ip).second)
					log::Info("array fault served: {} ip {} {} index {} element {} slots, array {}", f.name, f.ip, OpName(f.op), f.index, f.elementSize,
					    Where(f));
				continue;
			}
			if (IsArray(f.op))
				log::Warn("array fault #{}: {} ip {} {} index {} count {} element {} slots, array {}", g_total, f.name, f.ip, OpName(f.op), f.index,
				    f.count, f.elementSize, Where(f));
			else
				log::Warn("array fault #{}: {} ip {} opcode {} (shared failure path)", g_total, f.name, f.ip, f.op);
		}
	}
}
