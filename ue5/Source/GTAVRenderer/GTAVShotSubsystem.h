#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "GTAVShotSubsystem.generated.h"

// Screenshots for checking the conversion without anyone at the editor (research/phase0.md §58.5):
//   UnrealEditor.exe GTAVRenderer.uproject /Game/GTAV/Maps/legion -game -gtavcam=x,y,z,heading,pitch
//       -gtavshot=<file.png> [-gtavshotdelay=<seconds>]
// The camera is placed in the game's coordinates (metres, heading in degrees as the game counts it), the picture is
// taken after the delay (shaders and textures streamed in), then the program quits.
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
	double StartTime = 0;
	bool bPlaced = false;
	bool bShot = false;
	double ShotTime = 0;
};
