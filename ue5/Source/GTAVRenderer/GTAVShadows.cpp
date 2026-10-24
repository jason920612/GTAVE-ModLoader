#include "GTAVShadows.h"

#include "Components/PointLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	TAutoConsoleVariable<int32> CVarShowProxies(TEXT("gtav.ShowShadowProxies"), 0, TEXT("Checking: the peds' and vehicles' shadow shapes drawn (1)."));

	struct FPool
	{
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<AActor> Owner;
		TArray<TWeakObjectPtr<UStaticMeshComponent>> Cylinders, Boxes;
		TArray<TWeakObjectPtr<USpotLightComponent>> Heads;
		TArray<TWeakObjectPtr<UPointLightComponent>> Tails;
	};

	// The vehicles whose lamps are real lights (the nearest: the player's own first); only the first one's headlamps cast
	// shadows (a moving light's shadow map is drawn anew every frame: with two dozen of them the shadow pass was the biggest
	// part of a night frame).
	constexpr int32 kLitVehicles = 12;
	// A headlamp: a low beam's hot spot is narrow, a degree or two below the horizon (some 10,000-20,000 cd at its
	// brightest, on the road 30 m ahead about 15 lux), and falls off fast below it (the road a few metres ahead gets a
	// few thousand cd); full beam some 40,000 cd, straight ahead. A spot light is round, so the hot spot is kept small
	// and the cone short, else the road right in front of the car is lit as by a floodlight. Warm white (4,300 K). A
	// tail lamp: a few candela of red, the brake lamp's glow not known.
	constexpr float kLowBeam = 8000.0f, kFullBeam = 40000.0f, kTail = 5.0f;

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

void GTAVShadows::Update(UWorld* World, const GTAVBridge::Proxy* Proxies, uint32 Count, bool bGameLights)
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
	int32 UsedCylinders = 0, UsedBoxes = 0, UsedHeads = 0, UsedTails = 0, LitVehicles = 0;
	const auto Head = [&](const FVector& Where, const FRotator& Facing, bool bFull, bool bShadows) {
		if (UsedHeads == Pool.Heads.Num())
		{
			USpotLightComponent* L = NewObject<USpotLightComponent>(Owner);
			L->SetMobility(EComponentMobility::Movable);
			L->IntensityUnits = ELightUnits::Candelas;
			L->SetUseTemperature(true);
			L->SetTemperature(4300.0f);
			L->SetupAttachment(Owner->GetRootComponent());
			L->RegisterComponent();
			Pool.Heads.Add(L);
		}
		if (USpotLightComponent* L = Pool.Heads[UsedHeads++].Get())
		{
			L->SetWorldLocationAndRotation(Where, Facing);
			L->SetIntensity(bFull ? kFullBeam : kLowBeam);
			L->SetAttenuationRadius(bFull ? 12000.0f : 5000.0f);
			L->SetInnerConeAngle(bFull ? 3.0f : 2.0f);
			L->SetOuterConeAngle(bFull ? 15.0f : 20.0f);
			L->SetCastShadows(bShadows);
			L->SetVisibility(true);
		}
	};
	const auto Tail = [&](const FVector& Where) {
		if (UsedTails == Pool.Tails.Num())
		{
			UPointLightComponent* L = NewObject<UPointLightComponent>(Owner);
			L->SetMobility(EComponentMobility::Movable);
			L->IntensityUnits = ELightUnits::Candelas;
			L->SetLightColor(FLinearColor(1.0f, 0.02f, 0.01f));
			L->SetIntensity(kTail);
			L->SetAttenuationRadius(400.0f);
			L->SetCastShadows(false);
			L->SetupAttachment(Owner->GetRootComponent());
			L->RegisterComponent();
			Pool.Tails.Add(L);
		}
		if (UPointLightComponent* L = Pool.Tails[UsedTails++].Get())
		{
			L->SetWorldLocation(Where);
			L->SetVisibility(true);
		}
	};
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
			if (const bool bHidden = CVarShowProxies.GetValueOnGameThread() == 0; C->bHiddenInGame != bHidden)
			{
				C->SetHiddenInGame(bHidden);
			}
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
			// Its lamps (just outside its bounds: the body's shape would shade them).
			if (!bGameLights && (P.lights & 1) && LitVehicles < kLitVehicles)
			{
				++LitVehicles;
				const bool bFull = (P.lights & 2) != 0;
				const double Side = Extent.Y * 0.36, Height = Min.Z + Extent.Z * 0.3;
				const FRotator Beam = FRotationMatrix::MakeFromXZ(Axes.TransformVector(FRotator(bFull ? 0.0f : -1.5f, 0.0f, 0.0f).Vector()), Up).Rotator();
				for (const double S : {-Side, Side})
				{
					Head(At(FVector(Max.X + 15.0, (Min.Y + Max.Y) * 0.5 + S, Height)), Beam, bFull, LitVehicles == 1);
					Tail(At(FVector(Min.X - 15.0, (Min.Y + Max.Y) * 0.5 + S * 1.05, Min.Z + Extent.Z * 0.4)));
				}
			}
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
	for (int32 I = UsedHeads; I < Pool.Heads.Num(); ++I)
	{
		if (USpotLightComponent* L = Pool.Heads[I].Get())
		{
			L->SetVisibility(false);
		}
	}
	for (int32 I = UsedTails; I < Pool.Tails.Num(); ++I)
	{
		if (UPointLightComponent* L = Pool.Tails[I].Get())
		{
			L->SetVisibility(false);
		}
	}
}
