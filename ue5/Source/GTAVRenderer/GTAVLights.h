#pragma once

#include "CoreMinimal.h"

#include "GTAVBridgeProtocol.h"
#include "GTAVPackFormat.h"

class AActor;
class UInstancedStaticMeshComponent;
class ULocalLightComponent;
class UPointLightComponent;
class UStaticMesh;
class USpotLightComponent;

// The map's lights at night (research/phase0.md §59.8): the LOD lights the game has for street lights, windows and
// signs, by their hours. Near the camera they are real lights (spot or point, no shadows, the nearest first); out to
// the distance they are glowing points (one instanced mesh; the material keeps a far light a few pixels wide).
// With the game's own lights each frame (§59.12: what it draws, switched and faded as it does, a vehicle's lamps among
// them) those are the real lights near the camera, and the LOD lights only stand in where the game has none.
class FGTAVLights
{
public:
	void Load(TArray<GTAVPack::LodLight>&& InLights);
	int32 Num() const { return Lights.Num(); }
	// (checking: the real lights on, the components made, the drawn entities with lights)
	FString Counts() const
	{
		return FString::Printf(TEXT("%d real lights (%d free), %d entities with lights, %d of the game's"), Active.Num(), FreePoints.Num() + FreeSpots.Num(),
			EntityLights.Num(), GameActive.Num());
	}
	// The game's lights this frame (the bridge's, the game's space); bInside: the game's camera is in an interior.
	void SetGameLights(const TArray<GTAVBridge::GameLight>& InLights, bool bInside);
	// The game's lights came of late: they light the scene (a vehicle's lamps too).
	bool UsingGameLights() const { return FPlatformTime::Seconds() - GameLightsTime < 1.0; }

	// Game thread, each frame: Owner gets the components; Camera in Unreal's space; Hour 0-23.
	void Update(AActor* Owner, const FVector& Camera, int32 Hour);
	// The lights a drawn entity's model carries, placed by its transform (where the map has no LOD light of its own
	// for them); gone with the entity.
	void AddEntity(uint64 Key, const FTransform& Transform, const TArray<GTAVPack::ModelLight>& Model);
	void RemoveEntity(uint64 Key);
	// (checking: the real lights near a point, to the log)
	void Dump(const FVector& Point, float Radius) const;
	// The shapes the glows are drawn with, far and near (closed shapes 100 cm across, every corner as far from the
	// centre; null: the engine's sphere): the near ones finer (seen tens of pixels across).
	void SetGlowShape(UStaticMesh* Far, UStaticMesh* Near)
	{
		GlowShape = Far;
		NearGlowShape = Near;
	}

	float NearRadius = 40000.0f;  // cm: real lights within (gtav.Lights of them, those that matter most)
	float FarRadius = 500000.0f;  // cm: glowing points within

private:
	// An entity's light: as a LOD light, and a capsule's length (cm; 0 for the others) and axis.
	struct FEntityLight
	{
		GTAVPack::LodLight Light;
		float Length = 0;
		FVector3f Axis = FVector3f::ZeroVector;
		bool bInterior = false; // (a room's: casts shadows always, else it lights the next room through the wall)
		float CoronaSize = 0, CoronaIntensity = 0, Intensity = 0; // (the model's: its glow, cm; its light's intensity)
	};
	// Where the drawn entities' lights with a glow are (10 cm cells): a game light there shows its lamp lit.
	struct FCorona
	{
		float Size = 0, Intensity = 0, LightIntensity = 0;
		uint64 Entity = 0;
	};
	TMap<FIntVector, FCorona> Coronas;
	TSet<FIntVector> LodSpots; // where the LOD lights are, to a metre (their far glow is there already)
	TWeakObjectPtr<UInstancedStaticMeshComponent> NearGlow;
	TArray<float> NearGlowData; // (what it shows: rebuilt when that changes)
	void UpdateNearGlow(AActor* Owner);

	TArray<GTAVPack::LodLight> Lights;
	TMap<FIntPoint, TArray<int32>> Grid; // 256 m cells
	TMap<uint64, TArray<FEntityLight>> EntityLights; // (in the world)
	TMap<int64, TWeakObjectPtr<ULocalLightComponent>> Active; // the real lights by the light they are
	TArray<TWeakObjectPtr<ULocalLightComponent>> FreePoints, FreeSpots;
	bool bLastShadows = false;
	TWeakObjectPtr<UInstancedStaticMeshComponent> Glow, GlowRing;
	TArray<float> RingData; // (what the ring shows: rebuilt when that changes)
	double LastRingTime = 0;
	TWeakObjectPtr<UStaticMesh> GlowShape, NearGlowShape;
	UStaticMesh* GlowMesh() const;
	bool StandsDown(const GTAVPack::LodLight& L, float D2) const;
	void UpdateRing(AActor* Owner, const FVector& Camera, int32 Hour);
	FVector LastNear = FVector(1e30), LastFar = FVector(1e30);
	int32 LastHour = -1;
	double LastNearTime = 0;

	static FIntPoint CellOf(const FVector& P);
	bool On(const GTAVPack::LodLight& L, int32 Hour) const { return (L.flags >> Hour) & 1; }
	void Gather(const FVector& Camera, float Radius, int32 Hour, TArray<TPair<float, int32>>& Out) const;
	TMap<int64, FEntityLight> Extra; // the entities' lights near, by their keys (negative)
	TMap<int32, FVector3f> LodAxes;  // LOD capsule lights' axes, from their models' lights
	const GTAVPack::LodLight& LightAt(int64 Key) const { return Key >= 0 ? Lights[int32(Key)] : Extra[Key].Light; }
	float LengthAt(int64 Key) const;
	void UpdateNear(AActor* Owner, const FVector& Camera, int32 Hour);
	void UpdateFar(AActor* Owner, const FVector& Camera, int32 Hour);

	// The game's lights: this frame's (Unreal's space: cm, y mirrored), those lit last frame and their components.
	struct FGameActive
	{
		GTAVBridge::GameLight Light;
		TWeakObjectPtr<ULocalLightComponent> Component;
	};
	TArray<GTAVBridge::GameLight> GameLights;
	TArray<FGameActive> GameActive;
	TSet<FIntVector> GameSpots; // where they are, to a metre (a LOD light there stands down)
	double GameLightsTime = -1e9;
	bool bGameInside = false, bWasGame = false;
	void UpdateGame(AActor* Owner, const FVector& Camera);
	ULocalLightComponent* TakeLight(AActor* Owner, bool bSpot);
	void ReleaseLight(ULocalLightComponent* C);
};
