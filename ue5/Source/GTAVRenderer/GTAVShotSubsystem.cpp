#include "GTAVShotSubsystem.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
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
	return World && World->IsGameWorld() && FString(FCommandLine::Get()).Contains(TEXT("-gtavshot="));
}

void UGTAVShotSubsystem::OnWorldBeginPlay(UWorld& World)
{
	Super::OnWorldBeginPlay(World);
	UE_LOG(LogTemp, Display, TEXT("GTAVShot: begin play in %s"), *World.GetName());
}

void UGTAVShotSubsystem::Tick(float DeltaTime)
{
	UWorld* World = GetWorld();
	const double Now = FPlatformTime::Seconds();
	if (StartTime == 0)
	{
		StartTime = Now;
	}
	if (!bPlaced)
	{
		APlayerController* PC = World->GetFirstPlayerController();
		FString Cam;
		if (!PC || !FParse::Value(FCommandLine::Get(), TEXT("-gtavcam="), Cam, false))
		{
			return;
		}
		TArray<FString> V;
		Cam.ParseIntoArray(V, TEXT(","));
		if (V.Num() < 4)
		{
			return;
		}
		const float X = FCString::Atof(*V[0]), Y = FCString::Atof(*V[1]), Z = FCString::Atof(*V[2]);
		const float Heading = FMath::DegreesToRadians(FCString::Atof(*V[3]));
		const float Pitch = V.Num() > 4 ? FCString::Atof(*V[4]) : 0.0f;
		// The game faces (-sin h, cos h) at heading h; Unreal mirrors y.
		const FVector Forward(-FMath::Sin(Heading), -FMath::Cos(Heading), 0);
		FRotator Rotation = Forward.Rotation();
		Rotation.Pitch = Pitch;
		ACameraActor* Camera = World->SpawnActor<ACameraActor>(FVector(X * 100, -Y * 100, Z * 100), Rotation);
		Camera->GetCameraComponent()->SetFieldOfView(60.0f);
		Camera->GetCameraComponent()->bConstrainAspectRatio = false;
		PC->SetViewTarget(Camera);
		bPlaced = true;
		UE_LOG(LogTemp, Display, TEXT("GTAVShot: camera at %s looking %s"), *Camera->GetActorLocation().ToString(), *Rotation.ToString());
		return;
	}
#if WITH_EDITOR
	// (the first run compiles shaders, textures, meshes: the delay counts from when that is done)
	const int32 Pending = (GShaderCompilingManager ? GShaderCompilingManager->GetNumRemainingJobs() : 0) +
		FTextureCompilingManager::Get().GetNumRemainingTextures() + FStaticMeshCompilingManager::Get().GetNumRemainingMeshes();
	if (Pending > 0)
	{
		StartTime = Now;
		static double Told = 0;
		if (Now - Told > 30)
		{
			Told = Now;
			UE_LOG(LogTemp, Display, TEXT("GTAVShot: %d shaders, textures, meshes left"), Pending);
		}
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
	if (!bShot && Now - StartTime > Delay)
	{
		FString File;
		FParse::Value(FCommandLine::Get(), TEXT("-gtavshot="), File, false);
		FScreenshotRequest::RequestScreenshot(File, false, false);
		UE_LOG(LogTemp, Display, TEXT("GTAVShot: screenshot %s"), *File);
		bShot = true;
		ShotTime = Now;
	}
	if (bShot && Now - ShotTime > 3.0)
	{
		FPlatformMisc::RequestExit(false);
	}
}
