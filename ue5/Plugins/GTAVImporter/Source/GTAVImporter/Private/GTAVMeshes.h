#pragma once

#include "CoreMinimal.h"

class UStaticMesh;
class FGTAVMaterials;

// The exported models (models\<name>.gmesh + .json; research/phase0.md §58.4) as Nanite static meshes.
class FGTAVMeshes
{
public:
	FGTAVMeshes(const FString& InExportRoot, FGTAVMaterials& InMaterials);

	// The mesh of an archetype: loaded if it was imported before, else created (built later by Build()).
	UStaticMesh* Get(const FString& Name);
	// Builds the meshes created since the last call (in parallel).
	void Build();

	TArray<UPackage*> Dirty;
	int32 Created = 0;
	int32 Failed = 0;

private:
	FString ExportRoot;
	FGTAVMaterials& Materials;
	TMap<FString, UStaticMesh*> Meshes;
	TArray<UStaticMesh*> ToBuild;

	UStaticMesh* Create(const FString& Name, const FString& PackageName);
};
