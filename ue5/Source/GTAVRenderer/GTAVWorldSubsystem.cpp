#include "GTAVWorldSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"

DEFINE_LOG_CATEGORY_STATIC(LogGTAVWorld, Log, All);

namespace
{
	constexpr uint64 kInteriorKey = 1ull << 63;
	constexpr double kAddBudget = 0.004;   // seconds per frame making components
	constexpr double kAssetBudget = 0.006; // seconds per frame finishing models

	FVector Position(const float* P) { return FVector(P[0], P[1], P[2]); }
	FQuat Rotation(const float* Q) { return FQuat(Q[0], Q[1], Q[2], Q[3]); }
}

bool UGTAVWorldSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && World->IsGameWorld() && FString(FCommandLine::Get()).Contains(TEXT("-gtavpack="));
}

void UGTAVWorldSubsystem::Deinitialize()
{
	Drawn.Empty();
	DrawnGrass.Empty();
	Pool.Empty();
	Assets.Reset();
	Super::Deinitialize();
}

// Sun, sky, clouds, fog: lit by Lumen (the project's settings).
void UGTAVWorldSubsystem::AddSky(UWorld& World)
{
	ADirectionalLight* Sun = World.SpawnActor<ADirectionalLight>(FVector(0, 0, 10000), FRotator(-35, 40, 0));
	Sun->SetMobility(EComponentMobility::Movable);
	UDirectionalLightComponent* Light = CastChecked<UDirectionalLightComponent>(Sun->GetLightComponent());
	Light->SetAtmosphereSunLight(true);
	Light->SetIntensity(10.0f);
	World.SpawnActor<ASkyAtmosphere>(FVector::ZeroVector, FRotator::ZeroRotator);
	ASkyLight* Sky = World.SpawnActor<ASkyLight>(FVector(0, 0, 5000), FRotator::ZeroRotator);
	Sky->GetLightComponent()->SetMobility(EComponentMobility::Movable);
	Sky->GetLightComponent()->bRealTimeCapture = true;
	Sky->GetLightComponent()->RecaptureSky();
	World.SpawnActor<AVolumetricCloud>(FVector::ZeroVector, FRotator::ZeroRotator);
	World.SpawnActor<AExponentialHeightFog>(FVector::ZeroVector, FRotator::ZeroRotator);
}

void UGTAVWorldSubsystem::OnWorldBeginPlay(UWorld& World)
{
	Super::OnWorldBeginPlay(World);
	FString Folder, Error;
	FParse::Value(FCommandLine::Get(), TEXT("-gtavpack="), Folder, false);
	FParse::Value(FCommandLine::Get(), TEXT("-gtavlod="), DistanceScale);
	FParse::Value(FCommandLine::Get(), TEXT("-gtavhour="), CurrentHour);
	const double Start = FPlatformTime::Seconds();
	if (!Pack.Open(Folder, Error) || !Data.Load(Pack.World, Error))
	{
		UE_LOG(LogGTAVWorld, Error, TEXT("%s"), *Error);
		return;
	}
	if (FParse::Param(FCommandLine::Get(), TEXT("gtavsky")))
	{
		AddSky(World);
	}
	Assets = MakeUnique<FGTAVRuntimeAssets>(Pack);
	Root = World.SpawnActor<AActor>();
	USceneComponent* RootComponent = NewObject<USceneComponent>(Root, TEXT("Root"));
	Root->SetRootComponent(RootComponent);
	RootComponent->RegisterComponent();
	UpdateYmaps();
	bLoaded = true;
	int32 Roots = 0;
	for (const TPair<FIntPoint, FGTAVWorldData::FCell>& Cell : Data.Grid)
	{
		Roots += Cell.Value.Roots.Num();
	}
	UE_LOG(LogGTAVWorld, Display, TEXT("%s: %d archetypes, %d ymaps, %d entities (%d roots on %d cells, %d far), %d interiors, %d grass batches in %.2f s"),
		*Folder, Data.Archetypes.Num(), Data.Ymaps.Num(), Data.Entities.Num(), Roots, Data.Grid.Num(), Data.Far.Num(),
		Data.Interiors.Num(), Data.GrassBatches.Num(), FPlatformTime::Seconds() - Start);
}

void UGTAVWorldSubsystem::SetGroup(uint32 Name, bool bOn)
{
	if (bOn ? (GroupsOn.Add(Name), true) : GroupsOn.Remove(Name) > 0)
	{
		UpdateYmaps();
		LastUpdate = 0;
	}
}

void UGTAVWorldSubsystem::SetHour(int32 Hour)
{
	if (Hour != CurrentHour)
	{
		CurrentHour = Hour;
		UpdateYmaps();
		LastUpdate = 0;
	}
}

// A ymap of a map data group is there only while the group is on (switched by a script, or by the hour).
void UGTAVWorldSubsystem::UpdateYmaps()
{
	YmapOn.SetNum(Data.Ymaps.Num());
	for (int32 I = 0; I < Data.Ymaps.Num(); ++I)
	{
		const uint32 G = Data.Ymaps[I].group;
		bool bOn = true;
		if (G != GTAVPack::kNone && G < uint32(Data.Groups.Num()))
		{
			const GTAVPack::Group& Group = Data.Groups[G];
			bOn = GroupsOn.Contains(Group.name) || (Group.hoursOnOff && ((Group.hoursOnOff >> CurrentHour) & 1));
		}
		YmapOn[I] = bOn;
	}
}

bool UGTAVWorldSubsystem::Shown(uint32 Archetype) const
{
	const GTAVPack::Archetype* A = Data.Archetype(Archetype);
	if (!A || !A->hasModel)
	{
		return false;
	}
	return A->kind != GTAVPack::kTimed || ((A->timeFlags >> CurrentHour) & 1);
}

bool UGTAVWorldSubsystem::IsSettled() const
{
	if (!bLoaded)
	{
		return true;
	}
	if (Assets->Loading() || WantedMissing || Wanted.Num() != Drawn.Num() || WantedGrass.Num() != DrawnGrass.Num())
	{
		return false;
	}
	return true;
}

void UGTAVWorldSubsystem::Tick(float DeltaTime)
{
	if (!bLoaded)
	{
		return;
	}
	Assets->Tick(kAssetBudget);
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (!PC || !PC->PlayerCameraManager)
	{
		return;
	}
	const FVector Center = PC->PlayerCameraManager->GetCameraLocation();
	const double Now = FPlatformTime::Seconds();
	if (FVector::DistSquared(Center, LastCenter) > 100.0f * 100.0f || Now - LastUpdate > 0.25)
	{
		Update(Center);
		LastCenter = Center;
		LastUpdate = Now;
	}
	Apply();
	// (checking: -gtavdump=x,y,z,radius in the game's metres lists what is drawn there once everything is in)
	static bool bDumped = false;
	FString Dump;
	if (!bDumped && IsSettled() && Drawn.Num() && FParse::Value(FCommandLine::Get(), TEXT("-gtavdump="), Dump, false))
	{
		bDumped = true;
		TArray<FString> V;
		Dump.ParseIntoArray(V, TEXT(","));
		const FVector P(FCString::Atof(*V[0]) * 100, -FCString::Atof(*V[1]) * 100, FCString::Atof(*V[2]) * 100);
		const float Radius = V.Num() > 3 ? FCString::Atof(*V[3]) * 100 : 2000.0f;
		for (const TPair<uint64, TObjectPtr<UStaticMeshComponent>>& D : Drawn)
		{
			if (D.Key & kInteriorKey)
			{
				continue;
			}
			const uint32 I = uint32(D.Key);
			if (Data.Distance(I, P) > Radius)
			{
				continue;
			}
			const GTAVPack::Entity& E = Data.Entities[I];
			UE_LOG(LogGTAVWorld, Display, TEXT("dump: entity %u archetype %08x lod %u ymap %u (%08x) parent %d lodDist %.0f at %.1f %.1f %.1f"), I, E.archetype, E.lodLevel,
				E.ymap, Data.Ymaps[E.ymap].name, int32(E.parent), E.lodDist, E.position[0] / 100, -E.position[1] / 100, E.position[2] / 100);
		}
	}
	static double Told = 0;
	if (Now - Told > 10)
	{
		Told = Now;
		UE_LOG(LogGTAVWorld, Display, TEXT("drawn %d (%d wanted, %d waiting for models), grass %d; models %d built, %d loading, %d failed; textures %d (%.0f MB, %d failed)"),
			Drawn.Num(), Wanted.Num(), WantedMissing, DrawnGrass.Num(), Assets->MeshesBuilt, Assets->Loading(), Assets->MeshesFailed, Assets->TexturesMade,
			Assets->TextureBytes / 1048576.0, Assets->TexturesFailed);
	}
}

// An entity's children can take over: each is loaded, has nothing to load, or is not there now.
bool UGTAVWorldSubsystem::Ready(uint32 Entity, const FVector& Center)
{
	const GTAVPack::Entity& E = Data.Entities[Entity];
	if (!YmapOn[E.ymap] || E.interiorPlacement != GTAVPack::kNone || !Shown(E.archetype))
	{
		return true;
	}
	const FGTAVRuntimeAssets::EState State = Assets->Request(E.archetype, Data.Distance(Entity, Center));
	return State == FGTAVRuntimeAssets::EState::Ready || State == FGTAVRuntimeAssets::EState::Failed;
}

void UGTAVWorldSubsystem::Want(uint64 Key, uint32 Archetype, float Distance)
{
	if (!Shown(Archetype))
	{
		return;
	}
	switch (Assets->Request(Archetype, Distance))
	{
	case FGTAVRuntimeAssets::EState::Ready:
		Wanted.Add(Key, Archetype);
		break;
	case FGTAVRuntimeAssets::EState::Loading:
		++WantedMissing;
		break;
	default:
		break;
	}
}

void UGTAVWorldSubsystem::Visit(uint32 Entity, const FVector& Center, bool bForced)
{
	const GTAVPack::Entity& E = Data.Entities[Entity];
	if (!YmapOn[E.ymap])
	{
		return;
	}
	const float D = Data.Distance(Entity, Center);
	if (!bForced && D > E.lodDist * 100.0f * DistanceScale)
	{
		return;
	}
	const TConstArrayView<uint32> Children = Data.Children(Entity);
	if (Children.Num())
	{
		float Switch = E.childLodDist;
		if (Switch <= 0)
		{
			for (const uint32 C : Children)
			{
				Switch = FMath::Max(Switch, Data.Entities[C].lodDist);
			}
		}
		if (D < Switch * 100.0f * DistanceScale)
		{
			// (all children at once, once they are all loaded: no holes while they come in)
			bool bAll = true;
			for (const uint32 C : Children)
			{
				bAll &= Ready(C, Center);
			}
			if (bAll)
			{
				for (const uint32 C : Children)
				{
					Visit(C, Center, true);
				}
				return;
			}
		}
	}
	if (E.interiorPlacement != GTAVPack::kNone)
	{
		WantInterior(Entity, Center);
		return;
	}
	Want(Entity, E.archetype, D);
}

// An interior's entities and its entity sets on by default, near enough.
void UGTAVWorldSubsystem::WantInterior(uint32 Entity, const FVector& Center)
{
	const GTAVPack::Entity& E = Data.Entities[Entity];
	const GTAVPack::InteriorPlacement& P = Data.InteriorPlacements[E.interiorPlacement];
	if (P.interior == GTAVPack::kNone || P.interior >= uint32(Data.Interiors.Num()))
	{
		return;
	}
	const GTAVPack::Interior& In = Data.Interiors[P.interior];
	const FTransform Placement(Rotation(E.rotation), Position(E.position));
	const auto Add = [&](uint32 Index) {
		const GTAVPack::InteriorEntity& X = Data.InteriorEntities[Index];
		const float Range = (X.lodDist > 0 ? X.lodDist : 100.0f) * 100.0f * DistanceScale;
		const float D = FVector::Dist(Center, Placement.TransformPosition(Position(X.position)));
		if (D < Range)
		{
			Want(kInteriorKey | uint64(E.interiorPlacement) << 32 | Index, X.archetype, D);
		}
	};
	for (uint32 I = 0; I < In.entityCount; ++I)
	{
		Add(In.firstEntity + I);
	}
	for (uint32 S = 0; S < P.defaultSetCount; ++S)
	{
		const uint32 Name = Data.Lists[P.firstDefaultSet + S];
		for (uint32 K = 0; K < In.setCount; ++K)
		{
			const GTAVPack::EntitySet& Set = Data.EntitySets[In.firstSet + K];
			if (Set.name == Name)
			{
				for (uint32 I = 0; I < Set.entityCount; ++I)
				{
					Add(Set.firstEntity + I);
				}
			}
		}
	}
}

void UGTAVWorldSubsystem::Update(const FVector& Center)
{
	Wanted.Reset();
	WantedGrass.Reset();
	WantedMissing = 0;
	// (every cell: a few thousand box tests; an entity's box may reach far out of its cell)
	for (const TPair<FIntPoint, FGTAVWorldData::FCell>& Cell : Data.Grid)
	{
		if (FBox(Cell.Value.Bounds).ComputeSquaredDistanceToPoint(Center) > FMath::Square(Cell.Value.MaxDistance * DistanceScale))
		{
			continue;
		}
		for (const uint32 R : Cell.Value.Roots)
		{
			Visit(R, Center, false);
		}
	}
	for (const uint32 R : Data.Far)
	{
		Visit(R, Center, false);
	}

	// Grass: batches whose box is within their distance.
	constexpr float GrassReach = 40000.0f;
	const FIntPoint GLo = FGTAVWorldData::CellOf(Center.X - GrassReach, Center.Y - GrassReach),
					GHi = FGTAVWorldData::CellOf(Center.X + GrassReach, Center.Y + GrassReach);
	for (int32 X = GLo.X; X <= GHi.X; ++X)
	{
		for (int32 Y = GLo.Y; Y <= GHi.Y; ++Y)
		{
			const TArray<uint32>* Batches = Data.GrassGrid.Find(FIntPoint(X, Y));
			if (!Batches)
			{
				continue;
			}
			for (const uint32 B : *Batches)
			{
				const GTAVPack::GrassBatch& G = Data.GrassBatches[B];
				if (!YmapOn[G.ymap] || !Shown(G.archetype))
				{
					continue;
				}
				const FBox Box(FVector(G.boundsMin[0] * 100, -G.boundsMax[1] * 100, G.boundsMin[2] * 100),
					FVector(G.boundsMax[0] * 100, -G.boundsMin[1] * 100, G.boundsMax[2] * 100));
				const double D2 = Box.ComputeSquaredDistanceToPoint(Center);
				if (D2 > FMath::Square(G.lodDist * 100.0f * DistanceScale))
				{
					continue;
				}
				const FGTAVRuntimeAssets::EState State = Assets->Request(G.archetype, FMath::Sqrt(D2));
				if (State == FGTAVRuntimeAssets::EState::Ready)
				{
					WantedGrass.Add(B);
				}
				else if (State == FGTAVRuntimeAssets::EState::Loading)
				{
					++WantedMissing;
				}
			}
		}
	}
}

FTransform UGTAVWorldSubsystem::EntityTransform(uint64 Key) const
{
	if (Key & kInteriorKey)
	{
		const uint32 PlacementIndex = uint32(Key >> 32) & 0x7FFFFFFF, Index = uint32(Key);
		const GTAVPack::Entity& E = Data.Entities[Data.InteriorPlacements[PlacementIndex].entity];
		const GTAVPack::InteriorEntity& X = Data.InteriorEntities[Index];
		const FTransform Local(Rotation(X.rotation), Position(X.position), FVector(X.scaleXY, X.scaleXY, X.scaleZ));
		return Local * FTransform(Rotation(E.rotation), Position(E.position));
	}
	const GTAVPack::Entity& E = Data.Entities[uint32(Key)];
	return FTransform(Rotation(E.rotation), Position(E.position), FVector(E.scaleXY, E.scaleXY, E.scaleZ));
}

void UGTAVWorldSubsystem::Apply()
{
	const double Start = FPlatformTime::Seconds();
	bool bAllAdded = true;
	for (const TPair<uint64, uint32>& W : Wanted)
	{
		if (Drawn.Contains(W.Key))
		{
			continue;
		}
		if (FPlatformTime::Seconds() - Start > kAddBudget)
		{
			bAllAdded = false;
			break;
		}
		UStaticMesh* Mesh = Assets->Get(W.Value);
		if (!Mesh)
		{
			continue;
		}
		UStaticMeshComponent* C = Pool.Num() ? Pool.Pop(EAllowShrinking::No).Get() : nullptr;
		if (!C)
		{
			C = NewObject<UStaticMeshComponent>(Root);
			C->SetMobility(EComponentMobility::Static);
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			C->SetGenerateOverlapEvents(false);
		}
		C->SetStaticMesh(Mesh);
		C->SetWorldTransform(EntityTransform(W.Key));
		C->RegisterComponent();
		Drawn.Add(W.Key, C);
	}
	for (const uint32 B : WantedGrass)
	{
		if (DrawnGrass.Contains(B))
		{
			continue;
		}
		if (FPlatformTime::Seconds() - Start > kAddBudget)
		{
			bAllAdded = false;
			break;
		}
		const GTAVPack::GrassBatch& G = Data.GrassBatches[B];
		UStaticMesh* Mesh = Assets->Get(G.archetype);
		if (!Mesh)
		{
			continue;
		}
		UInstancedStaticMeshComponent* C = NewObject<UInstancedStaticMeshComponent>(Root);
		C->SetMobility(EComponentMobility::Static);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetStaticMesh(Mesh);
		C->SetCullDistances(int32(G.fadeStart * 100.0f * DistanceScale), int32(G.lodDist * 100.0f * DistanceScale));
		TArray<FTransform> Transforms;
		Transforms.Reserve(G.instanceCount);
		const FVector Min(G.boundsMin[0], G.boundsMin[1], G.boundsMin[2]), Size = FVector(G.boundsMax[0], G.boundsMax[1], G.boundsMax[2]) - Min;
		for (uint32 I = 0; I < G.instanceCount; ++I)
		{
			const GTAVPack::GrassInstance& X = Data.GrassInstances[G.firstInstance + I];
			const FVector P = Min + Size * FVector(X.position[0], X.position[1], X.position[2]) / 65535.0;
			const float Scale = FMath::Lerp(G.scaleRange[0], G.scaleRange[1], X.scale / 255.0f);
			// (no yaw stored: one from the position; tilted to the ground normal when the batch says so)
			const float Yaw = float((X.position[0] * 7919u + X.position[1] * 104729u) % 360u);
			FQuat Q = FQuat(FVector::UpVector, FMath::DegreesToRadians(Yaw));
			if (G.orientToTerrain > 0)
			{
				const float Nx = X.normalX / 127.5f - 1.0f, Ny = -(X.normalY / 127.5f - 1.0f);
				const FVector N(Nx, Ny, FMath::Sqrt(FMath::Max(0.0f, 1.0f - Nx * Nx - Ny * Ny)));
				Q = FQuat::Slerp(FQuat::Identity, FQuat::FindBetweenNormals(FVector::UpVector, N), G.orientToTerrain) * Q;
			}
			Transforms.Add(FTransform(Q, FVector(P.X * 100, -P.Y * 100, P.Z * 100), FVector(Scale)));
		}
		C->AddInstances(Transforms, false, true);
		C->RegisterComponent();
		DrawnGrass.Add(B, C);
	}
	// What is not wanted goes once everything wanted is there (a parent stays until its children are drawn).
	if (!bAllAdded)
	{
		return;
	}
	for (auto It = Drawn.CreateIterator(); It; ++It)
	{
		if (!Wanted.Contains(It.Key()))
		{
			It.Value()->UnregisterComponent();
			Pool.Add(It.Value());
			It.RemoveCurrent();
		}
	}
	for (auto It = DrawnGrass.CreateIterator(); It; ++It)
	{
		if (!WantedGrass.Contains(It.Key()))
		{
			It.Value()->DestroyComponent();
			It.RemoveCurrent();
		}
	}
}
