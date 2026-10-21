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
	// The open menu and the selected item (nullptr when none is open): what is on screen, for a test player.
	const List* Current(size_t& cursor);
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
	// A backup officer near a place, free to help (a search): taken off their duties until returned. 0 = none.
	::Ped Borrow(const Vector3& around);
	void Return(::Ped helper);
	// Backup officers within `radius` of a place (how many police a suspect has on them).
	int Near(const Vector3& at, float radius);
	// A backup officer on foot has a gun on them from within `radius` (covering a suspect who complies).
	bool Covering(const Vector3& at, float radius);
	// The car is a backup unit's.
	bool IsUnitCar(Vehicle car);
	// The nearest unit drives its car up to a place (to take a suspect over): the car, 0 = no unit.
	Vehicle Summon(const Vector3& to);
	// The unit of this car drives a suspect in to `to` (the transport); false when it is not a unit's car.
	bool Transport(Vehicle car, const Vector3& to);
	// The transport is there (or out of sight long enough to have got there).
	bool Delivered(Vehicle car);
	// The suspect is handed over: the unit goes back on patrol.
	void TransportDone(Vehicle car);
	// The backup's cars.
	std::vector<Vehicle> Cars();
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
		bool covered = false;  // a backup officer has a gun on them from a few metres
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
	// A suspect driving a car on the road nearest to `around` (unaware until a police car with its lights on is behind).
	int SpawnDriver(const Vector3& around, const ai::Profile& profile);
	size_t Count();
	// What the police know of the suspect being pursued (the first one active), for the backup.
	struct Target
	{
		enum class Mode
		{
			Foot,    // running or hiding on foot
			Vehicle, // getting away by road
			Armed,   // fighting or shooting
			Stopped, // complying, pulled over
		};
		Vector3 lastKnown{}; // where they were last seen by the police
		Vector3 heading{};   // where they are going (their route), when known
		::Ped ped = 0;
		Vehicle car = 0;
		Mode mode = Mode::Foot;
		bool known = false;  // someone of the police sees them now
	};
	bool Pursued(Target& out);
	// The officer dealing with a suspect (the action menu).
	struct Status
	{
		::Ped ped = 0;
		bool inVehicle = false;
		bool yielded = false; // complying or standing there: can be dealt with
		bool cuffed = false;
		bool custody = false; // in the patrol car
		bool gone = false;
		bool dead = false;
	};
	// A suspect (not in custody) within `radius` of a place: its id, 0 = none.
	int Within(const Vector3& at, float radius);
	Status StatusOf(int id);
	void OrderOut(int id);
	// Who a suspect is (false for anyone else), the suspect driving a vehicle, a suspect's vehicle.
	bool ProfileOf(::Ped ped, ai::Profile& out);
	::Ped OwnerOf(Vehicle car);
	Vehicle CarOf(int id);
	// The suspect was driving (a vehicle of theirs, taken or their own).
	bool Drove(::Ped ped);
	// Searched: what they carried is taken.
	void Disarm(::Ped ped);
	bool Cuff(int id);
	// A cuffed suspect into the back of a car.
	bool PutInCar(int id, Vehicle car);
	// In custody: out of the car again, cuffed, following the officer (at the station).
	void TakeOut(int id);
	// Cuffed: follows someone else than the officer (the jailer taking them in; 0 = the officer again).
	void LedBy(int id, ::Ped leader);
	// In custody: breaks out of the car and runs, cuffed.
	void Escape(int id);
	// Booked: taken away (removed from the world).
	void Delete(int id);
	// Let go (no arrest).
	void Release(int id);
	// A police officer (backup) sees the suspect: tells the others (the suspect's last known place).
	void Spotted(::Ped by);
	// Removes them all.
	void Clear();
	// The world was reset.
	void Forget();
	// Every frame.
	void Tick();
}

namespace police::dispatch
{
	// The police menu's dispatch item (the call on offer, or the one under way).
	menu::Item MenuItem();
	// Development: a call now.
	void CallNow(bool easy = false);
	// The world was reset.
	void Forget();
	// Every frame while the officer is played: calls, and the action menu next to a suspect (hold E).
	void Tick();
}

namespace police::records
{
	struct Person
	{
		std::vector<std::string> priors;
		std::string name, dob, licence, warrantFor;
		bool warrant = false;
		bool drunk = false;
	};
	struct VehicleRecord
	{
		std::string plate, owner, registration, insurance;
		Hash model = 0;
		bool stolen = false;
	};
	struct Case
	{
		std::vector<std::string> log, evidence, people, suggested, conduct;
		std::string title, place, date, time;
		int conductPoints = 0;
	};
	struct Report
	{
		std::vector<std::string> people, evidence, suggested, charges, conduct;
		std::string title, date, time, place, outcome, narrative;
		int id = 0;
		int conductPoints = 0;
		int score = 0;
		bool submitted = false;
	};
	std::string RandomName();
	// A shop picture of a vehicle model (https://gametextures/...), "" when there is none.
	std::string Photo(Hash model);
	Person& PersonOf(::Ped ped);
	// A suspect's record follows who they are.
	void SetSuspect(::Ped ped, const std::string& name, float guilt, bool drunk);
	VehicleRecord& VehicleOf(Vehicle v);
	void MarkStolen(Vehicle v);
	ml::Json PersonJson(const Person& p);
	ml::Json VehicleJson(Vehicle v);
	// A vehicle by its plate (known, or around the officer), 0 = none.
	Vehicle FindPlate(const std::string& plate);
	const Person* FindName(const std::string& name);
	// Someone's ID was checked (they show in the computer's list).
	void Checked(::Ped ped);
	ml::Json CheckedPeople();
	// The case under way: its log, evidence, the people in it, the charges the evidence supports.
	void BeginCase(const std::string& title, const std::string& place);
	bool CaseOpen();
	void Note(const std::string& what);
	void Evidence(const std::string& item);
	void Involved(const std::string& person);
	void Charge(const std::string& charge);
	// The charges the case supports so far.
	std::vector<std::string> Charges();
	// How the officer did (the transport): points for the report's score, with the reason.
	void Conduct(int points, const std::string& why);
	// Ends it: a report is drafted; its id.
	int EndCase(const std::string& outcome);
	ml::Json Reports();
	ml::Json CurrentCase();
	ml::Json Submit(int id, const std::vector<std::string>& charges, const std::string& narrative);
	// After a save loaded: the reports from it.
	void Loaded();
	void Forget();
}

namespace police::search
{
	constexpr int kZones = 5; // driver's seat, front passenger seat, glovebox, back seats, boot
	const char* ZoneName(int zone);
	bool Searched(Vehicle car, int zone);
	bool Frisked(::Ped ped);
	// The officer is searching now.
	bool Busy();
	// Frisk a person / search a vehicle area (-1 = every area left), by the officer or a backup officer.
	void Frisk(::Ped target, bool backup);
	// Booking: what they carry is listed and kept (recorded as found then when nobody searched them); true when it held
	// a weapon nobody had found.
	bool Book(::Ped target);
	void SearchCar(Vehicle car, int zone, bool backup);
	// A backup officer called away by their unit: their search stops where it is (what is left stays unsearched).
	void Drop(::Ped who);
	void Forget();
	void Tick();
}

namespace police::custody
{
	// The suspect is to be taken in by the backup (before they are put in the backup's car).
	void ByBackup(int id);
	// A suspect sat in a police car (suspect.cpp): the transport begins.
	void Begin(int id, Vehicle car);
	// Booked at a station or the prison: the case can close. The facility's name.
	bool Booked(int id, std::string* where = nullptr);
	// Booking at the door (the action menu on the suspect is not offered meanwhile).
	bool Busy();
	// The police menu's transport item (where to, by whom).
	menu::Item MenuItem();
	void Forget();
	// Every frame while the officer is played: the transport, the arrival, booking.
	void Tick();
}

namespace police::mdt
{
	// The in-car computer (a page of the in-game browser) and the hotkey (MLOnLoad).
	void Build();
	void Open();
	// Asking dispatch over the radio: a vehicle's plate, a person's name (the answer comes after a few seconds).
	void RadioPlate(Vehicle v);
	void RadioPerson(::Ped ped);
	// The plate of the vehicle in front of the officer's car (the plate reader), 0 = none.
	Vehicle Ahead();
	void Forget();
	// Every frame: the plate reader, radio answers.
	void Tick();
}

namespace police::ambient
{
	// The world was reset.
	void Forget();
	// Every frame while the officer is played: traffic giving way, onlookers and fleeing passers-by, witnesses.
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
