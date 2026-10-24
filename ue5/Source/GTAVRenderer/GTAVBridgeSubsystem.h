#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "GTAVBridgeProtocol.h"

#include "GTAVBridgeSubsystem.generated.h"

class ACameraActor;
class FGTAVBridgeViewExtension;

// The renderer's side of the bridge to the game (GTAVBridgeProtocol.h, research/phase0.md §59.5), with
// -gtavbridge=<game process id>: each game frame, the game's camera, hour and map data groups; the picture goes back
// through a shared texture (the HDR scene colour after motion blur, before tone mapping) and a shared fence.
UCLASS()
class UGTAVBridgeSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& World) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(UGTAVBridgeSubsystem, STATGROUP_Tickables); }

private:
	void* Mapping = nullptr;
	GTAVBridge::Shared* Shared = nullptr;
	uint32 GamePid = 0;
	uint32 NextSequence = 0, LastSequence = 0, Skipped = 0, LastGroups = 0, LastConsole = 0;
	TSharedPtr<FGTAVBridgeViewExtension, ESPMode::ThreadSafe> Extension;
	TArray<GTAVBridge::GameLight> GameLights; // (this frame's, copied out)

	UPROPERTY()
	TObjectPtr<ACameraActor> Camera;

	void Resize(uint32 Width, uint32 Height);
};
