#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

// A world builder land's map files in the game's streaming stores (research/phase0.md §63.12-63.14). A map group's revert
// only switches its archives off: their files stay registered - a slot in their store each, a streaming object - and each
// land gone to in a session added its own (20000 drawables, 7000 collision files, 4000 dictionaries, 3000 maps a big
// one): the stores filled up. On a switch the land put away gives its files back, as the game's own code does it: each
// object unloaded by the streaming engine (+0xCE4CD0, once nothing holds it), its slot taken out of its store (the store's
// own RemoveSlot, vtable +0x50: its name, its pool entry - the collision store's its box too), its streaming entry cleared
// (else the next file given that slot is taken for one already there and refused), a map's two boxes and flags in the map
// store's box streamer reset as its RemoveSlot does not. When the land comes again its change set registers its files
// anew. In the order they hold each other by slot: maps and archetype files first, then the models, then their texture
// dictionaries - none of a kind unloaded while a kind that holds it has any loaded (the archetypes of a map still in the
// world, the models of its archetypes: their stores count no hold of the entities on them); a kind whose objects cannot
// all be unloaded keeps its slots, and so do the kinds it holds. A name another land's file has now (the radar's
// supertiles: the same names in every land) is that land's: left as it is.
namespace loader::game::landfiles
{
	struct Progress
	{
		size_t released = 0; // (this call)
		size_t loaded = 0;   // (still loaded: not released yet)
		size_t kept = 0;     // (left registered for good: held, or held by a kind that is)
		bool done = false;
	};

	// Game thread, once a frame while it is not done: a land's files (its pack's map archives - the overlay's road network
	// and navmeshes are the game's own regions and squares, taken over by the next land's) unloaded, then given back once
	// all of a kind are; `last`: the last try - what is still loaded is kept.
	Progress Release(const std::string& land, const std::filesystem::path& pack, bool last);
	// The same over as many frames as it takes, `budget` ms a call (the land's 35000 files gone through in one call took a
	// frame of their own): each call goes on from where the last one stopped.
	Progress ReleaseSome(const std::string& land, const std::filesystem::path& pack, double budget, bool last);

	// Game thread, once a frame until it is done: a land's maps switched off (SwitchArea) whose boxes meet `box` and that
	// are still loaded unloaded as the game's own switch of a map group off does it (0x931770: a loaded map holds a hold of
	// kind 2 on itself, let go before the streaming engine unloads it and taken again if it cannot). With only the flag set
	// the box streamer never let go of the ones it had: a land gone stayed in the world, its entities drawn with the models
	// its files' release had taken from under them (+0x315532). The maps with a LOD parent first. `budget` ms a call.
	Progress UnloadMaps(const std::string& land, const std::filesystem::path& pack, const float* box, double budget);

	// Game thread: drawable dictionaries (by name hash - the radar's supertiles, the same names in every land) given another
	// land's files (`handles`, the files that land's registration brought and the game did not take, the names had): each
	// one loaded unloaded first once nothing holds it (the radar let go of them, minimap::Suspend). How many still wait; how
	// many have a file not theirs.
	size_t GiveDictionaries(const std::unordered_map<uint32_t, uint32_t>& handles);
	size_t DictionariesToGive(const std::unordered_map<uint32_t, uint32_t>& handles);

	// Game thread, once a land's change sets are taken back: its map caches (cacheLoaderData_DLC/<land>_<change set>_cache_y.dat,
	// one a set: where each map and collision file lies) taken off the game's list of caches read (+0x42F9BB8: the hashes of
	// their paths - one on it is not read again; the land's next registration had none of its maps' boxes, and the game
	// loaded all 3000 of them in one frame to find them, 0x935A30: 3 s). How many were on it.
	size_t ForgetCaches(const std::string& land, const std::vector<uint32_t>& sets);

	// A land's file names read on a thread of their own ahead of the frames that want them (they are read from its pack:
	// the archives' tables); whether they are in.
	void Prefetch(const std::string& land, const std::filesystem::path& pack);
	bool NamesReady(const std::string& land);

	// Game thread, once a frame before the land's maps are put away, until it is done: its pause map pictures
	// (minimap_*.ytd - the radar draws them too, zoomed out) let go while their archive is still there: the pause map's
	// squares holding them released (minimap::LetGo), then each unloaded by the streaming engine. (Held as the maps were
	// reverted, a square let go of its dictionary after the revert had cleared the file's handle with its archive: taking it
	// for one the game made itself, it deleted the dictionary outright, and the streaming engine kept it as loaded with
	// no object - the next RemoveObject on it, the archive's removal on the land's next going, broke into the int3 at
	// +0xCE4FB2.)
	bool LetPicturesGo(const std::string& land, const std::filesystem::path& pack, std::string* held = nullptr); // (held: what holds them, said)

	// Whether the game's code is where this build has it (else Release does nothing and says so).
	bool Supported();

	// Game thread, once the land's change set has registered its files again: the collision files' boxes and flags it had
	// when they were given back put back where the registration left them empty (it does not set them again: the box
	// streamer never asked for one, and the land had no ground). How many.
	size_t PutBoxesBack(const std::string& land);

	// Game thread: a land's maps and collision files whose boxes meet `box` (min x, min y, max x, max y) switched on or off
	// for the box streamers' searches (their flags' bit 2, as a map group's revert sets it; research/phase0.md §64: two lands
	// in the one box, each streamed only in its own part). The box trees must be made again after (the searches go through
	// them, the flags read for what they find). How many were switched.
	size_t SwitchArea(const std::string& land, const std::filesystem::path& pack, const float* box, bool on);

	// Development: the land's collision and map files' boxes in their stores (how many empty).
	std::string Boxes(const std::string& land, const std::filesystem::path& pack);

	// A store's slot as words: its kind, its name (the land's file it is, when it is one), its streaming entry and how many
	// hold it. `module`: the store (the streaming module).
	std::string Describe(const void* module, int32_t slot);

	// Game thread: the stores' objects the streaming engine has as loaded but their store has no object for - the state
	// RemoveObject breaks into an int3 on (+0xCE4FB2) - logged, each once, with `when`.
	void Audit(const std::string& when);
}
