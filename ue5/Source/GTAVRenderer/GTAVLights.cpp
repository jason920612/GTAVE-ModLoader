#include "GTAVLights.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInterface.h"

DEFINE_LOG_CATEGORY_STATIC(LogGTAVLights, Log, All);

namespace
{
	TAutoConsoleVariable<int32> CVarLights(TEXT("gtav.Lights"), 128, TEXT("How many of the map's lights near the camera are real lights (the presets)."));
	TAutoConsoleVariable<float> CVarLightRadius(TEXT("gtav.LightRadius"), 400.0f, TEXT("Real lights are chosen within this many metres (the presets)."));
	TAutoConsoleVariable<int32> CVarLightShadows(TEXT("gtav.LightShadows"), 0, TEXT("The real lights cast shadows (with MegaLights: the cinematic preset)."));
	constexpr float kCell = 25600.0f;
	// The game's light is a level of light within a reach (its falloff), the intensity byte -> that illuminance: 70 lux
	// at 255 (a street light, 170: 47 lux on the road, as real ones give 10-50). A spot light is a luminaire made to
	// light what it points at evenly (a street light's road, a billboard's face): its optics send more light the
	// farther the surface, so the illuminance stays about the same within its reach, falling off towards the edge
	// ((1 - (d / reach)^2): Unreal's non inverse-squared falloff, exponent 1). A point light is a bare bulb, falling
	// off with the square of the distance: as bright as that at half its reach (candela = lux x (reach / 2)^2).
	constexpr float kLux = 70.0f;
	// (M_GTAV_Corona's Candela: the custom data are the colour x candela over it)
	constexpr float kCoronaCandela = 4500.0f;

	// A light of a drawn entity, by the entity and its index (negative: the map's lights are 0 and up).
	int64 EntityLightKey(uint64 Entity, int32 Index)
	{
		uint64 H = Entity ^ (uint64(Index + 1) * 0x9E3779B97F4A7C15ull);
		H = (H ^ (H >> 30)) * 0xBF58476D1CE4E5B9ull;
		H = (H ^ (H >> 27)) * 0x94D049BB133111EBull;
		H ^= H >> 31;
		return -1 - int64(H & 0x3FFFFFFFFFFFFFFFull);
	}

	// Where a light is, to 50 cm.
	FIntVector Spot50(const GTAVPack::LodLight& L)
	{
		return FIntVector(FMath::RoundToInt32(L.position[0] / 50.0f), FMath::RoundToInt32(L.position[1] / 50.0f), FMath::RoundToInt32(L.position[2] / 50.0f));
	}
	// The game's cone bytes -> degrees (127.5: a half sphere).
	constexpr float kConeDegrees = 90.0f / 127.5f;

	// (linear, as the game lights with them: its sodium street lights' 255, 120, 10 an orange-yellow, as real ones are)
	FLinearColor ColorOf(const GTAVPack::LodLight& L)
	{
		return FColor(uint8(L.rgbi >> 16), uint8(L.rgbi >> 8), uint8(L.rgbi), 255).ReinterpretAsLinear();
	}

	// Lux within the reach.
	float IlluminanceOf(const GTAVPack::LodLight& L)
	{
		return float(L.rgbi >> 24) / 255.0f * kLux;
	}

	// How much more a capsule (a tube of that length) lights than a bulb of the same reach: the cylinder around it and
	// its ends over the sphere, (2 pi r length + pi r^2) / pi r^2.
	float TubeOf(const GTAVPack::LodLight& L, float Length)
	{
		return 1.0f + 2.0f * Length / FMath::Max(L.falloff, 1.0f);
	}

	// As a bulb (the corona; point lights).
	float CandelaOf(const GTAVPack::LodLight& L, float Length = 0)
	{
		return IlluminanceOf(L) * FMath::Square(L.falloff / 200.0f) * TubeOf(L, Length);
	}

	// The kind of light, in the flags' bits 24-28 (the LOD lights': 4 point, 8 spot, 16 capsule; the entities' lights
	// get the same).
	constexpr uint32 kPoint = 4, kSpot = 8, kCapsule = 16;
	uint32 KindOf(const GTAVPack::LodLight& L)
	{
		return (L.flags >> 24) & 0x1C;
	}

	bool IsSpot(const GTAVPack::LodLight& L)
	{
		return KindOf(L) == kSpot && L.coneOuter * kConeDegrees < 80.0f && FVector(L.direction[0], L.direction[1], L.direction[2]).SizeSquared() > 0.5f;
	}

	// The light it gives (lumens, but for a constant): lux over the lit area, a spot's cone of it.
	float FluxOf(const GTAVPack::LodLight& L, float Length)
	{
		const float Cone = IsSpot(L) ? FMath::Square(FMath::Sin(FMath::DegreesToRadians(L.coneOuter * kConeDegrees))) : 1.0f;
		return IlluminanceOf(L) * L.falloff * L.falloff * Cone * TubeOf(L, Length);
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

void FGTAVLights::AddEntity(uint64 Key, const FTransform& Transform, const TArray<GTAVPack::ModelLight>& Model)
{
	TArray<FEntityLight>& Out = EntityLights.FindOrAdd(Key);
	Out.Reset();
	for (const GTAVPack::ModelLight& M : Model)
	{
		FEntityLight& E = Out.AddDefaulted_GetRef();
		E.Length = M.type == 4 ? M.length * float(Transform.GetScale3D().X) : 0.0f;
		E.Axis = FVector3f(Transform.TransformVectorNoScale(FVector(M.tangent[0], M.tangent[1], M.tangent[2])).GetSafeNormal());
		GTAVPack::LodLight& L = E.Light;
		const FVector P = Transform.TransformPosition(FVector(M.position[0], M.position[1], M.position[2]));
		const FVector D = Transform.TransformVectorNoScale(FVector(M.direction[0], M.direction[1], M.direction[2])).GetSafeNormal();
		L.position[0] = float(P.X), L.position[1] = float(P.Y), L.position[2] = float(P.Z);
		L.direction[0] = float(D.X), L.direction[1] = float(D.Y), L.direction[2] = float(D.Z);
		L.falloff = M.falloff * float(Transform.GetScale3D().X);
		L.falloffExponent = M.falloffExponent;
		// (the game's intensity in the LOD lights' byte: a lit window, 8, is about 42 there)
		const uint32 I = uint32(FMath::Clamp(M.intensity * 5.33f, 0.0f, 255.0f));
		L.rgbi = (I << 24) | (uint32(M.color[0]) << 16) | (uint32(M.color[1]) << 8) | M.color[2];
		const bool bSpot = M.type == 2;
		L.flags = (M.timeFlags & 0xFFFFFF) | ((bSpot ? kSpot : M.type == 4 ? kCapsule : kPoint) << 24);
		L.coneInner = uint8(FMath::Clamp(M.coneInner / kConeDegrees, 0.0f, 127.0f));
		L.coneOuter = uint8(bSpot ? FMath::Clamp(M.coneOuter / kConeDegrees, 1.0f, 127.0f) : 127.0f);
	}
	LastNearTime = 0; // (at the next update)
}

float FGTAVLights::LengthAt(int64 Key) const
{
	if (Key < 0)
	{
		return Extra[Key].Length;
	}
	// (a LOD light's capsule: its length in the outer cone's byte, x 1.25 m; measured against the models' own)
	const GTAVPack::LodLight& L = Lights[int32(Key)];
	return KindOf(L) == kCapsule ? L.coneOuter / 1.25f * 100.0f : 0.0f;
}

void FGTAVLights::Dump(const FVector& Point, float Radius) const
{
	int32 Shown = 0;
	for (const TPair<int64, TWeakObjectPtr<ULocalLightComponent>>& A : Active)
	{
		const ULocalLightComponent* C = A.Value.Get();
		if (!C || FVector::Dist(C->GetComponentLocation(), Point) > Radius)
		{
			continue;
		}
		++Shown;
		const FVector P = C->GetComponentLocation() / 100.0;
		UE_LOG(LogGTAVLights, Display, TEXT("dump: light %s %lld at %.1f %.1f %.1f, %s %.1f, reach %.1f m, colour %s"), A.Key < 0 ? TEXT("entity") : TEXT("map"), A.Key,
			P.X, -P.Y, P.Z, Cast<USpotLightComponent>(C) ? TEXT("spot lux") : TEXT("point cd"), C->Intensity, C->AttenuationRadius / 100.0f, *C->GetLightColor().ToString());
	}
	int32 Candidates = 0;
	for (const TPair<uint64, TArray<FEntityLight>>& E : EntityLights)
	{
		for (const FEntityLight& L : E.Value)
		{
			Candidates += FVector::Dist(FVector(L.Light.position[0], L.Light.position[1], L.Light.position[2]), Point) <= Radius;
		}
	}
	UE_LOG(LogGTAVLights, Display, TEXT("dump: %d real lights of %d near; %d entity lights near; %d entities with lights"), Shown, Active.Num(), Candidates, EntityLights.Num());
}

void FGTAVLights::RemoveEntity(uint64 Key)
{
	if (EntityLights.Remove(Key))
	{
		LastNearTime = 0;
	}
}

void FGTAVLights::Update(AActor* Owner, const FVector& Camera, int32 Hour)
{
	if ((Lights.IsEmpty() && EntityLights.IsEmpty()) || !Owner)
	{
		return;
	}
	Hour = FMath::Clamp(Hour, 0, 23);
	const double Now = FPlatformTime::Seconds();
	if (Hour != LastHour || FVector::DistSquared(Camera, LastNear) > FMath::Square(1000.0) || Now - LastNearTime > 1.0)
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
	// Candidates: the map's LOD lights (their indices) and the drawn entities' own lights (keys below 0) but those a
	// LOD light already stands for: the LOD lights carry what the map changes per placed entity (a billboard's lamps
	// brighter and farther reaching than the lamp model's own), the models' lights add those the LOD lights leave out
	// (a sign's neon tubes; everything inside).
	TArray<TPair<float, int64>> Near;
	NearRadius = FMath::Max(CVarLightRadius.GetValueOnGameThread(), 10.0f) * 100.0f;
	TSet<FIntVector> Taken;
	{
		TArray<TPair<float, int32>> Map;
		Gather(Camera, NearRadius, Hour, Map);
		Near.Reserve(Map.Num());
		Taken.Reserve(Map.Num());
		for (const TPair<float, int32>& M : Map)
		{
			Near.Emplace(M.Key, M.Value);
			Taken.Add(Spot50(Lights[M.Value]));
		}
	}
	Extra.Reset();
	for (const TPair<uint64, TArray<FEntityLight>>& E : EntityLights)
	{
		for (int32 I = 0; I < E.Value.Num(); ++I)
		{
			const GTAVPack::LodLight& L = E.Value[I].Light;
			const float D2 = float(FVector::DistSquared(Camera, FVector(L.position[0], L.position[1], L.position[2])));
			if (D2 > NearRadius * NearRadius || !On(L, Hour))
			{
				continue;
			}
			bool bTaken = false;
			const FIntVector P = Spot50(L);
			for (int32 X = -1; X <= 1 && !bTaken; ++X)
			{
				for (int32 Y = -1; Y <= 1 && !bTaken; ++Y)
				{
					for (int32 Z = -1; Z <= 1 && !bTaken; ++Z)
					{
						bTaken = Taken.Contains(P + FIntVector(X, Y, Z));
					}
				}
			}
			if (!bTaken)
			{
				const int64 Key = EntityLightKey(E.Key, I);
				Extra.Add(Key, E.Value[I]);
				Near.Emplace(D2, Key);
			}
		}
	}
	// The lights that matter most to the picture first: a lit surface is as bright from afar as from near, it only
	// takes less of the picture, so the light given / distance squared.
	for (TPair<float, int64>& N : Near)
	{
		N.Key = -FluxOf(LightAt(N.Value), LengthAt(N.Value)) / FMath::Max(N.Key, 1.0e4f);
	}
	const int32 Most = FMath::Max(CVarLights.GetValueOnGameThread(), 0);
	if (Near.Num() > Most)
	{
		Near.Sort([](const TPair<float, int64>& A, const TPair<float, int64>& B) { return A.Key < B.Key; });
		Near.SetNum(Most);
	}
	TSet<int64> Chosen;
	Chosen.Reserve(Near.Num());
	for (const TPair<float, int64>& N : Near)
	{
		Chosen.Add(N.Value);
	}
	// Lights no longer chosen give their components back; the newly chosen take free ones (or new ones). The others
	// keep theirs untouched (no render state rebuilt for them).
	for (auto It = Active.CreateIterator(); It; ++It)
	{
		if (!Chosen.Contains(It.Key()))
		{
			if (ULocalLightComponent* C = It.Value().Get())
			{
				C->SetVisibility(false);
				(Cast<USpotLightComponent>(C) ? FreeSpots : FreePoints).Add(C);
			}
			It.RemoveCurrent();
		}
	}
	const bool bShadows = CVarLightShadows.GetValueOnGameThread() != 0;
	const auto Take = [&](bool bSpot) -> ULocalLightComponent* {
		TArray<TWeakObjectPtr<ULocalLightComponent>>& Free = bSpot ? FreeSpots : FreePoints;
		while (!Free.IsEmpty())
		{
			if (ULocalLightComponent* C = Free.Pop(EAllowShrinking::No).Get())
			{
				return C;
			}
		}
		UPointLightComponent* C = bSpot ? NewObject<USpotLightComponent>(Owner) : NewObject<UPointLightComponent>(Owner);
		C->SetMobility(EComponentMobility::Movable);
		C->SetCastShadows(false);
		C->IntensityUnits = ELightUnits::Candelas;
		if (bSpot)
		{
			C->SetUseInverseSquaredFalloff(false); // (lux, see kLux)
			C->SetLightFalloffExponent(1.0f);
		}
		C->RegisterComponent();
		return C;
	};
	for (const TPair<float, int64>& N : Near)
	{
		if (Active.Contains(N.Value))
		{
			continue;
		}
		const GTAVPack::LodLight& L = LightAt(N.Value);
		const bool bSpot = IsSpot(L);
		ULocalLightComponent* C = Take(bSpot);
		const float Length = LengthAt(N.Value);
		if (USpotLightComponent* S = bSpot ? Cast<USpotLightComponent>(C) : nullptr)
		{
			S->SetWorldRotation(FVector(L.direction[0], L.direction[1], L.direction[2]).Rotation());
			S->SetOuterConeAngle(FMath::Max(L.coneOuter * kConeDegrees, 1.0f));
			S->SetInnerConeAngle(FMath::Min(L.coneInner * kConeDegrees, L.coneOuter * kConeDegrees));
		}
		else if (UPointLightComponent* P = Cast<UPointLightComponent>(C))
		{
			// (a tube: the engine's capsule light lies along the light's z axis; a LOD light's has no axis, a bulb then)
			const FVector3f Axis = N.Value < 0 ? Extra[N.Value].Axis : FVector3f::ZeroVector;
			if (Length > 0 && !Axis.IsNearlyZero())
			{
				P->SetWorldRotation(FRotationMatrix::MakeFromZ(FVector(Axis)).Rotator());
				P->SetSourceLength(Length);
			}
			else
			{
				P->SetSourceLength(0.0f);
			}
		}
		C->SetWorldLocation(FVector(L.position[0], L.position[1], L.position[2]));
		C->SetLightColor(ColorOf(L));
		C->SetIntensity(bSpot ? IlluminanceOf(L) : CandelaOf(L, Length));
		C->SetAttenuationRadius(FMath::Max(L.falloff, 100.0f));
		C->SetCastShadows(bShadows);
		C->SetVisibility(true);
		Active.Add(N.Value, C);
	}
	if (bShadows != bLastShadows)
	{
		bLastShadows = bShadows;
		for (const TPair<int64, TWeakObjectPtr<ULocalLightComponent>>& A : Active)
		{
			if (ULocalLightComponent* C = A.Value.Get())
			{
				C->SetCastShadows(bShadows);
			}
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
		const FLinearColor C = ColorOf(L) * (CandelaOf(L) / kCoronaCandela);
		const float Data[3] = {C.R, C.G, C.B};
		G->SetCustomData(I, MakeArrayView(Data, 3), false);
	}
	G->MarkRenderStateDirty();
}
