#include "scripts.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <utility>
#include <cstring>
#include <format>
#include <map>

#include <modloader/script.hpp>

#include "../log.hpp"
#include "../pattern.hpp"
#include "pointers.hpp"

namespace loader::game::scripts
{
	namespace
	{
		// scrThread (build 0x6aa45f10). The context starts at +0x08.
		constexpr size_t kId = 0x08, kProgram = 0x10, kState = 0x18, kIp = 0x1C, kFp = 0x20, kSp = 0x24, kStackSize = 0x60,
		                 kCallDepth = 0x74, kStack = 0xB8, kName = 0x154;
		// State: 0 running, 1 waiting (natives such as WAIT set it; the interpreter stops after any native while
		// it is set), 2 and 3 killed.
		constexpr uint32_t kRunning = 0;
		// scrProgram: code page table, code size, hash (pages of 0x4000 bytes).
		constexpr size_t kCodePages = 0x10, kCodeSize = 0x1C, kNativeCount = 0x2C, kNatives = 0x40, kProgramHash = 0x58;
		constexpr uint32_t kPageSize = 0x4000;

		uint8_t* g_programs = nullptr; // program table: chain heads by (hash & 31), entries, next links
		uint64_t* g_globals = nullptr; // 64 block pointers

		template<class T>
		T& At(void* base, size_t offset)
		{
			return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
		}

		uint8_t* Thread(int32_t id)
		{
			if (!g_pointers.ScriptThreads || !*g_pointers.ScriptThreads)
				return nullptr;
			for (uint16_t i = 0; i < *g_pointers.ScriptThreadCount; ++i)
			{
				auto* t = reinterpret_cast<uint8_t*>((*g_pointers.ScriptThreads)[i]);
				if (t && At<int32_t>(t, kId) == id && id != 0)
					return t;
			}
			return nullptr;
		}

		uint8_t* Program(uint8_t* thread)
		{
			const uint32_t hash = At<uint32_t>(thread, kProgram);
			for (uint8_t i = g_programs[1 + (hash & 0x1F)]; i; i = g_programs[0x21 + i])
			{
				auto* p = At<uint8_t*>(g_programs, 0xD8 + 8 * static_cast<size_t>(i));
				if (p && At<uint32_t>(p, kProgramHash) == hash)
					return p;
			}
			return nullptr;
		}

		std::vector<uint8_t> ProgramCode(uint8_t* program)
		{
			const uint32_t size = At<uint32_t>(program, kCodeSize);
			auto** pages = At<uint8_t**>(program, kCodePages);
			std::vector<uint8_t> code(size);
			for (uint32_t at = 0; at < size; at += kPageSize)
				std::memcpy(code.data() + at, pages[at / kPageSize], std::min(kPageSize, size - at));
			return code;
		}

		struct Function
		{
			uint32_t params, frame;
		};

		// Functions by entry address (ENTER instructions found by walking the code).
		std::map<uint32_t, Function> Functions(std::span<const uint8_t> code)
		{
			std::map<uint32_t, Function> out;
			for (uint32_t at = 0; at < code.size();)
			{
				const auto in = ml::script::Decode(code, at);
				if (!in.length)
					break;
				if (in.op == ml::script::ENTER)
					out[at] = {code[at + 1], static_cast<uint32_t>(code[at + 2] | code[at + 3] << 8)};
				at += in.length;
			}
			return out;
		}
	}

	bool Install()
	{
		const auto module = pattern::Module::Main();
		// Script thread update: mov rdi,[rsi+10h] (program hash); lea rcx,[lock]; call; ...; lea rax,[program table]
		const auto at = pattern::Find(module.text,
		    pattern::Pattern::Parse("48 8B 7E 10 48 8D 0D ? ? ? ? E8 ? ? ? ? 89 F9 83 E1 1F 48 8D 05 ? ? ? ? 0F B6 4C 01 01"));
		const auto* bytes = at ? reinterpret_cast<const uint8_t*>(*at) : nullptr;
		// ... lea rdx,[globals] a little later, passed to the interpreter.
		if (!at || bytes[0x55] != 0x48 || bytes[0x56] != 0x8D || bytes[0x57] != 0x15)
		{
			log::Error("scripts: script program table not found; script access unavailable");
			return false;
		}
		g_programs = reinterpret_cast<uint8_t*>(pattern::Rip(*at + 0x18));
		g_globals = reinterpret_cast<uint64_t*>(pattern::Rip(*at + 0x58));
		log::Info("scripts: programs at +{:#x}, globals at +{:#x}", reinterpret_cast<uintptr_t>(g_programs) - module.base,
		    reinterpret_cast<uintptr_t>(g_globals) - module.base);
		return true;
	}

	int64_t* Global(uint32_t index)
	{
		if (!g_globals || (index >> 18) >= 64 || !g_globals[index >> 18])
			return nullptr;
		return reinterpret_cast<int64_t*>(g_globals[index >> 18]) + (index & 0x3FFFF);
	}

	std::vector<ThreadInfo> Threads()
	{
		std::vector<ThreadInfo> out;
		if (!g_pointers.ScriptThreads || !*g_pointers.ScriptThreads)
			return out;
		for (uint16_t i = 0; i < *g_pointers.ScriptThreadCount; ++i)
		{
			auto* t = reinterpret_cast<uint8_t*>((*g_pointers.ScriptThreads)[i]);
			if (!t || !At<int32_t>(t, kId) || At<uint32_t>(t, kState) >= 2)
				continue;
			const char* name = reinterpret_cast<const char*>(t + kName);
			out.push_back({At<int32_t>(t, kId), std::string(name, strnlen(name, 64))});
		}
		return out;
	}

	std::vector<uint8_t> Code(int32_t id)
	{
		uint8_t* thread = Thread(id);
		uint8_t* program = thread && g_programs ? Program(thread) : nullptr;
		return program ? ProgramCode(program) : std::vector<uint8_t>{};
	}

	int32_t NativeIndex(int32_t id, const void* handler)
	{
		uint8_t* thread = Thread(id);
		uint8_t* program = thread && g_programs ? Program(thread) : nullptr;
		if (!program || !handler)
			return -1;
		// InitNativeTables replaced the program's hashes with handler pointers.
		auto** natives = At<const void**>(program, kNatives);
		const uint32_t count = At<uint32_t>(program, kNativeCount);
		for (uint32_t i = 0; natives && i < count; ++i)
			if (natives[i] == handler)
				return static_cast<int32_t>(i);
		return -1;
	}

	int64_t* Static(int32_t id, uint32_t index)
	{
		uint8_t* thread = Thread(id);
		if (!thread || At<uint32_t>(thread, kState) >= 2 || index >= At<uint32_t>(thread, kStackSize))
			return nullptr;
		auto* stack = At<int64_t*>(thread, kStack);
		return stack ? stack + index : nullptr;
	}

	bool RedirectThread(int32_t id, uint32_t address, std::span<const int64_t> args, Redirect mode, std::string& error)
	{
		uint8_t* thread = Thread(id);
		if (!thread || At<uint32_t>(thread, kState) >= 2)
		{
			error = "script is not running";
			return false;
		}
		uint8_t* program = g_programs ? Program(thread) : nullptr;
		if (!program || address >= At<uint32_t>(program, kCodeSize))
		{
			error = "address outside the script";
			return false;
		}
		auto* stack = At<int64_t*>(thread, kStack);
		const uint32_t size = At<uint32_t>(thread, kStackSize);
		uint32_t fp = At<uint32_t>(thread, kFp), sp = At<uint32_t>(thread, kSp);

		if (mode == Redirect::MainFrame)
		{
			// Walk the frames back to main (the function at 0): return address at fp + params, caller's fp after it.
			const auto code = ProgramCode(program);
			const auto functions = Functions(code);
			uint32_t ip = At<uint32_t>(thread, kIp);
			for (int depth = 0;; ++depth)
			{
				auto it = functions.upper_bound(ip);
				if (it == functions.begin() || depth > 64)
				{
					error = "cannot walk the script's frames";
					return false;
				}
				--it;
				if (it->first == 0)
				{
					sp = fp + it->second.frame + 2; // empty operand stack in main's frame
					break;
				}
				if (fp + it->second.params + 1 >= size)
				{
					error = "broken frame chain";
					return false;
				}
				ip = static_cast<uint32_t>(stack[fp + it->second.params]);
				fp = static_cast<uint32_t>(stack[fp + it->second.params + 1]);
			}
		}
		else
		{
			if (sp + args.size() + 1 >= size)
			{
				error = "script stack full";
				return false;
			}
			for (const int64_t a : args)
				stack[sp++] = a;
			stack[sp++] = 0; // return address: never used, the code does not return
		}
		At<uint32_t>(thread, kFp) = fp;
		At<uint32_t>(thread, kSp) = sp;
		At<uint32_t>(thread, kIp) = address;
		At<uint8_t>(thread, kCallDepth) = 0;
		At<uint32_t>(thread, kState) = kRunning; // a pending WAIT would stop it after its first native
		log::Info("scripts: thread {} continues at {} ({})", id, address, mode == Redirect::Call ? "call" : "main frame");
		return true;
	}

	namespace
	{
		struct Override
		{
			uint32_t program = 0;
			void* original = nullptr;
			NativeOverride fn;
			bool used = false;
		};
		constexpr int kOverrideSlots = 64;
		Override g_overrides[kOverrideSlots];

		// Native handlers get the call context in rcx; the interpreter may pass more in rdx, r8 and r9, which are kept
		// for the game's handler.
		using NativeHandler = void (*)(void* context, void* a, void* b, void* c);
		thread_local void* t_extra[3];

		template<int N>
		void Thunk(void* context, void* a, void* b, void* c)
		{
			Override& o = g_overrides[N];
			void* saved[3] = {t_extra[0], t_extra[1], t_extra[2]};
			t_extra[0] = a, t_extra[1] = b, t_extra[2] = c;
			if (o.used && o.fn)
				o.fn(context, o.original);
			else if (o.original)
				reinterpret_cast<NativeHandler>(o.original)(context, a, b, c);
			t_extra[0] = saved[0], t_extra[1] = saved[1], t_extra[2] = saved[2];
		}

		template<int... N>
		constexpr auto MakeThunks(std::integer_sequence<int, N...>)
		{
			return std::array<NativeHandler, sizeof...(N)>{&Thunk<N>...};
		}
		constexpr auto kThunks = MakeThunks(std::make_integer_sequence<int, kOverrideSlots>{});

		// Replaces `from` with `to` in the program's native table.
		void Patch(uint32_t programHash, void* from, void* to)
		{
			uint8_t* program = ProgramByHash(programHash);
			if (!program)
				return;
			auto** natives = At<void**>(program, kNatives);
			const uint32_t count = At<uint32_t>(program, kNativeCount);
			for (uint32_t i = 0; natives && i < count; ++i)
				if (natives[i] == from)
					natives[i] = to;
		}
	}

	uint8_t* ProgramByHash(uint32_t hash)
	{
		if (!g_programs)
			return nullptr;
		for (uint8_t i = g_programs[1 + (hash & 0x1F)]; i; i = g_programs[0x21 + i])
		{
			auto* p = At<uint8_t*>(g_programs, 0xD8 + 8 * static_cast<size_t>(i));
			if (p && At<uint32_t>(p, kProgramHash) == hash)
				return p;
		}
		return nullptr;
	}

	int32_t AddNativeOverride(uint32_t programHash, void* original, NativeOverride fn)
	{
		for (int i = 0; i < kOverrideSlots; ++i)
			if (!g_overrides[i].used)
			{
				g_overrides[i] = {programHash, original, std::move(fn), true};
				Patch(programHash, original, reinterpret_cast<void*>(kThunks[i]));
				return i + 1;
			}
		return 0;
	}

	void RemoveNativeOverride(int32_t id)
	{
		if (id < 1 || id > kOverrideSlots || !g_overrides[id - 1].used)
			return;
		Override& o = g_overrides[id - 1];
		Patch(o.program, reinterpret_cast<void*>(kThunks[id - 1]), o.original);
		o.used = false; // the thunk keeps calling the original if the game still holds it
		o.fn = nullptr;
	}

	void* OverrideOriginal(int32_t id)
	{
		return id >= 1 && id <= kOverrideSlots ? g_overrides[id - 1].original : nullptr;
	}

	void CallOriginal(int32_t id, void* context)
	{
		if (void* original = OverrideOriginal(id))
			reinterpret_cast<NativeHandler>(original)(context, t_extra[0], t_extra[1], t_extra[2]);
	}

	void ApplyNativeOverrides(bool enabled)
	{
		for (int i = 0; i < kOverrideSlots; ++i)
			if (g_overrides[i].used)
			{
				if (enabled)
					Patch(g_overrides[i].program, g_overrides[i].original, reinterpret_cast<void*>(kThunks[i]));
				else
					Patch(g_overrides[i].program, reinterpret_cast<void*>(kThunks[i]), g_overrides[i].original);
			}
	}
}
