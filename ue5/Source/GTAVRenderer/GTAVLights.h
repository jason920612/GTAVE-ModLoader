#pragma once

#include "CoreMinimal.h"

#include "GTAVPackFormat.h"

class AActor;
class UInstancedStaticMeshComponent;
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

	// Game thread, each frame: Owner gets the components; Camera in Unreal's space; Hour 0-23.
	void Update(AActor* Owner, const FVector& Camera, int32 Hour);

	float NearRadius = 12000.0f;  // cm: real lights within (gtav.Lights of them, the nearest)
	float FarRadius = 500000.0f;  // cm: glowing points within

private:
	TArray<GTAVPack::LodLight> Lights;
	TMap<FIntPoint, TArray<int32>> Grid; // 256 m cells
	TArray<TWeakObjectPtr<UPointLightComponent>> Points;
	TArray<TWeakObjectPtr<USpotLightComponent>> Spots;
	TWeakObjectPtr<UInstancedStaticMeshComponent> Glow;
	FVector LastNear = FVector(1e30), LastFar = FVector(1e30);
	int32 LastHour = -1;
	double LastNearTime = 0;

	static FIntPoint CellOf(const FVector& P);
	bool On(const GTAVPack::LodLight& L, int32 Hour) const { return (L.flags >> Hour) & 1; }
	void Gather(const FVector& Camera, float Radius, int32 Hour, TArray<TPair<float, int32>>& Out) const;
	void UpdateNear(AActor* Owner, const FVector& Camera, int32 Hour);
	void UpdateFar(AActor* Owner, const FVector& Camera, int32 Hour);
};
