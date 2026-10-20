// Police: a fourth character, a police officer with a life of their own off duty (design and research: research/phase0.md §53).
// Stage 1: the officer on the character wheel, switching both ways.
#include "police.hpp"

ML_MOD_INFO("Police", "0.0.1", "ModLoader", "Play as a police officer (in development)")

extern "C" __declspec(dllexport) int MLOnLoad(const MLApi* api, const MLContext* ctx)
{
	ml::Init(api, ctx);
	return 1;
}

extern "C" __declspec(dllexport) void MLMain()
{
	for (;;)
	{
		ml::Wait(0);
		police::wheel::Tick();
	}
}
