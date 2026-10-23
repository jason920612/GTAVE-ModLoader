#pragma once

#include "CoreMinimal.h"

class UWorld;

// The sky the game has (research/phase0.md §59.7): the sun by the hour, and the weather's light, clouds, fog and wet
// ground, blended between the two weather types the game blends.
namespace GTAVSky
{
	// Hours: 0..24; weather: the game's type hashes (GET_CURR_WEATHER_STATE), Blend 0 = From, 1 = To.
	void Apply(UWorld* World, float Hours, uint32 WeatherFrom, uint32 WeatherTo, float Blend);
}
