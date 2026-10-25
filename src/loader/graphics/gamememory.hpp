#pragma once

// The GPU memory the game is told it may use while UE5 draws the world (research/phase0.md §60.26). The game asks DXGI
// for its budget every frame (one place, through its device's adapter) and streams its models and textures up to it:
// with the card to itself Windows gives it 5-7 GB of an 8 GB card, and it keeps 3.8-4 GB - for a world nobody sees
// (UE5's is drawn over it; of its picture only the peds and vehicles are kept). Told less, it lets its farther things go
// and streams again what it needs (its peds', vehicles', and their surroundings for their shadows and reflections): the
// memory stays with the renderer, which shares the card.
namespace loader::graphics::gamememory
{
	// Hooks DXGI's QueryVideoMemoryInfo (the class's own function, as particles.cpp hooks D3D12's), from the start.
	void Start();
}
