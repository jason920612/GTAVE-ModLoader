#pragma once

#include "CoreMinimal.h"

class UWorld;

// The sky the game has (research/phase0.md §59.7): the sun by the hour, and the weather's light, clouds, fog and wet
// ground, blended between the two weather types the game blends.
namespace GTAVSky
{
	// Camera: where the view is (Unreal's space; the air depends on the place); hours: 0..24; weather: the game's
	// type hashes (GET_CURR_WEATHER_STATE), Blend 0 = From, 1 = To.
	void Apply(UWorld* World, const FVector& Camera, float Hours, uint32 WeatherFrom, uint32 WeatherTo, float Blend);
	// The game's wind (the way it blows in Unreal's space, m/s), each frame the bridge has it.
	void SetWind(const FVector2f& Way, float Speed);
	// The camera is inside a building's rooms: no sunlit haze or ground fog in the air there.
	void SetIndoors(bool bIndoors);
}
