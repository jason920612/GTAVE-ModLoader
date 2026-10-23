#pragma once

#include "CoreMinimal.h"

// The export's DDS files (DX10 header, the game's texture data as it is) -> the top mip as BGRA8: BC1-BC5 decoded here
// (the engine's DDS import takes no block-compressed data), uncompressed BGRA / RGBA copied.
namespace GTAVDds
{
	// bNormalMap: a two-channel map (BC5) gets its blue (z) reconstructed.
	bool Decode(const TArray<uint8>& File, bool bNormalMap, int32& OutWidth, int32& OutHeight, TArray<FColor>& OutPixels, FString& OutError);
}
