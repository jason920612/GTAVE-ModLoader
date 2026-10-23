#include "GTAVLights.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"

namespace
{
	constexpr float kCell = 25600.0f;
	// The game's intensity byte -> candela (a street light, 170: about 3000 cd, some 40 lux on the road 8 m below).
	constexpr float kCandela = 4500.0f;
	// The game's cone bytes -> degrees (127.5: a half sphere).
	constexpr float kConeDegrees = 90.0f / 127.5f;

	FColor ColorOf(const GTAVPack::LodLight& L)
	{
		return FColor(uint8(L.rgbi >> 16), uint8(L.rgbi >> 8), uint8(L.rgbi), 255);
	}

	float IntensityOf(const GTAVPack::LodLight& L)
	{
		return float(L.rgbi >> 24) / 255.0f;
	}

	bool IsSpot(const GTAVPack::LodLight& L)
	{
		return L.coneOuter * kConeDegrees < 80.0f && FVector(L.direction[0], L.direction[1], L.direction[2]).SizeSquared() > 0.5f;
	}
}

FIntPoint FGTAVLights::CellOf(const FVector& P)
{
	return FIntPoint(FMath::FloorToInt32(P.X / kCell), FMath::FloorToInt32(P.Y / kCell));
}

void FGTAVLights::Load(TArray<GTAVPack::LodLight>&& InLights)
{
	Lights = MoveTemp(InLights);
	Grid.Reset();
	for (int32 I = 0; I < Lights.Num(); ++I)
	{
		const GTAVPack::LodLight& L = Lights[I];
		Grid.FindOrAdd(CellOf(FVector(L.position[0], L.position[1], L.position[2]))).Add(I);
	}
}

void FGTAVLights::Gather(const FVector& Camera, float Radius, int32 Hour, TArray<TPair<float, int32>>& Out) const
{
	const FIntPoint Lo = CellOf(Camera - FVector(Radius)), Hi = CellOf(Camera + FVector(Radius));
	const float R2 = Radius * Radius;
	for (int32 X = Lo.X; X <= Hi.X; ++X)
	{
		for (int32 Y = Lo.Y; Y <= Hi.Y; ++Y)
		{
			const TArray<int32>* Cell = Grid.Find(FIntPoint(X, Y));
			if (!Cell)
			{
				continue;
			}
			for (const int32 I : *Cell)
			{
				const GTAVPack::LodLight& L = Lights[I];
				const float D2 = float(FVector::DistSquared(Camera, FVector(L.position[0], L.position[1], L.position[2])));
				if (D2 <= R2 && On(L, Hour))
				{
					Out.Emplace(D2, I);
				}
			}
		}
	}
}

void FGTAVLights::Update(AActor* Owner, const FVector& Camera, int32 Hour)
{
	if (Lights.IsEmpty() || !Owner)
	{
		return;
	}
	Hour = FMath::Clamp(Hour, 0, 23);
	const double Now = FPlatformTime::Seconds();
	if (Hour != LastHour || FVector::DistSquared(Camera, LastNear) > FMath::Square(300.0) || Now - LastNearTime > 1.0)
	{
		UpdateNear(Owner, Camera, Hour);
		LastNear = Camera;
		LastNearTime = Now;
	}
	if (Hour != LastHour || FVector::DistSquared(Camera, LastFar) > FMath::Square(20000.0))
	{
		UpdateFar(Owner, Camera, Hour);
		LastFar = Camera;
	}
	LastHour = Hour;
}

void FGTAVLights::UpdateNear(AActor* Owner, const FVector& Camera, int32 Hour)
{
	TArray<TPair<float, int32>> Near;
	Gather(Camera, NearRadius, Hour, Near);
	Near.Sort([](const TPair<float, int32>& A, const TPair<float, int32>& B) { return A.Key < B.Key; });
	if (Near.Num() > MaxNear)
	{
		Near.SetNum(MaxNear);
	}
	int32 UsedPoints = 0, UsedSpots = 0;
	for (const TPair<float, int32>& N : Near)
	{
		const GTAVPack::LodLight& L = Lights[N.Value];
		ULocalLightComponent* C = nullptr;
		if (IsSpot(L))
		{
			if (UsedSpots == Spots.Num() || !Spots[UsedSpots].IsValid())
			{
				USpotLightComponent* S = NewObject<USpotLightComponent>(Owner);
				S->SetMobility(EComponentMobility::Movable);
				S->SetCastShadows(false);
				S->IntensityUnits = ELightUnits::Candelas;
				S->RegisterComponent();
				if (UsedSpots == Spots.Num())
				{
					Spots.Add(S);
				}
				else
				{
					Spots[UsedSpots] = S;
				}
			}
			USpotLightComponent* S = Spots[UsedSpots++].Get();
			S->SetWorldRotation(FVector(L.direction[0], L.direction[1], L.direction[2]).Rotation());
			S->SetOuterConeAngle(FMath::Max(L.coneOuter * kConeDegrees, 1.0f));
			S->SetInnerConeAngle(FMath::Min(L.coneInner * kConeDegrees, L.coneOuter * kConeDegrees));
			C = S;
		}
		else
		{
			if (UsedPoints == Points.Num() || !Points[UsedPoints].IsValid())
			{
				UPointLightComponent* P = NewObject<UPointLightComponent>(Owner);
				P->SetMobility(EComponentMobility::Movable);
				P->SetCastShadows(false);
				P->IntensityUnits = ELightUnits::Candelas;
				P->RegisterComponent();
				if (UsedPoints == Points.Num())
				{
					Points.Add(P);
				}
				else
				{
					Points[UsedPoints] = P;
				}
			}
			C = Points[UsedPoints++].Get();
		}
		C->SetWorldLocation(FVector(L.position[0], L.position[1], L.position[2]));
		C->SetLightColor(FLinearColor(ColorOf(L)));
		C->SetIntensity(IntensityOf(L) * kCandela);
		C->SetAttenuationRadius(FMath::Max(L.falloff, 100.0f));
		C->SetVisibility(true);
	}
	for (int32 I = UsedPoints; I < Points.Num(); ++I)
	{
		if (UPointLightComponent* P = Points[I].Get())
		{
			P->SetVisibility(false);
		}
	}
	for (int32 I = UsedSpots; I < Spots.Num(); ++I)
	{
		if (USpotLightComponent* S = Spots[I].Get())
		{
			S->SetVisibility(false);
		}
	}
}

void FGTAVLights::UpdateFar(AActor* Owner, const FVector& Camera, int32 Hour)
{
	UInstancedStaticMeshComponent* G = Glow.Get();
	if (!G)
	{
		UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
		UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/GTAV/Materials/M_GTAV_Corona.M_GTAV_Corona"), nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!Sphere || !Material)
		{
			return;
		}
		G = NewObject<UInstancedStaticMeshComponent>(Owner);
		G->SetMobility(EComponentMobility::Movable);
		G->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		G->SetCastShadow(false);
		G->SetStaticMesh(Sphere);
		G->SetMaterial(0, Material);
		G->NumCustomDataFloats = 3;
		G->RegisterComponent();
		Glow = G;
	}
	TArray<TPair<float, int32>> Far;
	Gather(Camera, FarRadius, Hour, Far);
	G->ClearInstances();
	TArray<FTransform> Transforms;
	Transforms.Reserve(Far.Num());
	for (const TPair<float, int32>& F : Far)
	{
		const GTAVPack::LodLight& L = Lights[F.Value];
		// (a 30 cm bulb: the engine's sphere has a 50 cm radius; brighter coronas a little bigger)
		const float Scale = 0.6f * (1.0f + L.corona / 128.0f);
		Transforms.Emplace(FQuat::Identity, FVector(L.position[0], L.position[1], L.position[2]), FVector(Scale));
	}
	G->AddInstances(Transforms, false, false);
	for (int32 I = 0; I < Far.Num(); ++I)
	{
		const GTAVPack::LodLight& L = Lights[Far[I].Value];
		const FLinearColor C = FLinearColor(ColorOf(L)) * IntensityOf(L);
		const float Data[3] = {C.R, C.G, C.B};
		G->SetCustomData(I, MakeArrayView(Data, 3), false);
	}
	G->MarkRenderStateDirty();
}
