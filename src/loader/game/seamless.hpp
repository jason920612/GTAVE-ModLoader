#pragma once
#include <string>

// Crossing between the world builder's lands without a cut (research/phase0.md §64; the user: a switch under 15 ms).
// The game registers a pack's maps in one go when it executes the map change sets: 94-138 ms on the game thread, the
// world's other threads waited for and everything asked for loaded at once. Here the same work is spread over frames: each
// data file of the change sets (a map archive: 1.3-2.8 ms) enabled in a frame of its own, the change set's own map
// processing (its cache's boxes, its maps' data) run once they all are - the waits, the world's resets and the box trees
// left out of it -, the two box trees made again in frames of their own.
namespace loader::game::seamless
{
	// Game thread: a land's map change sets registered over the next frames (its pack mounted first), only its part in
	// `band` (min x, min y, max x, max y) to be streamed - and the land there (`there`) streamed no more in that part, its
	// own being the rest of the box; an error, or empty.
	std::string Begin(const std::string& land, const std::string& there, const float* band);

	// Game thread, each frame: the next step of the registration under way; the frame's time measured while recording.
	void Tick();

	// What the registration did, step by step, with its times.
	std::string Status();

	// The frames' times: recording started (true) or stopped and summed up (false: the longest, the share over 15 ms and
	// the steps that ran in the long ones).
	std::string Frames(bool start);
}
