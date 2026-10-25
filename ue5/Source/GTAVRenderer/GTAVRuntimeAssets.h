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
// A texture is on the GPU only down from the mip its nearest use needs (its LODBias; all the mips stay in memory):
// each model's surface against its first UV set gives how many centimetres one UV unit spans, which at the model's
// distance (the streamer's requests, each round) is so many pixels on the screen; a texture gets the mip of about
// twice that (oblique views), and changes as the camera comes and goes (§59.12).
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
	// A loaded model kept as if it was asked for now (not loaded if it is not).
	void Touch(uint64 Key);
	EState State(uint64 Key) const;
	UStaticMesh* Get(uint64 Key) const;
	// The lights a loaded model carries (none: null).
	const TArray<GTAVPack::ModelLight>* LightsOf(uint64 Key) const { return ModelLights.Find(Key); }
	// A loaded model with a screen a script draws into (a TV's, a monitor's: GTAVPack::kScriptTarget).
	bool HasScreen(uint64 Key) const { return Screens.Contains(Key); }
	// Finishes loaded models for up to Budget seconds.
	void Tick(double Budget);
	int32 Loading() const;

	// The screen the textures' detail is chosen for: pixels per radian (the view's width over twice the tangent of half
	// its horizontal field of view).
	void SetPixelsPerRadian(float Value) { PixelsPerRadian = FMath::Max(Value, 1.0f); }
	// A round of requests is over (each streamer's update, at most twice a second): each texture's detail by its
	// nearest use since the last round.
	void EndRound();

	// One of the renderer's own textures (globals.gpk) by name, as a normal map or a colour.
	UTexture2D* GlobalTexture(const TCHAR* Name, bool bNormal, bool bLinear = false);
	// A material of a master (kind: GTAVPack::MaterialKind, 7 the sea) with the water's bump map set.
	UMaterialInterface* WaterMaterial(int32 Kind);
	// A static mesh from vertices and triangles (game thread), with one material.
	UStaticMesh* BuildMesh(TArray<FStaticMeshBuildVertex>& Vertices, TArray<uint32>& Indices, UMaterialInterface* Material);

	// The models not used for Stale seconds go, with the materials and textures nothing else uses: what the world asks
	// for again is made again from the pack (the CPU's work spent for the GPU's memory, which the game shares). Once
	// the textures and meshes take more than BudgetBytes, those not used for Unused seconds go too (oldest first), down
	// to 85% of it. Their memory comes back with the next garbage collection: one is asked for once 128 MB went, or 20 s
	// after anything went (not a full purge: the objects are destroyed a few at a time over the next frames; a full
	// collection every few seconds made the frames that had it stutter).
	void Evict(int64 BudgetBytes, double Unused, double Stale);

	int32 MeshesBuilt = 0, MeshesFailed = 0, TexturesMade = 0, TexturesFailed = 0, MaterialsMade = 0, MeshesEvicted = 0, TexturesRetuned = 0;
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
	TSet<uint64> Screens;
	TMap<uint64, TObjectPtr<UTexture2D>> Textures; // key * 4 + usage
	TMap<uint32, TObjectPtr<UMaterialInterface>> Materials; // by the material's bytes
	TMap<int32, TObjectPtr<UMaterial>> Masters;
	TObjectPtr<UMaterialInterface> Fallback;
	double Clock = 0; // (the time of this tick)
	int64 EvictedSinceCollection = 0;
	double LastCollection = 0;
	// The textures' detail: the screen, the models' nearest distances (cm) in this round of requests and the last, what
	// the texture being made is needed at (texels across a UV unit; -1: all of it, always: the renderer's own), the
	// textures to change (a few dozen a frame at most).
	float PixelsPerRadian = 1000.0f;
	TMap<uint64, float> Near, LastNear;
	double LastRound = 0;
	float PendingTexels = -1.0f;
	TArray<uint64> Retune;
	bool WarnedWidth = false;

	// What uses what (for eviction).
	struct FMeshUse
	{
		TArray<uint32> Materials;
		TArray<float> Density; // each material's: centimetres a UV unit spans
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
		int64 Bytes = 0; // (on the GPU)
		int32 Refs = 0;
		int32 Width = 0, Mips = 0, Bias = 0;
		bool bFixed = false; // (palettes, lookups: always whole)
		float Need = 0;      // texels across a UV unit its nearest use needs
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
	UTexture2D* Texture(uint64 Key, EUsage Usage, float Texels, int32* OutWidth = nullptr, int32* OutHeight = nullptr);
	UTexture2D* MakeTexture(const TArray<uint8>& Data, EUsage Usage, float Texels);
	// The mips a texture of that width can leave off when Texels (across a UV unit) are needed (0: none; at least 64
	// texels kept).
	static int32 BiasFor(int32 Width, int32 Mips, float Texels);
	// As much of Bias as the texture can take: a block-compressed one keeps a first mip of whole blocks (the engine
	// leaves a texture without one out altogether).
	static int32 UsableBias(const UTexture2D* T, int32 Bias);
	static int64 ResidentBytes(const UTexture2D* T, int32 Bias);
	void RetuneTextures(double Budget);
	UMaterialInterface* Material(const GTAVPack::Material& M);
	UMaterial* Master(int32 Kind);
};
