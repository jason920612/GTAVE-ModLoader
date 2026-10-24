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
	TAutoConsoleVariable<float> CVarInteriorShadowRadius(TEXT("gtav.InteriorShadowRadius"), 1000.0f,
		TEXT("A room's lights cast shadows within this many metres of the camera (else they light the next room through the wall)."));
	TAutoConsoleVariable<float> CVarGameLightShadowRadius(TEXT("gtav.GameLightShadowRadius"), 60.0f,
		TEXT("The game's own lights cast shadows within this many metres of the camera (else a porch lamp lights the room behind the wall)."));
	TAutoConsoleVariable<int32> CVarGameLights(TEXT("gtav.GameLights"), 1, TEXT("Light with the game's own lights when the bridge has them (0: the map's data alone)."));
	// A game light inside the interior the camera is in, or this near it: the LOD lights here stand down (cm).
	constexpr float kIndoorLodClear = 3000.0f;
	// The game's intensity -> lux within its reach: as its LOD lights' byte (a lit window's 8 is about 42 there), see kLux.
	constexpr float kLuxPerGameUnit = 5.33f / 255.0f;

	// Where a light is, to 10 cm (Unreal's space).
	FIntVector Spot10(const float* Position)
	{
		return FIntVector(FMath::RoundToInt32(Position[0] / 10.0f), FMath::RoundToInt32(Position[1] / 10.0f), FMath::RoundToInt32(Position[2] / 10.0f));
	}
	// A lamp's lit lens as the game shows it (its corona): a disk of this radius (cm) giving this many candela per unit
	// of the model's corona intensity x its size squared (m): a traffic signal's 0.2 x 2^2 some 300 cd, as real ones.
	constexpr float kLensRadius = 10.0f, kLensCandela = 400.0f;

	// Where a light is, to a metre (Unreal's space).
	FIntVector Spot100(const float* Position)
	{
		return FIntVector(FMath::RoundToInt32(Position[0] / 100.0f), FMath::RoundToInt32(Position[1] / 100.0f), FMath::RoundToInt32(Position[2] / 100.0f));
	}
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
	LodSpots.Reset();
	for (int32 I = 0; I < Lights.Num(); ++I)
	{
		const GTAVPack::LodLight& L = Lights[I];
		Grid.FindOrAdd(CellOf(FVector(L.position[0], L.position[1], L.position[2]))).Add(I);
		LodSpots.Add(Spot100(L.position));
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
		E.bInterior = (Key >> 63) != 0; // (UGTAVWorldSubsystem's keys of an interior's entities)
		E.Length = M.type == 4 ? M.length * float(Transform.GetScale3D().X) : 0.0f;
		E.CoronaSize = M.coronaSize * float(Transform.GetScale3D().X);
		E.CoronaIntensity = M.coronaIntensity;
		E.Intensity = M.intensity;
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
		if (E.CoronaSize > 0 && E.CoronaIntensity > 0)
		{
			Coronas.Add(Spot10(L.position), {E.CoronaSize, E.CoronaIntensity, E.Intensity, Key});
		}
	}
	LastNearTime = 0; // (at the next update)
}

float FGTAVLights::LengthAt(int64 Key) const
{
	if (Key < 0)
	{
		return Extra[Key].Length;
	}
	// (a LOD light's capsule: its length in the outer cone's byte, x 1.8 m; measured against the models' own: 20 m 36,
	// 24 m 43, 5 m 9)
	const GTAVPack::LodLight& L = Lights[int32(Key)];
	return KindOf(L) == kCapsule ? L.coneOuter / 1.8f * 100.0f : 0.0f;
}

void FGTAVLights::Dump(const FVector& Point, float Radius) const
{
	for (const FGameActive& G : GameActive)
	{
		const FVector P(G.Light.position[0], G.Light.position[1], G.Light.position[2]);
		if (FVector::Dist(P, Point) <= Radius)
		{
			UE_LOG(LogGTAVLights, Display, TEXT("dump: game light type %u flags %08x at %.2f %.2f %.2f, intensity %.3f, reach %.2f m, cone %.0f/%.0f, colour %.2f %.2f %.2f%s"),
				G.Light.type, G.Light.flags, P.X / 100.0, -P.Y / 100.0, P.Z / 100.0, G.Light.intensity, G.Light.radius / 100.0f, G.Light.coneInner,
				G.Light.coneOuter, G.Light.color[0], G.Light.color[1], G.Light.color[2], G.Component.IsValid() && G.Component->CastShadows ? TEXT(", shadows") : TEXT(""));
		}
	}
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
		UE_LOG(LogGTAVLights, Display, TEXT("dump: light %s %lld at %.1f %.1f %.1f, %s %.1f, reach %.1f m, tube %.1f m, colour %s"), A.Key < 0 ? TEXT("entity") : TEXT("map"), A.Key,
			P.X, -P.Y, P.Z, Cast<USpotLightComponent>(C) ? TEXT("spot lux") : TEXT("point cd"), C->Intensity, C->AttenuationRadius / 100.0f,
			Cast<UPointLightComponent>(C) ? Cast<UPointLightComponent>(C)->SourceLength / 100.0f : 0.0f, *C->GetLightColor().ToString());
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
	if (const TArray<FEntityLight>* Gone = EntityLights.Find(Key))
	{
		for (const FEntityLight& E : *Gone)
		{
			if (const FCorona* C = Coronas.Find(Spot10(E.Light.position)); C && C->Entity == Key)
			{
				Coronas.Remove(Spot10(E.Light.position));
			}
		}
		EntityLights.Remove(Key);
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
	// (the game's lights each frame; from or back to the map's data alone: the near lights chosen anew)
	const bool bGame = UsingGameLights() && CVarGameLights.GetValueOnGameThread() != 0;
	if (bGame != bWasGame)
	{
		bWasGame = bGame;
		LastNearTime = 0;
	}
	if (bGame)
	{
		UpdateGame(Owner, Camera);
	}
	else if (!GameActive.IsEmpty())
	{
		for (const FGameActive& G : GameActive)
		{
			ReleaseLight(G.Component.Get());
		}
		GameActive.Reset();
		if (UInstancedStaticMeshComponent* G = NearGlow.Get())
		{
			G->ClearInstances();
			G->MarkRenderStateDirty();
		}
		NearGlowData.Reset();
	}
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
	TMap<FIntVector, int32> Taken; // (where a LOD light is: which)
	// (with the game's lights: a LOD light where the game has one of its own stands down, and in an interior those near
	// too - the house's windows seen from outside, its porch lamp)
	const bool bGame = bWasGame;
	const auto StandsDown = [&](const GTAVPack::LodLight& L, float D2) {
		if (!bGame)
		{
			return false;
		}
		if (bGameInside && D2 < kIndoorLodClear * kIndoorLodClear)
		{
			return true;
		}
		const FIntVector P = Spot100(L.position);
		for (int32 X = -1; X <= 1; ++X)
		{
			for (int32 Y = -1; Y <= 1; ++Y)
			{
				for (int32 Z = -1; Z <= 1; ++Z)
				{
					if (GameSpots.Contains(P + FIntVector(X, Y, Z)))
					{
						return true;
					}
				}
			}
		}
		return false;
	};
	{
		TArray<TPair<float, int32>> Map;
		Gather(Camera, NearRadius, Hour, Map);
		Near.Reserve(Map.Num());
		Taken.Reserve(Map.Num());
		for (const TPair<float, int32>& M : Map)
		{
			if (StandsDown(Lights[M.Value], M.Key))
			{
				continue;
			}
			Near.Emplace(M.Key, M.Value);
			Taken.Add(Spot50(Lights[M.Value]), M.Value);
		}
	}
	// A LOD light's capsule has no axis in the map's data: the drawn model's own light at the same place lends its
	// (the tube's length and the light stay the LOD light's).
	LodAxes.Reset();
	Extra.Reset();
	// (the game's lights are the entities' as it shows them)
	static const TMap<uint64, TArray<FEntityLight>> NoEntities;
	for (const TPair<uint64, TArray<FEntityLight>>& E : bGame ? NoEntities : EntityLights)
	{
		for (int32 I = 0; I < E.Value.Num(); ++I)
		{
			const GTAVPack::LodLight& L = E.Value[I].Light;
			const float D2 = float(FVector::DistSquared(Camera, FVector(L.position[0], L.position[1], L.position[2])));
			if (D2 > NearRadius * NearRadius || !On(L, Hour))
			{
				continue;
			}
			const int32* Lod = nullptr;
			const FIntVector P = Spot50(L);
			for (int32 X = -1; X <= 1 && !Lod; ++X)
			{
				for (int32 Y = -1; Y <= 1 && !Lod; ++Y)
				{
					for (int32 Z = -1; Z <= 1 && !Lod; ++Z)
					{
						Lod = Taken.Find(P + FIntVector(X, Y, Z));
					}
				}
			}
			if (Lod && E.Value[I].Length > 0 && !E.Value[I].Axis.IsNearlyZero() && KindOf(Lights[*Lod]) == kCapsule)
			{
				LodAxes.Add(*Lod, E.Value[I].Axis);
			}
			if (!Lod)
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
			ReleaseLight(It.Value().Get());
			It.RemoveCurrent();
		}
	}
	const bool bShadows = CVarLightShadows.GetValueOnGameThread() != 0;
	const double InteriorShadowReach2 = FMath::Square(double(CVarInteriorShadowRadius.GetValueOnGameThread()) * 100.0);
	const auto Take = [&](bool bSpot) { return TakeLight(Owner, bSpot); };
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
			// (a tube: the engine's capsule light lies along the light's z axis; a LOD light's from its model's light,
			// else a bulb)
			const FVector3f* Lent = N.Value >= 0 ? LodAxes.Find(int32(N.Value)) : nullptr;
			const FVector3f Axis = N.Value < 0 ? Extra[N.Value].Axis : Lent ? *Lent : FVector3f::ZeroVector;
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
		// (a spot lights its reach evenly, see kLux; a game light's component before may have been inverse squared)
		if (UPointLightComponent* P = Cast<UPointLightComponent>(C); P && P->bUseInverseSquaredFalloff == bSpot)
		{
			P->SetUseInverseSquaredFalloff(!bSpot);
			P->SetLightFalloffExponent(1.0f);
		}
		C->SetWorldLocation(FVector(L.position[0], L.position[1], L.position[2]));
		C->SetLightColor(ColorOf(L));
		C->SetIntensity(bSpot ? IlluminanceOf(L) : CandelaOf(L, Length));
		C->SetAttenuationRadius(FMath::Max(L.falloff, 100.0f));
		C->SetCastShadows(bShadows || (N.Value < 0 && Extra[N.Value].bInterior && FVector::DistSquared(Camera, C->GetComponentLocation()) < InteriorShadowReach2));
		C->SetVisibility(true);
		Active.Add(N.Value, C);
	}
	// (a LOD capsule shaped as a bulb before its model came in: its tube now)
	for (const TPair<int32, FVector3f>& A : LodAxes)
	{
		const TWeakObjectPtr<ULocalLightComponent>* Found = Active.Find(A.Key);
		if (UPointLightComponent* P = Found ? Cast<UPointLightComponent>(Found->Get()) : nullptr; P && !Cast<USpotLightComponent>(P))
		{
			P->SetWorldRotation(FRotationMatrix::MakeFromZ(FVector(A.Value)).Rotator());
			P->SetSourceLength(LengthAt(A.Key));
		}
	}
	// (which cast shadows: the setting, and a room's lights as the camera comes near and goes)
	bLastShadows = bShadows;
	for (const TPair<int64, TWeakObjectPtr<ULocalLightComponent>>& A : Active)
	{
		if (ULocalLightComponent* C = A.Value.Get())
		{
			const FEntityLight* E = A.Key < 0 ? Extra.Find(A.Key) : nullptr;
			const bool bCast = bShadows || (E && E->bInterior && FVector::DistSquared(Camera, C->GetComponentLocation()) < InteriorShadowReach2);
			if (C->CastShadows != bCast)
			{
				C->SetCastShadows(bCast);
			}
		}
	}
}

ULocalLightComponent* FGTAVLights::TakeLight(AActor* Owner, bool bSpot)
{
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
}

void FGTAVLights::ReleaseLight(ULocalLightComponent* C)
{
	if (C)
	{
		C->SetVisibility(false);
		(Cast<USpotLightComponent>(C) ? FreeSpots : FreePoints).Add(C);
	}
}

void FGTAVLights::SetGameLights(const TArray<GTAVBridge::GameLight>& InLights, bool bInside)
{
	GameLights.Reset(InLights.Num());
	GameSpots.Reset();
	for (const GTAVBridge::GameLight& In : InLights)
	{
		GTAVBridge::GameLight& L = GameLights.Add_GetRef(In);
		L.position[0] = In.position[0] * 100.0f, L.position[1] = -In.position[1] * 100.0f, L.position[2] = In.position[2] * 100.0f;
		L.direction[1] = -In.direction[1];
		L.tangent[1] = -In.tangent[1];
		L.radius = In.radius * 100.0f;
		L.length = In.length * 100.0f;
		GameSpots.Add(Spot100(L.position));
	}
	bGameInside = bInside;
	GameLightsTime = FPlatformTime::Seconds();
}

void FGTAVLights::UpdateGame(AActor* Owner, const FVector& Camera)
{
	const double ShadowReach2 = FMath::Square(double(CVarGameLightShadowRadius.GetValueOnGameThread()) * 100.0);
	const bool bAllShadows = CVarLightShadows.GetValueOnGameThread() != 0;
	const auto Position = [](const GTAVBridge::GameLight& L) { return FVector(L.position[0], L.position[1], L.position[2]); };
	// (to 2 cm, with the kind)
	const auto Key = [](const GTAVBridge::GameLight& L) {
		return FIntVector(FMath::RoundToInt32(L.position[0] * 0.5f), FMath::RoundToInt32(L.position[1] * 0.5f),
			FMath::RoundToInt32(L.position[2] * 0.5f) * 8 + int32(L.type & 7));
	};
	// Last frame's light each of this frame's is: the same where a lamp stays put, the nearest of its kind within 1.5 m
	// where one moved (a vehicle's) - each keeps its component, nothing made anew for it.
	TArray<int32> Match;
	Match.Init(INDEX_NONE, GameLights.Num());
	TBitArray<> Kept(false, GameActive.Num());
	TMap<FIntVector, int32> Exact;
	Exact.Reserve(GameActive.Num());
	for (int32 I = 0; I < GameActive.Num(); ++I)
	{
		Exact.Add(Key(GameActive[I].Light), I);
	}
	for (int32 J = 0; J < GameLights.Num(); ++J)
	{
		if (const int32* I = Exact.Find(Key(GameLights[J])); I && !Kept[*I])
		{
			Match[J] = *I;
			Kept[*I] = true;
		}
	}
	for (int32 J = 0; J < GameLights.Num(); ++J)
	{
		if (Match[J] != INDEX_NONE)
		{
			continue;
		}
		double Best = FMath::Square(150.0);
		int32 Found = INDEX_NONE;
		for (int32 I = 0; I < GameActive.Num(); ++I)
		{
			if (!Kept[I] && GameActive[I].Light.type == GameLights[J].type)
			{
				const double D2 = FVector::DistSquared(Position(GameActive[I].Light), Position(GameLights[J]));
				if (D2 < Best)
				{
					Best = D2;
					Found = I;
				}
			}
		}
		if (Found != INDEX_NONE)
		{
			Match[J] = Found;
			Kept[Found] = true;
		}
	}
	for (int32 I = 0; I < GameActive.Num(); ++I)
	{
		if (!Kept[I])
		{
			ReleaseLight(GameActive[I].Component.Get());
		}
	}
	const auto Differs = [](float A, float B) { return FMath::Abs(A - B) > 0.005f * FMath::Max(FMath::Abs(A), FMath::Abs(B)) + 1.0e-4f; };
	TArray<FGameActive> Next;
	Next.Reserve(GameLights.Num());
	for (int32 J = 0; J < GameLights.Num(); ++J)
	{
		const GTAVBridge::GameLight& L = GameLights[J];
		const bool bSpot = L.type == 2;
		ULocalLightComponent* C = Match[J] != INDEX_NONE ? GameActive[Match[J]].Component.Get() : nullptr;
		const GTAVBridge::GameLight* Was = C ? &GameActive[Match[J]].Light : nullptr;
		if (C && (Cast<USpotLightComponent>(C) != nullptr) != bSpot)
		{
			ReleaseLight(C);
			C = nullptr, Was = nullptr;
		}
		// (off as the game has it - a display's light, a lamp switched off - or a glimmer: none)
		const float Lux = L.intensity * kLuxPerGameUnit * kLux;
		if (Lux < 0.03f || L.radius < 1.0f)
		{
			ReleaseLight(C);
			continue;
		}
		if (!C)
		{
			C = TakeLight(Owner, bSpot);
		}
		const FVector P = Position(L);
		if (!Was || FVector::DistSquared(P, Position(*Was)) > 0.25)
		{
			C->SetWorldLocation(P);
		}
		const float Length = L.type == 4 ? L.length : 0.0f;
		if (USpotLightComponent* S = bSpot ? Cast<USpotLightComponent>(C) : nullptr)
		{
			const FVector3f D(L.direction[0], L.direction[1], L.direction[2]);
			if (!Was || (D | FVector3f(Was->direction[0], Was->direction[1], Was->direction[2])) < 0.99999f)
			{
				S->SetWorldRotation(FVector(D).Rotation());
			}
			if (!Was || Was->coneOuter != L.coneOuter || Was->coneInner != L.coneInner)
			{
				const float Outer = FMath::Clamp(L.coneOuter, 1.0f, 89.0f);
				S->SetOuterConeAngle(Outer);
				S->SetInnerConeAngle(FMath::Clamp(L.coneInner, 0.0f, Outer));
			}
		}
		else if (UPointLightComponent* Bulb = Cast<UPointLightComponent>(C))
		{
			// (a tube: the engine's capsule light lies along the light's z axis)
			const FVector3f Axis(L.tangent[0], L.tangent[1], L.tangent[2]);
			if (!Was || Was->length != Length || (Length > 0 && (Axis | FVector3f(Was->tangent[0], Was->tangent[1], Was->tangent[2])) < 0.99999f))
			{
				const bool bTube = Length > 0 && !Axis.IsNearlyZero();
				if (bTube)
				{
					Bulb->SetWorldRotation(FRotationMatrix::MakeFromZ(FVector(Axis)).Rotator());
				}
				Bulb->SetSourceLength(bTube ? Length : 0.0f);
			}
		}
		if (!Was || Differs(Was->intensity, L.intensity) || Differs(Was->radius, L.radius) || Was->length != L.length ||
			Was->falloffExponent != L.falloffExponent)
		{
			// Measured on the game's own picture (a scripted spot over flat ground, its linear scene colour): the game's
			// lights, spots too, fall off with the square of the distance, and the steeper its falloff exponent the
			// dimmer a light is past the first part of its reach - at half its reach about half as bright from an
			// exponent of 16 on as with 1. So: as bright as the game's level at half the reach, times that.
			const float Steep = 0.5f + 0.5f * FMath::Exp(-(FMath::Max(L.falloffExponent, 1.0f) - 1.0f) / 6.0f);
			C->SetIntensity(Lux * Steep * FMath::Square(L.radius / 200.0f) * (1.0f + 2.0f * Length / FMath::Max(L.radius, 1.0f)));
			C->SetAttenuationRadius(FMath::Max(L.radius, 100.0f));
		}
		// (the LOD lights' spots light their reach evenly instead: the shared components switch)
		if (UPointLightComponent* Light = Cast<UPointLightComponent>(C); Light && !Light->bUseInverseSquaredFalloff)
		{
			Light->SetUseInverseSquaredFalloff(true);
		}
		if (!Was || Was->color[0] != L.color[0] || Was->color[1] != L.color[1] || Was->color[2] != L.color[2])
		{
			C->SetLightColor(FLinearColor(L.color[0], L.color[1], L.color[2]));
		}
		// (near ones cast shadows: a lamp outside lights no room through its wall; but not the big outdoor ones - a street
		// light's bulb sits in its housing, which would shade it all, and the game's own street lights cast none)
		const bool bCast = bAllShadows || (FVector::DistSquared(Camera, P) < ShadowReach2 && L.radius <= 1000.0f);
		if (C->CastShadows != bCast)
		{
			C->SetCastShadows(bCast);
		}
		if (!C->IsVisible())
		{
			C->SetVisibility(true);
		}
		Next.Add({L, C});
	}
	GameActive = MoveTemp(Next);
	UpdateNearGlow(Owner);
}

void FGTAVLights::UpdateNearGlow(AActor* Owner)
{
	// The lamps the game has lit that show a glow (a traffic signal's lens: red, amber or green as the game has it),
	// where no LOD light's far glow is already.
	TArray<float> Data; // (x, y, z, r, g, b) each
	for (const GTAVBridge::GameLight& L : GameLights)
	{
		const FIntVector P = Spot10(L.position);
		const FCorona* C = nullptr;
		for (int32 X = -1; X <= 1 && !C; ++X)
		{
			for (int32 Y = -1; Y <= 1 && !C; ++Y)
			{
				for (int32 Z = -1; Z <= 1 && !C; ++Z)
				{
					C = Coronas.Find(P + FIntVector(X, Y, Z));
				}
			}
		}
		// (only the lamps of the game's movable objects - its flags' top bit: a traffic signal, whose lens is plain
		// material and lit only by the game's glow; a candle's, a lamp's in a house: the model's own lit geometry)
		if (!C || L.intensity <= 0 || !(L.flags & 0x80000000u))
		{
			continue;
		}
		const FIntVector M = Spot100(L.position);
		bool bLod = false;
		for (int32 X = -1; X <= 1 && !bLod; ++X)
		{
			for (int32 Y = -1; Y <= 1 && !bLod; ++Y)
			{
				for (int32 Z = -1; Z <= 1 && !bLod; ++Z)
				{
					bLod = LodSpots.Contains(M + FIntVector(X, Y, Z));
				}
			}
		}
		if (bLod)
		{
			continue;
		}
		// (as bright as the game has the light now against the model's own: dimmed, flickering, off)
		const float Now = C->LightIntensity > 0 ? FMath::Clamp(L.intensity / C->LightIntensity, 0.0f, 2.0f) : 1.0f;
		const float Candela = kLensCandela * C->Intensity * FMath::Square(C->Size / 100.0f) * Now;
		const float Scale = Candela / kCoronaCandela;
		Data.Append({L.position[0], L.position[1], L.position[2], L.color[0] * Scale, L.color[1] * Scale, L.color[2] * Scale});
	}
	if (Data == NearGlowData && NearGlow.IsValid())
	{
		return;
	}
	NearGlowData = Data;
	UInstancedStaticMeshComponent* G = NearGlow.Get();
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
		NearGlow = G;
	}
	G->ClearInstances();
	const int32 Count = Data.Num() / 6;
	TArray<FTransform> Transforms;
	Transforms.Reserve(Count);
	for (int32 I = 0; I < Count; ++I)
	{
		// (the engine's sphere is 100 cm across)
		Transforms.Emplace(FQuat::Identity, FVector(Data[6 * I], Data[6 * I + 1], Data[6 * I + 2]), FVector(kLensRadius / 50.0f));
	}
	G->AddInstances(Transforms, false, false);
	for (int32 I = 0; I < Count; ++I)
	{
		G->SetCustomData(I, MakeArrayView(&Data[6 * I + 3], 3), false);
	}
	G->MarkRenderStateDirty();
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
