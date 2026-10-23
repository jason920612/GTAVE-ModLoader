#include "GTAVShotSubsystem.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "UnrealClient.h"

#include "GTAVWorldSubsystem.h"
#if WITH_EDITOR
#include "ShaderCompiler.h"
#include "StaticMeshCompiler.h"
#include "TextureCompiler.h"
#endif

bool UGTAVShotSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	const FString Line = FCommandLine::Get();
	return World && World->IsGameWorld() && (Line.Contains(TEXT("-gtavshot=")) || Line.Contains(TEXT("-gtavshots=")));
}

void UGTAVShotSubsystem::OnWorldBeginPlay(UWorld& World)
{
	Super::OnWorldBeginPlay(World);
	FString List, Cam, File;
	if (FParse::Value(FCommandLine::Get(), TEXT("-gtavshots="), List, false))
	{
		TArray<FString> Lines;
		FFileHelper::LoadFileToStringArray(Lines, *List);
		for (const FString& L : Lines)
		{
			TArray<FString> V;
			L.ParseIntoArrayWS(V);
			if (V.Num() >= 5 && !L.StartsWith(TEXT("#")))
			{
				Shots.Add({V[0], FVector(FCString::Atof(*V[1]), FCString::Atof(*V[2]), FCString::Atof(*V[3])), FCString::Atof(*V[4]),
					V.Num() > 5 ? FCString::Atof(*V[5]) : 0.0f});
			}
		}
	}
	else if (FParse::Value(FCommandLine::Get(), TEXT("-gtavcam="), Cam, false) && FParse::Value(FCommandLine::Get(), TEXT("-gtavshot="), File, false))
	{
		TArray<FString> V;
		Cam.ParseIntoArray(V, TEXT(","));
		if (V.Num() >= 4)
		{
			Shots.Add({File, FVector(FCString::Atof(*V[0]), FCString::Atof(*V[1]), FCString::Atof(*V[2])), FCString::Atof(*V[3]),
				V.Num() > 4 ? FCString::Atof(*V[4]) : 0.0f});
		}
	}
	UE_LOG(LogTemp, Display, TEXT("GTAVShot: %d shot(s) in %s"), Shots.Num(), *World.GetName());
}

void UGTAVShotSubsystem::Place(const FShot& Shot)
{
	UWorld* World = GetWorld();
	APlayerController* PC = World->GetFirstPlayerController();
	const float Heading = FMath::DegreesToRadians(Shot.Heading);
	// The game faces (-sin h, cos h) at heading h; Unreal mirrors y.
	const FVector Forward(-FMath::Sin(Heading), -FMath::Cos(Heading), 0);
	FRotator Rotation = Forward.Rotation();
	Rotation.Pitch = Shot.Pitch;
	const FVector Location(Shot.Position.X * 100, -Shot.Position.Y * 100, Shot.Position.Z * 100);
	if (!Camera)
	{
		Camera = World->SpawnActor<ACameraActor>(Location, Rotation);
		Camera->GetCameraComponent()->SetFieldOfView(60.0f);
		Camera->GetCameraComponent()->bConstrainAspectRatio = false;
	}
	Camera->SetActorLocationAndRotation(Location, Rotation);
	PC->SetViewTarget(Camera);
	UE_LOG(LogTemp, Display, TEXT("GTAVShot: camera at %s looking %s for %s"), *Location.ToString(), *Rotation.ToString(), *Shot.File);
}

void UGTAVShotSubsystem::Tick(float DeltaTime)
{
	UWorld* World = GetWorld();
	const double Now = FPlatformTime::Seconds();
	if (!World->GetFirstPlayerController())
	{
		return;
	}
	if (Current < 0 || (bShot && Now - ShotTime > 2.0))
	{
		if (++Current >= Shots.Num())
		{
			FPlatformMisc::RequestExit(false);
			return;
		}
		Place(Shots[Current]);
		bShot = false;
		StartTime = Now;
		return;
	}
	if (bShot)
	{
		return;
	}
#if WITH_EDITOR
	// (the first run compiles shaders, textures, meshes: the delay counts from when that is done)
	const int32 Pending = (GShaderCompilingManager ? GShaderCompilingManager->GetNumRemainingJobs() : 0) +
		FTextureCompilingManager::Get().GetNumRemainingTextures() + FStaticMeshCompilingManager::Get().GetNumRemainingMeshes();
	if (Pending > 0)
	{
		StartTime = Now;
		return;
	}
#endif
	// (the streamed world: everything in view loaded and drawn)
	if (const UGTAVWorldSubsystem* Streamer = World->GetSubsystem<UGTAVWorldSubsystem>(); Streamer && !Streamer->IsSettled())
	{
		StartTime = Now;
		return;
	}
	float Delay = 30.0f;
	FParse::Value(FCommandLine::Get(), TEXT("-gtavshotdelay="), Delay);
	if (Now - StartTime > Delay)
	{
		FScreenshotRequest::RequestScreenshot(Shots[Current].File, false, false);
		UE_LOG(LogTemp, Display, TEXT("GTAVShot: screenshot %s"), *Shots[Current].File);
		bShot = true;
		ShotTime = Now;
	}
}
