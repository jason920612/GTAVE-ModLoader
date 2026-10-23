#include "GTAVMeshes.h"

#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"

#include "GTAVConvert.h"
#include "GTAVMaterials.h"

DEFINE_LOG_CATEGORY_STATIC(LogGTAVMeshes, Log, All);

namespace
{
	// A cursor over the gmesh bytes.
	struct FReader
	{
		const TArray<uint8>& Data;
		int64 At = 0;
		bool bOk = true;

		template <typename T>
		T Get()
		{
			T Value{};
			if (At + int64(sizeof(T)) > Data.Num())
			{
				bOk = false;
				return Value;
			}
			FMemory::Memcpy(&Value, Data.GetData() + At, sizeof(T));
			At += sizeof(T);
			return Value;
		}
		template <typename T>
		TArray<T> Array(int64 Count)
		{
			TArray<T> Out;
			if (Count < 0 || At + Count * int64(sizeof(T)) > Data.Num())
			{
				bOk = false;
				return Out;
			}
			Out.SetNumUninitialized(Count);
			FMemory::Memcpy(Out.GetData(), Data.GetData() + At, Count * sizeof(T));
			At += Count * sizeof(T);
			return Out;
		}
	};

}

FGTAVMeshes::FGTAVMeshes(const FString& InExportRoot, FGTAVMaterials& InMaterials)
	: ExportRoot(InExportRoot), Materials(InMaterials)
{
}

UStaticMesh* FGTAVMeshes::Get(const FString& Name)
{
	if (UStaticMesh** Found = Meshes.Find(Name))
	{
		return *Found;
	}
	const FString Asset = GTAV::AssetName(Name);
	const FString PackageName = TEXT("/Game/GTAV/Models/") + Asset;
	UStaticMesh* Mesh = nullptr;
	if (FPackageName::DoesPackageExist(PackageName))
	{
		Mesh = LoadObject<UStaticMesh>(nullptr, *(PackageName + TEXT(".") + Asset), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	if (!Mesh)
	{
		Mesh = Create(Name, PackageName);
	}
	return Meshes.Add(Name, Mesh);
}

UStaticMesh* FGTAVMeshes::Create(const FString& Name, const FString& PackageName)
{
	TArray<uint8> Data;
	FString JsonText;
	const FString Base = FPaths::Combine(ExportRoot, TEXT("models"), Name);
	TSharedPtr<FJsonObject> Json;
	if (!FFileHelper::LoadFileToArray(Data, *(Base + TEXT(".gmesh"))) || !FFileHelper::LoadFileToString(JsonText, *(Base + TEXT(".json"))) ||
		!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JsonText), Json) || !Json.IsValid())
	{
		UE_LOG(LogGTAVMeshes, Warning, TEXT("%s: not exported"), *Name);
		++Failed;
		return nullptr;
	}
	FReader R{Data};
	if (R.Get<uint32>() != 0x48534D47 /* GMSH */ || R.Get<uint32>() != 1)
	{
		UE_LOG(LogGTAVMeshes, Warning, TEXT("%s: not a gmesh (version 1)"), *Name);
		++Failed;
		return nullptr;
	}
	const uint32 SectionCount = R.Get<uint32>();
	R.Array<float>(6); // bounds

	FMeshDescription Description;
	FStaticMeshAttributes Attributes(Description);
	Attributes.Register();
	TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
	TVertexInstanceAttributesRef<float> BinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
	TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
	TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
	TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();

	const TArray<TSharedPtr<FJsonValue>>* MaterialsJson = nullptr;
	Json->TryGetArrayField(TEXT("materials"), MaterialsJson);
	TMap<uint32, FPolygonGroupID> Groups; // material -> polygon group
	TArray<UMaterialInterface*> SlotMaterials;
	TArray<FName> SlotOrder;
	TSet<uint32> TerrainMaterials;
	bool bTranslucent = false, bNormals = true, bTangents = true;
	int32 UvChannels = 1;
	for (uint32 S = 0; S < SectionCount && R.bOk; ++S)
	{
		const uint32 Material = R.Get<uint32>(), VertexCount = R.Get<uint32>(), IndexCount = R.Get<uint32>(), Flags = R.Get<uint32>(), UvCount = R.Get<uint32>();
		const TArray<float> P = R.Array<float>(3 * int64(VertexCount));
		const TArray<float> N = (Flags & 1) ? R.Array<float>(3 * int64(VertexCount)) : TArray<float>();
		const TArray<float> T = (Flags & 2) ? R.Array<float>(4 * int64(VertexCount)) : TArray<float>();
		const TArray<uint8> C0 = (Flags & 4) ? R.Array<uint8>(4 * int64(VertexCount)) : TArray<uint8>();
		const TArray<uint8> C1 = (Flags & 8) ? R.Array<uint8>(4 * int64(VertexCount)) : TArray<uint8>();
		TArray<TArray<float>> Uv;
		for (uint32 K = 0; K < UvCount; ++K)
		{
			Uv.Add(R.Array<float>(2 * int64(VertexCount)));
		}
		const TArray<uint32> I = R.Array<uint32>(IndexCount);
		if (!R.bOk)
		{
			break;
		}
		bNormals &= (Flags & 1) != 0;
		bTangents &= (Flags & 2) != 0;
		if (int32(UvCount) > UvChannels)
		{
			UvChannels = FMath::Min<int32>(UvCount, 8);
			UVs.SetNumChannels(UvChannels);
		}
		FPolygonGroupID* Group = Groups.Find(Material);
		if (!Group)
		{
			const FName Slot(*FString::Printf(TEXT("m%u"), Material));
			Group = &Groups.Add(Material, Description.CreatePolygonGroup());
			SlotNames[*Group] = Slot;
			bool bThisTranslucent = false, bThisTerrain = false;
			UMaterialInterface* MI = MaterialsJson && MaterialsJson->IsValidIndex(Material)
				? Materials.Get((*MaterialsJson)[Material]->AsObject(), bThisTranslucent, bThisTerrain) : nullptr;
			bTranslucent |= bThisTranslucent;
			SlotMaterials.Add(MI);
			SlotOrder.Add(Slot);
			if (bThisTerrain)
			{
				TerrainMaterials.Add(Material);
			}
		}
		// Terrain: the blend weights are the second colour set (green, blue); red keeps the first set's red.
		const bool bTerrainColors = TerrainMaterials.Contains(Material) && C1.Num();
		TArray<FVertexInstanceID> Instances;
		Instances.SetNum(VertexCount);
		for (uint32 V = 0; V < VertexCount; ++V)
		{
			const FVertexID Vertex = Description.CreateVertex();
			Positions[Vertex] = GTAV::Point(P[3 * V], P[3 * V + 1], P[3 * V + 2]);
			const FVertexInstanceID Instance = Description.CreateVertexInstance(Vertex);
			Instances[V] = Instance;
			if (N.Num())
			{
				Normals[Instance] = GTAV::Direction(N[3 * V], N[3 * V + 1], N[3 * V + 2]);
			}
			if (T.Num())
			{
				Tangents[Instance] = GTAV::Direction(T[4 * V], T[4 * V + 1], T[4 * V + 2]);
				BinormalSigns[Instance] = T[4 * V + 3] < 0 ? 1.0f : -1.0f; // (mirrored: the sign flips)
			}
			// (the exact inverse of the engine's sRGB conversion when it builds the mesh: the material reads the game's bytes)
			FColor Color(255, 255, 255, 255);
			if (bTerrainColors)
			{
				Color = FColor(C0.Num() ? C0[4 * V] : 255, C1[4 * V + 1], C1[4 * V + 2], 255);
			}
			else if (C0.Num())
			{
				Color = FColor(C0[4 * V], C0[4 * V + 1], C0[4 * V + 2], C0[4 * V + 3]);
			}
			const FLinearColor Linear = FLinearColor::FromSRGBColor(Color);
			Colors[Instance] = FVector4f(Linear.R, Linear.G, Linear.B, Linear.A);
			for (int32 K = 0; K < UvChannels; ++K)
			{
				UVs.Set(Instance, K, K < Uv.Num() ? FVector2f(Uv[K][2 * V], Uv[K][2 * V + 1]) : FVector2f::ZeroVector);
			}
		}
		for (uint32 K = 0; K + 2 < IndexCount; K += 3)
		{
			const uint32 A = I[K], B = I[K + 1], C = I[K + 2];
			if (A == B || B == C || A == C)
			{
				continue;
			}
			// (the game's order as it is: mirroring flips the winding, and the engine's front faces wind the other way)
			Description.CreateTriangle(*Group, {Instances[A], Instances[B], Instances[C]});
		}
	}
	if (!R.bOk || Description.Triangles().Num() == 0)
	{
		UE_LOG(LogGTAVMeshes, Warning, TEXT("%s: no geometry"), *Name);
		++Failed;
		return nullptr;
	}

	UPackage* Package = CreatePackage(*PackageName);
	UStaticMesh* Mesh = NewObject<UStaticMesh>(Package, FName(*GTAV::AssetName(Name)), RF_Public | RF_Standalone);
	Mesh->SetNumSourceModels(1);
	FStaticMeshSourceModel& Source = Mesh->GetSourceModel(0);
	Source.BuildSettings.bRecomputeNormals = !bNormals;
	Source.BuildSettings.bRecomputeTangents = !bTangents;
	Source.BuildSettings.bGenerateLightmapUVs = false;
	Source.BuildSettings.bRemoveDegenerates = true;
	Mesh->CreateMeshDescription(0, MoveTemp(Description));
	Mesh->CommitMeshDescription(0);
	// One slot per polygon group, matched by name.
	for (int32 K = 0; K < SlotMaterials.Num(); ++K)
	{
		Mesh->GetStaticMaterials().Add(FStaticMaterial(SlotMaterials[K], SlotOrder[K], SlotOrder[K]));
	}
	FMeshNaniteSettings Nanite = Mesh->GetNaniteSettings();
	Nanite.bEnabled = !bTranslucent; // (Nanite draws no translucency)
	Mesh->SetNaniteSettings(Nanite);
	Mesh->SetLightingGuid();
	Package->MarkPackageDirty();
	Dirty.Add(Package);
	ToBuild.Add(Mesh);
	++Created;
	return Mesh;
}

void FGTAVMeshes::Build()
{
	if (ToBuild.Num())
	{
		UStaticMesh::BatchBuild(ToBuild, true);
		for (UStaticMesh* Mesh : ToBuild)
		{
			Mesh->PostEditChange();
		}
		ToBuild.Reset();
	}
}
