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
	// The camera indoors: by the game (the interior its camera is in, while the bridge says so), else by the rooms'
	// boxes (bFromGame false: checking without the game).
	void SetIndoors(bool bIndoors, bool bFromGame);
	// The game's lightning this moment (stops over its sky's usual brightness; the bridge's).
	void SetLightning(float Stops);
	// The game's wet ground (0 dry - 1 soaked; it dries over an hour of game time after the rain), each frame the bridge
	// has it; negative: not known (then by the weather).
	void SetWetness(float Wetness);
	// The game's date (month 1-12): the moon's phase and the stars' turn.
	void SetDate(int32 Year, int32 Month, int32 Day);
	// The game's moon and starfield textures (globals.gpk), once the pack is open.
	void SetSkyTextures(class UTexture2D* Moon, class UTexture2D* Stars);
}
