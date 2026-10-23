#pragma once

#include "CoreMinimal.h"

#include "GTAVPackFormat.h"

class AActor;
class UInstancedStaticMeshComponent;
class ULocalLightComponent;
class UPointLightComponent;
class USpotLightComponent;

// The map's lights at night (research/phase0.md §59.8): the LOD lights the game has for street lights, windows and
// signs, by their hours. Near the camera they are real lights (spot or point, no shadows, the nearest first); out to
// the distance they are glowing points (one instanced mesh; the material keeps a far light a few pixels wide).
class FGTAVLights
{
public:
	void Load(TArray<GTAVPack::LodLight>&& InLights);
	int32 Num() const { return Lights.Num(); }
	// (checking: the real lights on, the components made, the drawn entities with lights)
	FString Counts() const { return FString::Printf(TEXT("%d real lights (%d free), %d entities with lights"), Active.Num(), FreePoints.Num() + FreeSpots.Num(), EntityLights.Num()); }

	// Game thread, each frame: Owner gets the components; Camera in Unreal's space; Hour 0-23.
	void Update(AActor* Owner, const FVector& Camera, int32 Hour);
	// The lights a drawn entity's model carries, placed by its transform (where the map has no LOD light of its own
	// for them); gone with the entity.
	void AddEntity(uint64 Key, const FTransform& Transform, const TArray<GTAVPack::ModelLight>& Model);
	void RemoveEntity(uint64 Key);
	// (checking: the real lights near a point, to the log)
	void Dump(const FVector& Point, float Radius) const;

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
	};

	TArray<GTAVPack::LodLight> Lights;
	TMap<FIntPoint, TArray<int32>> Grid; // 256 m cells
	TMap<uint64, TArray<FEntityLight>> EntityLights; // (in the world)
	TMap<int64, TWeakObjectPtr<ULocalLightComponent>> Active; // the real lights by the light they are
	TArray<TWeakObjectPtr<ULocalLightComponent>> FreePoints, FreeSpots;
	bool bLastShadows = false;
	TWeakObjectPtr<UInstancedStaticMeshComponent> Glow;
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
};
