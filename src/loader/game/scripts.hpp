#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

// Script threads, their programs and globals (research/phase0.md §22). Game thread only, between script
// updates (mods run after the game's scripts each frame).
namespace loader::game::scripts
{
	bool Install();

	// Address of script global `index` (8-byte slots), or nullptr when its block is not allocated.
	int64_t* Global(uint32_t index);

	struct ThreadInfo
	{
		int32_t id;
		std::string name;
	};
	std::vector<ThreadInfo> Threads();

	// Copies the bytecode of the thread's program. Empty when the thread or program is gone.
	std::vector<uint8_t> Code(int32_t id);

	// Index of the native with this handler in the thread's program, or -1.
	int32_t NativeIndex(int32_t id, const void* handler);

	enum class Redirect
	{
		Call,     // push `args` and a return address, then continue at `address` (a function's ENTER)
		MainFrame // drop every frame above the script's main function and continue at `address` inside it
	};
	// Makes the thread continue at `address` from its next update. The frames it leaves are abandoned: use
	// it for code that does not return (e.g. a mission's own pass routine, which ends the script).
	// Returns false (and writes `error`) when the thread or its frames cannot be used.
	bool RedirectThread(int32_t id, uint32_t address, std::span<const int64_t> args, Redirect mode, std::string& error);
}
