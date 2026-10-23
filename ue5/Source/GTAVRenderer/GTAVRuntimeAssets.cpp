#include "GTAVRuntimeAssets.h"

#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/CommandLine.h"
#include "MeshCardBuild.h"
#include "MeshCardRepresentation.h"
#include "StaticMeshResources.h"
#include "Tasks/Task.h"
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
		case 65: return PF_A8;
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

	// The master materials' texture parameters, by the pack's slot.
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

	constexpr int32 kPumps = 6; // worker loops preparing models
}

// A model read and turned into render data on a worker thread, with the textures no other model asked for yet.
struct FGTAVRuntimeAssets::FPrepared
{
	uint64 Key = 0;
	bool bOk = false;
	TArray<GTAVPack::Material> Materials;
	TArray<uint16> SlotMaterials; // the material of each of the mesh's slots
	TUniquePtr<FStaticMeshRenderData> RenderData;
	bool bTranslucent = false;
	TArray<TPair<uint64, TArray<uint8>>> NewTextures;
	bool bTexturesMoved = false;
};

FGTAVRuntimeAssets::FGTAVRuntimeAssets(const FGTAVPack& InPack)
	: Pack(InPack)
{
	Fallback = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"));
}

FGTAVRuntimeAssets::~FGTAVRuntimeAssets()
{
	{
		FScopeLock Lock(&QueueLock);
		bStopping = true;
	}
	UE::Tasks::Wait(Tasks);
	FPrepared* P = nullptr;
	while (Done.Dequeue(P))
	{
		delete P;
	}
}

void FGTAVRuntimeAssets::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObjects(Meshes);
	Collector.AddReferencedObjects(Textures);
	Collector.AddReferencedObjects(Materials);
	Collector.AddReferencedObjects(Masters);
	Collector.AddReferencedObject(Fallback);
}

FGTAVRuntimeAssets::EState FGTAVRuntimeAssets::State(uint64 Key) const
{
	const EState* S = States.Find(Key);
	return S ? *S : EState::None;
}

UStaticMesh* FGTAVRuntimeAssets::Get(uint64 Key) const
{
	const TObjectPtr<UStaticMesh>* M = Meshes.Find(Key);
	return M ? M->Get() : nullptr;
}

int32 FGTAVRuntimeAssets::Loading() const
{
	FScopeLock Lock(&QueueLock);
	return Queue.Num() + Working + Waiting.Num();
}

FGTAVRuntimeAssets::EState FGTAVRuntimeAssets::Request(uint64 Key, float Priority)
{
	EState& S = States.FindOrAdd(Key, EState::None);
	if (S != EState::None)
	{
		return S;
	}
	S = EState::Loading;
	FScopeLock Lock(&QueueLock);
	Queue.HeapPush({Priority, Key}, [](const TPair<float, uint64>& A, const TPair<float, uint64>& B) { return A.Key < B.Key; });
	if (Pumps < kPumps)
	{
		++Pumps;
		Tasks.RemoveAll([](const UE::Tasks::FTask& T) { return T.IsCompleted(); });
		Tasks.Add(UE::Tasks::Launch(UE_SOURCE_LOCATION, [this] { Pump(); }));
	}
	return S;
}

// A worker loop: the nearest queued model, until there are none.
void FGTAVRuntimeAssets::Pump()
{
	for (;;)
	{
		uint64 Key = 0;
		{
			FScopeLock Lock(&QueueLock);
			if (bStopping || Queue.IsEmpty())
			{
				--Pumps;
				return;
			}
			TPair<float, uint64> Top;
			Queue.HeapPop(Top, [](const TPair<float, uint64>& A, const TPair<float, uint64>& B) { return A.Key < B.Key; }, EAllowShrinking::No);
			Key = Top.Value;
			++Working;
		}
		Done.Enqueue(Prepare(Key).Release());
	}
}

TUniquePtr<FGTAVRuntimeAssets::FPrepared> FGTAVRuntimeAssets::Prepare(uint64 Key)
{
	TUniquePtr<FPrepared> P = MakeUnique<FPrepared>();
	P->Key = Key;
	TArray<uint8> Data;
	if (!Pack.Models.Read(Key, Data))
	{
		return P;
	}
	FReader R{Data};
	const GTAVPack::ModelHeader H = R.Get<GTAVPack::ModelHeader>();
	const GTAVPack::Material* Mats = R.Array<GTAVPack::Material>(H.materialCount);
	if (!R.bOk || FMemory::Memcmp(H.magic, "GMDL", 4) != 0)
	{
		return P;
	}
	P->Materials.Append(Mats, H.materialCount);

	// The vertices, indices and sections of the most detailed level, in the engine's layout.
	TArray<FStaticMeshBuildVertex> Vertices;
	TArray<uint32> Indices;
	FStaticMeshSectionArray Sections;
	TMap<uint16, int32> SlotOf;
	int32 UvChannels = 1;
	FBox3f Box(ForceInit);
	for (int32 S = 0; S < H.sectionCount && R.bOk; ++S)
	{
		const GTAVPack::SectionHeader SH = R.Get<GTAVPack::SectionHeader>();
		const int64 V = SH.vertexCount;
		const float* Pos = R.Array<float>(3 * V);
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
		if (SH.lod != 0 || V == 0)
		{
			continue; // (the drawable's own lower levels of detail: not used yet)
		}
		const GTAVPack::Material& Mat = Mats[SH.material];
		UvChannels = FMath::Clamp<int32>(FMath::Max<int32>(UvChannels, SH.uvCount), 1, MAX_STATIC_TEXCOORDS);
		int32* Slot = SlotOf.Find(SH.material);
		if (!Slot)
		{
			Slot = &SlotOf.Add(SH.material, P->SlotMaterials.Add(SH.material));
			P->bTranslucent |= Mat.kind == GTAVPack::kTranslucent || Mat.kind == GTAVPack::kDecal || Mat.kind == GTAVPack::kWater;
		}
		const bool bTerrain = Mat.kind == GTAVPack::kTerrain && C1;
		const uint32 Base = Vertices.Num();
		Vertices.AddUninitialized(V);
		for (int64 X = 0; X < V; ++X)
		{
			FStaticMeshBuildVertex& Out = Vertices[Base + X];
			Out.Position = FVector3f(Pos[3 * X], Pos[3 * X + 1], Pos[3 * X + 2]);
			Out.CurveRadius = 0;
			Box += Out.Position;
			const FVector3f Normal = N ? FVector3f(N[4 * X], N[4 * X + 1], N[4 * X + 2]).GetSafeNormal(UE_SMALL_NUMBER, FVector3f::UpVector) : FVector3f::UpVector;
			FVector3f Tangent;
			float Sign = 1.0f;
			if (T)
			{
				Tangent = FVector3f(T[4 * X], T[4 * X + 1], T[4 * X + 2]).GetSafeNormal(UE_SMALL_NUMBER, FVector3f::ForwardVector);
				Sign = T[4 * X + 3] < 0 ? -1.0f : 1.0f;
			}
			else
			{
				FVector3f Unused;
				Normal.FindBestAxisVectors(Tangent, Unused);
			}
			Out.TangentX = Tangent;
			Out.TangentY = FVector3f::CrossProduct(Normal, Tangent).GetSafeNormal() * Sign;
			Out.TangentZ = Normal;
			// (the game's bytes as they are; terrain: the blend weights are the second colour set's green and blue)
			if (bTerrain)
			{
				Out.Color = FColor(C0 ? C0[4 * X] : 255, C1[4 * X + 1], C1[4 * X + 2], 255);
			}
			else
			{
				Out.Color = C0 ? FColor(C0[4 * X], C0[4 * X + 1], C0[4 * X + 2], C0[4 * X + 3]) : FColor::White;
			}
			for (int32 K = 0; K < MAX_STATIC_TEXCOORDS; ++K)
			{
				Out.UVs[K] = K < Uv.Num() ? FVector2f(Uv[K][2 * X], Uv[K][2 * X + 1]) : FVector2f::ZeroVector;
			}
		}
		FStaticMeshSection& Section = Sections.AddDefaulted_GetRef();
		Section.MaterialIndex = *Slot;
		Section.FirstIndex = Indices.Num();
		Section.MinVertexIndex = Base;
		Section.MaxVertexIndex = Base + V - 1;
		Section.bEnableCollision = false;
		Section.bCastShadow = true;
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
			Indices.Append({Base + A, Base + B, Base + C});
			++Section.NumTriangles;
		}
		if (Section.NumTriangles == 0)
		{
			Sections.Pop(EAllowShrinking::No);
		}
	}
	if (!R.bOk || Indices.IsEmpty())
	{
		return P;
	}
	P->RenderData = MakeUnique<FStaticMeshRenderData>();
	P->RenderData->AllocateLODResources(1);
	FStaticMeshLODResources& L = P->RenderData->LODResources[0];
	L.VertexBuffers.PositionVertexBuffer.Init(Vertices);
	FStaticMeshVertexBufferFlags Flags;
	Flags.bNeedsCPUAccess = true;
	L.VertexBuffers.StaticMeshVertexBuffer.Init(Vertices, UvChannels, Flags);
	L.VertexBuffers.ColorVertexBuffer.Init(Vertices);
	L.bHasColorVertexData = true;
	L.IndexBuffer.SetIndices(Indices, Vertices.Num() > 0xFFFF ? EIndexBufferStride::Force32Bit : EIndexBufferStride::Force16Bit);
	L.Sections = MoveTemp(Sections);
	P->RenderData->Bounds = FBoxSphereBounds(FBox(Box));
	P->RenderData->ScreenSize[0].Default = 1.0f;

	// The textures nobody asked for yet.
	for (const GTAVPack::Material& M : P->Materials)
	{
		for (int32 Slot = 0; Slot < GTAVPack::kSlotCount; ++Slot)
		{
			const uint64 T = M.textures[Slot];
			if (!T || !SlotParameter(Slot))
			{
				continue;
			}
			{
				FScopeLock Lock(&RequestedLock);
				bool bAlready = false;
				Requested.Add(T, &bAlready);
				if (bAlready)
				{
					continue;
				}
			}
			TArray<uint8>& Bytes = P->NewTextures.Emplace_GetRef(T, TArray<uint8>()).Value;
			Pack.Textures.Read(T, Bytes); // (empty if it is not there: made as nothing)
		}
	}
	P->bOk = true;
	return P;
}

void FGTAVRuntimeAssets::Tick(double Budget)
{
	const double Start = FPlatformTime::Seconds();
	for (FPrepared* P = nullptr; Done.Dequeue(P);)
	{
		{
			FScopeLock Lock(&QueueLock);
			--Working;
		}
		Waiting.Emplace(P);
		// (its textures are there for every model from now on)
		for (TPair<uint64, TArray<uint8>>& T : Waiting.Last()->NewTextures)
		{
			Arrived.Add(T.Key);
			TextureData.Add(T.Key, MoveTemp(T.Value));
		}
		Waiting.Last()->NewTextures.Empty();
	}
	// Finish what has its textures (some may come with another model still on its way).
	for (int32 I = 0; I < Waiting.Num() && FPlatformTime::Seconds() - Start < Budget;)
	{
		if (Finish(*Waiting[I]))
		{
			Waiting.RemoveAt(I, EAllowShrinking::No);
		}
		else
		{
			++I;
		}
	}
}

// The engine objects of a prepared model; false if a texture it uses has not arrived yet.
bool FGTAVRuntimeAssets::Finish(FPrepared& P)
{
	if (!P.bOk)
	{
		++MeshesFailed;
		States.Add(P.Key, EState::Failed);
		return true;
	}
	for (const GTAVPack::Material& M : P.Materials)
	{
		for (int32 Slot = 0; Slot < GTAVPack::kSlotCount; ++Slot)
		{
			if (M.textures[Slot] && SlotParameter(Slot) && !Arrived.Contains(M.textures[Slot]))
			{
				return false;
			}
		}
	}
	UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
	Mesh->NeverStream = true;
	for (int32 Slot = 0; Slot < P.SlotMaterials.Num(); ++Slot)
	{
		const FName Name(*FString::Printf(TEXT("m%u"), P.SlotMaterials[Slot]));
		Mesh->GetStaticMaterials().Add(FStaticMaterial(Material(P.Materials[P.SlotMaterials[Slot]]), Name, Name));
	}
	// Lumen's surface cache: cards from the bounds (what the engine does for meshes built at run time).
	FCardRepresentationData* Cards = new FCardRepresentationData();
	Cards->MeshCardsBuildData.Bounds = P.RenderData->Bounds.GetBox();
	Cards->MeshCardsBuildData.bMostlyTwoSided = P.bTranslucent;
	MeshCardRepresentation::SetCardsFromBounds(Cards->MeshCardsBuildData);
	P.RenderData->LODResources[0].CardRepresentationData = Cards;
#if WITH_EDITORONLY_DATA
	// (an editor build takes each section's material from this map, not from the section)
	for (int32 Section = 0; Section < P.RenderData->LODResources[0].Sections.Num(); ++Section)
	{
		const FStaticMeshSection& S = P.RenderData->LODResources[0].Sections[Section];
		FMeshSectionInfo Info;
		Info.MaterialIndex = S.MaterialIndex;
		Info.bEnableCollision = false;
		Info.bCastShadow = S.bCastShadow;
		Mesh->GetSectionInfoMap().Set(0, Section, Info);
	}
#endif
	Mesh->SetRenderData(MoveTemp(P.RenderData));
	if (IsRayTracingAllowed())
	{
		Mesh->GetRenderData()->InitializeRayTracingRepresentationFromRenderingLODs();
	}
	Mesh->InitResources();
	Mesh->CalculateExtendedBounds();
	++MeshesBuilt;
	Meshes.Add(P.Key, Mesh);
	States.Add(P.Key, EState::Ready);
	return true;
}

UTexture2D* FGTAVRuntimeAssets::MakeTexture(const TArray<uint8>& Data, EUsage Usage)
{
	FReader R{Data};
	const GTAVPack::TextureHeader H = R.Get<GTAVPack::TextureHeader>();
	const EPixelFormat Format = PixelFormat(H.dxgi);
	if (!R.bOk || Format == PF_Unknown)
	{
		return nullptr;
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
		return nullptr;
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
	return T;
}

UTexture2D* FGTAVRuntimeAssets::Texture(uint64 Key, EUsage Usage, int32* OutWidth, int32* OutHeight)
{
	const uint64 CacheKey = Key * 4 + uint64(Usage);
	UTexture2D* T = nullptr;
	if (const TObjectPtr<UTexture2D>* Found = Textures.Find(CacheKey))
	{
		T = *Found;
	}
	else
	{
		// The bytes a worker read (the first use), or read again (another use of the same texture: rare).
		TArray<uint8> Data;
		if (TArray<uint8>* Read = TextureData.Find(Key))
		{
			Data = MoveTemp(*Read);
			TextureData.Remove(Key);
		}
		else
		{
			Pack.Textures.Read(Key, Data);
		}
		T = Data.Num() ? MakeTexture(Data, Usage) : nullptr;
		++(T ? TexturesMade : TexturesFailed);
		if (TexturesMade + TexturesFailed <= 12 && Data.Num() >= 8)
		{
			const GTAVPack::TextureHeader* H = reinterpret_cast<const GTAVPack::TextureHeader*>(Data.GetData());
			UE_LOG(LogGTAVAssets, Display, TEXT("texture %016llx usage %d: %dx%d dxgi %d mips %d, %d bytes -> %s"), Key, int32(Usage), H->width, H->height,
				H->dxgi, H->mips, Data.Num(), T ? *FString::Printf(TEXT("%dx%d %s"), T->GetSizeX(), T->GetSizeY(), GPixelFormats[T->GetPixelFormat()].Name) : TEXT("none"));
		}
		Textures.Add(CacheKey, T);
	}
	if (T && OutWidth)
	{
		*OutWidth = T->GetSizeX();
		*OutHeight = T->GetSizeY();
	}
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
	static const FString Skip = [] {
		FString S;
		FParse::Value(FCommandLine::Get(), TEXT("-gtavskip="), S); // (checking: parameters left at their defaults)
		return S;
	}();
	for (int32 Slot = 0; Slot < GTAVPack::kSlotCount; ++Slot)
	{
		const TCHAR* Parameter = SlotParameter(Slot);
		if (!Parameter || !M.textures[Slot] || (!Skip.IsEmpty() && Skip.Contains(Parameter)))
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
