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

	// The game's date, as days since 2000-01-01 (the game's start: 2013-09-17).
	double GameDay = 5007.0;
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

	// Where the sun is at an hour (unit, towards it, Unreal's space): east at 6, highest at 12 (to the south), west at
	// 18, its daily circle about the celestial pole 30 degrees up in the north.
	FVector SunAt(float Hours)
	{
		const float Angle = (Hours - 6.0f) / 12.0f * PI;
		return FVector(FMath::Cos(Angle), FMath::Sin(Angle) * 0.5f, FMath::Sin(Angle) * 0.866f).GetSafeNormal(); // (y: south, mirrored)
	}
	const FVector kPole = FVector(0.0, -0.866, 0.5); // (the axis the sky turns about)

	// The game's wind when the bridge has it (Unreal's space, m/s), and when it came.
	bool bInside = false;
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

void GTAVSky::SetIndoors(bool bIndoors)
{
	bInside = bIndoors;
}

void GTAVSky::SetDate(int32 Year, int32 Month, int32 Day)
{
	GameDay = DaysOf(Year, FMath::Clamp(Month, 1, 12), FMath::Clamp(Day, 1, 31));
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

	// The sun from the hour: east at 6, highest at 12 (to the south), west at 18; at night the moon opposite. Physical
	// units throughout (lux; the lights in candela, glowing surfaces in nits), the exposure adapting as an eye does:
	// a clear sun gives 100,000 lux, a full moon a quarter of one.
	const FVector Toward = SunAt(Hours);
	// The moon: its age from the date (days since the new moon of 2000-01-06 18:14 UTC, the game's clock in Los
	// Santos, UTC-8; a synodic month 29.53 days), its elongation from the sun that age's share of 360 degrees: it
	// keeps to the sun's daily circle, that many hours behind (full: opposite, rising at sunset). Moonlight by the
	// phase angle (0.25 lux full, the lunar phase law), none while it is down.
	const double Days = GameDay + (Hours + 8.0) / 24.0 - 5.7597;
	const double Age = FMath::Fmod(Days, 29.530588853);
	const float Elongation = float((Age < 0 ? Age + 29.530588853 : Age) / 29.530588853 * 360.0);
	const FVector MoonToward = SunAt(Hours - Elongation / 15.0f);
	const float Phase = FMath::Abs(180.0f - Elongation); // (degrees; 0 full)
	{
		static double LoggedDay = -1;
		if (GameDay != LoggedDay)
		{
			LoggedDay = GameDay;
			UE_LOG(LogGTAVSky, Display, TEXT("day %.0f: the moon %.1f days old, %.0f degrees from the sun"), GameDay, Age < 0 ? Age + 29.530588853 : Age, Elongation);
		}
	}
	const float MoonLux = 0.25f * FMath::Pow(10.0f, -0.4f * (0.026f * Phase + 4.0e-9f * FMath::Pow(Phase, 4.0f))) *
		FMath::SmoothStep(-0.03f, 0.05f, float(MoonToward.Z));
	if (ADirectionalLight* Sun = A.Sun.Get())
	{
		const bool bDay = Toward.Z > -0.05f;
		Sun->SetActorRotation((bDay ? -Toward : -MoonToward).Rotation());
		ULightComponent* Light = Sun->GetLightComponent();
		Light->SetIntensity(bDay ? 100000.0f * L.Sun : MoonLux * FMath::Max(L.Sun, 0.2f));
		Light->SetLightColor(bDay ? FLinearColor::White : FLinearColor(0.6f, 0.7f, 1.0f));
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
		static int32 LoggedHour = -1;
		if (int32(Hours) != LoggedHour)
		{
			LoggedHour = int32(Hours);
			UE_LOG(LogGTAVSky, Display, TEXT("%02d h: the moon at %s (towards %s), camera %s, visible %d registered %d, scale %s"), LoggedHour,
				*Moon->GetComponentLocation().ToString(), *MoonToward.ToString(), *Camera.ToString(), Moon->IsVisible(), Moon->IsRegistered(),
				*Moon->GetComponentScale().ToString());
		}
	}
	if (UStaticMeshComponent* Stars = A.Stars.Get())
	{
		const double Turn = (GameDay / 365.2422 + Hours / 24.0) * 2.0 * PI; // (a sidereal day: the sun's plus one turn a year)
		Stars->SetWorldLocationAndRotation(Camera, FQuat(kPole, FMath::Fmod(Turn, 2.0 * PI)));
		Stars->SetWorldScale3D(FVector(20000000.0 / 50.0));
	}
	if (ASkyLight* Sky = A.Sky.Get())
	{
		Sky->GetLightComponent()->SetIntensity(L.Sky);
	}
	const float CoverOverride = CVarCloudCoverage.GetValueOnGameThread();
	const float Cover = FMath::Clamp(CoverOverride >= 0 ? CoverOverride : L.Clouds, 0.0f, 1.0f);
	if (UMaterialInstanceDynamic* Clouds = A.CloudMaterial.Get())
	{
		// (the engine's cloud material: Cloud_GlobalCoverage 0 gives scattered cumulus, 0.3 a hazy overcast, 0.45 a milky
		// one, measured; a clear sky has none at all)
		if (UVolumetricCloudComponent* Layer = A.Clouds.Get())
		{
			Layer->SetVisibility(Cover >= 0.1f);
		}
		Clouds->SetScalarParameterValue(TEXT("Cloud_GlobalCoverage"), FMath::Max(Cover - 0.1f, 0.0f) / 0.9f * 0.45f);
	}
	// The air: aerosols (Mie scattering) that blur and tint the distance, thickest in the Los Santos basin (a city's
	// haze under an inversion about a kilometre up: the hills rise out of it), thinner over the countryside north of
	// it; a ground fog mostly in fog and rain.
	Indoors = FMath::FInterpConstantTo(Indoors, bInside ? 1.0f : 0.0f, 1.0f / 30.0f, 1.0f); // (about half a second)
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
	// Rain: three layers around the camera (3, 7 and 15 m out), while it rains.
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
		Rain->SetActorHiddenInGame(L.Rain <= 0.05f);
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
			Instance->SetScalarParameterValue(TEXT("Wetness"), L.Wet);
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
