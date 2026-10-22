#pragma once

// The script VM's array bounds failure (research/char4-design.md §3.3). The six ARRAY handlers (ARRAY_U8, _LOAD,
// _STORE and the U16 forms) compare the index with the array's count and jump to one shared block, which records the
// instruction and kills the thread. For the fourth character that block becomes the place where index 3 is served
// from a shadow element. For now it only watches (experiment U1): each failure is recorded (script, instruction,
// opcode, index, count, array) and the game's own code runs unchanged. Enabled by ModLoader\debug_arrayfault.txt.
namespace loader::game::arrayfault
{
	// Puts the watch on the failure block when requested. After ResolvePointers and scripts::Install.
	void Install();
	// Writes the failures recorded since the last call to the log. Game thread, once per frame.
	void Tick();
}
