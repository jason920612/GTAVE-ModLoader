#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Crossing between the world builder's lands without a cut (research/phase0.md §64-65; the user: a switch under 15 ms).
// The game registers a pack's maps in one go when it executes the map change sets: 94-138 ms on the game thread, the
// world's other threads waited for and everything asked for loaded at once. Here the same work is spread over frames: each
// data file of the change sets (a map archive: 1.3-2.8 ms) enabled in a frame of its own, the change set's own processing
// (its cache's boxes, its maps' data) run once they all are - the waits, the world's resets and the box trees left out of
// it -, the two box trees made again in frames of their own. The land's overlay (its start-up change sets: the road
// network's regions and the navmeshes) goes first; each region's and navmesh's streaming object keeps one file, set here by
// place (the land's in the band it is streamed in). Taking a land back goes the same way the other way round, the world
// never cleared.
namespace loader::game::seamless
{
	// At start: the content manager's sites checked against this build, the file registration hooked (hooking stops every
	// thread of the game for ~100 ms: never in play). Whether it is.
	void Install();
	bool Available();
	// The lands (sectors' packs): an archive of one of them is a land's.
	void SetLands(const std::vector<std::string>& lands);

	// Game thread: a land's change sets registered over the next frames (its pack mounted first), only its part in `band`
	// (min x, min y, max x, max y) to be streamed - and the land there (`there`) streamed no more in that part, its own being
	// the rest of the box; an error, or empty. Registered: once it is.
	std::string Register(const std::string& land, const std::string& there, const float* band);
	std::string Begin(const std::string& land, const std::string& there, const float* band); // (Register: the research command's name)
	bool Registered(const std::string& land);

	// Game thread, once the player is in the land registered (`to`): it streamed everywhere, the land they left (`from`)
	// nowhere, every region and navmesh given `to`'s files (the old place's once they are unloaded), then `from` taken
	// back: its files given back to the stores, its data files off, its change sets reverted - over frames.
	void Handover(const std::string& to, const std::string& from, const float* band);
	// Game thread: a land registered for a crossing that did not come taken back (the land there streamed in the band again).
	void Cancel(const std::string& land, const std::string& there, const float* band);
	// Whether any of it is under way; whether a crossing's is (a land being registered, handed over or taken back - not
	// only one gone being removed).
	bool Busy();
	bool Crossing();

	// Game thread, each frame: the next step of the work under way; the frame's time measured while recording.
	void Tick();

	// What the work did, step by step, with its times (`brief`: only where it is).
	std::string Status(bool brief = false);

	// Research: each data file's enabling sampled (debug/profile.hpp) by its calls in [from, to) - 0, not.
	std::string Profile(uintptr_t from, uintptr_t to);

	// The frames' times: recording started (true; the game's thread sampled all along, and the `others` - thread ids - too)
	// or stopped and summed up (false: the longest, the share over 15 ms and the steps that ran in the long ones, where the
	// threads were in them).
	std::string Frames(bool start, const std::vector<unsigned long>& others = {});
}
