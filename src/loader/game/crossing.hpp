#pragma once
#include <string>

// Crossing between the world builder's lands without a cut (research/phase0.md §64-65; the user: no frame over 15 ms).
// Neighbouring lands share a band of the same real-world content round their common edge (worldgen: their boxes overlap
// by it, every random choice keyed to the real world's place). With the next land registered over frames beside the one
// there (game/seamless: only its part round where the player will arrive on), the player crosses in one frame: they, their
// vehicle and the traffic and people round them moved by the offset between the two lands' places in the game's world -
// from the one land's band into the same place of the other's.
namespace loader::game::crossing
{
	// Game thread: the player - in their vehicle, if in one - and every vehicle and person within `radius` of them moved by
	// (dx, dy, dz) in one frame, as they were going (each one's velocity kept; the gameplay camera's heading and pitch round the
	// player kept), the traffic and the people moved set on their way again from where they are now (their road nodes and
	// routes were the old place's). What it did and how long it took.
	std::string Shift(float dx, float dy, float dz, float radius);

	// Game thread: the place the player will arrive at made ready ahead of the shift - everything within `radius` of it
	// asked for (the game's load scene, its collision included), and its collision asked for every frame as the game asks
	// for the camera's: a jump to where the collision round the camera is not all in makes the game load everything asked
	// for at once (a 60-80 ms frame). Ready: all in (else why not, in `why`). Release: no longer.
	void Prepare(const float* at, float radius);
	bool Ready(std::string* why = nullptr);
	void Release();

	// Game thread, each frame.
	void Tick();
}
