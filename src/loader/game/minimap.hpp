#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The game's map of the world builder's new land (research/phase0.md §61.26). A land's pack brings it in the game's own
// style (worldgen's minimap.cpp) and says what it has in land.json ("map": "name"; "radar": [column, row] of each supertile
// on the game's radar lattice, its file minimap_<column + 1000>_<row + 1000>.ydd; "pictures": [row, column] of each
// square on the pause map's picture lattice, minimap_<name>_<row>_<column>.ytd and minimap_sea_<name>_...).
//  - The radar: the game streams and draws a fixed 8 x 9 array of supertiles (minimap_0_0..7_8). The new land's are
//    entries of the same kind beside it, wanted for the same view (the game's own selection, unclamped), streamed and
//    drawn by the game's own functions with the game's.
//  - The pause map: its pictures are a grid minimap.ymt sizes (2 x 3 squares of 4500 m); at the session's start it is
//    widened to take in the new land's squares, each square's dictionary then named after whoever has a picture of it:
//    a land, the game, or a land's open sea.
namespace loader::game::minimap
{
	// Reads the lands of the enabled packs and hooks the radar and the pause map (each patched call checked against the
	// build first: on any difference nothing is patched). Once the game is unpacked.
	bool Install();

	// The game thread: the lands' supertiles let go (their dictionaries released, none wanted) until Resume - a sector
	// switch takes the old land's files away (game/sectors; a supertile still holding one of its dictionaries then found it
	// "loaded" with no dictionary there, 0x56FB80).
	void Suspend();
	void Resume();

	// A land registered for a crossing (game/seamless) has its radar supertiles registered under names of its own
	// (minimap_<its number + column>_<row>): the land there keeps the pack's names, and the radar streams the arriving
	// land's supertiles before the crossing and draws them from the frame the player is in it - no blank radar while the
	// names would change hands.
	// OwnName: the name a land's supertile file `name` (minimap_<column>_<row>.ydd, any folder) is registered under, "" to
	// keep the pack's; OwnTile: it was (its slot then found by that name, as the radar's stream finds one).
	std::string OwnName(const std::string& land, std::string_view name);
	void OwnTile(const std::string& land, std::string_view name);
	// The game thread: the land arriving - its supertiles streamed for where the player arrives, (dx, dy) metres from
	// where they are - until it is the player's land or let go.
	void Arriving(const std::string& land, float dx, float dy);
	// The game thread, a step a frame while a land is taken back: its own supertiles let go (the render thread's last use
	// waited for); true once they are and the land forgotten.
	bool LetTilesGo(const std::string& land);

	// The game thread, from a sector switch's start until Resume: the lands being put away named no more on the pause
	// map's squares (a square looking a picture up finds the next land's, or none).
	void Going(const std::vector<std::string>& lands);

	// The game thread, once a frame: the pause map's squares holding any of these texture dictionaries (their slots in the
	// texture store) taken a step on toward letting them go, as the game's own let them go when not wanted (the render
	// thread's last use waited for); true once none holds one.
	bool LetGo(const std::vector<int32_t>& slots);
}
