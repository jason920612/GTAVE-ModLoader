#include "GTAVWorldSubsystem.h"
#include "GTAVSky.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/GameViewportClient.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/ScopeExit.h"
#include "RenderTimer.h"
#include "StaticMeshResources.h"
#include "UnrealClient.h"
#include "UObject/UObjectArray.h"

#include "ID3D12DynamicRHI.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <dxgi1_4.h>
#include "Windows/HideWindowsPlatformTypes.h"

DEFINE_LOG_CATEGORY_STATIC(LogGTAVWorld, Log, All);

namespace
{
	constexpr uint64 kInteriorKey = 1ull << 63;
	constexpr double kAddBudget = 0.004;   // seconds per frame making components
	constexpr int32 kRemovals = 400;       // components a frame takes away
	constexpr double kAssetBudget = 0.006; // seconds per frame finishing models

	FVector Position(const float* P) { return FVector(P[0], P[1], P[2]); }
	FQuat Rotation(const float* Q) { return FQuat(Q[0], Q[1], Q[2], Q[3]); }

	// Checking while it runs (console, or ModLoader/ue5console.txt through the bridge).
	TAutoConsoleVariable<int32> CVarGrass(TEXT("gtav.Grass"), 1, TEXT("Grass batches drawn (0: none)."));
	TAutoConsoleVariable<int32> CVarWater(TEXT("gtav.Water"), 1, TEXT("The sea and the water quads drawn (0: hidden)."));
	TAutoConsoleVariable<float> CVarLodScale(TEXT("gtav.LodScale"), 0.0f, TEXT("Every distance scaled by this (0: -gtavlod, else 1: the game's)."));
	TSet<uint32> GHidden; // archetypes hidden (checking)
	FAutoConsoleCommandWithWorldAndArgs HideCommand(TEXT("gtav.Hide"), TEXT("gtav.Hide <archetype name or hex hash> ...: not drawn (gtav.Hide with nothing: all back)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
			if (Args.IsEmpty())
			{
				GHidden.Reset();
			}
			for (const FString& A : Args)
			{
				uint32 Hash = 0;
				if (A.StartsWith(TEXT("0x")))
				{
					Hash = FParse::HexNumber(*A.Mid(2));
				}
				else
				{
					Hash = GTAVHash(A);
				}
				GHidden.Add(Hash);
				UE_LOG(LogGTAVWorld, Display, TEXT("hidden: %s (%08x)"), *A, Hash);
			}
		}));
	// What Windows lets this process have of the GPU's memory (its share beside the game and everything else running) and
	// what it uses; false when it cannot be asked.
	bool GpuMemoryShare(int64& OutBudget, int64& OutUsage)
	{
		static IDXGIAdapter3* Adapter = [] {
			IDXGIAdapter3* Found = nullptr;
			ID3D12DynamicRHI* RHI = GetID3D12DynamicRHI();
			ID3D12Device* Device = RHI ? RHI->RHIGetDevice(0) : nullptr;
			IDXGIFactory4* Factory = nullptr;
			if (Device && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&Factory))))
			{
				Factory->EnumAdapterByLuid(Device->GetAdapterLuid(), IID_PPV_ARGS(&Found));
				Factory->Release();
			}
			return Found;
		}();
		DXGI_QUERY_VIDEO_MEMORY_INFO Info{};
		if (!Adapter || FAILED(Adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &Info)))
		{
			return false;
		}
		OutBudget = int64(Info.Budget);
		OutUsage = int64(Info.CurrentUsage);
		return true;
	}

	FAutoConsoleCommandWithWorldAndArgs DumpCommand(TEXT("gtav.Dump"), TEXT("gtav.Dump x y z [radius]: what is drawn near a point (the game's metres)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
			if (UGTAVWorldSubsystem* S = World ? World->GetSubsystem<UGTAVWorldSubsystem>() : nullptr; S && Args.Num() >= 3)
			{
				S->DumpNear(FVector(FCString::Atof(*Args[0]), FCString::Atof(*Args[1]), FCString::Atof(*Args[2])), Args.Num() > 3 ? FCString::Atof(*Args[3]) : 20.0f);
			}
		}));
}

void UGTAVWorldSubsystem::DumpNear(const FVector& GamePoint, float Radius)
{
	const FVector P(GamePoint.X * 100, -GamePoint.Y * 100, GamePoint.Z * 100);
	Lights.Dump(P, Radius * 100);
	// (an interior's entities: wanted, drawn or not)
	for (const TPair<uint64, uint32>& W : Wanted)
	{
		if (!(W.Key & kInteriorKey))
		{
			continue;
		}
		const FVector At = EntityTransform(W.Key).GetLocation();
		if (FVector::Dist(At, P) > Radius * 100)
		{
			continue;
		}
		const UStaticMeshComponent* C = Drawn.FindRef(W.Key);
		UE_LOG(LogGTAVWorld, Display, TEXT("dump: interior entity %llx archetype %08x at %.2f %.2f %.2f: %s"), W.Key, W.Value, At.X / 100, -At.Y / 100, At.Z / 100,
			!C ? TEXT("not drawn") : C->IsVisible() ? TEXT("drawn") : TEXT("drawn, hidden"));
	}
	for (const TPair<uint64, TObjectPtr<UStaticMeshComponent>>& D : Drawn)
	{
		if (D.Key & kInteriorKey)
		{
			continue;
		}
		const uint32 I = uint32(D.Key);
		if (Data.Distance(I, P) > Radius * 100)
		{
			continue;
		}
		const GTAVPack::Entity& E = Data.Entities[I];
		UE_LOG(LogGTAVWorld, Display, TEXT("dump: entity %u archetype %08x lod %u ymap %u (%08x) parent %d lodDist %.0f at %.1f %.1f %.1f"), I, E.archetype, E.lodLevel,
			E.ymap, Data.Ymaps[E.ymap].name, int32(E.parent), E.lodDist, E.position[0] / 100, -E.position[1] / 100, E.position[2] / 100);
	}
	for (const TPair<uint32, TObjectPtr<UInstancedStaticMeshComponent>>& G : DrawnGrass)
	{
		const GTAVPack::GrassBatch& B = Data.GrassBatches[G.Key];
		const FBox Box(FVector(B.boundsMin[0], B.boundsMin[1], B.boundsMin[2]), FVector(B.boundsMax[0], B.boundsMax[1], B.boundsMax[2]));
		if (Box.ComputeSquaredDistanceToPoint(GamePoint) > Radius * Radius)
		{
			continue;
		}
		UE_LOG(LogGTAVWorld, Display, TEXT("dump: grass %u archetype %08x %u instances, scale %.2f..%.2f, box %s"), G.Key, B.archetype, B.instanceCount,
			B.scaleRange[0], B.scaleRange[1], *Box.ToString());
	}
}

bool UGTAVWorldSubsystem::IsInside(const FVector& Point) const
{
	for (const uint32 Entity : NearInteriors)
	{
		const GTAVPack::Entity& E = Data.Entities[Entity];
		const GTAVPack::InteriorPlacement& P = Data.InteriorPlacements[E.interiorPlacement];
		if (P.interior >= uint32(Data.Interiors.Num()))
		{
			continue;
		}
		const GTAVPack::Interior& In = Data.Interiors[P.interior];
		const FVector Local = FTransform(Rotation(E.rotation), Position(E.position)).InverseTransformPosition(Point);
		// (room 0 is the limbo outside)
		for (uint32 R = 1; R < In.roomCount; ++R)
		{
			const GTAVPack::Room& Room = Data.Rooms[In.firstRoom + R];
			if (FBox(Position(Room.boundsMin), Position(Room.boundsMax)).IsInsideOrOn(Local))
			{
				return true;
			}
		}
	}
	return false;
}

bool UGTAVWorldSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && World->IsGameWorld() && FString(FCommandLine::Get()).Contains(TEXT("-gtavpack="));
}

void UGTAVWorldSubsystem::Deinitialize()
{
	Drawn.Empty();
	DrawnGrass.Empty();
	Pool.Empty();
	Assets.Reset();
	Super::Deinitialize();
}

// Sun, sky, clouds, fog: lit by Lumen (the project's settings).
void UGTAVWorldSubsystem::AddSky(UWorld& World)
{
	ADirectionalLight* Sun = World.SpawnActor<ADirectionalLight>(FVector(0, 0, 10000), FRotator(-35, 40, 0));
	Sun->SetMobility(EComponentMobility::Movable);
	UDirectionalLightComponent* Light = CastChecked<UDirectionalLightComponent>(Sun->GetLightComponent());
	Light->SetAtmosphereSunLight(true);
	Light->bCastCloudShadows = true; // (the volumetric clouds shade the ground)
	Light->SetIntensity(10.0f);
	World.SpawnActor<ASkyAtmosphere>(FVector::ZeroVector, FRotator::ZeroRotator);
	ASkyLight* Sky = World.SpawnActor<ASkyLight>(FVector(0, 0, 5000), FRotator::ZeroRotator);
	Sky->GetLightComponent()->SetMobility(EComponentMobility::Movable);
	Sky->GetLightComponent()->bRealTimeCapture = true;
	Sky->GetLightComponent()->RecaptureSky();
	World.SpawnActor<AVolumetricCloud>(FVector::ZeroVector, FRotator::ZeroRotator);
	World.SpawnActor<AExponentialHeightFog>(FVector::ZeroVector, FRotator::ZeroRotator);
}

// The glows' shape (the map's lights seen from afar, the lamps' lit lenses): a sphere 100 cm across, an icosahedron's
// with each face split in four `Splits` times, the new corners out on the sphere. The engine's sphere has hundreds of
// triangles; with the tens of thousands of glows over a city by night they took 8 ms of the frame in the translucent
// pass (research §60.13), and a glow is mostly a disk a few pixels wide: those have 80 faces (one split) - but a lamp's
// near the camera is a disk of tens of pixels, where the icosahedron's own 20 faces showed as a hexagon and 80 as an
// octagon: the near lamps' (few) have 1,280 (three).
UStaticMesh* UGTAVWorldSubsystem::MakeGlowShape(int32 Splits)
{
	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/GTAV/Materials/M_GTAV_Corona.M_GTAV_Corona"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Assets || !Material)
	{
		return nullptr;
	}
	const float P = (1.0f + FMath::Sqrt(5.0f)) / 2.0f;
	TArray<FVector3f> Points = {{-1, P, 0}, {1, P, 0}, {-1, -P, 0}, {1, -P, 0}, {0, -1, P}, {0, 1, P}, {0, -1, -P}, {0, 1, -P}, {P, 0, -1}, {P, 0, 1},
		{-P, 0, -1}, {-P, 0, 1}};
	for (FVector3f& Point : Points)
	{
		Point = Point.GetSafeNormal();
	}
	static constexpr uint32 Faces[20][3] = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
		{3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
	TMap<uint64, uint32> Middles;
	const auto Middle = [&](uint32 A, uint32 B) {
		const uint64 Key = uint64(FMath::Min(A, B)) << 32 | FMath::Max(A, B);
		if (const uint32* Found = Middles.Find(Key))
		{
			return *Found;
		}
		const uint32 M = uint32(Points.Add(((Points[A] + Points[B]) * 0.5f).GetSafeNormal()));
		Middles.Add(Key, M);
		return M;
	};
	TArray<uint32> Corners;
	for (const auto& F : Faces)
	{
		Corners.Append({F[0], F[1], F[2]});
	}
	for (int32 Split = 0; Split < Splits; ++Split)
	{
		TArray<uint32> Finer;
		for (int32 I = 0; I + 2 < Corners.Num(); I += 3)
		{
			const uint32 A = Corners[I], B = Corners[I + 1], C = Corners[I + 2];
			const uint32 AB = Middle(A, B), BC = Middle(B, C), CA = Middle(C, A);
			Finer.Append({A, AB, CA, B, BC, AB, C, CA, BC, AB, BC, CA});
		}
		Corners = MoveTemp(Finer);
	}
	TArray<FStaticMeshBuildVertex> Vertices;
	for (const FVector3f& N : Points)
	{
		FStaticMeshBuildVertex V{};
		V.Position = N * 50.0f;
		V.TangentZ = N;
		V.TangentX = (FMath::Abs(N.Z) < 0.9f ? FVector3f(0, 0, 1) ^ N : FVector3f(1, 0, 0) ^ N).GetSafeNormal();
		V.TangentY = N ^ V.TangentX;
		V.Color = FColor::White;
		Vertices.Add(V);
	}
	TArray<uint32> Indices;
	for (int32 I = 0; I + 2 < Corners.Num(); I += 3)
	{
		// (a face is seen from the side its corners go round clockwise, Unreal's way: (c - a) x (b - a) outwards)
		const FVector3f& A = Vertices[Corners[I]].Position;
		const FVector3f& B = Vertices[Corners[I + 1]].Position;
		const FVector3f& C = Vertices[Corners[I + 2]].Position;
		const bool bOut = (((C - A) ^ (B - A)) | (A + B + C)) > 0;
		Indices.Append({Corners[I], bOut ? Corners[I + 1] : Corners[I + 2], bOut ? Corners[I + 2] : Corners[I + 1]});
	}
	return Assets->BuildMesh(Vertices, Indices, Material);
}

void UGTAVWorldSubsystem::OnWorldBeginPlay(UWorld& World)
{
	Super::OnWorldBeginPlay(World);
	FString Folder, Error;
	FParse::Value(FCommandLine::Get(), TEXT("-gtavpack="), Folder, false);
	FParse::Value(FCommandLine::Get(), TEXT("-gtavlod="), DistanceScale);
	FParse::Value(FCommandLine::Get(), TEXT("-gtavhour="), CurrentHour);
	// (checking without the game: -gtavweather=RAIN and so on, at -gtavhour)
	FString Weather;
	if (FParse::Value(FCommandLine::Get(), TEXT("-gtavweather="), Weather))
	{
		OfflineWeather = GTAVHash(Weather);
	}
	int32 BudgetMB = 3072;
	FParse::Value(FCommandLine::Get(), TEXT("-gtavbudget="), BudgetMB);
	BudgetBytes = int64(BudgetMB) << 20;
	const double Start = FPlatformTime::Seconds();
	if (!Pack.Open(Folder, Error) || !Data.Load(Pack.World, Error))
	{
		UE_LOG(LogGTAVWorld, Error, TEXT("%s"), *Error);
		return;
	}
	if (FParse::Param(FCommandLine::Get(), TEXT("gtavsky")))
	{
		AddSky(World);
	}
	Assets = MakeUnique<FGTAVRuntimeAssets>(Pack);
	Root = World.SpawnActor<AActor>();
	USceneComponent* RootComponent = NewObject<USceneComponent>(Root, TEXT("Root"));
	Root->SetRootComponent(RootComponent);
	RootComponent->RegisterComponent();
	UpdateYmaps();
	BuildWater();
	GTAVSky::SetSkyTextures(Assets->GlobalTexture(TEXT("moon-new"), false), Assets->GlobalTexture(TEXT("starfield"), false));
	const int32 LightCount = Data.LodLights.Num();
	Lights.Load(MoveTemp(Data.LodLights));
	Lights.SetGlowShape(MakeGlowShape(1), MakeGlowShape(3));
	UE_LOG(LogGTAVWorld, Display, TEXT("%d LOD lights"), LightCount);
	bLoaded = true;
	int32 Roots = 0;
	for (const TPair<FIntPoint, FGTAVWorldData::FCell>& Cell : Data.Grid)
	{
		Roots += Cell.Value.Roots.Num();
	}
	UE_LOG(LogGTAVWorld, Display, TEXT("%s: %d archetypes, %d ymaps, %d entities (%d roots on %d cells, %d far), %d interiors, %d grass batches in %.2f s"),
		*Folder, Data.Archetypes.Num(), Data.Ymaps.Num(), Data.Entities.Num(), Roots, Data.Grid.Num(), Data.Far.Num(),
		Data.Interiors.Num(), Data.GrassBatches.Num(), FPlatformTime::Seconds() - Start);
}

// The sea, lakes and rivers (water.xml): one mesh of the quads (the box, or the right triangle of it its type says:
// 1 to 4 leave out the corner at max x / max y, max x / min y, min x / min y, min x / max y), and the open sea around
// the map out to the horizon.
void UGTAVWorldSubsystem::BuildWater()
{
	if (Data.WaterQuads.IsEmpty())
	{
		return;
	}
	TArray<FStaticMeshBuildVertex> Vertices;
	TArray<uint32> Indices;
	const auto Vertex = [&](float X, float Y, float Z) {
		FStaticMeshBuildVertex V{};
		V.Position = FVector3f(X * 100, -Y * 100, Z * 100);
		V.TangentX = FVector3f(1, 0, 0);
		V.TangentY = FVector3f(0, 1, 0);
		V.TangentZ = FVector3f(0, 0, 1);
		V.Color = FColor::White;
		V.UVs[0] = FVector2f(X, Y) * 0.01f;
		return uint32(Vertices.Add(V));
	};
	// (the winding the game's models have here, mirrored: seen from above, counter-clockwise in the game's space;
	// the other way round the sea is a back face from above and nothing shows)
	const auto Quad = [&](float X0, float Y0, float X1, float Y1, float Z, int32 Type) {
		const uint32 C0 = Vertex(X0, Y0, Z), C1 = Vertex(X1, Y0, Z), C2 = Vertex(X1, Y1, Z), C3 = Vertex(X0, Y1, Z);
		switch (Type)
		{
		case 1: Indices.Append({C0, C1, C3}); break;
		case 2: Indices.Append({C0, C2, C3}); break;
		case 3: Indices.Append({C1, C2, C3}); break;
		case 4: Indices.Append({C0, C1, C2}); break;
		default: Indices.Append({C0, C1, C2, C0, C2, C3}); break;
		}
	};
	FBox2f Covered(ForceInit);
	for (const GTAVPack::WaterQuad& Q : Data.WaterQuads)
	{
		if (Q.flags & GTAVPack::kInvisible)
		{
			continue;
		}
		Quad(Q.minX, Q.minY, Q.maxX, Q.maxY, Q.z, Q.type);
		Covered += FVector2f(Q.minX, Q.minY);
		Covered += FVector2f(Q.maxX, Q.maxY);
	}
	// (out past any horizon: from high up the horizon is 60 km and more away, and the sea there has to be the sea,
	// reflecting the bright sky at that grazing angle, not the atmosphere's dark ground)
	constexpr float Far = 400000.0f;
	const auto Add = [&](int32 Kind) {
		UMaterialInterface* Material = Assets->WaterMaterial(Kind);
		UE_LOG(LogGTAVWorld, Display, TEXT("water (kind %d): %d vertices, %d indices, material %s"), Kind, Vertices.Num(), Indices.Num(), *GetNameSafe(Material));
		UStaticMesh* Mesh = Assets->BuildMesh(Vertices, Indices, Material);
		Vertices.Reset();
		Indices.Reset();
		if (!Mesh)
		{
			return;
		}
		UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(Root);
		C->SetMobility(EComponentMobility::Static);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCastShadow(false);
		C->SetStaticMesh(Mesh);
		C->RegisterComponent();
		Water.Add(C);
	};
	Add(7);
	// The open sea around (opaque: the haze then covers it as it does the land).
	Quad(-Far, -Far, Far, Covered.Min.Y, 0, 0);
	Quad(-Far, Covered.Max.Y, Far, Far, 0, 0);
	Quad(-Far, Covered.Min.Y, Covered.Min.X, Covered.Max.Y, 0, 0);
	Quad(Covered.Max.X, Covered.Min.Y, Far, Covered.Max.Y, 0, 0);
	Add(9);
}

void UGTAVWorldSubsystem::SetGroup(uint32 Name, bool bOn)
{
	if (bOn ? (GroupsOn.Add(Name), true) : GroupsOn.Remove(Name) > 0)
	{
		UpdateYmaps();
		LastUpdate = 0;
	}
}

void UGTAVWorldSubsystem::SetGroupsOn(const TSet<uint32>& On)
{
	if (On.Num() != GroupsOn.Num() || !On.Includes(GroupsOn))
	{
		GroupsOn = On;
		UpdateYmaps();
		LastUpdate = 0;
	}
}

void UGTAVWorldSubsystem::SetHour(int32 Hour)
{
	if (Hour != CurrentHour)
	{
		CurrentHour = Hour;
		UpdateYmaps();
		LastUpdate = 0;
	}
}

// A ymap of a map data group is there only while the group is on (switched by a script, or by the hour).
void UGTAVWorldSubsystem::UpdateYmaps()
{
	YmapOn.SetNum(Data.Ymaps.Num());
	for (int32 I = 0; I < Data.Ymaps.Num(); ++I)
	{
		const uint32 G = Data.Ymaps[I].group;
		bool bOn = true;
		if (G != GTAVPack::kNone && G < uint32(Data.Groups.Num()))
		{
			const GTAVPack::Group& Group = Data.Groups[G];
			bOn = GroupsOn.Contains(Group.name) || (Group.hoursOnOff && ((Group.hoursOnOff >> CurrentHour) & 1));
		}
		YmapOn[I] = bOn;
	}
}

bool UGTAVWorldSubsystem::Shown(uint32 Archetype) const
{
	if (GHidden.Contains(Archetype))
	{
		return false;
	}
	const GTAVPack::Archetype* A = Data.Archetype(Archetype);
	if (!A || !A->hasModel)
	{
		return false;
	}
	return A->kind != GTAVPack::kTimed || ((A->timeFlags >> CurrentHour) & 1);
}

bool UGTAVWorldSubsystem::IsSettled() const
{
	if (!bLoaded)
	{
		return true;
	}
	if (Assets->Loading() || WantedMissing || Wanted.Num() != Drawn.Num() || WantedGrass.Num() != DrawnGrass.Num())
	{
		return false;
	}
	return true;
}

void UGTAVWorldSubsystem::Tick(float DeltaTime)
{
	if (!bLoaded)
	{
		return;
	}
	const double TickStart = FPlatformTime::Seconds();
	const int32 BuiltBefore = Assets->MeshesBuilt, TexturesBefore = Assets->TexturesMade, DrawnBefore = Drawn.Num();
	double AssetsSpent = 0, UpdateSpent = 0, ApplySpent = 0;
	ON_SCOPE_EXIT
	{
		const double Spent = FPlatformTime::Seconds() - TickStart;
		TickSeconds += Spent;
		TickMax = FMath::Max(TickMax, Spent);
		++Ticks;
		const double Gpu = FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0)), Render = FPlatformTime::ToMilliseconds(GRenderThreadTime);
		GpuSeconds += Gpu / 1000.0;
		RenderSeconds += Render / 1000.0;
		// (checking: a frame of the renderer's own that took long, and what it did)
		if (Spent > 0.02 || Render > 40.0 || Gpu > 40.0)
		{
			UE_LOG(LogGTAVWorld, Display, TEXT("slow: world %.1f ms (assets %.1f, update %.1f, apply %.1f; %d models, %d textures, %+d drawn), render thread %.1f ms, GPU %.1f ms"),
				Spent * 1000.0, AssetsSpent * 1000.0, UpdateSpent * 1000.0, ApplySpent * 1000.0, Assets->MeshesBuilt - BuiltBefore,
				Assets->TexturesMade - TexturesBefore, Drawn.Num() - DrawnBefore, Render, Gpu);
		}
	};
	{
		const double Start = FPlatformTime::Seconds();
		Assets->Tick(kAssetBudget);
		AssetsSpent = FPlatformTime::Seconds() - Start;
	}
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (!PC || !PC->PlayerCameraManager)
	{
		return;
	}
	const FVector Center = PC->PlayerCameraManager->GetCameraLocation();
	// (the screen the textures' detail is for)
	if (GEngine->GameViewport && GEngine->GameViewport->Viewport)
	{
		const float Width = float(GEngine->GameViewport->Viewport->GetSizeXY().X);
		const float Fov = FMath::Clamp(PC->PlayerCameraManager->GetFOVAngle(), 5.0f, 170.0f);
		Assets->SetPixelsPerRadian(Width / (2.0f * FMath::Tan(FMath::DegreesToRadians(Fov) * 0.5f)));
	}
	if (OfflineWeather)
	{
		GTAVSky::Apply(GetWorld(), Center, float(CurrentHour), OfflineWeather, OfflineWeather, 0.0f);
	}
	Lights.Update(Root, Center, CurrentHour);
	GTAVSky::SetIndoors(IsInside(Center), false);
	const double Now = FPlatformTime::Seconds();
	if (FVector::DistSquared(Center, LastCenter) > 100.0f * 100.0f || Now - LastUpdate > 0.25)
	{
		const double Start = FPlatformTime::Seconds();
		Update(Center);
		UpdateSpent = FPlatformTime::Seconds() - Start;
		LastCenter = Center;
		LastUpdate = Now;
	}
	{
		const double Start = FPlatformTime::Seconds();
		Apply();
		ApplyObjects(Center);
		ApplySpent = FPlatformTime::Seconds() - Start;
	}
	for (UStaticMeshComponent* W : Water)
	{
		if (W && W->IsVisible() != (CVarWater.GetValueOnGameThread() != 0))
		{
			W->SetVisibility(CVarWater.GetValueOnGameThread() != 0);
		}
	}
	if (Now - LastEvict > 2.0)
	{
		// (-gtavbudget, or less: what Windows gives this process of the GPU's memory, 90% of it, less what the rest of
		// the renderer takes; over its share Windows moves memory out to the system's, and every frame waits on that)
		LastEvict = Now;
		int64 Budget = BudgetBytes, Share = 0, Usage = 0;
		if (GpuMemoryShare(Share, Usage))
		{
			const int64 Held = Assets->TextureBytes + Assets->MeshBytes;
			Budget = FMath::Clamp(Share / 10 * 9 - FMath::Max<int64>(Usage - Held, 0), int64(768) << 20, BudgetBytes);
		}
		AssetBudget = Budget;
		Assets->Evict(Budget, 20.0);
	}
	// (checking: -gtavdump=x,y,z,radius in the game's metres lists what is drawn there once everything is in)
	static bool bDumped = false;
	FString Dump;
	if (!bDumped && IsSettled() && Drawn.Num() && FParse::Value(FCommandLine::Get(), TEXT("-gtavdump="), Dump, false))
	{
		bDumped = true;
		TArray<FString> V;
		Dump.ParseIntoArray(V, TEXT(","));
		if (V.Num() >= 3)
		{
			DumpNear(FVector(FCString::Atof(*V[0]), FCString::Atof(*V[1]), FCString::Atof(*V[2])), V.Num() > 3 ? FCString::Atof(*V[3]) : 20.0f);
		}
	}
	static double Told = 0;
	if (Now - Told > 10)
	{
		Told = Now;
		UE_LOG(LogGTAVWorld, Display, TEXT("drawn %d (%d wanted, %d waiting for models), grass %d; models %d built, %d loading, %d failed; textures %d (%.0f MB, %d failed, %d retuned), meshes %.0f MB, %d evicted"),
			Drawn.Num(), Wanted.Num(), WantedMissing, DrawnGrass.Num(), Assets->MeshesBuilt, Assets->Loading(), Assets->MeshesFailed, Assets->TexturesMade,
			Assets->TextureBytes / 1048576.0, Assets->TexturesFailed, Assets->TexturesRetuned, Assets->MeshBytes / 1048576.0, Assets->MeshesEvicted);
		// (where a frame's time goes: this subsystem's own share of the game thread, the render thread, the GPU)
		int64 Share = 0, Usage = 0;
		GpuMemoryShare(Share, Usage);
		UE_LOG(LogGTAVWorld, Display, TEXT("time: world %.2f ms a frame (slowest %.1f), render thread %.1f ms, GPU %.1f ms; %d components pooled, %d objects; %s; GPU memory %lld of %lld MB, assets' budget %lld MB"),
			Ticks ? TickSeconds * 1000.0 / Ticks : 0.0, TickMax * 1000.0, Ticks ? RenderSeconds * 1000.0 / Ticks : 0.0,
			Ticks ? GpuSeconds * 1000.0 / Ticks : 0.0, Pool.Num(), GUObjectArray.GetObjectArrayNumMinusAvailable(), *Lights.Counts(),
			Usage >> 20, Share >> 20, AssetBudget >> 20);
		// (checking, -gtavprofileheavy: a GPU profile of a heavy stretch into the log, once a minute at most)
		static double LastProfile = 0;
		if (Ticks && GpuSeconds * 1000.0 / Ticks > 25.0 && Now - LastProfile > 60.0 && FParse::Param(FCommandLine::Get(), TEXT("gtavprofileheavy")))
		{
			LastProfile = Now;
			// (no window for it: one stays open and drawn each frame after)
			GEngine->Exec(GetWorld(), TEXT("r.ProfileGPU.ShowUI 0"));
			GEngine->Exec(GetWorld(), TEXT("ProfileGPU"));
		}
		TickSeconds = TickMax = GpuSeconds = RenderSeconds = 0;
		Ticks = 0;
	}
}

// An entity's children can take over: each is loaded, has nothing to load, or is not there now.
bool UGTAVWorldSubsystem::Ready(uint32 Entity, const FVector& Center)
{
	const GTAVPack::Entity& E = Data.Entities[Entity];
	if (!YmapOn[E.ymap] || E.interiorPlacement != GTAVPack::kNone || !Shown(E.archetype) || !GTAVPack::InMainView(E.flags))
	{
		return true;
	}
	const FGTAVRuntimeAssets::EState State = Assets->Request(E.archetype, Data.Distance(Entity, Center));
	return State == FGTAVRuntimeAssets::EState::Ready || State == FGTAVRuntimeAssets::EState::Failed;
}

void UGTAVWorldSubsystem::Want(uint64 Key, uint32 Archetype, float Distance)
{
	if (!Shown(Archetype))
	{
		return;
	}
	switch (Assets->Request(Archetype, Distance))
	{
	case FGTAVRuntimeAssets::EState::Ready:
		Wanted.Add(Key, Archetype);
		break;
	case FGTAVRuntimeAssets::EState::Loading:
		++WantedMissing;
		break;
	default:
		break;
	}
}

void UGTAVWorldSubsystem::Visit(uint32 Entity, const FVector& Center, bool bForced)
{
	const GTAVPack::Entity& E = Data.Entities[Entity];
	if (!YmapOn[E.ymap])
	{
		return;
	}
	const float D = Data.Distance(Entity, Center);
	if (!bForced && D > E.lodDist * 100.0f * DistanceScale)
	{
		return;
	}
	const TConstArrayView<uint32> Children = Data.Children(Entity);
	if (Children.Num())
	{
		float Switch = E.childLodDist;
		if (Switch <= 0)
		{
			for (const uint32 C : Children)
			{
				Switch = FMath::Max(Switch, Data.Entities[C].lodDist);
			}
		}
		if (D < Switch * 100.0f * DistanceScale)
		{
			// (all children at once, once they are all loaded: no holes while they come in)
			bool bAll = true;
			for (const uint32 C : Children)
			{
				bAll &= Ready(C, Center);
			}
			if (bAll)
			{
				for (const uint32 C : Children)
				{
					Visit(C, Center, true);
				}
				return;
			}
		}
	}
	if (E.interiorPlacement != GTAVPack::kNone)
	{
		WantInterior(Entity, Center);
		return;
	}
	if (GTAVPack::InMainView(E.flags))
	{
		Want(Entity, E.archetype, D);
	}
}

void UGTAVWorldSubsystem::SetGameInteriorSets(const GTAVBridge::InteriorSets* Interiors, uint32 Count)
{
	GameInteriorSets.SetNum(int32(FMath::Min(Count, GTAVBridge::kMaxInteriors)));
	for (int32 I = 0; I < GameInteriorSets.Num(); ++I)
	{
		GameInteriorSets[I] = Interiors[I];
		GameInteriorSets[I].position[0] = Interiors[I].position[0] * 100.0f; // (to Unreal's space: centimetres, y mirrored)
		GameInteriorSets[I].position[1] = -Interiors[I].position[1] * 100.0f;
		GameInteriorSets[I].position[2] = Interiors[I].position[2] * 100.0f;
	}
}

// An interior's entities and its entity sets on (the game's, where it says; else the default ones), near enough.
void UGTAVWorldSubsystem::WantInterior(uint32 Entity, const FVector& Center)
{
	const GTAVPack::Entity& E = Data.Entities[Entity];
	const GTAVPack::InteriorPlacement& P = Data.InteriorPlacements[E.interiorPlacement];
	if (P.interior == GTAVPack::kNone || P.interior >= uint32(Data.Interiors.Num()))
	{
		return;
	}
	const GTAVPack::Interior& In = Data.Interiors[P.interior];
	const FTransform Placement(Rotation(E.rotation), Position(E.position));
	NearInteriors.AddUnique(Entity);
	const auto Add = [&](uint32 Index) {
		const GTAVPack::InteriorEntity& X = Data.InteriorEntities[Index];
		if (!GTAVPack::InMainView(X.flags))
		{
			return;
		}
		const float Range = (X.lodDist > 0 ? X.lodDist : 100.0f) * 100.0f * DistanceScale;
		// (to its box: a tunnel's pieces are long)
		const FTransform Local(Rotation(X.rotation), Position(X.position), FVector(X.scaleXY, X.scaleXY, X.scaleZ));
		const GTAVPack::Archetype* A = Data.Archetype(X.archetype);
		const FBox Box = A && A->boundsMax[0] > A->boundsMin[0]
			? FBox(Position(A->boundsMin), Position(A->boundsMax)).TransformBy(Local * Placement)
			: FBox(Placement.TransformPosition(Position(X.position)), Placement.TransformPosition(Position(X.position)));
		const float D = FMath::Sqrt(float(Box.ComputeSquaredDistanceToPoint(Center)));
		if (D < Range)
		{
			Want(kInteriorKey | uint64(E.interiorPlacement) << 32 | Index, X.archetype, D);
		}
	};
	for (uint32 I = 0; I < In.entityCount; ++I)
	{
		Add(In.firstEntity + I);
	}
	// (the game's sets for this interior: its archetype's name, placed within 5 m of here)
	const GTAVBridge::InteriorSets* Game = nullptr;
	const FVector Here = Position(E.position);
	for (const GTAVBridge::InteriorSets& G : GameInteriorSets)
	{
		if (G.name == In.archetype && FVector::DistSquared(FVector(G.position[0], G.position[1], G.position[2]), Here) < FMath::Square(500.0))
		{
			Game = &G;
			break;
		}
	}
	const auto On = [&](uint32 Name) {
		for (uint32 K = 0; K < In.setCount; ++K)
		{
			const GTAVPack::EntitySet& Set = Data.EntitySets[In.firstSet + K];
			if (Set.name == Name)
			{
				for (uint32 I = 0; I < Set.entityCount; ++I)
				{
					Add(Set.firstEntity + I);
				}
			}
		}
	};
	if (Game)
	{
		for (uint32 S = 0; S < FMath::Min(Game->count, GTAVBridge::kMaxInteriorSets); ++S)
		{
			On(Game->sets[S]);
		}
	}
	else
	{
		for (uint32 S = 0; S < P.defaultSetCount; ++S)
		{
			On(Data.Lists[P.firstDefaultSet + S]);
		}
	}
}

void UGTAVWorldSubsystem::Update(const FVector& Center)
{
	NearInteriors.Reset();
	Wanted.Reset();
	WantedGrass.Reset();
	WantedMissing = 0;
	// (every cell: a few thousand box tests; an entity's box may reach far out of its cell)
	for (const TPair<FIntPoint, FGTAVWorldData::FCell>& Cell : Data.Grid)
	{
		if (FBox(Cell.Value.Bounds).ComputeSquaredDistanceToPoint(Center) > FMath::Square(Cell.Value.MaxDistance * DistanceScale))
		{
			continue;
		}
		for (const uint32 R : Cell.Value.Roots)
		{
			Visit(R, Center, false);
		}
	}
	for (const uint32 R : Data.Far)
	{
		Visit(R, Center, false);
	}

	if (const float Scale = CVarLodScale.GetValueOnGameThread(); Scale > 0)
	{
		DistanceScale = Scale;
	}
	ON_SCOPE_EXIT
	{
		Assets->EndRound();
	};
	// Grass: batches whose box is within their distance.
	constexpr float GrassReach = 40000.0f;
	if (!CVarGrass.GetValueOnGameThread())
	{
		return;
	}
	const FIntPoint GLo = FGTAVWorldData::CellOf(Center.X - GrassReach, Center.Y - GrassReach),
					GHi = FGTAVWorldData::CellOf(Center.X + GrassReach, Center.Y + GrassReach);
	for (int32 X = GLo.X; X <= GHi.X; ++X)
	{
		for (int32 Y = GLo.Y; Y <= GHi.Y; ++Y)
		{
			const TArray<uint32>* Batches = Data.GrassGrid.Find(FIntPoint(X, Y));
			if (!Batches)
			{
				continue;
			}
			for (const uint32 B : *Batches)
			{
				const GTAVPack::GrassBatch& G = Data.GrassBatches[B];
				if (!YmapOn[G.ymap] || !Shown(G.archetype))
				{
					continue;
				}
				const FBox Box(FVector(G.boundsMin[0] * 100, -G.boundsMax[1] * 100, G.boundsMin[2] * 100),
					FVector(G.boundsMax[0] * 100, -G.boundsMin[1] * 100, G.boundsMax[2] * 100));
				const double D2 = Box.ComputeSquaredDistanceToPoint(Center);
				if (D2 > FMath::Square(G.lodDist * 100.0f * DistanceScale))
				{
					continue;
				}
				const FGTAVRuntimeAssets::EState State = Assets->Request(G.archetype, FMath::Sqrt(D2));
				if (State == FGTAVRuntimeAssets::EState::Ready)
				{
					WantedGrass.Add(B);
				}
				else if (State == FGTAVRuntimeAssets::EState::Loading)
				{
					++WantedMissing;
				}
			}
		}
	}
}

namespace
{
	// A game object's place in Unreal's space: the models and the map are mirrored in y (y negated), so its axes are
	// too (the matrix M R M, M = diag(1, -1, 1)): x = M right, y = -M forward, z = M up; their lengths its scale.
	FTransform ObjectTransform(const GTAVBridge::GameObject& O)
	{
		const FVector X(O.right[0], -O.right[1], O.right[2]);
		const FVector Y(-O.forward[0], O.forward[1], -O.forward[2]);
		const FVector Z(O.up[0], -O.up[1], O.up[2]);
		const FVector P(O.position[0] * 100.0, -O.position[1] * 100.0, O.position[2] * 100.0);
		return FTransform(FMatrix(FPlane(X, 0.0), FPlane(Y, 0.0), FPlane(Z, 0.0), FPlane(P, 1.0)));
	}
}

// The map entity drawn with this model where the map placed a game object (within 30 cm), or 0.
uint64 UGTAVWorldSubsystem::FindPlaced(uint32 Model, const FVector& Placed) const
{
	for (const TPair<uint64, uint32>& W : Wanted)
	{
		if (W.Value == Model && Drawn.Contains(W.Key) && FVector::DistSquared(EntityTransform(W.Key).GetLocation(), Placed) < 30.0 * 30.0)
		{
			return W.Key;
		}
	}
	return 0;
}

void UGTAVWorldSubsystem::ApplyObjects(const FVector& Center)
{
	const double Now = FPlatformTime::Seconds();
	TSet<uint32> Seen;
	TSet<uint64> Hide;
	for (const GTAVBridge::GameObject& O : GameObjects)
	{
		Seen.Add(O.id);
		FObjectState& S = ObjectStates.FindOrAdd(O.id);
		if (S.Model != O.model)
		{
			S = FObjectState();
			S.Model = O.model;
		}
		UStaticMeshComponent* C = ObjectComponents.FindRef(O.id);
		// The map's copy of it (looked for once a second until found: it may not be drawn yet). One as the map has it
		// (the near ones come however they are) leaves the map's copy be.
		if (!FMath::IsNaN(O.placed[0]))
		{
			if (!S.Hides && Now - S.LastLook > 1.0)
			{
				S.LastLook = Now;
				S.Hides = FindPlaced(O.model, FVector(O.placed[0] * 100.0, -O.placed[1] * 100.0, O.placed[2] * 100.0));
			}
			if (S.Hides && (O.flags & GTAVBridge::kObjectVisible))
			{
				const FTransform Map = EntityTransform(S.Hides), Game = ObjectTransform(O);
				if (FVector::DistSquared(Map.GetLocation(), Game.GetLocation()) < 4.0 && Map.GetRotation().AngularDistance(Game.GetRotation()) < 0.02)
				{
					if (C && C->IsVisible())
					{
						C->SetVisibility(false);
					}
					continue;
				}
			}
			if (S.Hides)
			{
				Hide.Add(S.Hides);
			}
		}
		if (!(O.flags & GTAVBridge::kObjectVisible) || !Shown(O.model))
		{
			if (C)
			{
				C->SetVisibility(false);
			}
			continue;
		}
		const FTransform Transform = ObjectTransform(O);
		if (Assets->Request(O.model, float(FVector::Dist(Transform.GetLocation(), Center))) != FGTAVRuntimeAssets::EState::Ready)
		{
			continue;
		}
		UStaticMesh* Mesh = Assets->Get(O.model);
		if (!Mesh)
		{
			continue;
		}
		if (!C)
		{
			C = NewObject<UStaticMeshComponent>(Root);
			C->SetMobility(EComponentMobility::Movable);
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			C->SetGenerateOverlapEvents(false);
			C->SetStaticMesh(Mesh);
			C->SetWorldTransform(Transform);
			// (the tint of the entity it stands in for)
			C->SetCustomPrimitiveDataFloat(0, S.Hides ? float(EntityTint(S.Hides)) : 0.0f);
			C->RegisterComponent();
			ObjectComponents.Add(O.id, C);
		}
		else
		{
			if (C->GetStaticMesh() != Mesh)
			{
				C->SetStaticMesh(Mesh);
			}
			if (!C->GetComponentTransform().Equals(Transform, 0.01))
			{
				C->SetWorldTransform(Transform);
			}
		}
		if (!C->IsVisible())
		{
			C->SetVisibility(true);
		}
	}
	// Objects gone: their components, and the map's copies they stood in for back.
	for (auto It = ObjectComponents.CreateIterator(); It; ++It)
	{
		if (!Seen.Contains(It.Key()))
		{
			It.Value()->DestroyComponent();
			It.RemoveCurrent();
		}
	}
	for (auto It = ObjectStates.CreateIterator(); It; ++It)
	{
		if (!Seen.Contains(It.Key()))
		{
			It.RemoveCurrent();
		}
	}
	for (const uint64 K : HiddenByObjects)
	{
		if (!Hide.Contains(K))
		{
			if (UStaticMeshComponent* D = Drawn.FindRef(K))
			{
				D->SetVisibility(true);
			}
		}
	}
	for (const uint64 K : Hide)
	{
		if (UStaticMeshComponent* D = Drawn.FindRef(K); D && D->IsVisible())
		{
			D->SetVisibility(false);
		}
	}
	HiddenByObjects = MoveTemp(Hide);
}

uint32 UGTAVWorldSubsystem::EntityTint(uint64 Key) const
{
	if (Key & kInteriorKey)
	{
		return Data.InteriorEntities[uint32(Key)].tintValue;
	}
	return Data.Entities[uint32(Key)].tintValue;
}

FTransform UGTAVWorldSubsystem::EntityTransform(uint64 Key) const
{
	if (Key & kInteriorKey)
	{
		const uint32 PlacementIndex = uint32(Key >> 32) & 0x7FFFFFFF, Index = uint32(Key);
		const GTAVPack::Entity& E = Data.Entities[Data.InteriorPlacements[PlacementIndex].entity];
		const GTAVPack::InteriorEntity& X = Data.InteriorEntities[Index];
		const FTransform Local(Rotation(X.rotation), Position(X.position), FVector(X.scaleXY, X.scaleXY, X.scaleZ));
		return Local * FTransform(Rotation(E.rotation), Position(E.position));
	}
	const GTAVPack::Entity& E = Data.Entities[uint32(Key)];
	return FTransform(Rotation(E.rotation), Position(E.position), FVector(E.scaleXY, E.scaleXY, E.scaleZ));
}

void UGTAVWorldSubsystem::Apply()
{
	const double Start = FPlatformTime::Seconds();
	bool bAllAdded = true;
	for (const TPair<uint64, uint32>& W : Wanted)
	{
		if (Drawn.Contains(W.Key))
		{
			continue;
		}
		if (FPlatformTime::Seconds() - Start > kAddBudget)
		{
			bAllAdded = false;
			break;
		}
		UStaticMesh* Mesh = Assets->Get(W.Value);
		if (!Mesh)
		{
			continue;
		}
		UStaticMeshComponent* C = Pool.Num() ? Pool.Pop(EAllowShrinking::No).Get() : nullptr;
		if (!C)
		{
			C = NewObject<UStaticMeshComponent>(Root);
			C->SetMobility(EComponentMobility::Static);
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			C->SetGenerateOverlapEvents(false);
		}
		C->SetStaticMesh(Mesh);
		const FTransform Transform = EntityTransform(W.Key);
		C->SetWorldTransform(Transform);
		// (a pooled component may come hidden: a game object stood in for its last entity)
		C->SetVisibility(!HiddenByObjects.Contains(W.Key));
		// (the entity's tint: the palette row its tinted materials take; see EntityRow in the masters)
		C->SetCustomPrimitiveDataFloat(0, float(EntityTint(W.Key)));
		C->RegisterComponent();
		Drawn.Add(W.Key, C);
		if (const TArray<GTAVPack::ModelLight>* ML = Assets->LightsOf(W.Value))
		{
			Lights.AddEntity(W.Key, Transform, *ML);
		}
	}
	for (const uint32 B : WantedGrass)
	{
		if (DrawnGrass.Contains(B))
		{
			continue;
		}
		if (FPlatformTime::Seconds() - Start > kAddBudget)
		{
			bAllAdded = false;
			break;
		}
		const GTAVPack::GrassBatch& G = Data.GrassBatches[B];
		UStaticMesh* Mesh = Assets->Get(G.archetype);
		if (!Mesh)
		{
			continue;
		}
		UInstancedStaticMeshComponent* C = NewObject<UInstancedStaticMeshComponent>(Root);
		C->SetMobility(EComponentMobility::Static);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetStaticMesh(Mesh);
		C->SetCullDistances(int32(G.fadeStart * 100.0f * DistanceScale), int32(G.lodDist * 100.0f * DistanceScale));
		TArray<FTransform> Transforms;
		Transforms.Reserve(G.instanceCount);
		const FVector Min(G.boundsMin[0], G.boundsMin[1], G.boundsMin[2]), Size = FVector(G.boundsMax[0], G.boundsMax[1], G.boundsMax[2]) - Min;
		for (uint32 I = 0; I < G.instanceCount; ++I)
		{
			const GTAVPack::GrassInstance& X = Data.GrassInstances[G.firstInstance + I];
			const FVector P = Min + Size * FVector(X.position[0], X.position[1], X.position[2]) / 65535.0;
			const float Scale = FMath::Lerp(G.scaleRange[0], G.scaleRange[1], X.scale / 255.0f);
			// (no yaw stored: one from the position; tilted to the ground normal when the batch says so)
			const float Yaw = float((X.position[0] * 7919u + X.position[1] * 104729u) % 360u);
			FQuat Q = FQuat(FVector::UpVector, FMath::DegreesToRadians(Yaw));
			if (G.orientToTerrain > 0)
			{
				const float Nx = X.normalX / 127.5f - 1.0f, Ny = -(X.normalY / 127.5f - 1.0f);
				const FVector N(Nx, Ny, FMath::Sqrt(FMath::Max(0.0f, 1.0f - Nx * Nx - Ny * Ny)));
				Q = FQuat::Slerp(FQuat::Identity, FQuat::FindBetweenNormals(FVector::UpVector, N), G.orientToTerrain) * Q;
			}
			Transforms.Add(FTransform(Q, FVector(P.X * 100, -P.Y * 100, P.Z * 100), FVector(Scale)));
		}
		C->NumCustomDataFloats = 3;
		C->AddInstances(Transforms, false, true);
		// (the ground's colour each blade carries: a factor around the game's mid grey 128, in its gamma space)
		for (uint32 I = 0; I < G.instanceCount; ++I)
		{
			const GTAVPack::GrassInstance& X = Data.GrassInstances[G.firstInstance + I];
			float Tint[3];
			for (int32 K = 0; K < 3; ++K)
			{
				Tint[K] = FMath::Pow(X.color[K] / 127.5f, 2.2f);
			}
			C->SetCustomData(int32(I), MakeArrayView(Tint, 3), false);
		}
		C->RegisterComponent();
		DrawnGrass.Add(B, C);
	}
	// What is not wanted goes once everything wanted is there (a parent stays until its children are drawn).
	if (!bAllAdded)
	{
		return;
	}
	// (a few hundred a frame: thousands at once, after a long way, hold the render thread up for a tenth of a second)
	int32 Removed = 0;
	for (auto It = Drawn.CreateIterator(); It && Removed < kRemovals; ++It)
	{
		if (!Wanted.Contains(It.Key()))
		{
			++Removed;
			It.Value()->UnregisterComponent();
			Pool.Add(It.Value());
			Lights.RemoveEntity(It.Key());
			It.RemoveCurrent();
		}
	}
	for (auto It = DrawnGrass.CreateIterator(); It; ++It)
	{
		if (!WantedGrass.Contains(It.Key()))
		{
			It.Value()->DestroyComponent();
			It.RemoveCurrent();
		}
	}
}
