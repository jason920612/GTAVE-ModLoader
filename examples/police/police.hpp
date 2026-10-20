#pragma once
// Police: a fourth character, a police officer with a life of their own off duty (design and research: research/phase0.md §53).

#define NOMINMAX
#include <Windows.h>

#include <modloader/game.hpp>
#include <modloader/natives.hpp>

#include <functional>
#include <string>
#include <vector>

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
	// Into the uniform with the equipment (on duty) or into their own clothes (off duty).
	void Dress(bool uniform);
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

namespace police::duty
{
	// In uniform is on duty.
	bool OnDuty();
	// Where the officer stands about at their department's station when switched to on duty.
	Place Idle();
	// The department's police ped model (backup, colleagues).
	const char* CopModel();
	// A car of the department (its first patrol car, in its livery) at a place; 0 when it could not be made.
	Vehicle UnitCar(const Place& at);
	// The patrol car the officer picked in the lot (0 = none).
	Vehicle PatrolCar();
	// The duty equipment (pistol, taser, baton, flashlight) given or taken.
	void Equip(::Ped ped, bool on);
	// The world was reset: the patrol car is gone.
	void Forget();
	// Every frame while the officer is played: the station's locker and lot.
	void Tick();
}

namespace police::menu
{
	struct Item
	{
		std::string label;
		std::string detail; // right-aligned (a count, a state)
		std::string help;   // a line under the menu while the item is selected
		bool enabled = true;
		bool keepOpen = false;
		std::function<void()> action;
		std::function<struct List()> sub; // opens a submenu instead
	};
	struct List
	{
		std::string title;
		std::vector<Item> items;
	};
	// Opens a menu (on top of the open one, Back returns to it).
	void Open(List list);
	void Close();
	bool IsOpen();
	// Every frame: draws the open menu and takes its keys.
	void Tick();
}

namespace police::units
{
	void CallBackup();
	void BringCar();
	// Ends the incident: the backup goes back to patrol.
	void Clear();
	// Backup units coming or on scene.
	size_t Backup();
	// The world was reset.
	void Forget();
	// Every frame.
	void Tick();
	// The police menu (F9).
	menu::List Menu();
}

namespace police::lights
{
	// The emergency lights' stage of a vehicle (0 off, 1 rear warning, 2 lights, 3 lights and siren).
	void Set(Vehicle v, int stage);
	int Stage(Vehicle v);
	// The stage and tone keys (MLOnLoad).
	void BuildKeys();
	// The world was reset.
	void Forget();
	// Every frame: the keys in the player's emergency vehicle, the sirens' sounds.
	void Tick();
}

namespace police::ai
{
	enum class Skill
	{
		Novice,
		Average,
		Veteran,
	};
	enum class Impairment
	{
		Sober,
		Drunk,
		Substance, // under a controlled substance (graded 1-4, never named)
	};
	// Who a ped is (0..1 values).
	struct Profile
	{
		float nerve = 0.5f;     // dares to run or fight
		float hostility = 0.3f; // inclined to violence
		float guilt = 0.5f;     // what they have to lose (warrants, what they carry)
		int armed = 0;          // 0 none, 1 knife/blunt, 2 handgun, 3 more
		Impairment impairment = Impairment::Sober;
		int substanceGrade = 0; // controlled substance grade carried or taken, 0 none
		Skill skill = Skill::Average;
	};
	// What a ped perceives of the officer (the player).
	struct Senses
	{
		Vector3 playerAt{};
		Vector3 lastSeen{}; // where the officer was last seen
		uint64_t lastSeenAt = 0;
		float distance = 1e9f;
		bool visible = false;    // nothing between them and the officer (either could see the other)
		bool seesPlayer = false; // ... and the officer is in front of them
		bool aimedAt = false;  // the officer points a gun at them
		bool gunDrawn = false; // a firearm in hand
		int copsNear = 0;      // backup on scene
		bool siren = false;    // a police car with lights or siren close by
		bool hurt = false;
	};
	float Distance(const Vector3& a, const Vector3& b);
	float Random();
	Vector3 Eye(::Ped ped);
	// Nothing of the map or objects between two points.
	bool Clear(const Vector3& from, const Vector3& to, Entity ignore);
	// A point on the navmesh near (x, y, z) a ped can stand on.
	bool OnFoot(float x, float y, float z, Vector3& out);
	// The mod decides for this ped: the game's own reactions to events are blocked.
	void TakeControl(::Ped ped);
	void Release(::Ped ped);
	void Sense(::Ped ped, Senses& s);
	// How much the officer's presence weighs on them (0 none, ~1 strong).
	float Pressure(const Senses& s);
}

namespace police::suspect
{
	ai::Profile RandomProfile(ai::Skill skill);
	// A suspect at a place (unaware until the officer comes up to them); returns its id.
	int Spawn(const Vector3& at, float heading, const ai::Profile& profile);
	size_t Count();
	// Removes them all.
	void Clear();
	// The world was reset.
	void Forget();
	// Every frame.
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
