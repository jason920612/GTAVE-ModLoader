#pragma once

#include "CoreMinimal.h"

#include "GTAVBridgeProtocol.h"

class UWorld;

// The peds' and vehicles' shadows on the renderer's world (research/phase0.md §59.11): the game's picture shows them
// (composited by its stencil), but the ground under them is the renderer's, so they cast no shadow there of their own.
// Each one near the player stands in as a shape that is not drawn but casts shadows and darkens the light around
// (a ped legs, body and head, a vehicle its body and a narrower cabin, by the model's bounds).
namespace GTAVShadows
{
	// Each frame the bridge has them (the game's space).
	void Update(UWorld* World, const GTAVBridge::Proxy* Proxies, uint32 Count);
}
