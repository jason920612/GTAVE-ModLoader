#pragma once

// The world editor (research/phase0.md §61.19, §61.24): the land worldgen built, edited the way city builders and paint
// programs do. F7 (rebindable) switches it on: a camera above the land (W A S D move, Q E turn, R F tilt, wheel zoom),
// the mouse cursor, two tool bars (the tools; the current tool's choices).
//   Roads: straight (click where it starts, click where it ends - and on: the next one starts there), curved (start, a
//   point it bends towards, end), delete (a road's piece between two junctions). Ends snap to roads; crossings become
//   junctions; the road types are the city-builder presets of roadedit::Types(). While a road is drawn its finished
//   surfaces - junctions, kerbs, markings - show in blue (the same roads::Geometry worldgen builds with).
//   Buildings: put one up (click a corner, the next along its front, then how deep; [ ] its height a storey at a time;
//   1-5 what it is: house, apartments, shops, hotel, offices) or pull down those in a rectangle (two corners).
//   Ground: paint park, grass, beach, paving, built-up land or water (water makes canals and ponds) - hold the button
//   down and draw; [ ] the brush's size. Terrain: raise, lower or flatten the same way. Contours: lines a metre apart
//   over the ground in view. Climate: the land's weather (the loader's game/sea: tropical, dry, temperate or the game's).
// Every change goes into the world's edits.json at once (Ctrl+Z undoes this session's, newest first); "Apply" runs the
// rebuild command (config.json "rebuild": worldgen build + pack into dlc.rpf.new, which the loader puts in place at the
// next start).
//   config.json: {"roads": "<roads.bin>", "edits": "<edits.json>", "rebuild": "<rebuild.cmd>"}
namespace worldedit
{
	// MLOnLoad: settings, the hotkey, the web functions a tester (or a page) can drive it with.
	void Init();
	// MLMain, every frame.
	void Frame();
}
