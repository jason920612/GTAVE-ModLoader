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
			// if ((state - 7) >= -2u || force) g_flowState = state;
			{"SetFlowState", "8B 05 ? ? ? ? 83 C0 F9 83 F8 FE 72 05 84 D2 75 01 C3 89 0D ? ? ? ? C3",
			    [&](uintptr_t at) { p.SetFlowState = reinterpret_cast<decltype(p.SetFlowState)>(at); }},
			// Landing page, Story Mode branch: mov ecx, <state>; xor edx, edx; call SetFlowState
			{"StoryFlowState", "83 7C 24 30 05 0F 85 ? ? ? ? 83 7C 24 38 0B 0F 85 ? ? ? ?",
			    [&](uintptr_t at) {
				    const uintptr_t site = Rip(at + 7); // target of the first jne
				    const auto* code = reinterpret_cast<const uint8_t*>(site);
				    if (code[0] == 0xB9 && code[5] == 0x31 && code[6] == 0xD2 && code[7] == 0xE8 &&
				        Rip(site + 8) == reinterpret_cast<uintptr_t>(p.SetFlowState))
					    p.StoryFlowState = *reinterpret_cast<const int32_t*>(site + 1);
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
