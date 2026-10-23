#include "GTAVImportCommandlet.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StaticMeshCompiler.h"
#include "TextureCompiler.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "GTAVConvert.h"
#include "GTAVMaterials.h"
#include "GTAVMeshes.h"

DEFINE_LOG_CATEGORY_STATIC(LogGTAVImport, Log, All);

namespace
{
	bool Save(UPackage* Package, UObject* Asset, const FString& Extension)
	{
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), Extension);
		return UPackage::SavePackage(Package, Asset, *File, Args);
	}

	FVector3f Vec(const TArray<TSharedPtr<FJsonValue>>& A)
	{
		return FVector3f(A[0]->AsNumber(), A[1]->AsNumber(), A[2]->AsNumber());
	}

	// Sun, sky, clouds, fog: lit by Lumen (the project's settings).
	void AddSky(UWorld* World)
	{
		ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(FVector(0, 0, 10000), FRotator(-35, 40, 0));
		Sun->SetMobility(EComponentMobility::Movable);
		UDirectionalLightComponent* Light = CastChecked<UDirectionalLightComponent>(Sun->GetLightComponent());
		Light->SetAtmosphereSunLight(true);
		Light->SetIntensity(10.0f);
		World->SpawnActor<ASkyAtmosphere>(FVector::ZeroVector, FRotator::ZeroRotator);
		ASkyLight* Sky = World->SpawnActor<ASkyLight>(FVector(0, 0, 5000), FRotator::ZeroRotator);
		Sky->GetLightComponent()->SetMobility(EComponentMobility::Movable);
		Sky->GetLightComponent()->bRealTimeCapture = true;
		World->SpawnActor<AVolumetricCloud>(FVector::ZeroVector, FRotator::ZeroRotator);
		World->SpawnActor<AExponentialHeightFog>(FVector(0, 0, 0), FRotator::ZeroRotator);
	}
}

UGTAVImportCommandlet::UGTAVImportCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UGTAVImportCommandlet::Main(const FString& Params)
{
	FString Export, Area = TEXT("legion");
	if (FParse::Param(*Params, TEXT("masters")))
	{
		// The master materials only, made anew (the renderer's run-time materials are their instances).
		for (const TCHAR* Name : {TEXT("M_GTAV_Opaque"), TEXT("M_GTAV_Translucent"), TEXT("M_GTAV_Cutout"), TEXT("M_GTAV_Terrain4"), TEXT("M_GTAV_Water"), TEXT("M_GTAV_Ocean"), TEXT("M_GTAV_OceanFar"), TEXT("M_GTAV_Corona"), TEXT("M_GTAV_Rain"), TEXT("M_GTAV_Glow"), TEXT("M_GTAV_Moon"), TEXT("M_GTAV_Stars"), TEXT("M_GTAV_Foam"), TEXT("MPC_GTAV")})
		{
			IFileManager::Get().Delete(*FPackageName::LongPackageNameToFilename(FString(TEXT("/Game/GTAV/Materials/")) + Name, FPackageName::GetAssetPackageExtension()));
		}
		FGTAVMaterials Materials(TEXT(""));
		for (const int32 Kind : {0, 1, 3, 4, 6, 7, 9})
		{
			Materials.Master(Kind);
		}
		Materials.Corona();
		Materials.Rain();
		Materials.Glow();
		Materials.Moon();
		Materials.Stars();
		Materials.Foam();
		int32 Saved = 0;
		for (UPackage* Package : Materials.Dirty)
		{
			Saved += Save(Package, Package->FindAssetInPackage(), FPackageName::GetAssetPackageExtension());
		}
		UE_LOG(LogGTAVImport, Display, TEXT("%d master material packages saved"), Saved);
		return Saved == Materials.Dirty.Num() ? 0 : 1;
	}
	if (!FParse::Value(*Params, TEXT("-export="), Export))
	{
		UE_LOG(LogGTAVImport, Error, TEXT("usage: -run=GTAVImport -export=<ue5export's folder> [-area=<name>] [-inverse]"));
		return 1;
	}
	FParse::Value(*Params, TEXT("-area="), Area);
	const bool bInverse = FParse::Param(*Params, TEXT("inverse"));
	const FString Root = FPaths::Combine(Export, TEXT("export"));
	FString Text;
	TSharedPtr<FJsonObject> Json;
	if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(Root, TEXT("areas"), Area + TEXT(".json"))) ||
		!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) || !Json.IsValid())
	{
		UE_LOG(LogGTAVImport, Error, TEXT("no area %s in %s"), *Area, *Root);
		return 1;
	}
	const double Start = FPlatformTime::Seconds();
	FGTAVMaterials Materials(Root);
	FGTAVMeshes Meshes(Root, Materials);
	const TArray<TSharedPtr<FJsonValue>>& Entities = Json->GetArrayField(TEXT("entities"));
	for (const auto& E : Entities)
	{
		Meshes.Get(E->AsObject()->GetStringField(TEXT("archetype")));
	}
	Meshes.Build();
	// Compressed textures and built meshes in the cache now, not when the level is first shown.
	FTextureCompilingManager::Get().FinishAllCompilation();
	FStaticMeshCompilingManager::Get().FinishAllCompilation();
	UE_LOG(LogGTAVImport, Display, TEXT("%d meshes built (%d failed), %d textures imported (%d missing) in %.0f s"), Meshes.Created, Meshes.Failed,
		Materials.TexturesImported, Materials.TexturesMissing, FPlatformTime::Seconds() - Start);

	// The area as a level.
	const FString MapName = GTAV::AssetName(Area) + (bInverse ? TEXT("_inv") : TEXT(""));
	UPackage* MapPackage = CreatePackage(*(TEXT("/Game/GTAV/Maps/") + MapName));
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, FName(*MapName), MapPackage);
	World->SetFlags(RF_Public | RF_Standalone);
	AddSky(World);
	int32 Placed = 0;
	for (const auto& E : Entities)
	{
		const TSharedPtr<FJsonObject> O = E->AsObject();
		UStaticMesh* Mesh = Meshes.Get(O->GetStringField(TEXT("archetype")));
		if (!Mesh)
		{
			continue;
		}
		const FVector3f P = Vec(O->GetArrayField(TEXT("position")));
		const TArray<TSharedPtr<FJsonValue>>& Q = O->GetArrayField(TEXT("rotation"));
		const FVector3f S = Vec(O->GetArrayField(TEXT("scale")));
		const FTransform Transform(GTAV::Rotation(Q[0]->AsNumber(), Q[1]->AsNumber(), Q[2]->AsNumber(), Q[3]->AsNumber(), bInverse),
			FVector(GTAV::Point(P.X, P.Y, P.Z)), FVector(S));
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Transform);
		Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		Actor->SetActorLabel(O->GetStringField(TEXT("archetype")));
		++Placed;
	}
	UE_LOG(LogGTAVImport, Display, TEXT("%d of %d entities placed"), Placed, Entities.Num());

	int32 Saved = 0;
	for (UPackage* Package : Materials.Dirty)
	{
		Saved += Save(Package, Package->FindAssetInPackage(), FPackageName::GetAssetPackageExtension());
	}
	for (UPackage* Package : Meshes.Dirty)
	{
		Saved += Save(Package, Package->FindAssetInPackage(), FPackageName::GetAssetPackageExtension());
	}
	const bool bMap = Save(MapPackage, World, FPackageName::GetMapPackageExtension());
	UE_LOG(LogGTAVImport, Display, TEXT("%d assets saved, map %s; %.0f s"), Saved, bMap ? TEXT("saved") : TEXT("NOT saved"), FPlatformTime::Seconds() - Start);
	World->DestroyWorld(false);
	return bMap ? 0 : 1;
}
