#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "GTAVShotSubsystem.generated.h"

class ACameraActor;

// Screenshots for checking the conversion without anyone at the editor (research/phase0.md §58.5):
//   UnrealEditor.exe GTAVRenderer.uproject <map> -game -gtavcam=x,y,z,heading,pitch -gtavshot=<file.png>
//       [-gtavshotdelay=<seconds>]
//   ... -gtavshots=<list.txt>: one shot per line "<file.png> x y z heading pitch"
// The camera is placed in the game's coordinates (metres, heading in degrees as the game counts it), each picture is
// taken once the streamed world is in and after the delay, then the program quits.
UCLASS()
class UGTAVShotSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& World) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(UGTAVShotSubsystem, STATGROUP_Tickables); }

private:
	struct FShot
	{
		FString File;
		FVector Position; // game space
		float Heading = 0, Pitch = 0;
	};
	TArray<FShot> Shots;
	int32 Current = -1;
	UPROPERTY()
	TObjectPtr<ACameraActor> Camera;
	double StartTime = 0;
	bool bShot = false;
	double ShotTime = 0;

	void Place(const FShot& Shot);
};
