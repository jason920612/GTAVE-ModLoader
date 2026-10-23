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
	TArray<uint32> NearInteriors; // the placing entities of the interiors wanted at the last update

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
	FVector LastCenter = FVector(1e30);
	double LastUpdate = 0;
	double LastEvict = 0;
	double TickSeconds = 0, TickMax = 0; // (checking: this subsystem's own time per frame, since the last report)
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
