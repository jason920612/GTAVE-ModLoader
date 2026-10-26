#pragma once

// The world editor (research/phase0.md §61.19): roads drawn the way city builders draw them, over the world worldgen
// built. F7 (rebindable) switches it on: a camera above the land (W A S D move, Q E turn, R F tilt, wheel zoom), the
// mouse cursor, a tool bar. Straight roads: click where it starts, click where it ends (and on: the next one starts
// there); curved: start, a point it bends towards, end; delete: click a road (its piece between two junctions goes).
// Ends snap to roads; crossings become junctions; the road types are the city-builder presets of roadedit::Types().
// While a road is drawn its finished surfaces - junctions, kerbs, markings - show in blue (the same roads::Geometry
// worldgen builds with). Every change goes into the world's edits.json at once; "Apply" runs the rebuild command
// (config.json "rebuild": worldgen build + pack into dlc.rpf.new, which the loader puts in place at the next start).
//   config.json: {"roads": "<roads.bin>", "edits": "<edits.json>", "rebuild": "<rebuild.cmd>"}
namespace worldedit
{
	// MLOnLoad: settings, the hotkey, the web functions a tester (or a page) can drive it with.
	void Init();
	// MLMain, every frame.
	void Frame();
}
