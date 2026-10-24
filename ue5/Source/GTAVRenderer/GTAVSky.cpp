#include "GTAVSky.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/SkyLight.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"

#include "HAL/IConsoleManager.h"

#include "GTAVPack.h"

DEFINE_LOG_CATEGORY_STATIC(LogGTAVSky, Log, All);

namespace
{
	// Tuning (and the presets): a cloud cover in place of the weather's (-1: the weather's), the haze scaled.
	TAutoConsoleVariable<float> CVarCloudCoverage(TEXT("gtav.CloudCoverage"), -1.0f, TEXT("Cloud cover 0..1 (-1: by the weather)."));
	TAutoConsoleVariable<float> CVarHaze(TEXT("gtav.Haze"), 1.0f, TEXT("The air's aerosols, scaled (1: by the weather and the place)."));
	TAutoConsoleVariable<int32> CVarMoonShadows(TEXT("gtav.MoonShadows"), 1, TEXT("Moonlight casts shadows (0: not)."));
	TAutoConsoleVariable<float> CVarSunStep(TEXT("gtav.SunStep"), 0.2f, TEXT("The sun (or moon) turns in steps of this many degrees (0: each frame; its shadow map is drawn anew each step)."));

	// What a weather type looks like: the sun's and the sky's light (1 = clear), cloud cover, the air's haze (aerosols,
	// 1 = an ordinary day in the city), ground fog density, wet ground.
	struct FLook
	{
		float Sun = 1, Sky = 1, Clouds = 0.35f, Haze = 1, Fog = 0.002f, Wet = 0, Wind = 4, Rain = 0; // (rain in mm/h)
	};

	FLook Lerp(const FLook& A, const FLook& B, float T)
	{
		return {FMath::Lerp(A.Sun, B.Sun, T), FMath::Lerp(A.Sky, B.Sky, T), FMath::Lerp(A.Clouds, B.Clouds, T), FMath::Lerp(A.Haze, B.Haze, T),
			FMath::Lerp(A.Fog, B.Fog, T), FMath::Lerp(A.Wet, B.Wet, T), FMath::Lerp(A.Wind, B.Wind, T), FMath::Lerp(A.Rain, B.Rain, T)};
	}

	// The game's date, as days since 2000-01-01 (the game's start: 2013-09-17), and its month.
	double GameDay = 5007.0;
	int32 GameMonth = 9;
	TWeakObjectPtr<UTexture2D> MoonTexture, StarTexture;

	// Days from 2000-01-01 to a date (month 1-12).
	double DaysOf(int32 Year, int32 Month, int32 Day)
	{
		// (days from the civil date, the proleptic Gregorian calendar)
		const int32 Y = Year - (Month <= 2 ? 1 : 0);
		const int32 Era = (Y >= 0 ? Y : Y - 399) / 400;
		const int32 YearOfEra = Y - Era * 400;
		const int32 DayOfYear = (153 * (Month + (Month > 2 ? -3 : 9)) + 2) / 5 + Day - 1;
		const int32 DayOfEra = YearOfEra * 365 + YearOfEra / 4 - YearOfEra / 100 + DayOfYear;
		return double(Era) * 146097.0 + double(DayOfEra) - 719468.0 - 10957.0;
	}

	// The sun where the game has it (research/phase0.md §59.11; its own path, measured from the running game, the same
	// on every date): up due east at 6, highest at 13 (58 degrees, due south), down due west at 20, on a circle through
	// east and west tilted 32 degrees to the south; the 14 hours of day at an even pace, the night's half circle a
	// quarter by midnight and the rest by 6. Its timecycle (the colours of the sky and of the light on the people and
	// vehicles in its picture, the street lights) keeps to this path, so the renderer's sun does too.
	FVector GameSun(double Hours)
	{
		const double H = FMath::Fmod(FMath::Fmod(Hours, 24.0) + 24.0, 24.0);
		const double Angle = H >= 6.0 && H < 20.0 ? (H - 6.0) / 14.0 * PI
							 : H >= 20.0		  ? PI + (H - 20.0) / 4.0 * HALF_PI
												  : 1.5 * PI + H / 6.0 * HALF_PI;
		const double Up = FMath::DegreesToRadians(58.0);
		// (Unreal's space: x east, y south)
		return FVector(FMath::Cos(Angle), FMath::Sin(Angle) * FMath::Cos(Up), FMath::Sin(Angle) * FMath::Sin(Up));
	}

	// The night sky over Los Santos as over Los Angeles (34.05 N, 118.24 W) on the game's date, its clock the local
	// time (daylight saving time April to October): the moon by the main terms of its longitude and latitude (to about a
	// degree), its phase from the true sun (the low precision solar coordinates, the Astronomical Almanac's), the stars
	// turned by the sidereal time. Days (n) from J2000.0, angles in degrees.
	constexpr double kLatitude = 34.05, kLongitude = -118.24;

	struct FSkyPositions
	{
		FVector Moon;        // unit, towards it, Unreal's space
		double Elongation;   // the moon's from the sun (degrees: 180 full)
		double SiderealTime; // local, degrees
	};

	// Right ascension, declination (degrees) -> towards it in Unreal's space (x east, y south: the game's y mirrored).
	FVector Local(double RightAscension, double Declination, double SiderealTime)
	{
		const double H = FMath::DegreesToRadians(SiderealTime - RightAscension), D = FMath::DegreesToRadians(Declination);
		const double P = FMath::DegreesToRadians(kLatitude);
		const double Altitude = FMath::Asin(FMath::Sin(P) * FMath::Sin(D) + FMath::Cos(P) * FMath::Cos(D) * FMath::Cos(H));
		// (azimuth from the north towards the east)
		const double Azimuth = FMath::Atan2(-FMath::Sin(H) * FMath::Cos(D), FMath::Sin(D) * FMath::Cos(P) - FMath::Cos(D) * FMath::Sin(P) * FMath::Cos(H));
		const double C = FMath::Cos(Altitude);
		return FVector(C * FMath::Sin(Azimuth), -C * FMath::Cos(Azimuth), FMath::Sin(Altitude));
	}

	// Ecliptic longitude, latitude -> right ascension, declination (degrees).
	void Equatorial(double Longitude, double Latitude, double Obliquity, double& RightAscension, double& Declination)
	{
		const double L = FMath::DegreesToRadians(Longitude), B = FMath::DegreesToRadians(Latitude), E = FMath::DegreesToRadians(Obliquity);
		RightAscension = FMath::RadiansToDegrees(FMath::Atan2(FMath::Sin(L) * FMath::Cos(E) - FMath::Tan(B) * FMath::Sin(E), FMath::Cos(L)));
		Declination = FMath::RadiansToDegrees(FMath::Asin(FMath::Sin(B) * FMath::Cos(E) + FMath::Cos(B) * FMath::Sin(E) * FMath::Sin(L)));
	}

	FSkyPositions SkyAt(double Day, int32 Month, double Hours)
	{
		const double Zone = Month >= 4 && Month <= 10 ? 7.0 : 8.0; // (hours behind UTC)
		const double N = Day - 0.5 + (Hours + Zone) / 24.0;
		const auto Deg = [](double A) { return FMath::DegreesToRadians(A); };
		const double Obliquity = 23.439 - 4.0e-7 * N;
		// The sun (its ecliptic longitude, for the moon's phase).
		const double MeanLongitude = 280.460 + 0.9856474 * N, Anomaly = Deg(357.528 + 0.9856003 * N);
		const double SunLongitude = MeanLongitude + 1.915 * FMath::Sin(Anomaly) + 0.020 * FMath::Sin(2.0 * Anomaly);
		// The moon (its mean longitude, mean anomaly, argument of latitude; the main periodic terms).
		const double L0 = 218.316 + 13.176396 * N, M = Deg(134.963 + 13.064993 * N), F = Deg(93.272 + 13.229350 * N);
		const double D = Deg(297.850 + 12.190749 * N);
		const double MoonLongitude = L0 + 6.289 * FMath::Sin(M) + 1.274 * FMath::Sin(2.0 * D - M) + 0.658 * FMath::Sin(2.0 * D) -
									 0.186 * FMath::Sin(Anomaly) + 0.214 * FMath::Sin(2.0 * M);
		const double MoonLatitude = 5.128 * FMath::Sin(F);
		double MoonRa, MoonDec;
		Equatorial(MoonLongitude, MoonLatitude, Obliquity, MoonRa, MoonDec);
		// The local sidereal time.
		const double Sidereal = FMath::Fmod(280.46061837 + 360.98564736629 * N + kLongitude, 360.0);
		FSkyPositions Out;
		Out.Moon = Local(MoonRa, MoonDec, Sidereal);
		Out.Elongation = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(
			FMath::Cos(Deg(MoonLatitude)) * FMath::Cos(Deg(MoonLongitude - SunLongitude)), -1.0, 1.0)));
		Out.SiderealTime = Sidereal;
		return Out;
	}

	// The game's wind when the bridge has it (Unreal's space, m/s), and when it came.
	bool bInside = false, bInsideByRooms = false;
	float Lightning = 0;          // (stops)
	float GameWetness = -1.0f;
	double GameWetnessTime = 0;
	FVector LightningWay = FVector(0.3, 0.2, 1.0).GetSafeNormal(); // where the flash comes from (a new way each flash)
	double GameIndoorsTime = -1e9;
	float Indoors = 0; // (eased)
	FVector2f GameWind = FVector2f::ZeroVector;
	float GameWindSpeed = 0;
	double GameWindTime = -1e9;

	const FLook& Look(uint32 Weather)
	{
		static const TMap<uint32, FLook> Looks = [] {
			TMap<uint32, FLook> Out;
			const auto Add = [&](const TCHAR* Name, float Sun, float Sky, float Clouds, float Haze, float Fog, float Wet, float Wind, float Rain = 0) {
				Out.Add(GTAVHash(Name), {Sun, Sky, Clouds, Haze, Fog, Wet, Wind, Rain});
			};
			// (rain washes the air: clearing and rain are the clearest, smog the worst; wind in m/s when the game
			// gives none)
			Add(TEXT("EXTRASUNNY"), 1.0f, 1.0f, 0.0f, 0.8f, 0.001f, 0.0f, 3.0f);
			Add(TEXT("CLEAR"), 1.0f, 1.0f, 0.12f, 1.0f, 0.002f, 0.0f, 4.0f);
			Add(TEXT("NEUTRAL"), 0.9f, 1.0f, 0.4f, 1.0f, 0.002f, 0.0f, 4.0f);
			Add(TEXT("CLOUDS"), 0.6f, 0.9f, 0.45f, 1.0f, 0.003f, 0.0f, 6.0f);
			Add(TEXT("SMOG"), 0.7f, 0.9f, 0.35f, 3.0f, 0.004f, 0.0f, 2.0f);
			Add(TEXT("FOGGY"), 0.25f, 0.8f, 0.6f, 1.5f, 0.06f, 0.15f, 2.0f);
			Add(TEXT("OVERCAST"), 0.12f, 0.75f, 0.95f, 1.2f, 0.006f, 0.1f, 7.0f);
			Add(TEXT("CLEARING"), 0.55f, 0.8f, 0.6f, 0.45f, 0.004f, 0.7f, 6.0f, 0.5f);
			Add(TEXT("RAIN"), 0.03f, 0.6f, 1.0f, 0.5f, 0.012f, 1.0f, 9.0f, 8.0f);
			Add(TEXT("THUNDER"), 0.02f, 0.5f, 1.0f, 0.5f, 0.016f, 1.0f, 14.0f, 25.0f);
			Add(TEXT("SNOW"), 0.3f, 0.8f, 0.9f, 0.7f, 0.02f, 0.2f, 6.0f);
			Add(TEXT("BLIZZARD"), 0.1f, 0.6f, 1.0f, 1.0f, 0.06f, 0.2f, 16.0f);
			Add(TEXT("SNOWLIGHT"), 0.5f, 0.9f, 0.7f, 0.7f, 0.008f, 0.1f, 4.0f);
			Add(TEXT("XMAS"), 0.4f, 0.9f, 0.8f, 0.7f, 0.01f, 0.1f, 5.0f);
			Add(TEXT("HALLOWEEN"), 0.1f, 0.5f, 1.0f, 2.0f, 0.03f, 0.5f, 10.0f);
			return Out;
		}();
		static const FLook Clear;
		const FLook* Found = Looks.Find(Weather);
		return Found ? *Found : Clear;
	}

	// The world's sky actors (found once per world).
	struct FActors
	{
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<ADirectionalLight> Sun;
		TWeakObjectPtr<ASkyLight> Sky;
		TWeakObjectPtr<UVolumetricCloudComponent> Clouds;
		TWeakObjectPtr<UMaterialInstanceDynamic> CloudMaterial;
		TWeakObjectPtr<AExponentialHeightFog> Fog;
		FLinearColor FogLight = FLinearColor::Black; // the fog's own inscattering (by day)
		TWeakObjectPtr<USkyAtmosphereComponent> Atmosphere;
		TWeakObjectPtr<UMaterialParameterCollection> Collection;
		TWeakObjectPtr<AActor> Rain; // the rain's layers around the camera
		TWeakObjectPtr<UStaticMeshComponent> Moon, Stars;
		bool bSkyMade = false; // (tried once)
		FVector Shining = FVector::ZeroVector; // the way the sun (or the moon) was last turned
	};

	void Find(UWorld* World, FActors& A)
	{
		A = {};
		A.World = World;
		for (TActorIterator<ADirectionalLight> It(World); It; ++It)
		{
			A.Sun = *It;
		}
		for (TActorIterator<ASkyLight> It(World); It; ++It)
		{
			A.Sky = *It;
		}
		for (TActorIterator<AExponentialHeightFog> It(World); It; ++It)
		{
			A.Fog = *It;
			A.FogLight = It->GetComponent()->FogInscatteringLuminance;
		}
		for (TActorIterator<ASkyAtmosphere> It(World); It; ++It)
		{
			A.Atmosphere = It->FindComponentByClass<USkyAtmosphereComponent>();
		}
		for (TActorIterator<AVolumetricCloud> It(World); It; ++It)
		{
			UVolumetricCloudComponent* C = It->FindComponentByClass<UVolumetricCloudComponent>();
			if (C && C->GetMaterial())
			{
				A.Clouds = C;
				UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(C->GetMaterial(), C);
				C->SetMaterial(M);
				A.CloudMaterial = M;
			}
		}
		A.Collection = LoadObject<UMaterialParameterCollection>(nullptr, TEXT("/Game/GTAV/Materials/MPC_GTAV.MPC_GTAV"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
}

void GTAVSky::SetIndoors(bool bIndoors, bool bFromGame)
{
	if (bFromGame)
	{
		bInside = bIndoors;
		GameIndoorsTime = FPlatformTime::Seconds();
	}
	else
	{
		bInsideByRooms = bIndoors;
	}
}

void GTAVSky::SetWetness(float Wetness)
{
	GameWetness = Wetness;
	GameWetnessTime = FPlatformTime::Seconds();
}

void GTAVSky::SetLightning(float Stops)
{
	// (each flash from somewhere else in the clouds overhead)
	if (Stops > 1.0f && Lightning <= 1.0f)
	{
		const float Azimuth = FMath::FRandRange(0.0f, 2.0f * PI), Elevation = FMath::FRandRange(0.6f, 1.3f);
		LightningWay = FVector(FMath::Cos(Azimuth) * FMath::Cos(Elevation), FMath::Sin(Azimuth) * FMath::Cos(Elevation), FMath::Sin(Elevation));
	}
	Lightning = Stops;
}

void GTAVSky::SetDate(int32 Year, int32 Month, int32 Day)
{
	GameDay = DaysOf(Year, FMath::Clamp(Month, 1, 12), FMath::Clamp(Day, 1, 31));
	GameMonth = FMath::Clamp(Month, 1, 12);
}

void GTAVSky::SetSkyTextures(UTexture2D* Moon, UTexture2D* Stars)
{
	MoonTexture = Moon;
	StarTexture = Stars;
}

void GTAVSky::SetWind(const FVector2f& Way, float Speed)
{
	GameWind = Way;
	GameWindSpeed = Speed;
	GameWindTime = FPlatformTime::Seconds();
}

void GTAVSky::Apply(UWorld* World, const FVector& Camera, float Hours, uint32 WeatherFrom, uint32 WeatherTo, float Blend)
{
	static FActors A;
	if (A.World.Get() != World)
	{
		Find(World, A);
	}
	const FLook L = Lerp(Look(WeatherFrom), Look(WeatherTo), FMath::Clamp(Blend, 0.0f, 1.0f));

	// The sun on the game's path, the moon and the stars as in the real sky. Physical units throughout (lux; the lights
	// in candela, glowing surfaces in nits), the exposure adapting as an eye does: a clear sun gives 100,000 lux, a full
	// moon a quarter of one.
	const FSkyPositions Heavens = SkyAt(GameDay, GameMonth, Hours);
	const FVector Toward = GameSun(Hours);
	// The moon: moonlight by the phase angle (0.25 lux full, the lunar phase law), none while it is down.
	const FVector MoonToward = Heavens.Moon;
	const float Elongation = float(Heavens.Elongation);
	const float Phase = 180.0f - Elongation; // (degrees; 0 full)
	{
		static double LoggedDay = -1;
		if (GameDay != LoggedDay)
		{
			LoggedDay = GameDay;
			UE_LOG(LogGTAVSky, Display, TEXT("day %.0f, %.2f h: the sun %s, the moon %s, %.0f degrees from the sun"), GameDay, Hours,
				*Toward.ToString(), *MoonToward.ToString(), Elongation);
		}
	}
	const float MoonLux = 0.25f * FMath::Pow(10.0f, -0.4f * (0.026f * Phase + 4.0e-9f * FMath::Pow(Phase, 4.0f))) *
		FMath::SmoothStep(-0.03f, 0.05f, float(MoonToward.Z));
	// A lightning flash (the game's, while it thunders): the clouds lit from inside for a moment, a cold white (about
	// 8,000 K) of up to a few thousand lux on the ground nearby, the sky light with it. The game says how much brighter
	// its own sky got (stops); a flash a stop over is some 200 lux here, each stop more doubles it (the game's people
	// and cars are lit by its own flash: the world about them has to brighten as much).
	const float Flash = L.Rain >= 15.0f && Lightning > 1.0f ? FMath::Min(Lightning, 6.0f) : 0.0f;
	const float FlashLux = Flash > 0 ? FMath::Min(200.0f * (FMath::Exp2(Flash) - 1.0f), 5000.0f) : 0.0f;
	if (ADirectionalLight* Sun = A.Sun.Get())
	{
		const bool bDay = Toward.Z > -0.05f;
		const bool bFlash = FlashLux > (bDay ? 100000.0f * L.Sun : MoonLux);
		// (turned in steps of a fifth of a degree - every second and a half or so as the clock runs: a shadow map's
		// pages stay as they were drawn until the light turns, and turned a little each frame they were all drawn
		// anew each frame; a shadow's end moves a few centimetres a step)
		const FVector Way = (bFlash ? -LightningWay : bDay ? -Toward : -MoonToward).GetSafeNormal();
		if ((Way | A.Shining) < FMath::Cos(FMath::DegreesToRadians(FMath::Max(CVarSunStep.GetValueOnGameThread(), 0.0f))) || CVarSunStep.GetValueOnGameThread() <= 0)
		{
			A.Shining = Way;
			Sun->SetActorRotation(Way.Rotation());
		}
		ULightComponent* Light = Sun->GetLightComponent();
		Light->SetIntensity(bFlash ? FlashLux : bDay ? 100000.0f * L.Sun : MoonLux * FMath::Max(L.Sun, 0.2f));
		Light->SetCastShadows(bDay || bFlash || CVarMoonShadows.GetValueOnGameThread() != 0);
		// The sun shines as a 5,778 K black body (the air reddens it further, low down: the sky atmosphere's
		// transmittance); white, as it was, is D65's 6,500 K, a cooler light than sunlight. Moonlight bluish: what an
		// eye sees by night (its rods), the reflected sunlight itself being warmer still.
		Light->SetUseTemperature(bDay || bFlash);
		Light->SetTemperature(bFlash ? 8000.0f : 5778.0f);
		Light->SetLightColor(bDay || bFlash ? FLinearColor::White : FLinearColor(0.6f, 0.7f, 1.0f));
	}
	// The moon itself, 100 km off (as big as it looks: 0.52 degrees; the engine's sphere is 50 cm across its
	// radius), and the stars 200 km off, turned about the pole by the hour and the day (a sidereal day).
	if (!A.bSkyMade)
	{
		A.bSkyMade = true;
		UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
		UMaterialInterface* MoonMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/GTAV/Materials/M_GTAV_Moon.M_GTAV_Moon"), nullptr, LOAD_NoWarn | LOAD_Quiet);
		UMaterialInterface* StarMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/GTAV/Materials/M_GTAV_Stars.M_GTAV_Stars"), nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (Sphere && MoonMaterial && StarMaterial)
		{
			AActor* Owner = World->SpawnActor<AActor>();
			USceneComponent* Root = NewObject<USceneComponent>(Owner, TEXT("Root"));
			Owner->SetRootComponent(Root);
			Root->SetMobility(EComponentMobility::Movable);
			Root->RegisterComponent();
			const auto Make = [&](UMaterialInterface* Material, UTexture2D* Texture, const TCHAR* Parameter) {
				UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(Owner);
				C->SetMobility(EComponentMobility::Movable);
				C->SetStaticMesh(Sphere);
				C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				C->SetCastShadow(false);
				C->bAffectDistanceFieldLighting = false;
				C->bVisibleInRayTracing = false;
				C->SetAffectDynamicIndirectLighting(false);
				UMaterialInstanceDynamic* MI = UMaterialInstanceDynamic::Create(Material, C);
				if (Texture)
				{
					MI->SetTextureParameterValue(Parameter, Texture);
				}
				C->SetMaterial(0, MI);
				C->SetupAttachment(Root);
				C->RegisterComponent();
				return C;
			};
			A.Moon = Make(MoonMaterial, MoonTexture.Get(), TEXT("MoonTex"));
			A.Stars = Make(StarMaterial, StarTexture.Get(), TEXT("StarTex"));
			UE_LOG(LogGTAVSky, Display, TEXT("the moon and the stars (textures: moon %d, stars %d)"), MoonTexture.IsValid(), StarTexture.IsValid());
		}
		else
		{
			UE_LOG(LogGTAVSky, Warning, TEXT("no moon or stars: sphere %d, M_GTAV_Moon %d, M_GTAV_Stars %d (GTAVImport -masters makes them)"), Sphere != nullptr,
				MoonMaterial != nullptr, StarMaterial != nullptr);
		}
	}
	if (UStaticMeshComponent* Moon = A.Moon.Get())
	{
		constexpr double Distance = 10000000.0; // cm
		Moon->SetWorldLocationAndRotation(Camera + MoonToward * Distance, FQuat::Identity);
		Moon->SetWorldScale3D(FVector(Distance * FMath::Tan(FMath::DegreesToRadians(0.2575)) / 50.0));
	}
	if (UStaticMeshComponent* Stars = A.Stars.Get())
	{
		// (turned about the celestial pole, as high as the latitude in the north, by the sidereal time)
		const FVector Pole(0.0, -FMath::Cos(FMath::DegreesToRadians(kLatitude)), FMath::Sin(FMath::DegreesToRadians(kLatitude)));
		Stars->SetWorldLocationAndRotation(Camera, FQuat(Pole, FMath::DegreesToRadians(Heavens.SiderealTime)));
		Stars->SetWorldScale3D(FVector(20000000.0 / 50.0));
		Stars->SetVisibility(Toward.Z < 0.05); // (a full-screen layer: not drawn by day, when no star shows)
	}
	if (ASkyLight* Sky = A.Sky.Get())
	{
		Sky->GetLightComponent()->SetIntensity(L.Sky * FMath::Exp2(Flash));
	}
	const float CoverOverride = CVarCloudCoverage.GetValueOnGameThread();
	const float Cover = FMath::Clamp(CoverOverride >= 0 ? CoverOverride : L.Clouds, 0.0f, 1.0f);
	if (UMaterialInstanceDynamic* Clouds = A.CloudMaterial.Get())
	{
		// (the engine's cloud material: Cloud_GlobalCoverage 0 gives scattered cumulus, 0.3 a hazy overcast, 0.45 a milky
		// one, measured; a clear sky has none at all: its few wisps are cirrus, which a cumulus layer cannot be)
		if (UVolumetricCloudComponent* Layer = A.Clouds.Get())
		{
			Layer->SetVisibility(Cover >= 0.2f);
		}
		Clouds->SetScalarParameterValue(TEXT("Cloud_GlobalCoverage"), FMath::Max(Cover - 0.2f, 0.0f) / 0.8f * 0.45f);
	}
	// The air: aerosols (Mie scattering) that blur and tint the distance, thickest in the Los Santos basin (a city's
	// haze under an inversion about a kilometre up: the hills rise out of it), thinner over the countryside north of
	// it; a ground fog mostly in fog and rain.
	const bool bIn = FPlatformTime::Seconds() - GameIndoorsTime < 2.0 ? bInside : bInsideByRooms;
	Indoors = FMath::FInterpConstantTo(Indoors, bIn ? 1.0f : 0.0f, 1.0f / 30.0f, 1.0f); // (about half a second)
	const float GameY = float(-Camera.Y / 100.0);
	const float City = 1.0f - FMath::SmoothStep(1500.0f, 3500.0f, GameY);
	if (USkyAtmosphereComponent* Air = A.Atmosphere.Get())
	{
		// (per km at the ground: a city day 0.06, visibility about 60 km; smog 0.19; the open country a third of the city)
		const float Scattering = 0.035f * L.Haze * FMath::Lerp(0.6f, 1.8f, City) * FMath::Max(CVarHaze.GetValueOnGameThread(), 0.0f);
		Air->SetMieScatteringScale(Scattering);
		Air->SetMieAbsorptionScale(Scattering * FMath::Lerp(0.12f, 0.3f, City));
		// (city aerosols absorb blue more than red: brownish)
		Air->SetMieAbsorption(FLinearColor(FMath::Lerp(0.44f, 0.3f, City), FMath::Lerp(0.44f, 0.4f, City), FMath::Lerp(0.44f, 0.55f, City)));
		Air->SetMieExponentialDistribution(FMath::Lerp(1.4f, 0.7f, City));
		// (indoors the air is not lit by the sky: the haze there is the room's distance, nothing)
		Air->SetAerialPespectiveViewDistanceScale(FMath::Lerp(1.0f, 0.001f, Indoors));
	}
	// Rain: three layers around the camera (3, 7 and 15 m out), while it rains outdoors (the layers know no roof: a room
	// has none of them).
	if (L.Rain > 0.05f && !A.Rain.IsValid())
	{
		UStaticMesh* Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
		UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/GTAV/Materials/M_GTAV_Rain.M_GTAV_Rain"), nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (Cylinder && Material)
		{
			AActor* Owner = World->SpawnActor<AActor>();
			USceneComponent* Root = NewObject<USceneComponent>(Owner, TEXT("Root"));
			Owner->SetRootComponent(Root);
			Root->SetMobility(EComponentMobility::Movable);
			Root->RegisterComponent();
			for (const float Radius : {3.0f, 7.0f, 15.0f})
			{
				UStaticMeshComponent* Layer = NewObject<UStaticMeshComponent>(Owner);
				Layer->SetMobility(EComponentMobility::Movable);
				Layer->SetStaticMesh(Cylinder);
				Layer->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				Layer->SetCastShadow(false);
				UMaterialInstanceDynamic* MI = UMaterialInstanceDynamic::Create(Material, Layer);
				MI->SetScalarParameterValue(TEXT("Radius"), Radius);
				Layer->SetMaterial(0, MI);
				// (the engine's cylinder: 50 cm radius, 1 m high, centred)
				Layer->SetRelativeScale3D(FVector(Radius * 2.0, Radius * 2.0, 30.0));
				Layer->SetupAttachment(Root);
				Layer->RegisterComponent();
			}
			A.Rain = Owner;
		}
	}
	if (AActor* Rain = A.Rain.Get())
	{
		Rain->SetActorLocation(Camera);
		Rain->SetActorHiddenInGame(L.Rain <= 0.05f || Indoors > 0.5f);
	}
	if (AExponentialHeightFog* Fog = A.Fog.Get())
	{
		UExponentialHeightFogComponent* C = Fog->GetComponent();
		C->SetFogDensity(L.Fog * (1.0f - Indoors));
		C->SetFogHeightFalloff(0.35f);
		// Light pollution: a city's lights light the aerosols over it from below (and its clouds, many times more), an
		// orange-grey glow brightest low over the horizon, where a line of sight crosses the most air, the zenith
		// darker: some 0.25 nits towards the horizon on a clear night over Los Santos and 0.03 overhead (a city sky of
		// about 17 mag/arcsec^2), out in the country a thirtieth of that (about 20.7: the stars come out), a low
		// overcast lit orange.
		// Sodium and white LED street lights mixed. The aerosols as the second fog: a layer about 1.2 km thick (scale
		// height; the fog's falloff is per 1000 cm), optical depth 0.15 straight up (density per 1000 cm), at night
		// only (by day the sky atmosphere's haze is the air's).
		const float Night = 1.0f - FMath::SmoothStep(-0.12f, 0.02f, float(Toward.Z));
		const float Haze = FMath::Lerp(0.6f, 1.4f, FMath::Clamp(L.Haze * 0.5f, 0.0f, 1.0f));
		const float Glow = 0.25f * FMath::Lerp(0.03f, 1.0f, City) * (1.0f + 6.0f * Cover) * Haze;
		C->SetFogInscatteringColor(FMath::Lerp(A.FogLight, FLinearColor(1.0f, 0.68f, 0.42f) * Glow, Night));
		FExponentialHeightFogData Aerosols;
		Aerosols.FogHeightFalloff = 1000.0f / 120000.0f;
		Aerosols.FogDensity = 0.15f / 120000.0f * 1000.0f * Haze * FMath::Lerp(0.5f, 1.0f, City) * Night * (1.0f - Indoors);
		C->SetSecondFogData(Aerosols);
	}
	if (UMaterialParameterCollection* Collection = A.Collection.Get())
	{
		if (UMaterialParameterCollectionInstance* Instance = World->GetParameterCollectionInstance(Collection))
		{
			// (indoors the floors are dry: what is seen from in there is mostly in there; outdoors as wet as the game has
			// the ground - after the rain it dries over an hour of game time - else by the weather)
			const bool bGameWet = GameWetness >= 0 && FPlatformTime::Seconds() - GameWetnessTime < 2.0;
			Instance->SetScalarParameterValue(TEXT("Wetness"), (bGameWet ? FMath::Clamp(GameWetness, 0.0f, 1.0f) : L.Wet) * (1.0f - Indoors));
			// The wind: the game's while the bridge sends it, else the weather's from the west (the sea breeze).
			const bool bGame = FPlatformTime::Seconds() - GameWindTime < 2.0;
			const FVector2f Way = bGame && !GameWind.IsNearlyZero() ? GameWind.GetSafeNormal() : FVector2f(1, 0);
			Instance->SetScalarParameterValue(TEXT("WindX"), Way.X);
			Instance->SetScalarParameterValue(TEXT("WindY"), Way.Y);
			Instance->SetScalarParameterValue(TEXT("WindSpeed"), bGame ? GameWindSpeed : L.Wind);
			Instance->SetScalarParameterValue(TEXT("RainRate"), L.Rain);
			Instance->SetScalarParameterValue(TEXT("SunX"), float(Toward.X));
			Instance->SetScalarParameterValue(TEXT("SunY"), float(Toward.Y));
			Instance->SetScalarParameterValue(TEXT("SunZ"), float(Toward.Z));
		}
	}
}
