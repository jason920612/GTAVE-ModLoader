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

	struct Place
	{
		float x, y, z, heading;
	};
	bool Near(const Vector3& a, float x, float y, float z, float radius);
	// Screen fade: out waits until black (at most 0.6 s), in returns at once.
	void Fade(bool out);
	// Loads the interior around a position and waits for it (at most 5 s); a disabled interior is enabled until
	// RestoreInterior.
	void LoadAt(float x, float y, float z);
	void RestoreInterior();
	void MovePlayer(const Place& to);
	// The blue cylinder marking an interaction point (this frame).
	void Marker(float x, float y, float z, float size = 1.2f);

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
	// Into the garage on foot (the elevator from the apartment).
	void EnterOnFoot(int id);
	bool Inside();
}

// Apartments: entering and leaving, sleeping and saving, the wardrobe, the elevator (apartment.cpp).
namespace property::apartment
{
	// Game thread (MLMain), every frame.
	void Tick();
	void Refresh();
	// Into the apartment (the elevator from the garage).
	void Enter(int id);
	bool Inside();
	// The wardrobe page's functions (MLOnLoad).
	void RegisterWebFunctions();
}

// Research aids (research.cpp).
namespace property::research
{
	void OnLoad();
	void Tick();
}
