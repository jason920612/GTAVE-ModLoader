#pragma once

#include <string>

// The GPU memory the game is told it may use (research/phase0.md §60.26, §67). The game asks DXGI for its budget every
// frame (one place, through its device's adapter) and streams its models and textures up to it: with the card to itself
// Windows gives it 5-7 GB of an 8 GB card, and it keeps what it is given. Told less, it lets its farther things go and
// streams again what it needs.
// - While UE5 draws the world (ue5GameMemory): the game's own world is not seen, the memory stays with the renderer.
// - Always (gameVideoMemory): a share of the card, the rest left to the system and other programs.
namespace loader::graphics::gamememory
{
	// Hooks DXGI's QueryVideoMemoryInfo (the class's own function, as particles.cpp hooks D3D12's), from the start.
	void Start();
	// UE5 draws the world from now on (its cap applies too).
	void SetUe5(bool on);
	// gameVideoMemory for the rest of this run (percent of the card's; 0: what Windows gives the game).
	void SetShare(int percent);
	// What Windows gives the game, what the game is told, and what it uses (the card's own memory, and the system's it
	// borrows), for the debug pipe.
	std::string Report();
}
