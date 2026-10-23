#include "GTAVRuntimeAssets.h"

#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MeshCardBuild.h"
#include "MeshCardRepresentation.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "TextureResource.h"

DEFINE_LOG_CATEGORY_STATIC(LogGTAVAssets, Log, All);

namespace
{
	EPixelFormat PixelFormat(uint8 Dxgi)
	{
		switch (Dxgi)
		{
		case 71: case 72: return PF_DXT1;
		case 74: case 75: return PF_DXT3;
		case 77: case 78: return PF_DXT5;
		case 80: case 81: return PF_BC4;
		case 83: case 84: return PF_BC5;
		case 98: case 99: return PF_BC7;
		case 87: case 88: return PF_B8G8R8A8;
		case 28: case 29: return PF_R8G8B8A8;
		case 61: return PF_G8;
		default: return PF_Unknown;
		}
	}

	// A cursor over an entry's bytes.
	struct FReader
	{
		const TArray<uint8>& Data;
		int64 At = 0;
		bool bOk = true;

		const uint8* Take(int64 Size)
		{
			if (Size < 0 || At + Size > Data.Num())
			{
				bOk = false;
				return nullptr;
			}
			const uint8* P = Data.GetData() + At;
			At += Size;
			return P;
		}
		template <typename T>
		const T* Array(int64 Count) { return reinterpret_cast<const T*>(Take(Count * int64(sizeof(T)))); }
		template <typename T>
		T Get()
		{
			T Value{};
			if (const uint8* P = Take(sizeof(T)))
			{
				FMemory::Memcpy(&Value, P, sizeof(T));
			}
			return Value;
		}
	};

	const TCHAR* SlotParameter(int32 Slot)
	{
		switch (Slot)
		{
		case GTAVPack::kDiffuse: return TEXT("DiffuseTex");
		case GTAVPack::kBump: return TEXT("BumpTex");
		case GTAVPack::kSpecular: return TEXT("SpecularTex");
		case GTAVPack::kTintPalette: return TEXT("TintPaletteTex");
		case GTAVPack::kLayerDiffuse0: return TEXT("DiffuseTexture_layer0");
		case GTAVPack::kLayerDiffuse1: return TEXT("DiffuseTexture_layer1");
		case GTAVPack::kLayerDiffuse2: return TEXT("DiffuseTexture_layer2");
		case GTAVPack::kLayerDiffuse3: return TEXT("DiffuseTexture_layer3");
		case GTAVPack::kLayerBump0: return TEXT("BumpTexture_layer0");
		case GTAVPack::kLayerBump1: return TEXT("BumpTexture_layer1");
		case GTAVPack::kLayerBump2: return TEXT("BumpTexture_layer2");
		case GTAVPack::kLayerBump3: return TEXT("BumpTexture_layer3");
		default: return nullptr; // (not used by the masters yet)
		}
	}
}

FGTAVRuntimeAssets::FGTAVRuntimeAssets(const FGTAVPack& InPack)
	: Pack(InPack)
{
	Fallback = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"));
}

void FGTAVRuntimeAssets::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObjects(Meshes);
	Collector.AddReferencedObjects(Textures);
	Collector.AddReferencedObjects(Materials);
	Collector.AddReferencedObjects(Masters);
	Collector.AddReferencedObject(Fallback);
}

UTexture2D* FGTAVRuntimeAssets::Texture(uint64 Key, EUsage Usage, int32* OutWidth, int32* OutHeight)
{
	const uint64 CacheKey = Key * 4 + uint64(Usage);
	if (TObjectPtr<UTexture2D>* Found = Textures.Find(CacheKey))
	{
		if (*Found && OutWidth)
		{
			*OutWidth = (*Found)->GetSizeX();
			*OutHeight = (*Found)->GetSizeY();
		}
		return *Found;
	}
	const auto Fail = [&] {
		++TexturesFailed;
		Textures.Add(CacheKey, nullptr);
		return nullptr;
	};
	TArray<uint8> Data;
	FReader R{Data};
	if (!Pack.Textures.Read(Key, Data))
	{
		return Fail();
	}
	const GTAVPack::TextureHeader H = R.Get<GTAVPack::TextureHeader>();
	const EPixelFormat Format = PixelFormat(H.dxgi);
	if (!R.bOk || Format == PF_Unknown)
	{
		return Fail();
	}
	const FPixelFormatInfo& Info = GPixelFormats[Format];
	FTexturePlatformData* Platform = new FTexturePlatformData();
	Platform->SizeX = H.width;
	Platform->SizeY = H.height;
	Platform->SetNumSlices(1);
	Platform->PixelFormat = Format;
	for (int32 Mip = 0, W = H.width, Ht = H.height; Mip < H.mips; ++Mip, W = FMath::Max(1, W / 2), Ht = FMath::Max(1, Ht / 2))
	{
		const int64 Size = int64(FMath::DivideAndRoundUp(W, Info.BlockSizeX)) * FMath::DivideAndRoundUp(Ht, Info.BlockSizeY) * Info.BlockBytes;
		const uint8* Bytes = R.Take(Size);
		if (!Bytes)
		{
			break;
		}
		FTexture2DMipMap* M = new FTexture2DMipMap(W, Ht, 1);
		Platform->Mips.Add(M);
		M->BulkData.Lock(LOCK_READ_WRITE);
		FMemory::Memcpy(M->BulkData.Realloc(Size), Bytes, Size);
		M->BulkData.Unlock();
		TextureBytes += Size;
	}
	if (Platform->Mips.IsEmpty())
	{
		delete Platform;
		return Fail();
	}
	UTexture2D* T = NewObject<UTexture2D>(GetTransientPackage(), NAME_None, RF_Transient);
	T->SetPlatformData(Platform);
	T->bNotOfflineProcessed = true;
	T->NeverStream = true;
	T->SRGB = Usage == EUsage::Color || Usage == EUsage::Palette;
	T->CompressionSettings = Usage == EUsage::Normal ? TC_Normalmap : Usage == EUsage::Linear ? TC_Masks : TC_Default;
	if (Usage == EUsage::Palette)
	{
		// (a lookup table: exact texels)
		T->Filter = TF_Nearest;
	}
	T->UpdateResource();
	++TexturesMade;
	if (OutWidth)
	{
		*OutWidth = H.width;
		*OutHeight = H.height;
	}
	Textures.Add(CacheKey, T);
	return T;
}

UMaterial* FGTAVRuntimeAssets::Master(int32 Kind)
{
	if (TObjectPtr<UMaterial>* Found = Masters.Find(Kind))
	{
		return *Found;
	}
	const TCHAR* Name = Kind == GTAVPack::kWater ? TEXT("M_GTAV_Water")
		: Kind == GTAVPack::kTerrain ? TEXT("M_GTAV_Terrain4")
		: Kind == GTAVPack::kCutout ? TEXT("M_GTAV_Cutout")
		: Kind == GTAVPack::kTranslucent || Kind == GTAVPack::kDecal ? TEXT("M_GTAV_Translucent")
		: TEXT("M_GTAV_Opaque");
	UMaterial* M = LoadObject<UMaterial>(nullptr, *FString::Printf(TEXT("/Game/GTAV/Materials/%s.%s"), Name, Name));
	if (!M)
	{
		UE_LOG(LogGTAVAssets, Error, TEXT("no master material %s (GTAVImport -masters makes them)"), Name);
	}
	return Masters.Add(Kind, M);
}

UMaterialInterface* FGTAVRuntimeAssets::Material(const GTAVPack::Material& M)
{
	const uint32 Key = FCrc::MemCrc32(&M, sizeof(M));
	if (TObjectPtr<UMaterialInterface>* Found = Materials.Find(Key))
	{
		return *Found;
	}
	UMaterial* Parent = Master(M.kind);
	if (!Parent)
	{
		return Materials.Add(Key, Fallback);
	}
	UMaterialInstanceDynamic* MI = UMaterialInstanceDynamic::Create(Parent, GetTransientPackage());
	for (int32 Slot = 0; Slot < GTAVPack::kSlotCount; ++Slot)
	{
		const TCHAR* Parameter = SlotParameter(Slot);
		if (!Parameter || !M.textures[Slot])
		{
			continue;
		}
		const bool bNormal = Slot == GTAVPack::kBump || (Slot >= GTAVPack::kLayerBump0 && Slot <= GTAVPack::kLayerBump3);
		const EUsage Usage = Slot == GTAVPack::kTintPalette ? EUsage::Palette : bNormal ? EUsage::Normal : Slot == GTAVPack::kSpecular ? EUsage::Linear : EUsage::Color;
		int32 Width = 0, Height = 0;
		if (UTexture2D* T = Texture(M.textures[Slot], Usage, &Width, &Height))
		{
			MI->SetTextureParameterValue(Parameter, T);
			if (Usage == EUsage::Palette)
			{
				MI->SetScalarParameterValue(TEXT("UseTint"), 1.0f);
				MI->SetScalarParameterValue(TEXT("TintPaletteWidth"), Width);
				MI->SetScalarParameterValue(TEXT("TintPaletteHeight"), Height);
				MI->SetScalarParameterValue(TEXT("TintRow"), M.tintRow);
			}
		}
	}
	if (M.emissive > 0)
	{
		MI->SetScalarParameterValue(TEXT("EmissiveMultiplier"), M.emissive);
	}
	if (M.kind == GTAVPack::kWater && (M.color[0] > 0 || M.color[1] > 0 || M.color[2] > 0))
	{
		MI->SetVectorParameterValue(TEXT("WaterColor"), FLinearColor(M.color[0], M.color[1], M.color[2]));
	}
	++MaterialsMade;
	return Materials.Add(Key, MI);
}

UStaticMesh* FGTAVRuntimeAssets::Mesh(uint64 Key)
{
	if (TObjectPtr<UStaticMesh>* Found = Meshes.Find(Key))
	{
		return *Found;
	}
	TArray<uint8> Data;
	if (!Pack.Models.Read(Key, Data))
	{
		++MeshesFailed;
		return Meshes.Add(Key, nullptr);
	}
	FReader R{Data};
	const GTAVPack::ModelHeader H = R.Get<GTAVPack::ModelHeader>();
	const GTAVPack::Material* Mats = R.Array<GTAVPack::Material>(H.materialCount);
	if (!R.bOk || FMemory::Memcmp(H.magic, "GMDL", 4) != 0)
	{
		++MeshesFailed;
		return Meshes.Add(Key, nullptr);
	}

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
	int32 UvChannels = 1;
	TMap<uint16, FPolygonGroupID> Groups;
	TArray<TPair<FName, uint16>> Slots;
	bool bTranslucent = false;
	for (int32 S = 0; S < H.sectionCount && R.bOk; ++S)
	{
		const GTAVPack::SectionHeader SH = R.Get<GTAVPack::SectionHeader>();
		const int64 V = SH.vertexCount;
		const float* P = R.Array<float>(3 * V);
		const int8* N = (SH.flags & GTAVPack::kNormals) ? R.Array<int8>(4 * V) : nullptr;
		const int8* T = (SH.flags & GTAVPack::kTangents) ? R.Array<int8>(4 * V) : nullptr;
		const uint8* C0 = (SH.flags & GTAVPack::kColor0) ? R.Array<uint8>(4 * V) : nullptr;
		const uint8* C1 = (SH.flags & GTAVPack::kColor1) ? R.Array<uint8>(4 * V) : nullptr;
		TArray<const float*, TInlineAllocator<8>> Uv;
		for (int32 K = 0; K < SH.uvCount; ++K)
		{
			Uv.Add(R.Array<float>(2 * V));
		}
		const bool bWide = (SH.flags & GTAVPack::kIndex32) != 0;
		const uint8* I = R.Take(int64(SH.indexCount) * (bWide ? 4 : 2));
		R.Take((4 - R.At % 4) % 4);
		if (!R.bOk || SH.material >= H.materialCount)
		{
			break;
		}
		const GTAVPack::Material& Mat = Mats[SH.material];
		if (int32(SH.uvCount) > UvChannels)
		{
			UvChannels = FMath::Min<int32>(SH.uvCount, 8);
			UVs.SetNumChannels(UvChannels);
		}
		FPolygonGroupID* Group = Groups.Find(SH.material);
		if (!Group)
		{
			const FName Slot(*FString::Printf(TEXT("m%u"), SH.material));
			Group = &Groups.Add(SH.material, Description.CreatePolygonGroup());
			SlotNames[*Group] = Slot;
			Slots.Add({Slot, SH.material});
			bTranslucent |= Mat.kind == GTAVPack::kTranslucent || Mat.kind == GTAVPack::kDecal || Mat.kind == GTAVPack::kWater;
		}
		const bool bTerrain = Mat.kind == GTAVPack::kTerrain && C1;
		TArray<FVertexInstanceID> Instances;
		Instances.SetNumUninitialized(V);
		for (int64 X = 0; X < V; ++X)
		{
			const FVertexID Vertex = Description.CreateVertex();
			Positions[Vertex] = FVector3f(P[3 * X], P[3 * X + 1], P[3 * X + 2]);
			const FVertexInstanceID Instance = Description.CreateVertexInstance(Vertex);
			Instances[X] = Instance;
			if (N)
			{
				Normals[Instance] = FVector3f(N[4 * X], N[4 * X + 1], N[4 * X + 2]) / 127.0f;
			}
			if (T)
			{
				Tangents[Instance] = FVector3f(T[4 * X], T[4 * X + 1], T[4 * X + 2]) / 127.0f;
				BinormalSigns[Instance] = T[4 * X + 3] < 0 ? -1.0f : 1.0f;
			}
			// (the exact inverse of the engine's sRGB conversion when it builds the mesh: the material reads the game's bytes;
			// terrain: the blend weights are the second colour set's green and blue)
			FColor Color(255, 255, 255, 255);
			if (bTerrain)
			{
				Color = FColor(C0 ? C0[4 * X] : 255, C1[4 * X + 1], C1[4 * X + 2], 255);
			}
			else if (C0)
			{
				Color = FColor(C0[4 * X], C0[4 * X + 1], C0[4 * X + 2], C0[4 * X + 3]);
			}
			const FLinearColor Linear = FLinearColor::FromSRGBColor(Color);
			Colors[Instance] = FVector4f(Linear.R, Linear.G, Linear.B, Linear.A);
			for (int32 K = 0; K < UvChannels; ++K)
			{
				UVs.Set(Instance, K, K < Uv.Num() ? FVector2f(Uv[K][2 * X], Uv[K][2 * X + 1]) : FVector2f::ZeroVector);
			}
		}
		for (uint32 K = 0; K + 2 < SH.indexCount; K += 3)
		{
			uint32 A, B, C;
			if (bWide)
			{
				const uint32* W = reinterpret_cast<const uint32*>(I);
				A = W[K], B = W[K + 1], C = W[K + 2];
			}
			else
			{
				const uint16* W = reinterpret_cast<const uint16*>(I);
				A = W[K], B = W[K + 1], C = W[K + 2];
			}
			if (A == B || B == C || A == C || A >= V || B >= V || C >= V)
			{
				continue;
			}
			// (the game's order as it is: mirroring flips the winding, and the engine's front faces wind the other way)
			Description.CreateTriangle(*Group, {Instances[A], Instances[B], Instances[C]});
		}
	}
	if (!R.bOk || Description.Triangles().Num() == 0)
	{
		UE_LOG(LogGTAVAssets, Warning, TEXT("model %016llx: no geometry"), Key);
		++MeshesFailed;
		return Meshes.Add(Key, nullptr);
	}

	UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
	for (const TPair<FName, uint16>& Slot : Slots)
	{
		Mesh->GetStaticMaterials().Add(FStaticMaterial(Material(Mats[Slot.Value]), Slot.Key, Slot.Key));
	}
	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bFastBuild = true;
	Params.bMarkPackageDirty = false;
	Params.bCommitMeshDescription = false;
	Params.bBuildSimpleCollision = false;
	Mesh->BuildFromMeshDescriptions({&Description}, Params);
	// Lumen's surface cache: cards from the bounds (what the engine does for meshes built at run time).
	if (FStaticMeshRenderData* RenderData = Mesh->GetRenderData(); RenderData && RenderData->LODResources.Num())
	{
		FCardRepresentationData* Cards = new FCardRepresentationData();
		Cards->MeshCardsBuildData.Bounds = RenderData->Bounds.GetBox();
		Cards->MeshCardsBuildData.bMostlyTwoSided = bTranslucent;
		MeshCardRepresentation::SetCardsFromBounds(Cards->MeshCardsBuildData);
		RenderData->LODResources[0].CardRepresentationData = Cards;
	}
	++MeshesBuilt;
	return Meshes.Add(Key, Mesh);
}
