#pragma once

#include "CoreMinimal.h"

class UMaterial;
class UMaterialInterface;
class UTexture2D;
class FJsonObject;

// Materials of the exported models (research/phase0.md §58.5): one master per draw bucket (0 opaque, 1 translucent,
// 2 decal, 3 cutout), an instance per distinct set of textures and constants, the textures from the export's DDS.
class FGTAVMaterials
{
public:
	explicit FGTAVMaterials(const FString& InExportRoot);

	// The instance for one exported material (a "materials" entry of a model's json).
	UMaterialInterface* Get(const TSharedPtr<FJsonObject>& Material, bool& bOutTranslucent);
	// Packages created or changed (to save).
	TArray<UPackage*> Dirty;
	int32 TexturesImported = 0;
	int32 TexturesMissing = 0;

private:
	FString ExportRoot;
	TMap<int32, UMaterial*> Masters;
	TMap<FString, UMaterialInterface*> Instances;
	TMap<FString, UTexture2D*> Textures;

	UMaterial* Master(int32 Bucket);
	UTexture2D* Texture(const FString& Path, bool bLinear, bool bNormalMap);
};
