#pragma once

#include "CoreMinimal.h"

// GTA's world (metres, right-handed, z up) -> Unreal's (centimetres, left-handed, z up): y is mirrored.
namespace GTAV
{
	inline FVector3f Point(float X, float Y, float Z) { return FVector3f(X * 100.0f, -Y * 100.0f, Z * 100.0f); }
	inline FVector3f Direction(float X, float Y, float Z) { return FVector3f(X, -Y, Z); }

	// A rotation of the mirrored world: (x, y, z, w) -> (-x, y, -z, w). bInverse: the ymap stores the inverse.
	inline FQuat Rotation(float X, float Y, float Z, float W, bool bInverse)
	{
		if (bInverse)
		{
			X = -X, Y = -Y, Z = -Z;
		}
		return FQuat(-X, Y, -Z, W).GetNormalized();
	}

	// An asset name from a game name ("dt1_12_build+hi" -> "dt1_12_build_hi").
	inline FString AssetName(const FString& Name)
	{
		FString Out;
		for (const TCHAR C : Name)
		{
			Out.AppendChar(FChar::IsAlnum(C) || C == TEXT('_') ? C : TEXT('_'));
		}
		return Out;
	}
}
