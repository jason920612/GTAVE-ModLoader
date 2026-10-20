#pragma once
// Police: a fourth character, a police officer with a life of their own off duty (design and research: research/phase0.md §53).

#define NOMINMAX
#include <Windows.h>

#include <modloader/game.hpp>
#include <modloader/natives.hpp>

#include <string>

namespace police
{
	struct Place
	{
		float x, y, z, heading;
	};

	// The character wheel's slots (selector script layout).
	enum Slot : int
	{
		kSlotFranklin = 0, // up
		kSlotTrevor = 1,   // right
		kSlotOfficer = 2,  // down: GTA Online's slot, empty in story mode
		kSlotMichael = 3,  // left
	};
}

namespace police::officer
{
	// The officer's ped (exists while the mod has placed them in the world), 0 = none.
	Ped Ped();
	bool IsPlayer();
	// Headshot texture dictionary of the officer, or nullptr while it is not ready.
	const char* Headshot();
	// Places the officer in the world where the hour puts them, if needed, and prepares the headshot.
	void Ensure();
	// Removes the officer from the world while nobody plays them.
	void Drop();
	// Makes the officer anew from their look (after the sex changed), keeping the player in control of them.
	void Rebuild();
	bool InUniform();
	// The wheel's picture of the officer anew (after their look changed).
	void RefreshHeadshot();
	// Every frame: ends the switch scene's activity once the player moves.
	void Tick();
	// Switches from the current protagonist to the officer, with the game's sky camera.
	void SwitchTo();
	// Switches from the officer back to a protagonist (the wheel's slot).
	void SwitchFrom(int slot);
	// After a load: the officer takes over from the loaded protagonist at `at`, without a switch camera.
	void Restore(Vector3 at, float heading);
	// The world was reset (a save is loading): the mod's peds are gone.
	void Forget();
	// Saves the game while the officer is played: the protagonist left behind is the player for the moment of the save
	// (the game only saves with a protagonist), then the officer again. False when it did not save.
	bool SaveForOfficer();
	// The game wrote a save (ml::game::Event::Saved).
	void NoteSaved();
}

namespace police::persist
{
	// Where the officer was last (false when the save has no officer yet).
	bool Where(Vector3& at, float& heading);
	// Records the officer's place now; `active` = the officer is the player.
	void Update(bool active);
	// A save is loading (ml::game::Event::SaveLoading).
	void Loading();
	// Every frame: keeps the record current and puts the officer back in control after a load.
	void Tick();
}

namespace police::wheel
{
	// Every frame: adds the officer to the game's wheel while a protagonist is played, or draws the wheel itself while
	// the officer is played (the game's wheel only works for the three protagonists).
	void Tick();
}

namespace police::life
{
	// Where the hour puts the officer when nobody plays them (game clock: 8-17 on duty, 17-23 out, 23-8 home).
	enum class Scene
	{
		OnDuty,
		Out,
		Home,
	};
	Scene Now();
	// A place for the scene and what the officer is busy with there (a scenario name).
	Place Pick(Scene scene, const char*& scenario);
	// Loads the interior and the world around a place (blocks until ready, a few seconds at most).
	void Load(const Place& at);
	bool InApartment(const Vector3& at);
	// Every frame while the officer is played: the apartment's doors and bed.
	void Tick();
}

namespace police::look
{
	// The officer's model (freemode male or female).
	Hash Model();
	std::string Name();
	int Department();
	// Head, hair and clothes (the uniform or own clothes) on a ped of Model().
	void Apply(::Ped ped, bool uniform);
	// True when the save has a look (the creator was used).
	bool Exists();
	// After a save loaded (or the game started): the look from the save, into the menu.
	void Loaded();
	// The creator page in the loader window (MLOnLoad).
	void BuildMenu();
	// Every frame: the editing camera.
	void Tick();
}
