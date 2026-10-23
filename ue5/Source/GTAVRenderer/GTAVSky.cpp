#include "GTAVSky.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
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

#include "GTAVPack.h"

namespace
{
	// What a weather type looks like: the sun's and the sky's light (1 = clear), cloud cover, fog density, wet ground.
	struct FLook
	{
		float Sun = 1, Sky = 1, Clouds = 0.35f, Fog = 0.015f, Wet = 0;
	};

	FLook Lerp(const FLook& A, const FLook& B, float T)
	{
		return {FMath::Lerp(A.Sun, B.Sun, T), FMath::Lerp(A.Sky, B.Sky, T), FMath::Lerp(A.Clouds, B.Clouds, T), FMath::Lerp(A.Fog, B.Fog, T),
			FMath::Lerp(A.Wet, B.Wet, T)};
	}

	const FLook& Look(uint32 Weather)
	{
		static const TMap<uint32, FLook> Looks = [] {
			TMap<uint32, FLook> Out;
			const auto Add = [&](const TCHAR* Name, float Sun, float Sky, float Clouds, float Fog, float Wet) {
				Out.Add(GTAVHash(Name), {Sun, Sky, Clouds, Fog, Wet});
			};
			Add(TEXT("EXTRASUNNY"), 1.0f, 1.0f, 0.15f, 0.01f, 0.0f);
			Add(TEXT("CLEAR"), 1.0f, 1.0f, 0.35f, 0.015f, 0.0f);
			Add(TEXT("NEUTRAL"), 0.9f, 1.0f, 0.4f, 0.02f, 0.0f);
			Add(TEXT("CLOUDS"), 0.75f, 0.9f, 0.65f, 0.02f, 0.0f);
			Add(TEXT("SMOG"), 0.7f, 0.9f, 0.35f, 0.06f, 0.0f);
			Add(TEXT("FOGGY"), 0.35f, 0.8f, 0.6f, 0.25f, 0.15f);
			Add(TEXT("OVERCAST"), 0.3f, 0.75f, 0.95f, 0.04f, 0.1f);
			Add(TEXT("CLEARING"), 0.55f, 0.8f, 0.6f, 0.04f, 0.7f);
			Add(TEXT("RAIN"), 0.12f, 0.6f, 1.0f, 0.07f, 1.0f);
			Add(TEXT("THUNDER"), 0.08f, 0.5f, 1.0f, 0.09f, 1.0f);
			Add(TEXT("SNOW"), 0.3f, 0.8f, 0.9f, 0.1f, 0.2f);
			Add(TEXT("BLIZZARD"), 0.1f, 0.6f, 1.0f, 0.3f, 0.2f);
			Add(TEXT("SNOWLIGHT"), 0.5f, 0.9f, 0.7f, 0.05f, 0.1f);
			Add(TEXT("XMAS"), 0.4f, 0.9f, 0.8f, 0.06f, 0.1f);
			Add(TEXT("HALLOWEEN"), 0.1f, 0.5f, 1.0f, 0.15f, 0.5f);
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

void GTAVSky::Apply(UWorld* World, float Hours, uint32 WeatherFrom, uint32 WeatherTo, float Blend)
{
	static FActors A;
	if (A.World.Get() != World)
	{
		Find(World, A);
	}
	const FLook L = Lerp(Look(WeatherFrom), Look(WeatherTo), FMath::Clamp(Blend, 0.0f, 1.0f));

	// The sun from the hour: east at 6, highest at 12 (to the south), west at 18; at night a faint blue moon opposite.
	if (ADirectionalLight* Sun = A.Sun.Get())
	{
		const float Angle = (Hours - 6.0f) / 12.0f * PI;
		const FVector Toward = FVector(FMath::Cos(Angle), FMath::Sin(Angle) * 0.5f, FMath::Sin(Angle) * 0.866f).GetSafeNormal(); // (y: south, mirrored)
		const bool bDay = Toward.Z > -0.05f;
		Sun->SetActorRotation((bDay ? -Toward : Toward).Rotation());
		ULightComponent* Light = Sun->GetLightComponent();
		Light->SetIntensity(bDay ? 10.0f * L.Sun : 0.3f * FMath::Max(L.Sun, 0.2f));
		Light->SetLightColor(bDay ? FLinearColor::White : FLinearColor(0.6f, 0.7f, 1.0f));
	}
	if (ASkyLight* Sky = A.Sky.Get())
	{
		Sky->GetLightComponent()->SetIntensity(L.Sky);
	}
	if (UMaterialInstanceDynamic* Clouds = A.CloudMaterial.Get())
	{
		Clouds->SetScalarParameterValue(TEXT("Cloud_GlobalCoverage"), L.Clouds);
	}
	if (AExponentialHeightFog* Fog = A.Fog.Get())
	{
		Fog->GetComponent()->SetFogDensity(L.Fog);
	}
	if (UMaterialParameterCollection* Collection = A.Collection.Get())
	{
		if (UMaterialParameterCollectionInstance* Instance = World->GetParameterCollectionInstance(Collection))
		{
			Instance->SetScalarParameterValue(TEXT("Wetness"), L.Wet);
		}
	}
}
