#include "GTAVShadows.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	struct FPool
	{
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<AActor> Owner;
		TArray<TWeakObjectPtr<UStaticMeshComponent>> Cylinders, Boxes;
	};

	// A shape that is not drawn but casts shadows (and occludes the light bounced around: Lumen sees it).
	UStaticMeshComponent* Make(AActor* Owner, UStaticMesh* Mesh)
	{
		UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(Owner);
		C->SetMobility(EComponentMobility::Movable);
		C->SetStaticMesh(Mesh);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCastShadow(true);
		C->bCastHiddenShadow = true;
		C->bAffectIndirectLightingWhileHidden = true;
		C->SetHiddenInGame(true);
		C->SetupAttachment(Owner->GetRootComponent());
		C->RegisterComponent();
		return C;
	}

	// The game's space (metres) -> Unreal's (cm, y mirrored).
	FVector Point(const float P[3])
	{
		return FVector(P[0] * 100.0, -P[1] * 100.0, P[2] * 100.0);
	}

	FVector Direction(const float D[3])
	{
		return FVector(D[0], -D[1], D[2]);
	}
}

void GTAVShadows::Update(UWorld* World, const GTAVBridge::Proxy* Proxies, uint32 Count)
{
	static FPool Pool;
	if (Pool.World.Get() != World || !Pool.Owner.IsValid())
	{
		Pool = {};
		Pool.World = World;
		AActor* Owner = World->SpawnActor<AActor>();
		USceneComponent* Root = NewObject<USceneComponent>(Owner, TEXT("Root"));
		Root->SetMobility(EComponentMobility::Movable);
		Owner->SetRootComponent(Root);
		Root->RegisterComponent();
		Pool.Owner = Owner;
	}
	// (held: until a shape uses it, nothing else would keep the engine's mesh from being collected)
	static TStrongObjectPtr<UStaticMesh> CylinderMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder")));
	static TStrongObjectPtr<UStaticMesh> CubeMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
	UStaticMesh* Cylinder = CylinderMesh.Get();
	UStaticMesh* Cube = CubeMesh.Get();
	if (!Cylinder || !Cube)
	{
		return;
	}
	AActor* Owner = Pool.Owner.Get();
	int32 UsedCylinders = 0, UsedBoxes = 0;
	// (the engine's cylinder and cube: 100 cm across and high, centred)
	const auto Place = [&](TArray<TWeakObjectPtr<UStaticMeshComponent>>& Shapes, int32& Used, UStaticMesh* Mesh, const FVector& Centre,
						   const FRotator& Rotation, const FVector& Size) {
		if (Used == Shapes.Num())
		{
			Shapes.Add(Make(Owner, Mesh));
		}
		if (UStaticMeshComponent* C = Shapes[Used++].Get())
		{
			C->SetWorldLocationAndRotation(Centre, Rotation);
			C->SetWorldScale3D(Size / 100.0);
			C->SetVisibility(true);
		}
	};
	for (uint32 I = 0; I < FMath::Min(Count, GTAVBridge::kMaxProxies); ++I)
	{
		const GTAVBridge::Proxy& P = Proxies[I];
		const FVector Forward = Direction(P.forward).GetSafeNormal(), Up = Direction(P.up).GetSafeNormal();
		if (Forward.IsNearlyZero() || Up.IsNearlyZero())
		{
			continue;
		}
		const FRotationMatrix Axes(FRotationMatrix::MakeFromXZ(Forward, Up).Rotator());
		const FRotator Rotation = Axes.Rotator();
		// (the model's bounds: x right, y forward, z up; Unreal's local x forward, y right)
		const FVector Min(P.boundsMin[1] * 100.0, P.boundsMin[0] * 100.0, P.boundsMin[2] * 100.0);
		const FVector Max(P.boundsMax[1] * 100.0, P.boundsMax[0] * 100.0, P.boundsMax[2] * 100.0);
		const FVector Extent = (Max - Min).ComponentMax(FVector(10.0));
		const FVector Origin = Point(P.position);
		const auto At = [&](const FVector& Local) { return Origin + Axes.TransformVector(Local); };
		if (P.kind == 1)
		{
			// A ped, as tall as its bounds: legs (the lower half, 30 cm across), the body (45 cm) and the head (20 cm).
			const FVector Foot((Min.X + Max.X) * 0.5, (Min.Y + Max.Y) * 0.5, Min.Z);
			const auto Part = [&](double From, double To, double Across) {
				Place(Pool.Cylinders, UsedCylinders, Cylinder, At(Foot + FVector(0, 0, Extent.Z * (From + To) * 0.5)), Rotation,
					FVector(Across, Across * 0.7, Extent.Z * (To - From)));
			};
			Part(0.0, 0.5, 30.0);
			Part(0.5, 0.87, 45.0);
			Part(0.87, 1.0, 20.0);
		}
		else
		{
			// A vehicle: its body from the ground clearance to a little over half its height, the cabin narrower and
			// shorter above it.
			const double Clear = Extent.Z * 0.12, Waist = Extent.Z * 0.55;
			const FVector Body((Min.X + Max.X) * 0.5, (Min.Y + Max.Y) * 0.5, Min.Z + (Clear + Waist) * 0.5);
			Place(Pool.Boxes, UsedBoxes, Cube, At(Body), Rotation, FVector(Extent.X, Extent.Y, Waist - Clear));
			const FVector Cabin((Min.X + Max.X) * 0.5 - Extent.X * 0.05, (Min.Y + Max.Y) * 0.5, Min.Z + (Waist + Extent.Z) * 0.5);
			Place(Pool.Boxes, UsedBoxes, Cube, At(Cabin), Rotation, FVector(Extent.X * 0.55, Extent.Y * 0.85, Extent.Z - Waist));
		}
	}
	for (int32 I = UsedCylinders; I < Pool.Cylinders.Num(); ++I)
	{
		if (UStaticMeshComponent* C = Pool.Cylinders[I].Get())
		{
			C->SetVisibility(false);
		}
	}
	for (int32 I = UsedBoxes; I < Pool.Boxes.Num(); ++I)
	{
		if (UStaticMeshComponent* C = Pool.Boxes[I].Get())
		{
			C->SetVisibility(false);
		}
	}
}
