#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "GTAVLights.h"
#include "GTAVPack.h"
#include "GTAVRuntimeAssets.h"
#include "GTAVWorldData.h"

#include "GTAVWorldSubsystem.generated.h"

class UInstancedStaticMeshComponent;
class UStaticMeshComponent;

// The game's world, drawn from the pack files at run time and streamed around the camera the way the game does it
// (research/phase0.md §59.3): an entity is drawn while the camera is nearer than its lodDist; closer than its
// childLodDist it hands over to its children (the next level of detail in) once they are loaded; ymaps of map data
// groups only while their group is on; interiors near the camera; grass batches within their distance.
//   UnrealEditor.exe GTAVRenderer.uproject <map> -game -gtavpack=<folder> [-gtavsky] [-gtavlod=<scale>] [-gtavhour=<h>]
// -gtavsky adds a sun, sky, clouds and fog (for an empty map); -gtavlod scales every distance (1: the game's);
// -gtavbudget=<MB> is what textures and meshes may take before unused ones go (3072).
UCLASS()
class UGTAVWorldSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& World) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(UGTAVWorldSubsystem, STATGROUP_Tickables); }

	// Nothing left to load for the current view (screenshots wait for this).
	bool IsSettled() const;

	// Map data groups on (by name hash); the hour (0-23) for timed archetypes and groups.
	void SetGroup(uint32 Name, bool bOn);
	// All the groups on at once (the game's state, from the bridge).
	void SetGroupsOn(const TSet<uint32>& On);
	void SetHour(int32 Hour);
	// The game's own lights this frame (the bridge's, the game's space): near the camera they light the scene.
	void SetGameLights(const TArray<GTAVBridge::GameLight>& InLights, bool bInside) { Lights.SetGameLights(InLights, bInside); }
	bool UsingGameLights() const { return Lights.UsingGameLights(); }
	// The game's interiors near its camera and the entity sets it has on in each (the bridge's): an interior listed
	// shows those sets, one not listed its default ones.
	void SetGameInteriorSets(const GTAVBridge::InteriorSets* Interiors, uint32 Count);
	// The game's objects near its camera that are not as the map placed them (the bridge's, the game's space): each is
	// drawn where the game has it, and the map's own copy of a moved one is hidden (research/phase0.md §60.20).
	void SetGameObjects(const GTAVBridge::GameObject* Objects, uint32 Count) { GameObjects = TArray<GTAVBridge::GameObject>(Objects, int32(Count)); }
	// Checking: what is drawn near a point (the game's metres), to the log.
	void DumpNear(const FVector& GamePoint, float Radius);
	// A point (Unreal's space) inside one of the rooms of an interior near the camera.
	bool IsInside(const FVector& Point) const;

private:
	FGTAVPack Pack;
	FGTAVWorldData Data;
	TUniquePtr<FGTAVRuntimeAssets> Assets;
	bool bLoaded = false;
	float DistanceScale = 1.0f;
	int32 CurrentHour = 12;
	uint32 OfflineWeather = 0; // -gtavweather
	TSet<uint32> GroupsOn;
	TArray<bool> YmapOn; // per ymap, from the groups
	FGTAVLights Lights;
	UStaticMesh* MakeGlowShape(int32 Splits);
	TArray<uint32> NearInteriors; // the placing entities of the interiors wanted at the last update
	TArray<GTAVBridge::InteriorSets> GameInteriorSets; // (the game's, positions in Unreal's space)

	UPROPERTY()
	TObjectPtr<AActor> Root;
	UPROPERTY()
	TArray<TObjectPtr<UStaticMeshComponent>> Water; // the water quads, the open sea

	// What is drawn: map entities by index; interior entities by (1 << 63 | placement << 32 | index into InteriorEntities).
	TMap<uint64, TObjectPtr<UStaticMeshComponent>> Drawn;
	TMap<uint32, TObjectPtr<UInstancedStaticMeshComponent>> DrawnGrass;
	TArray<TObjectPtr<UStaticMeshComponent>> Pool;
	TMap<uint64, uint32> Wanted; // key -> archetype
	TSet<uint32> WantedGrass;
	// The game's objects (see SetGameObjects): the newest list, a component each (by the object's id), the map entity
	// each stands in for (found by its model where the map placed it; 0 none yet, retried a second later), and the map
	// entities hidden for them.
	TArray<GTAVBridge::GameObject> GameObjects;
	UPROPERTY()
	TMap<uint32, TObjectPtr<UStaticMeshComponent>> ObjectComponents;
	struct FObjectState
	{
		uint32 Model = 0;
		uint64 Hides = 0;
		double LastLook = -1e9;
	};
	TMap<uint32, FObjectState> ObjectStates;
	TSet<uint64> HiddenByObjects;
	void ApplyObjects(const FVector& Center);
	uint64 FindPlaced(uint32 Model, const FVector& Placed) const;
	FVector LastCenter = FVector(1e30);
	double LastUpdate = 0;
	double LastEvict = 0;
	double TickSeconds = 0, TickMax = 0; // (checking: this subsystem's own time per frame, since the last report;
	double GpuSeconds = 0, RenderSeconds = 0; // the GPU's and the render thread's, summed)
	int64 AssetBudget = 0;
	int32 Ticks = 0;
	int64 BudgetBytes = 0; // textures and meshes (-gtavbudget=<MB>)
	int32 WantedMissing = 0; // wanted things whose models are still loading

	void AddSky(UWorld& World);
	void BuildWater();
	void UpdateYmaps();
	void Update(const FVector& Center);
	void Visit(uint32 Entity, const FVector& Center, bool bForced);
	bool Ready(uint32 Entity, const FVector& Center);
	bool Shown(uint32 Archetype) const;
	void Want(uint64 Key, uint32 Archetype, float Distance);
	void WantInterior(uint32 Entity, const FVector& Center);
	void Apply();
	FTransform EntityTransform(uint64 Key) const;
	uint32 EntityTint(uint64 Key) const;
};
