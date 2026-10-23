#pragma once

#include "CoreMinimal.h"

#include "GTAVPack.h"

// The map as world.gpk has it (GTAVPackFormat.h), held in memory, with what the streamer needs to find things fast:
// each entity's children (the next level of detail in), the entities without a parent on a grid by position, and
// archetypes by name (research/phase0.md §59.3).
class FGTAVWorldData
{
public:
	bool Load(const FGTAVPackFile& World, FString& OutError);

	TArray<GTAVPack::Archetype> Archetypes;
	TArray<GTAVPack::Ymap> Ymaps;
	TArray<GTAVPack::Entity> Entities;
	TArray<GTAVPack::Group> Groups;
	TArray<GTAVPack::Interior> Interiors;
	TArray<GTAVPack::InteriorEntity> InteriorEntities;
	TArray<GTAVPack::Room> Rooms;
	TArray<GTAVPack::Portal> Portals;
	TArray<GTAVPack::EntitySet> EntitySets;
	TArray<GTAVPack::InteriorPlacement> InteriorPlacements;
	TArray<GTAVPack::GrassBatch> GrassBatches;
	TArray<GTAVPack::GrassInstance> GrassInstances;
	TArray<uint32> Lists;

	const GTAVPack::Archetype* Archetype(uint32 Name) const;
	// The entities an entity hands over to when the camera comes closer than its childLodDist.
	TConstArrayView<uint32> Children(uint32 Entity) const;

	// Each entity's box in the world (its archetype's box placed), what distances are measured to.
	TArray<FBox3f> Bounds;
	float Distance(uint32 Entity, const FVector& Point) const { return FMath::Sqrt(float(FBox(Bounds[Entity]).ComputeSquaredDistanceToPoint(Point))); }

	// Entities without a parent, by grid cell (CellSize centimetres), with their boxes together and the farthest any of
	// them is seen from (lodDist, centimetres); the ones seen from farther than FarDistance are in Far instead.
	static constexpr float CellSize = 25600.0f;
	static constexpr float FarDistance = 200000.0f;
	struct FCell
	{
		TArray<uint32> Roots;
		FBox3f Bounds = FBox3f(ForceInit);
		float MaxDistance = 0;
	};
	TMap<FIntPoint, FCell> Grid;
	TArray<uint32> Far;
	static FIntPoint CellOf(float X, float Y) { return FIntPoint(FMath::FloorToInt(X / CellSize), FMath::FloorToInt(Y / CellSize)); }

	// Grass batches by grid cell too.
	TMap<FIntPoint, TArray<uint32>> GrassGrid;

private:
	TMap<uint32, int32> ArchetypeIndex;
	TArray<uint32> ChildStart; // per entity: first index into ChildList (Entities.Num() + 1 entries)
	TArray<uint32> ChildList;
};
