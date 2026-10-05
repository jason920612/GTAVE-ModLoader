#include "pointers.hpp"

#include <functional>

#include "../log.hpp"
#include "../pattern.hpp"

namespace loader::game
{
	namespace
	{
		struct Signature
		{
			const char* name;
			const char* pattern;
			std::function<void(uintptr_t)> apply;
		};
	}

	bool ResolvePointers()
	{
		using pattern::Rip;
		auto& p = g_pointers;

		const Signature signatures[] = {
			{"InitNativeTables", "EB 2A 0F 1F 40 00 48 8B 54 17 10",
			    [&](uintptr_t at) { p.InitNativeTables = reinterpret_cast<decltype(p.InitNativeTables)>(at - 0x2A); }},
			// mov esi, 0xC65D40 is the default op budget; the function starts 0xA bytes earlier.
			{"RunScriptThreads", "85 C9 BE 40 5D C6 00 0F 45 F1 0F B7 05 ? ? ? ? 66 85 C0 0F 84 ? ? ? ? 48 8B 15",
			    [&](uintptr_t at) {
				    p.RunScriptThreads = reinterpret_cast<void*>(at - 0x8);
				    p.ScriptThreadCount = reinterpret_cast<uint16_t*>(Rip(at + 0xD));
				    p.ScriptThreads = reinterpret_cast<scrThread***>(Rip(at + 0x1D));
			    }},
			// mov eax,[_tls_index]; mov rcx,gs:[58h]; mov rax,[rcx+rax*8]; mov rax,[rax+7A0h]; ret
			{"GetCurrentScriptThread", "8B 05 ? ? ? ? 65 48 8B 0C 25 58 00 00 00 48 8B 04 C1 48 8B 80 A0 07 00 00 C3",
			    [&](uintptr_t at) { p.TlsIndex = reinterpret_cast<uint32_t*>(Rip(at + 2)); }},
			// tls->currentThread = t; g_activeThread = t; tls->threadActive = 1
			{"ActiveThread", "48 89 B0 A0 07 00 00 48 89 35 ? ? ? ? C6 80 A8 07 00 00 01",
			    [&](uintptr_t at) { p.ActiveThread = reinterpret_cast<scrThread**>(Rip(at + 10)); }},
			// if (pending.length) return; pending = "source=" ...  (rdi = &pending)
			{"SetRouterLink", "56 57 53 48 83 EC 30 0F B7 1D ? ? ? ? 66 85 DB 0F 85 ? ? ? ? 48 89 CE 48 8D 3D ? ? ? ?",
			    [&](uintptr_t at) {
				    p.SetRouterLink = reinterpret_cast<decltype(p.SetRouterLink)>(at);
				    p.RouterLink = reinterpret_cast<Pointers::AtString*>(Rip(at + 0x1D));
			    }},
			// Landing page entry-point branch; a few instructions later it calls ClearRouterLink.
			{"ClearRouterLink", "83 7C 24 30 05 0F 85 ? ? ? ? 83 7C 24 38 0B 0F 85 ? ? ? ? 48 8D 4C 24 40 E8 ? ? ? ? 89 86 ? ? ? ? EB ?",
			    [&](uintptr_t at) {
				    // jmp short -> call ClearRouterLink; accept it only if it frees our pending link.
				    const uintptr_t jmp = at + 0x26;
				    const uintptr_t call = jmp + 2 + *reinterpret_cast<const int8_t*>(jmp + 1);
				    if (*reinterpret_cast<const uint8_t*>(call) != 0xE8)
					    return;
				    const uintptr_t fn = Rip(call + 1);
				    const auto* code = reinterpret_cast<const uint8_t*>(fn);
				    // sub rsp,28h; mov rcx,[rip+X] where X is the pending link's data pointer
				    if (code[4] == 0x48 && code[5] == 0x8B && code[6] == 0x0D && Rip(fn + 7) == reinterpret_cast<uintptr_t>(p.RouterLink))
					    p.ClearRouterLink = reinterpret_cast<decltype(p.ClearRouterLink)>(fn);
			    }},
		};

		const auto module = pattern::Module::Main();
		bool ok = true;
		for (const auto& sig : signatures)
		{
			const auto pat = pattern::Pattern::Parse(sig.pattern);
			const auto hit = pattern::Find(module.text, pat);
			if (!hit)
			{
				log::Error("signature {} not found", sig.name);
				ok = false;
				continue;
			}
			if (pattern::Count(module.text, pat) > 1)
				log::Warn("signature {} is ambiguous, using the first match", sig.name);
			sig.apply(*hit);
			log::Debug("signature {} at rva {:#x}", sig.name, *hit - module.base);
		}
		return ok;
	}
}
