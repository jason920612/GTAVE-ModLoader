#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "GTAVPack.h"
#include "GTAVRuntimeAssets.h"

#include "GTAVWorldSubsystem.generated.h"

// The game's world, drawn from the pack files at run time (research/phase0.md §59):
//   UnrealEditor.exe GTAVRenderer.uproject <map> -game -gtavpack=<folder> [-gtavsky]
// -gtavsky adds a sun, sky, clouds and fog (for an empty map).
UCLASS()
class UGTAVWorldSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& World) override;
	virtual void Deinitialize() override;

	// Everything asked for is built and placed.
	bool IsReady() const { return bReady; }

private:
	FGTAVPack Pack;
	TUniquePtr<FGTAVRuntimeAssets> Assets;
	bool bReady = false;

	void AddSky(UWorld& World);
};
