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
	// asked for (the game's load scene, its collision included). Arrive: where the shift would put them now (each frame;
	// Prepare's place until then): the collision round it asked for every frame, with the camera's: a jump to where the
	// collision round the camera is not all in makes the game load everything asked for at once (a 60-150 ms frame).
	// Ready: the scene and that collision all in (else why not, in `why`). Release: no longer.
	void Prepare(const float* at, float radius);
	void Arrive(const float* at);
	bool Ready(std::string* why = nullptr);
	void Release();

	// Game thread: what round (x, y, z) does not go with the shift - rockets and grenades in flight (the game's objects),
	// explosions, fires (places of their own) -, "" none: the shift waits for them (the band is the same place in both lands).
	std::string Hazards(float x, float y, float z);

	// Game thread, each frame.
	void Tick();
}
