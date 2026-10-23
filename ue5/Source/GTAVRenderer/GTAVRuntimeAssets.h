#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "Tasks/Task.h"
#include "UObject/GCObject.h"

#include "GTAVPack.h"

class UMaterial;
class UMaterialInterface;
class UStaticMesh;
class UTexture2D;
struct FStaticMeshBuildVertex;

// The game's models, textures and materials made at run time from the pack (research/phase0.md §59): textures keep
// the game's own compressed data (no re-encoding: the same memory as the game), meshes get their vertex and index
// buffers straight from the converted data, materials are instances of the master materials (/Game/GTAV/Materials).
// Models load in the background, nearest first: worker threads read the pack and build the render data, the game
// thread makes the engine objects within a time budget.
class FGTAVRuntimeAssets : public FGCObject
{
public:
	explicit FGTAVRuntimeAssets(const FGTAVPack& InPack);
	virtual ~FGTAVRuntimeAssets() override;

	enum class EState : uint8
	{
		None,
		Loading,
		Ready,
		Failed,
	};
	// Starts loading a model (by its key: the archetype's name hash) unless it is loaded or loading; lower priorities
	// load first (the streamer gives the distance).
	EState Request(uint64 Key, float Priority = 0);
	EState State(uint64 Key) const;
	UStaticMesh* Get(uint64 Key) const;
	// The lights a loaded model carries (none: null).
	const TArray<GTAVPack::ModelLight>* LightsOf(uint64 Key) const { return ModelLights.Find(Key); }
	// Finishes loaded models for up to Budget seconds.
	void Tick(double Budget);
	int32 Loading() const;

	// One of the renderer's own textures (globals.gpk) by name, as a normal map or a colour.
	UTexture2D* GlobalTexture(const TCHAR* Name, bool bNormal);
	// A material of a master (kind: GTAVPack::MaterialKind, 7 the sea) with the water's bump map set.
	UMaterialInterface* WaterMaterial(int32 Kind);
	// A static mesh from vertices and triangles (game thread), with one material.
	UStaticMesh* BuildMesh(TArray<FStaticMeshBuildVertex>& Vertices, TArray<uint32>& Indices, UMaterialInterface* Material);

	// While the textures and meshes take more than BudgetBytes, the models not used for Unused seconds go (oldest
	// first), with the materials and textures nothing else uses.
	void Evict(int64 BudgetBytes, double Unused);

	int32 MeshesBuilt = 0, MeshesFailed = 0, TexturesMade = 0, TexturesFailed = 0, MaterialsMade = 0, MeshesEvicted = 0;
	int64 TextureBytes = 0, MeshBytes = 0;

	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override { return TEXT("FGTAVRuntimeAssets"); }

private:
	struct FPrepared;
	enum class EUsage : uint8 { Color, Linear, Normal, Palette };

	const FGTAVPack& Pack;
	TMap<uint64, EState> States;
	TMap<uint64, TObjectPtr<UStaticMesh>> Meshes;
	TMap<uint64, TArray<GTAVPack::ModelLight>> ModelLights;
	TMap<uint64, TObjectPtr<UTexture2D>> Textures; // key * 4 + usage
	TMap<uint32, TObjectPtr<UMaterialInterface>> Materials; // by the material's bytes
	TMap<int32, TObjectPtr<UMaterial>> Masters;
	TObjectPtr<UMaterialInterface> Fallback;
	double Clock = 0; // (the time of this tick)

	// What uses what (for eviction).
	struct FMeshUse
	{
		TArray<uint32> Materials;
		int64 Bytes = 0;
		double LastUsed = 0;
	};
	struct FMaterialUse
	{
		TArray<uint64> Textures; // texture cache keys
		int32 Refs = 0;
	};
	struct FTextureUse
	{
		uint64 Key = 0; // the pack's key
		int64 Bytes = 0;
		int32 Refs = 0;
	};
	TMap<uint64, FMeshUse> MeshUse;
	TMap<uint32, FMaterialUse> MaterialUse;
	TMap<uint64, FTextureUse> TextureUse;

	// Loading: a queue by priority the workers take from (guarded), then the game thread.
	mutable FCriticalSection QueueLock;
	TArray<TPair<float, uint64>> Queue; // a heap
	int32 Working = 0;                  // models being prepared
	int32 Pumps = 0;                    // worker loops running
	bool bStopping = false;
	TArray<UE::Tasks::FTask> Tasks;
	TQueue<FPrepared*, EQueueMode::Mpsc> Done;
	TArray<TUniquePtr<FPrepared>> Waiting;
	// Textures: the keys some worker reads (guarded), the bytes read and not made into textures yet.
	FCriticalSection RequestedLock;
	TSet<uint64> Requested;
	TSet<uint64> Arrived;
	TMap<uint64, TArray<uint8>> TextureData;

	void Pump();
	TUniquePtr<FPrepared> Prepare(uint64 Key);
	bool Finish(FPrepared& P);
	UTexture2D* Texture(uint64 Key, EUsage Usage, int32* OutWidth = nullptr, int32* OutHeight = nullptr);
	UTexture2D* MakeTexture(const TArray<uint8>& Data, EUsage Usage);
	UMaterialInterface* Material(const GTAVPack::Material& M);
	UMaterial* Master(int32 Kind);
};
