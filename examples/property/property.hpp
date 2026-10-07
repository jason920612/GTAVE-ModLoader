#pragma once
// Shared parts of the property mod: the game's property table, the player, ownership.
#include <cstdint>
#include <set>
#include <string>

#include <modloader/natives.hpp>

namespace property
{
	constexpr int kLastProperty = 85; // 1..85: apartments, houses and garages (then yachts, offices, ...)

	// The property's entry in the game's property table (Global 1312440, 1951 slots each), or nullptr.
	int64_t* Entry(int id);
	// 6 / 5 / 4 = high / medium / low end apartment, 3 / 2 / 1 = 10 / 6 / 2 car garage; 0 = not for sale.
	int TierOf(int id);
	// Cars the property's garage holds (2, 6 or 10).
	int GarageSize(int id);
	// Slot `slot` of the entry as a position (three floats); zero when unset.
	Vector3 EntryPosition(int id, int slot);
	float EntryFloat(int id, int slot);
	// The property's name in the game's language.
	std::string Name(int id);
	// Text of a game label in the game's language ("" when there is none).
	std::string Text(const char* label);

	// Story character of the player (0 Michael, 1 Franklin, 2 Trevor), or -1.
	int Character();
	// Properties the character owns (with purchases not saved yet).
	std::set<int>& Owned(int character);

	// On-screen help text for this frame.
	void Help(const std::string& text);
	// A notification above the minimap.
	void Notify(const std::string& text);
}

// Garages: entering and leaving, storing and taking out vehicles (garage.cpp).
namespace property::garage
{
	// Game thread (MLMain), every frame.
	void Tick();
	// The game saved: write the stored vehicles. A save was loaded: back to what was saved.
	void Commit();
	void Drop();
	bool Changed();
	// The ownership or the character changed: blips are rebuilt.
	void Refresh();
}

// Research aids (research.cpp).
namespace property::research
{
	void OnLoad();
	void Tick();
}
