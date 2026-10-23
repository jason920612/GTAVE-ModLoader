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
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"

#include "HAL/IConsoleManager.h"

#include "GTAVPack.h"

namespace
{
	// Tuning (and the presets): a cloud cover in place of the weather's (-1: the weather's), the haze scaled.
	TAutoConsoleVariable<float> CVarCloudCoverage(TEXT("gtav.CloudCoverage"), -1.0f, TEXT("Cloud cover 0..1 (-1: by the weather)."));
	TAutoConsoleVariable<float> CVarHaze(TEXT("gtav.Haze"), 1.0f, TEXT("The air's aerosols, scaled (1: by the weather and the place)."));

	// What a weather type looks like: the sun's and the sky's light (1 = clear), cloud cover, the air's haze (aerosols,
	// 1 = an ordinary day in the city), ground fog density, wet ground.
	struct FLook
	{
		float Sun = 1, Sky = 1, Clouds = 0.35f, Haze = 1, Fog = 0.002f, Wet = 0, Wind = 4;
	};

	FLook Lerp(const FLook& A, const FLook& B, float T)
	{
		return {FMath::Lerp(A.Sun, B.Sun, T), FMath::Lerp(A.Sky, B.Sky, T), FMath::Lerp(A.Clouds, B.Clouds, T), FMath::Lerp(A.Haze, B.Haze, T),
			FMath::Lerp(A.Fog, B.Fog, T), FMath::Lerp(A.Wet, B.Wet, T), FMath::Lerp(A.Wind, B.Wind, T)};
	}

	// The game's wind when the bridge has it (Unreal's space, m/s), and when it came.
	FVector2f GameWind = FVector2f::ZeroVector;
	float GameWindSpeed = 0;
	double GameWindTime = -1e9;

	const FLook& Look(uint32 Weather)
	{
		static const TMap<uint32, FLook> Looks = [] {
			TMap<uint32, FLook> Out;
			const auto Add = [&](const TCHAR* Name, float Sun, float Sky, float Clouds, float Haze, float Fog, float Wet, float Wind) {
				Out.Add(GTAVHash(Name), {Sun, Sky, Clouds, Haze, Fog, Wet, Wind});
			};
			// (rain washes the air: clearing and rain are the clearest, smog the worst; wind in m/s when the game
			// gives none)
			Add(TEXT("EXTRASUNNY"), 1.0f, 1.0f, 0.0f, 0.8f, 0.001f, 0.0f, 3.0f);
			Add(TEXT("CLEAR"), 1.0f, 1.0f, 0.12f, 1.0f, 0.002f, 0.0f, 4.0f);
			Add(TEXT("NEUTRAL"), 0.9f, 1.0f, 0.4f, 1.0f, 0.002f, 0.0f, 4.0f);
			Add(TEXT("CLOUDS"), 0.75f, 0.9f, 0.45f, 1.0f, 0.003f, 0.0f, 6.0f);
			Add(TEXT("SMOG"), 0.7f, 0.9f, 0.35f, 3.0f, 0.004f, 0.0f, 2.0f);
			Add(TEXT("FOGGY"), 0.35f, 0.8f, 0.6f, 1.5f, 0.06f, 0.15f, 2.0f);
			Add(TEXT("OVERCAST"), 0.3f, 0.75f, 0.95f, 1.2f, 0.006f, 0.1f, 7.0f);
			Add(TEXT("CLEARING"), 0.55f, 0.8f, 0.6f, 0.45f, 0.004f, 0.7f, 6.0f);
			Add(TEXT("RAIN"), 0.12f, 0.6f, 1.0f, 0.5f, 0.012f, 1.0f, 9.0f);
			Add(TEXT("THUNDER"), 0.08f, 0.5f, 1.0f, 0.5f, 0.016f, 1.0f, 14.0f);
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
		TWeakObjectPtr<USkyAtmosphereComponent> Atmosphere;
		TWeakObjectPtr<UMaterialParameterCollection> Collection;
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
	if (ADirectionalLight* Sun = A.Sun.Get())
	{
		const float Angle = (Hours - 6.0f) / 12.0f * PI;
		const FVector Toward = FVector(FMath::Cos(Angle), FMath::Sin(Angle) * 0.5f, FMath::Sin(Angle) * 0.866f).GetSafeNormal(); // (y: south, mirrored)
		const bool bDay = Toward.Z > -0.05f;
		Sun->SetActorRotation((bDay ? -Toward : Toward).Rotation());
		ULightComponent* Light = Sun->GetLightComponent();
		Light->SetIntensity(bDay ? 100000.0f * L.Sun : 0.25f * FMath::Max(L.Sun, 0.2f));
		Light->SetLightColor(bDay ? FLinearColor::White : FLinearColor(0.6f, 0.7f, 1.0f));
	}
	if (ASkyLight* Sky = A.Sky.Get())
	{
		Sky->GetLightComponent()->SetIntensity(L.Sky);
	}
	if (UMaterialInstanceDynamic* Clouds = A.CloudMaterial.Get())
	{
		// (the engine's cloud material: Cloud_GlobalCoverage 0 gives scattered cumulus, 0.3 a hazy overcast, 0.45 a milky
		// one, measured; a clear sky has none at all)
		const float Override = CVarCloudCoverage.GetValueOnGameThread();
		const float Cover = FMath::Clamp(Override >= 0 ? Override : L.Clouds, 0.0f, 1.0f);
		if (UVolumetricCloudComponent* Layer = A.Clouds.Get())
		{
			Layer->SetVisibility(Cover >= 0.1f);
		}
		Clouds->SetScalarParameterValue(TEXT("Cloud_GlobalCoverage"), FMath::Max(Cover - 0.1f, 0.0f) / 0.9f * 0.45f);
	}
	// The air: aerosols (Mie scattering) that blur and tint the distance, thickest in the Los Santos basin (a city's
	// haze under an inversion about a kilometre up: the hills rise out of it), thinner over the countryside north of
	// it; a ground fog mostly in fog and rain.
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
	}
	if (AExponentialHeightFog* Fog = A.Fog.Get())
	{
		UExponentialHeightFogComponent* C = Fog->GetComponent();
		C->SetFogDensity(L.Fog);
		C->SetFogHeightFalloff(0.35f);
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
		}
	}
}
