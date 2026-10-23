#include "GTAVWorldSubsystem.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Misc/CommandLine.h"

#include "GTAVRuntimeAssets.h"

DEFINE_LOG_CATEGORY_STATIC(LogGTAVWorld, Log, All);

bool UGTAVWorldSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && World->IsGameWorld() && FString(FCommandLine::Get()).Contains(TEXT("-gtavpack="));
}

void UGTAVWorldSubsystem::Deinitialize()
{
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
	Light->SetIntensity(10.0f);
	World.SpawnActor<ASkyAtmosphere>(FVector::ZeroVector, FRotator::ZeroRotator);
	ASkyLight* Sky = World.SpawnActor<ASkyLight>(FVector(0, 0, 5000), FRotator::ZeroRotator);
	Sky->GetLightComponent()->SetMobility(EComponentMobility::Movable);
	Sky->GetLightComponent()->bRealTimeCapture = true;
	Sky->GetLightComponent()->RecaptureSky();
	World.SpawnActor<AVolumetricCloud>(FVector::ZeroVector, FRotator::ZeroRotator);
	World.SpawnActor<AExponentialHeightFog>(FVector::ZeroVector, FRotator::ZeroRotator);
}

void UGTAVWorldSubsystem::OnWorldBeginPlay(UWorld& World)
{
	Super::OnWorldBeginPlay(World);
	FString Folder, Error;
	FParse::Value(FCommandLine::Get(), TEXT("-gtavpack="), Folder, false);
	if (!Pack.Open(Folder, Error))
	{
		UE_LOG(LogGTAVWorld, Error, TEXT("%s"), *Error);
		bReady = true;
		return;
	}
	if (FParse::Param(FCommandLine::Get(), TEXT("gtavsky")))
	{
		AddSky(World);
	}
	const double Start = FPlatformTime::Seconds();
	Assets = MakeUnique<FGTAVRuntimeAssets>(Pack);

	// A test area's placements: one instanced component per model.
	TArray<uint8> Data;
	if (!Pack.World.Read(GTAVPack::kPlacements, Data))
	{
		UE_LOG(LogGTAVWorld, Error, TEXT("%s: no placements"), *Folder);
		bReady = true;
		return;
	}
	const int32 Count = Data.Num() / sizeof(GTAVPack::Placement);
	const GTAVPack::Placement* Placements = reinterpret_cast<const GTAVPack::Placement*>(Data.GetData());
	TMap<uint64, TArray<FTransform>> ByModel;
	for (int32 I = 0; I < Count; ++I)
	{
		const GTAVPack::Placement& P = Placements[I];
		ByModel.FindOrAdd(P.model).Add(FTransform(FQuat(P.rotation[0], P.rotation[1], P.rotation[2], P.rotation[3]),
			FVector(P.position[0], P.position[1], P.position[2]), FVector(P.scale[0], P.scale[1], P.scale[2])));
	}
	AActor* Root = World.SpawnActor<AActor>();
	USceneComponent* RootComponent = NewObject<USceneComponent>(Root, TEXT("Root"));
	Root->SetRootComponent(RootComponent);
	RootComponent->RegisterComponent();
	int32 Placed = 0;
	for (const TPair<uint64, TArray<FTransform>>& Model : ByModel)
	{
		UStaticMesh* Mesh = Assets->Mesh(Model.Key);
		if (!Mesh)
		{
			continue;
		}
		UInstancedStaticMeshComponent* Component = NewObject<UInstancedStaticMeshComponent>(Root);
		Component->SetStaticMesh(Mesh);
		Component->SetMobility(EComponentMobility::Static);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetupAttachment(RootComponent);
		Component->AddInstances(Model.Value, false, true);
		Component->RegisterComponent();
		Placed += Model.Value.Num();
	}
	UE_LOG(LogGTAVWorld, Display, TEXT("%d of %d placements, %d models (%d failed), %d textures (%d failed, %.1f MB), %d materials in %.2f s"), Placed, Count,
		Assets->MeshesBuilt, Assets->MeshesFailed, Assets->TexturesMade, Assets->TexturesFailed, Assets->TextureBytes / 1048576.0, Assets->MaterialsMade,
		FPlatformTime::Seconds() - Start);
	bReady = true;
}
