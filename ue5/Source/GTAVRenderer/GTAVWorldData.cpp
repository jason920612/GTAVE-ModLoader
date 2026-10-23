#include "GTAVWorldData.h"

namespace
{
	template <typename T>
	bool Section(const FGTAVPackFile& World, GTAVPack::WorldSection Key, TArray<T>& Out, FString& OutError)
	{
		TArray<uint8> Data;
		if (!World.Read(Key, Data) || Data.Num() % sizeof(T) != 0)
		{
			OutError = FString::Printf(TEXT("world.gpk: no section %llu (an older pack? ue5export pack writes version %u)"), uint64(Key), GTAVPack::kVersion);
			return false;
		}
		Out.SetNumUninitialized(Data.Num() / sizeof(T));
		FMemory::Memcpy(Out.GetData(), Data.GetData(), Data.Num());
		return true;
	}
}

bool FGTAVWorldData::Load(const FGTAVPackFile& World, FString& OutError)
{
	if (!Section(World, GTAVPack::kArchetypes, Archetypes, OutError) || !Section(World, GTAVPack::kYmaps, Ymaps, OutError) ||
		!Section(World, GTAVPack::kEntities, Entities, OutError) || !Section(World, GTAVPack::kGroups, Groups, OutError) ||
		!Section(World, GTAVPack::kInteriors, Interiors, OutError) || !Section(World, GTAVPack::kInteriorEntities, InteriorEntities, OutError) ||
		!Section(World, GTAVPack::kRooms, Rooms, OutError) || !Section(World, GTAVPack::kPortals, Portals, OutError) ||
		!Section(World, GTAVPack::kEntitySets, EntitySets, OutError) || !Section(World, GTAVPack::kInteriorPlacements, InteriorPlacements, OutError) ||
		!Section(World, GTAVPack::kGrassBatches, GrassBatches, OutError) || !Section(World, GTAVPack::kGrassInstances, GrassInstances, OutError) ||
		!Section(World, GTAVPack::kLists, Lists, OutError))
	{
		return false;
	}
	FString Ignored;
	Section(World, GTAVPack::kWaterQuads, WaterQuads, Ignored);
	Section(World, GTAVPack::kLodLights, LodLights, Ignored);
	for (int32 I = 0; I < Archetypes.Num(); ++I)
	{
		ArchetypeIndex.Add(Archetypes[I].name, I);
	}
	// An entity without a distance of its own (-1: a third of the map, nearly all the street props) has its
	// archetype's, as in the game.
	for (GTAVPack::Entity& E : Entities)
	{
		if (const GTAVPack::Archetype* A = E.lodDist <= 0 ? Archetype(E.archetype) : nullptr)
		{
			E.lodDist = A->lodDist;
		}
	}
	for (GTAVPack::InteriorEntity& E : InteriorEntities)
	{
		if (const GTAVPack::Archetype* A = E.lodDist <= 0 ? Archetype(E.archetype) : nullptr)
		{
			E.lodDist = A->lodDist;
		}
	}

	// Children: counted, then placed (a compact list per parent).
	ChildStart.SetNumZeroed(Entities.Num() + 1);
	for (const GTAVPack::Entity& E : Entities)
	{
		if (E.parent != GTAVPack::kNone && E.parent < uint32(Entities.Num()))
		{
			++ChildStart[E.parent + 1];
		}
	}
	for (int32 I = 1; I <= Entities.Num(); ++I)
	{
		ChildStart[I] += ChildStart[I - 1];
	}
	ChildList.SetNumUninitialized(ChildStart.Last());
	TArray<uint32> Fill(ChildStart.GetData(), Entities.Num());
	for (int32 I = 0; I < Entities.Num(); ++I)
	{
		const uint32 Parent = Entities[I].parent;
		if (Parent != GTAVPack::kNone && Parent < uint32(Entities.Num()))
		{
			ChildList[Fill[Parent]++] = I;
		}
	}

	// World boxes: the archetype's box turned, scaled and moved (just the position for an unknown archetype).
	Bounds.SetNumUninitialized(Entities.Num());
	for (int32 I = 0; I < Entities.Num(); ++I)
	{
		const GTAVPack::Entity& E = Entities[I];
		const FVector3f P(E.position[0], E.position[1], E.position[2]);
		const GTAVPack::Archetype* A = Archetype(E.archetype);
		if (!A)
		{
			Bounds[I] = FBox3f(P, P);
			continue;
		}
		FBox3f Local(FVector3f(A->boundsMin[0], A->boundsMin[1], A->boundsMin[2]), FVector3f(A->boundsMax[0], A->boundsMax[1], A->boundsMax[2]));
		// (an interior's archetype has no box of its own: its rooms' (room 0 is the limbo outside). Else its distance is
		// to its origin, and a long one - a tunnel - is not there yet when the camera is at its mouth)
		if (E.interiorPlacement != GTAVPack::kNone && E.interiorPlacement < uint32(InteriorPlacements.Num()))
		{
			const uint32 In = InteriorPlacements[E.interiorPlacement].interior;
			if (In < uint32(Interiors.Num()))
			{
				FBox3f RoomBox(ForceInit);
				for (uint32 R = 1; R < Interiors[In].roomCount && Interiors[In].firstRoom + R < uint32(Rooms.Num()); ++R)
				{
					const GTAVPack::Room& Room = Rooms[Interiors[In].firstRoom + R];
					RoomBox += FBox3f(FVector3f(Room.boundsMin[0], Room.boundsMin[1], Room.boundsMin[2]), FVector3f(Room.boundsMax[0], Room.boundsMax[1], Room.boundsMax[2]));
				}
				if (RoomBox.IsValid)
				{
					Local = RoomBox;
				}
			}
		}
		const FTransform3f T(FQuat4f(E.rotation[0], E.rotation[1], E.rotation[2], E.rotation[3]), P, FVector3f(E.scaleXY, E.scaleXY, E.scaleZ));
		Bounds[I] = Local.TransformBy(T);
	}

	// The roots on the grid.
	for (int32 I = 0; I < Entities.Num(); ++I)
	{
		const GTAVPack::Entity& E = Entities[I];
		if (E.parent != GTAVPack::kNone && E.parent < uint32(Entities.Num()))
		{
			continue;
		}
		const float Distance = E.lodDist * 100.0f;
		if (Distance > FarDistance)
		{
			Far.Add(I);
			continue;
		}
		FCell& Cell = Grid.FindOrAdd(CellOf(E.position[0], E.position[1]));
		Cell.Roots.Add(I);
		Cell.Bounds += Bounds[I];
		Cell.MaxDistance = FMath::Max(Cell.MaxDistance, Distance);
	}
	for (int32 I = 0; I < GrassBatches.Num(); ++I)
	{
		const GTAVPack::GrassBatch& B = GrassBatches[I];
		// (the batch's box is in the game's space)
		const float X = (B.boundsMin[0] + B.boundsMax[0]) * 50.0f, Y = -(B.boundsMin[1] + B.boundsMax[1]) * 50.0f;
		GrassGrid.FindOrAdd(CellOf(X, Y)).Add(I);
	}
	return true;
}

const GTAVPack::Archetype* FGTAVWorldData::Archetype(uint32 Name) const
{
	const int32* Index = ArchetypeIndex.Find(Name);
	return Index ? &Archetypes[*Index] : nullptr;
}

TConstArrayView<uint32> FGTAVWorldData::Children(uint32 Entity) const
{
	return TConstArrayView<uint32>(ChildList.GetData() + ChildStart[Entity], ChildStart[Entity + 1] - ChildStart[Entity]);
}
