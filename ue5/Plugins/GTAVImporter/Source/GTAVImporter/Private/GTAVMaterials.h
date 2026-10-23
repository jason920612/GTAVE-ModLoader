#pragma once

#include "CoreMinimal.h"

class UMaterial;
class UMaterialInterface;
class UMaterialParameterCollection;
class UTexture2D;
class FJsonObject;

// Materials of the exported models (research/phase0.md §58.5): one master per draw bucket (0 opaque, 1 translucent,
// 2 decal, 3 cutout) and one for the four-layer terrain, an instance per distinct set of textures, the textures from
// the export's DDS.
class FGTAVMaterials
{
public:
	explicit FGTAVMaterials(const FString& InExportRoot);

	// The instance for one exported material (a "materials" entry of a model's json).
	// bOutTerrain: a four-layer terrain material (its mesh carries the blend weights as the vertex colour).
	UMaterialInterface* Get(const TSharedPtr<FJsonObject>& Material, bool& bOutTranslucent, bool& bOutTerrain);
	// Packages created or changed (to save).
	TArray<UPackage*> Dirty;
	int32 TexturesImported = 0;
	int32 TexturesMissing = 0;

	// The master material of a kind (0 opaque, 1 translucent, 3 cutout, 4 terrain, 6 water, 7 the sea, 9 the open sea
	// out to the horizon: opaque, so that the air's haze covers it as it does the land); the renderer makes its
	// instances at run time.
	UMaterial* Master(int32 Kind);
	// The distant lights' material (the renderer's LOD lights).
	UMaterial* Corona();

private:
	FString ExportRoot;
	TMap<int32, UMaterial*> Masters;
	UMaterialParameterCollection* GlobalParameters = nullptr; // MPC_GTAV
	TMap<FString, UMaterialInterface*> Instances;
	TMap<FString, UTexture2D*> Textures;

	static constexpr int32 kTerrain = 4, kWater = 6, kOcean = 7, kCorona = 8, kOceanFar = 9;
	UTexture2D* BlackMask();
	UMaterialParameterCollection* Collection();
	void Wetten(UMaterial* M);
	UTexture2D* Texture(const FString& Path, bool bLinear, bool bNormalMap, bool bPalette = false);
};
