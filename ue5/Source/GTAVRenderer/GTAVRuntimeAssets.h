#pragma once

#include "CoreMinimal.h"
#include "UObject/GCObject.h"

#include "GTAVPack.h"

class UMaterial;
class UMaterialInterface;
class UStaticMesh;
class UTexture2D;

// The game's models, textures and materials made at run time from the pack (research/phase0.md §59): textures keep
// the game's own compressed data (no re-encoding: the same memory as the game), meshes are built from the converted
// vertices, materials are instances of the master materials (/Game/GTAV/Materials).
class FGTAVRuntimeAssets : public FGCObject
{
public:
	explicit FGTAVRuntimeAssets(const FGTAVPack& InPack);

	// A model by its key (the archetype's name hash); nullptr if it is not in the pack or cannot be built.
	UStaticMesh* Mesh(uint64 Key);

	int32 MeshesBuilt = 0, MeshesFailed = 0, TexturesMade = 0, TexturesFailed = 0, MaterialsMade = 0;
	int64 TextureBytes = 0;

	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override { return TEXT("FGTAVRuntimeAssets"); }

private:
	const FGTAVPack& Pack;
	TMap<uint64, TObjectPtr<UStaticMesh>> Meshes;
	TMap<uint64, TObjectPtr<UTexture2D>> Textures; // key ^ usage
	TMap<uint32, TObjectPtr<UMaterialInterface>> Materials; // by the material's bytes
	TMap<int32, TObjectPtr<UMaterial>> Masters;
	TObjectPtr<UMaterialInterface> Fallback;

	enum class EUsage : uint8 { Color, Linear, Normal, Palette };
	UTexture2D* Texture(uint64 Key, EUsage Usage, int32* OutWidth = nullptr, int32* OutHeight = nullptr);
	UMaterialInterface* Material(const GTAVPack::Material& M);
	UMaterial* Master(int32 Kind);
};
