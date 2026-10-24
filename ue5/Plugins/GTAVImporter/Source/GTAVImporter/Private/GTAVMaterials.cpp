#include "GTAVMaterials.h"

#include "Dom/JsonObject.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionMax.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialExpressionPanner.h"
#include "Materials/MaterialExpressionNormalize.h"
#include "Materials/MaterialExpressionFresnel.h"
#include "Materials/MaterialExpressionDepthFade.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionVertexInterpolator.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionCameraPositionWS.h"
#include "Materials/MaterialExpressionDistance.h"
#include "Materials/MaterialExpressionObjectPositionWS.h"
#include "Materials/MaterialExpressionPerInstanceCustomData.h"
#include "Materials/MaterialExpressionPixelDepth.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionSceneDepth.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionViewProperty.h"
#include "Materials/MaterialExpressionLocalPosition.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionFrac.h"
#include "Materials/MaterialExpressionAbs.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"

#include "GTAVConvert.h"
#include "GTAVDds.h"

DEFINE_LOG_CATEGORY_STATIC(LogGTAVMaterials, Log, All);

namespace
{
	// The palette row a tinted model is drawn with: the entity's (the map's tintValue, which the renderer gives each
	// component as its custom primitive data 0: the same model in another colour, a house's walls, a room's paint) where
	// it has one, else the material's own.
	UMaterialExpression* EntityRow(UMaterial* M, UMaterialExpression* MaterialRow)
	{
		auto* Entity = NewObject<UMaterialExpressionScalarParameter>(M);
		Entity->ParameterName = TEXT("EntityTint");
		Entity->bUseCustomPrimitiveData = true;
		Entity->PrimitiveDataIndex = 0;
		Entity->DefaultValue = 0.0f;
		M->GetExpressionCollection().AddExpression(Entity);
		auto* Row = NewObject<UMaterialExpressionCustom>(M);
		Row->Description = TEXT("Tint row");
		Row->OutputType = CMOT_Float1;
		Row->Code = TEXT("return Entity > 0.5 ? Entity : Material;");
		Row->Inputs.Reset();
		FCustomInput& A = Row->Inputs.AddDefaulted_GetRef();
		A.InputName = TEXT("Entity");
		A.Input.Connect(0, Entity);
		FCustomInput& B = Row->Inputs.AddDefaulted_GetRef();
		B.InputName = TEXT("Material");
		B.Input.Connect(0, MaterialRow);
		M->GetExpressionCollection().AddExpression(Row);
		return Row;
	}

	template <typename T>
	T* LoadExisting(const FString& PackageName)
	{
		if (!FPackageName::DoesPackageExist(PackageName))
		{
			return nullptr;
		}
		return LoadObject<T>(nullptr, *(PackageName + TEXT(".") + FPaths::GetBaseFilename(PackageName)), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	UTexture2D* EngineTexture(const TCHAR* Path)
	{
		return LoadObject<UTexture2D>(nullptr, Path);
	}
}

FGTAVMaterials::FGTAVMaterials(const FString& InExportRoot)
	: ExportRoot(InExportRoot)
{
}

// A black linear 4x4 texture: the specular maps' default (the sampler's type has to match the texture's).
UTexture2D* FGTAVMaterials::BlackMask()
{
	static const TCHAR* PackageName = TEXT("/Game/GTAV/Materials/T_GTAV_BlackMask");
	if (UTexture2D* Existing = LoadExisting<UTexture2D>(PackageName))
	{
		return Existing;
	}
	UPackage* Package = CreatePackage(PackageName);
	UTexture2D* T = NewObject<UTexture2D>(Package, TEXT("T_GTAV_BlackMask"), RF_Public | RF_Standalone);
	TArray<FColor> Pixels;
	Pixels.Init(FColor::Black, 16);
	T->Source.Init(4, 4, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
	T->CompressionSettings = TC_Masks;
	T->SRGB = false;
	T->PostEditChange();
	Package->MarkPackageDirty();
	Dirty.Add(Package);
	return T;
}

UMaterial* FGTAVMaterials::Master(int32 Kind)
{
	if (UMaterial** Found = Masters.Find(Kind))
	{
		return *Found;
	}
	static const TCHAR* Names[] = {TEXT("M_GTAV_Opaque"), TEXT("M_GTAV_Translucent"), nullptr, TEXT("M_GTAV_Cutout"), TEXT("M_GTAV_Terrain4"), nullptr,
		TEXT("M_GTAV_Water"), TEXT("M_GTAV_Ocean"), nullptr, TEXT("M_GTAV_OceanFar")};
	const FString PackageName = FString(TEXT("/Game/GTAV/Materials/")) + Names[Kind];
	if (UMaterial* Existing = LoadExisting<UMaterial>(PackageName))
	{
		return Masters.Add(Kind, Existing);
	}
	UPackage* Package = CreatePackage(*PackageName);
	UMaterial* M = NewObject<UMaterial>(Package, Names[Kind], RF_Public | RF_Standalone);
	M->BlendMode = Kind == 1 || Kind == kWater || Kind == kOcean ? BLEND_Translucent : Kind == 3 ? BLEND_Masked : BLEND_Opaque;
	M->TwoSided = Kind == 3; // (foliage, fences)
	// (cut out where the texture is half see-through, as the game's alpha test: lower, the edges keep texels the filtering
	// mixed with the colour the transparent ones hold - leaves and bushes then wear a red rim)
	M->OpacityMaskClipValue = 0.5f;
	// (usage flags cannot be added in the game: the material would fall back to the default one)
	M->bUsedWithNanite = true;
	M->bUsedWithInstancedStaticMeshes = true;
	if (Kind == 1 || Kind == kWater || Kind == kOcean)
	{
		M->TranslucencyLightingMode = TLM_SurfacePerPixelLighting;
		// (fog and the air's haze per pixel: the sea's triangles are kilometres long)
		M->bUseTranslucencyVertexFog = true; // ("Apply Fogging")
		M->bComputeFogPerPixel = true;
	}
	UMaterialEditorOnlyData* Inputs = M->GetEditorOnlyData();
	const auto Sample = [&](const TCHAR* Parameter, UTexture2D* Default, EMaterialSamplerType Type, int32 X, int32 Y) {
		auto* E = NewObject<UMaterialExpressionTextureSampleParameter2D>(M);
		E->ParameterName = Parameter;
		E->Texture = Default;
		E->SamplerType = Type;
		E->MaterialExpressionEditorX = X;
		E->MaterialExpressionEditorY = Y;
		M->GetExpressionCollection().AddExpression(E);
		return E;
	};
	UTexture2D* White = EngineTexture(TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
	UTexture2D* FlatNormal = EngineTexture(TEXT("/Engine/EngineMaterials/DefaultNormal.DefaultNormal"));
	if (Kind == kWater || Kind == kOcean || Kind == kOceanFar)
	{
		const bool bSea = Kind != kWater;
		// Water (draw bucket 6: pools, fountains) and the sea (water.xml): ripples from the game's water bump map moving
		// two ways across the world, dark, smooth and reflective (Lumen's front layer), clear where it is shallow.
		M->bAllowFrontLayerTranslucency = true;
		const auto Add = [&](UMaterialExpression* E) {
			M->GetExpressionCollection().AddExpression(E);
			return E;
		};
		auto* World = NewObject<UMaterialExpressionWorldPosition>(M);
		Add(World);
		auto* XY = NewObject<UMaterialExpressionComponentMask>(M);
		XY->Input.Connect(0, World);
		XY->R = XY->G = 1;
		XY->B = XY->A = 0;
		Add(XY);
		// Rivers: the ripples carried by the current (the fourth UV set: the game's flow map, -1..1 a channel; still water
		// has none), flow-mapped: two copies of the ripples a half period apart, each moved along with the water and
		// pulled back to where it started once a period, the one about to be pulled back faded out.
		UMaterialExpression* Moved[2] = {nullptr, nullptr};
		UMaterialExpression* Crossfade = nullptr;
		if (!bSea)
		{
			constexpr float FlowSpeed = 350.0f; // cm/s at a flow of 1: what a swimmer left in the river drifts at (§60)
			constexpr float Period = 1.2f;      // s
			auto* FlowUv = NewObject<UMaterialExpressionTextureCoordinate>(M);
			FlowUv->CoordinateIndex = 3;
			Add(FlowUv);
			auto* Time = NewObject<UMaterialExpressionTime>(M);
			Add(Time);
			auto* Cycles = NewObject<UMaterialExpressionDivide>(M);
			Cycles->A.Connect(0, Time);
			Cycles->ConstB = Period;
			Add(Cycles);
			for (int32 K = 0; K < 2; ++K)
			{
				auto* Shifted = NewObject<UMaterialExpressionAdd>(M);
				Shifted->A.Connect(0, Cycles);
				Shifted->ConstB = 0.5f * K;
				Add(Shifted);
				auto* Phase = NewObject<UMaterialExpressionFrac>(M);
				Phase->Input.Connect(0, Shifted);
				Add(Phase);
				auto* Distance = NewObject<UMaterialExpressionMultiply>(M); // (cm along the current so far)
				Distance->A.Connect(0, Phase);
				Distance->ConstB = FlowSpeed * Period;
				Add(Distance);
				auto* Offset = NewObject<UMaterialExpressionMultiply>(M);
				Offset->A.Connect(0, FlowUv);
				Offset->B.Connect(0, Distance);
				Add(Offset);
				auto* Back = NewObject<UMaterialExpressionSubtract>(M);
				Back->A.Connect(0, XY);
				Back->B.Connect(0, Offset);
				Moved[K] = Add(Back);
				if (K == 0)
				{
					auto* Twice = NewObject<UMaterialExpressionMultiply>(M);
					Twice->A.Connect(0, Phase);
					Twice->ConstB = 2.0f;
					Add(Twice);
					auto* Centred = NewObject<UMaterialExpressionSubtract>(M);
					Centred->A.Connect(0, Twice);
					Centred->ConstB = 1.0f;
					Add(Centred);
					auto* Weight = NewObject<UMaterialExpressionAbs>(M); // (1 as the first copy is pulled back: the second shows)
					Weight->Input.Connect(0, Centred);
					Crossfade = Add(Weight);
				}
			}
		}
		const auto Ripples = [&](float Size, float SpeedX, float SpeedY, int32 Y) -> UMaterialExpression* {
			const auto One = [&](UMaterialExpression* Where) {
				auto* Scale = NewObject<UMaterialExpressionDivide>(M);
				Scale->A.Connect(0, Where);
				Scale->ConstB = Size;
				Add(Scale);
				auto* Pan = NewObject<UMaterialExpressionPanner>(M);
				Pan->Coordinate.Connect(0, Scale);
				Pan->SpeedX = SpeedX;
				Pan->SpeedY = SpeedY;
				Add(Pan);
				auto* S = Sample(TEXT("WaterBump"), FlatNormal, SAMPLERTYPE_Normal, -700, Y);
				S->Coordinates.Connect(0, Pan);
				return S;
			};
			if (!Crossfade)
			{
				return One(XY);
			}
			auto* Mix = NewObject<UMaterialExpressionLinearInterpolate>(M);
			Mix->A.Connect(0, One(Moved[0]));
			Mix->B.Connect(0, One(Moved[1]));
			Mix->Alpha.Connect(0, Crossfade);
			return Add(Mix);
		};
		auto* Sum = NewObject<UMaterialExpressionAdd>(M);
		Sum->A.Connect(0, Ripples(bSea ? 900.0f : 200.0f, 0.012f, 0.007f, -200));
		Sum->B.Connect(0, Ripples(bSea ? 2300.0f : 450.0f, -0.006f, 0.010f, 100));
		Add(Sum);
		// The ripples fade out with the distance (gone at 200 m; 60 m on lakes): ripples finer than a pixel cannot be
		// seen, their texture only aliases there (streaks towards the horizon); the roughness takes them over below.
		auto* RippleDepth = NewObject<UMaterialExpressionPixelDepth>(M);
		Add(RippleDepth);
		auto* RippleFar = NewObject<UMaterialExpressionMultiply>(M);
		RippleFar->A.Connect(0, RippleDepth);
		RippleFar->ConstB = 1.0f / (bSea ? 20000.0f : 6000.0f);
		Add(RippleFar);
		auto* RippleGone = NewObject<UMaterialExpressionSaturate>(M);
		RippleGone->Input.Connect(0, RippleFar);
		Add(RippleGone);
		auto* RippleNear = NewObject<UMaterialExpressionOneMinus>(M);
		RippleNear->Input.Connect(0, RippleGone);
		Add(RippleNear);
		auto* RippleXY = NewObject<UMaterialExpressionMultiply>(M); // (the slopes halved near: gentler waves)
		RippleXY->A.Connect(0, RippleNear);
		RippleXY->ConstB = 0.5f;
		Add(RippleXY);
		auto* WeightsXY = NewObject<UMaterialExpressionAppendVector>(M);
		WeightsXY->A.Connect(0, RippleXY);
		WeightsXY->B.Connect(0, RippleXY);
		Add(WeightsXY);
		auto* Zero = NewObject<UMaterialExpressionConstant>(M);
		Zero->R = 0.0f;
		Add(Zero);
		auto* Weights = NewObject<UMaterialExpressionAppendVector>(M);
		Weights->A.Connect(0, WeightsXY);
		Weights->B.Connect(0, Zero);
		Add(Weights);
		auto* Slopes = NewObject<UMaterialExpressionMultiply>(M);
		Slopes->A.Connect(0, Sum);
		Slopes->B.Connect(0, Weights);
		Add(Slopes);
		// The normal from the two samples' slopes over the two unit z's: the game's water bump map's own z comes out
		// as 0 (a two-channel map read as a normal map), and with the ripples faded out (0, 0, 0) normalised was NaN,
		// spread by the temporal upscaler and the bloom into a glaring blob over a pool seen from 60 m.
		auto* Up = NewObject<UMaterialExpressionConstant3Vector>(M);
		Up->Constant = FLinearColor(0.0f, 0.0f, 2.0f);
		Add(Up);
		auto* Flatter = NewObject<UMaterialExpressionAdd>(M);
		Flatter->A.Connect(0, Slopes);
		Flatter->B.Connect(0, Up);
		Add(Flatter);
		// The sea's swell over the ripples: the slopes of a wind sea (Pierson-Moskowitz spectrum, deep water dispersion)
		// for the wind in MPC_GTAV (lakes and pools keep the ripples).
		UMaterialExpression* Normal = nullptr;
		if (bSea)
		{
			UMaterialParameterCollection* C = Collection();
			const auto Global = [&](const TCHAR* Name) {
				auto* E = NewObject<UMaterialExpressionCollectionParameter>(M);
				E->Collection = C;
				E->ParameterName = Name;
				E->ParameterId = C->GetParameterId(Name);
				return Add(E);
			};
			auto* Time = NewObject<UMaterialExpressionTime>(M);
			Add(Time);
			auto* Waves = NewObject<UMaterialExpressionCustom>(M);
			Waves->OutputType = CMOT_Float3;
			Waves->Description = TEXT("Wind sea");
			Waves->Code = TEXT(R"HLSL(// A wind sea (Pierson-Moskowitz): deep water, w^2 = g k; the peak at wp = 0.877 g / U, Hs = 0.21 U^2 / g.
const float g = 9.81;
float U = max(WindSpeed, 0.5);
float2 D = normalize(float2(WindX, WindY) + float2(1e-4, 0));
float wp = 0.877 * g / U;
float2 P = Pos.xy * 0.01;
float Footprint = max(length(ddx(P)), length(ddy(P)));
float sx = 0, sy = 0;
[unroll] for (int i = 0; i < 12; i++)
{
    float w = wp * (0.75 + i * 0.22);
    float k = w * w / g;
    float spread = (frac(sin(i * 12.9898) * 43758.5453) - 0.5) * 2.0;
    float2 d = float2(D.x * cos(spread) - D.y * sin(spread), D.x * sin(spread) + D.y * cos(spread));
    float S = 0.0081 * g * g / pow(w, 5) * exp(-1.25 * pow(wp / w, 4));
    float a = sqrt(2 * S * 0.22 * wp);
    // (a wave shorter than about two pixels cannot be seen, only aliased: it fades out with the pixel's footprint)
    a *= saturate(1.0 - k * Footprint * 0.5);
    float c = cos(k * dot(d, P) - w * Time + i * 1.7);
    sx += a * k * d.x * c;
    sy += a * k * d.y * c;
}
float3 R = normalize(Ripple);
return normalize(float3(R.xy - float2(sx, sy), R.z));
)HLSL");
			Waves->Inputs.Reset();
			const auto Input = [&](const TCHAR* Name, UMaterialExpression* E) {
				FCustomInput& I = Waves->Inputs.AddDefaulted_GetRef();
				I.InputName = Name;
				I.Input.Connect(0, E);
			};
			Input(TEXT("Pos"), World);
			Input(TEXT("Time"), Time);
			Input(TEXT("Ripple"), Flatter);
			Input(TEXT("WindX"), Global(TEXT("WindX")));
			Input(TEXT("WindY"), Global(TEXT("WindY")));
			Input(TEXT("WindSpeed"), Global(TEXT("WindSpeed")));
			Normal = Add(Waves);
		}
		else
		{
			auto* N = NewObject<UMaterialExpressionNormalize>(M);
			N->VectorInput.Connect(0, Flatter);
			Normal = Add(N);
		}
		Inputs->Normal.Connect(0, Normal);
		auto* Color = NewObject<UMaterialExpressionVectorParameter>(M);
		Color->ParameterName = TEXT("WaterColor");
		Color->DefaultValue = bSea ? FLinearColor(0.10f, 0.30f, 0.33f) : FLinearColor(0.65f, 0.76f, 0.78f);
		Add(Color);
		// (the game's water is mostly what it reflects: its colour is the depth fog, kept dark here)
		auto* Dark = NewObject<UMaterialExpressionMultiply>(M);
		Dark->A.Connect(0, Color);
		Dark->ConstB = 0.12f;
		Add(Dark);
		Inputs->BaseColor.Connect(0, Dark);
		// Water reflects 2% straight on (F0 0.02: the engine's Specular 0.25).
		auto* Specular = NewObject<UMaterialExpressionConstant>(M);
		Specular->R = 0.25f;
		Add(Specular);
		Inputs->Specular.Connect(0, Specular);
		// The ripples too fine to see become roughness (Cox and Munk's sea surface slopes: mean square slope
		// 0.003 + 0.00512 U for a wind of U m/s; GGX alpha about that slope, the engine's roughness its square root),
		// pools and rivers sheltered (half the wind). Flat, the water mirrored the white haze at the horizon.
		auto* Wind = NewObject<UMaterialExpressionCollectionParameter>(M);
		Wind->Collection = Collection();
		Wind->ParameterName = TEXT("WindSpeed");
		Wind->ParameterId = Wind->Collection->GetParameterId(TEXT("WindSpeed"));
		Add(Wind);
		auto* FarRough = NewObject<UMaterialExpressionCustom>(M);
		FarRough->Description = TEXT("Cox-Munk roughness");
		FarRough->OutputType = CMOT_Float1;
		FarRough->Code = bSea ? TEXT("return sqrt(sqrt(0.003 + 0.00512 * max(Wind, 0)));") : TEXT("return sqrt(sqrt(0.003 + 0.00512 * 0.5 * max(Wind, 0)));");
		FarRough->Inputs.Reset();
		{
			FCustomInput& In = FarRough->Inputs.AddDefaulted_GetRef();
			In.InputName = TEXT("Wind");
			In.Input.Connect(0, Wind);
		}
		Add(FarRough);
		auto* Rough = NewObject<UMaterialExpressionLinearInterpolate>(M);
		Rough->ConstA = 0.04f;
		Rough->B.Connect(0, FarRough);
		Rough->Alpha.Connect(0, RippleGone);
		Add(Rough);
		Inputs->Roughness.Connect(0, Rough);
		if (Kind == kOceanFar)
		{
			M->PreEditChange(nullptr);
			M->PostEditChange();
			Package->MarkPackageDirty();
			Dirty.Add(Package);
			return Masters.Add(Kind, M);
		}
		// Opacity: how much of what lies behind does not come through (Fresnel at the surface, Beer-Lambert in the
		// water; coastal sea water about 0.35 per metre, lakes and rivers murkier).
		auto* Fresnel = NewObject<UMaterialExpressionFresnel>(M);
		Fresnel->Exponent = 5.0f;
		Fresnel->BaseReflectFraction = 0.0f;
		Add(Fresnel);
		auto* Scene = NewObject<UMaterialExpressionSceneDepth>(M);
		Add(Scene);
		auto* Here = NewObject<UMaterialExpressionPixelDepth>(M);
		Add(Here);
		auto* Sigma = NewObject<UMaterialExpressionScalarParameter>(M); // (per metre; the renderer's per water shader)
		Sigma->ParameterName = TEXT("Sigma");
		Sigma->DefaultValue = bSea ? 0.35f : 0.6f;
		Add(Sigma);
		auto* AtLeast = NewObject<UMaterialExpressionCustom>(M);
		AtLeast->OutputType = CMOT_Float1;
		AtLeast->Description = TEXT("Water absorption");
		AtLeast->Code = TEXT(R"HLSL(// What is seen through water: the light that crosses the surface (1 - Fresnel) and the water itself
// (Beer-Lambert: exp(-sigma d), d the water's thickness in metres).
float d = max(Scene - Pixel, 0) * 0.01;
float F = 0.02 + 0.98 * Fresnel;
return saturate(1 - (1 - F) * exp(-Sigma * d));
)HLSL");
		AtLeast->Inputs.Reset();
		for (const TPair<const TCHAR*, UMaterialExpression*>& I : {TPair<const TCHAR*, UMaterialExpression*>(TEXT("Fresnel"), Fresnel),
				 {TEXT("Scene"), Scene}, {TEXT("Pixel"), Here}, {TEXT("Sigma"), Sigma}})
		{
			FCustomInput& In = AtLeast->Inputs.AddDefaulted_GetRef();
			In.InputName = I.Key;
			In.Input.Connect(0, I.Value);
		}
		Add(AtLeast);
		// Opaque in the distance (water far off shows no bottom, and the air's haze then covers it fully instead of
		// the empty sky behind showing through): fully from 200 m (the sea) or 60 m (lakes, rivers).
		auto* Depth = NewObject<UMaterialExpressionPixelDepth>(M);
		Add(Depth);
		auto* Distant = NewObject<UMaterialExpressionMultiply>(M);
		Distant->A.Connect(0, Depth);
		Distant->ConstB = 1.0f / (Kind == kOcean ? 20000.0f : 6000.0f);
		Add(Distant);
		auto* DistantClamped = NewObject<UMaterialExpressionSaturate>(M);
		DistantClamped->Input.Connect(0, Distant);
		Add(DistantClamped);
		auto* Opacity = NewObject<UMaterialExpressionMax>(M);
		Opacity->A.Connect(0, AtLeast);
		Opacity->B.Connect(0, DistantClamped);
		Add(Opacity);
		Inputs->Opacity.Connect(0, Opacity);
		M->PreEditChange(nullptr);
		M->PostEditChange();
		Package->MarkPackageDirty();
		Dirty.Add(Package);
		return Masters.Add(Kind, M);
	}
	if (Kind == kTerrain)
	{
		// Four layers blended as the game's terrain_cb shaders do (their pixel shaders, sga_win32_60_final.awc; the
		// mesh carries the game's second colour set as the vertex colour): weights W = the vertex colour's rgb, or
		// where the material has a lookup texture (sampled with the second UV set) lerp(lookup.rgb, rgb, alpha);
		// green picks the pair (0 / 1 or 2 / 3), blue the layer in it, red darkens all: layer 0 (1-r)(1-g)(1-b),
		// 1 (1-r)(1-g)b, 2 (1-r)g(1-b), 3 (1-r)gb.
		auto* Vertex = NewObject<UMaterialExpressionVertexColor>(M);
		M->GetExpressionCollection().AddExpression(Vertex);
		auto* LookupUv = NewObject<UMaterialExpressionTextureCoordinate>(M);
		LookupUv->CoordinateIndex = 1;
		M->GetExpressionCollection().AddExpression(LookupUv);
		auto* Lookup = Sample(TEXT("LookupTex"), BlackMask(), SAMPLERTYPE_Masks, -1200, -600);
		Lookup->Coordinates.Connect(0, LookupUv);
		auto* UseLookup = NewObject<UMaterialExpressionScalarParameter>(M);
		UseLookup->ParameterName = TEXT("UseLookup");
		UseLookup->DefaultValue = 0.0f;
		M->GetExpressionCollection().AddExpression(UseLookup);
		auto* Weighed = NewObject<UMaterialExpressionCustom>(M);
		Weighed->Description = TEXT("Terrain weights");
		Weighed->OutputType = CMOT_Float3;
		Weighed->Code = TEXT("float3 W = lerp(Rgb, lerp(Lookup.rgb, Rgb, Alpha), UseLookup);\nreturn saturate(W);");
		Weighed->Inputs.Reset();
		for (const TTuple<const TCHAR*, UMaterialExpression*, int32>& I : {MakeTuple(TEXT("Rgb"), (UMaterialExpression*)Vertex, 0),
				 MakeTuple(TEXT("Alpha"), (UMaterialExpression*)Vertex, 4), MakeTuple(TEXT("Lookup"), (UMaterialExpression*)Lookup, 0),
				 MakeTuple(TEXT("UseLookup"), (UMaterialExpression*)UseLookup, 0)})
		{
			FCustomInput& In = Weighed->Inputs.AddDefaulted_GetRef();
			In.InputName = I.Get<0>();
			In.Input.Connect(I.Get<2>(), I.Get<1>());
		}
		M->GetExpressionCollection().AddExpression(Weighed);
		auto* WR = NewObject<UMaterialExpressionComponentMask>(M);
		WR->Input.Connect(0, Weighed);
		WR->R = true;
		M->GetExpressionCollection().AddExpression(WR);
		auto* WG = NewObject<UMaterialExpressionComponentMask>(M);
		WG->Input.Connect(0, Weighed);
		WG->G = true;
		M->GetExpressionCollection().AddExpression(WG);
		auto* WB = NewObject<UMaterialExpressionComponentMask>(M);
		WB->Input.Connect(0, Weighed);
		WB->B = true;
		M->GetExpressionCollection().AddExpression(WB);
		const auto Blend = [&](const TCHAR* Prefix, UTexture2D* Default, EMaterialSamplerType Type, int32 Y) {
			UMaterialExpressionTextureSampleParameter2D* L[4];
			for (int32 I = 0; I < 4; ++I)
			{
				L[I] = Sample(*FString::Printf(TEXT("%s%d"), Prefix, I), Default, Type, -900, Y + 250 * I);
			}
			UMaterialExpressionLinearInterpolate* Pair[2];
			for (int32 I = 0; I < 2; ++I)
			{
				Pair[I] = NewObject<UMaterialExpressionLinearInterpolate>(M);
				Pair[I]->A.Connect(0, L[2 * I]);
				Pair[I]->B.Connect(0, L[2 * I + 1]);
				Pair[I]->Alpha.Connect(0, WB);
				M->GetExpressionCollection().AddExpression(Pair[I]);
			}
			auto* Out = NewObject<UMaterialExpressionLinearInterpolate>(M);
			Out->A.Connect(0, Pair[0]);
			Out->B.Connect(0, Pair[1]);
			Out->Alpha.Connect(0, WG);
			M->GetExpressionCollection().AddExpression(Out);
			return Out;
		};
		auto* Dark = NewObject<UMaterialExpressionOneMinus>(M);
		Dark->Input.Connect(0, WR);
		M->GetExpressionCollection().AddExpression(Dark);
		auto* Colour = NewObject<UMaterialExpressionMultiply>(M);
		Colour->A.Connect(0, Blend(TEXT("DiffuseTexture_layer"), White, SAMPLERTYPE_Color, -1200));
		Colour->B.Connect(0, Dark);
		M->GetExpressionCollection().AddExpression(Colour);
		// The tint (terrain_cb_*_tnt): each layer lerp(D, lerp(grey(T), T, b) D, b), T the palette's colour at the
		// first colour set's blue (third UV set) on the material's row, b tintBlendLayer (the shader's own default,
		// which no material overrides: 1, the tint in full; desaturateTint on). The same T and b for the four layers:
		// applied to their blend.
		auto* TintUv = NewObject<UMaterialExpressionTextureCoordinate>(M);
		TintUv->CoordinateIndex = 2;
		M->GetExpressionCollection().AddExpression(TintUv);
		const auto TerrainScalar = [&](const TCHAR* Parameter, float Default) {
			auto* E = NewObject<UMaterialExpressionScalarParameter>(M);
			E->ParameterName = Parameter;
			E->DefaultValue = Default;
			M->GetExpressionCollection().AddExpression(E);
			return E;
		};
		auto* PaletteUv = NewObject<UMaterialExpressionCustom>(M);
		PaletteUv->Description = TEXT("Palette texel");
		PaletteUv->OutputType = CMOT_Float2;
		PaletteUv->Code = TEXT("return float2((Column.x * 255.0 + 0.5) / Width, (Row + 0.5) / Height);");
		PaletteUv->Inputs.Reset();
		for (const TPair<const TCHAR*, UMaterialExpression*>& I : {TPair<const TCHAR*, UMaterialExpression*>(TEXT("Column"), TintUv),
				 {TEXT("Width"), TerrainScalar(TEXT("TintPaletteWidth"), 256.0f)}, {TEXT("Height"), TerrainScalar(TEXT("TintPaletteHeight"), 4.0f)},
				 {TEXT("Row"), EntityRow(M, TerrainScalar(TEXT("TintRow"), 0.0f))}})
		{
			FCustomInput& In = PaletteUv->Inputs.AddDefaulted_GetRef();
			In.InputName = I.Key;
			In.Input.Connect(0, I.Value);
		}
		M->GetExpressionCollection().AddExpression(PaletteUv);
		auto* Palette = Sample(TEXT("TintPaletteTex"), White, SAMPLERTYPE_Color, -1500, -900);
		Palette->Coordinates.Connect(0, PaletteUv);
		Palette->MipValueMode = TMVM_MipLevel;
		Palette->ConstMipValue = 0;
		auto* PaletteAtVertex = NewObject<UMaterialExpressionVertexInterpolator>(M);
		PaletteAtVertex->Input.Connect(0, Palette);
		M->GetExpressionCollection().AddExpression(PaletteAtVertex);
		auto* Tint = NewObject<UMaterialExpressionCustom>(M);
		Tint->Description = TEXT("Terrain tint");
		Tint->OutputType = CMOT_Float3;
		Tint->Code = TEXT("float g = dot(T, float3(1, 1, 1)) / 3.0;\nfloat3 Tinted = lerp(float3(g, g, g), T, Blend);\nreturn lerp(float3(1, 1, 1), Tinted, Blend * UseTint);");
		Tint->Inputs.Reset();
		for (const TPair<const TCHAR*, UMaterialExpression*>& I : {TPair<const TCHAR*, UMaterialExpression*>(TEXT("T"), PaletteAtVertex),
				 {TEXT("Blend"), TerrainScalar(TEXT("TintBlend"), 1.0f)}, {TEXT("UseTint"), TerrainScalar(TEXT("UseTint"), 0.0f)}})
		{
			FCustomInput& In = Tint->Inputs.AddDefaulted_GetRef();
			In.InputName = I.Key;
			In.Input.Connect(0, I.Value);
		}
		M->GetExpressionCollection().AddExpression(Tint);
		auto* Tinted = NewObject<UMaterialExpressionMultiply>(M);
		Tinted->A.Connect(0, Colour);
		Tinted->B.Connect(0, Tint);
		M->GetExpressionCollection().AddExpression(Tinted);
		Inputs->BaseColor.Connect(0, Tinted);
		Inputs->Normal.Connect(0, Blend(TEXT("BumpTexture_layer"), FlatNormal, SAMPLERTYPE_Normal, 0));
		auto* Rough = NewObject<UMaterialExpressionConstant>(M);
		Rough->R = 0.85f;
		M->GetExpressionCollection().AddExpression(Rough);
		Inputs->Roughness.Connect(0, Rough);
		Wetten(M);
		M->PreEditChange(nullptr);
		M->PostEditChange();
		Package->MarkPackageDirty();
		Dirty.Add(Package);
		return Masters.Add(Kind, M);
	}
	auto* Diffuse = Sample(TEXT("DiffuseTex"), White, SAMPLERTYPE_Color, -600, -200);
	auto* Bump = Sample(TEXT("BumpTex"), FlatNormal, SAMPLERTYPE_Normal, -600, 100);
	auto* Spec = Sample(TEXT("SpecularTex"), BlackMask(), SAMPLERTYPE_Masks, -600, 400);
	const auto Add = [&](UMaterialExpression* E) {
		M->GetExpressionCollection().AddExpression(E);
		return E;
	};
	const auto Scalar = [&](const TCHAR* Parameter, float Default) {
		auto* E = NewObject<UMaterialExpressionScalarParameter>(M);
		E->ParameterName = Parameter;
		E->DefaultValue = Default;
		Add(E);
		return E;
	};
	// The vertex colour as the game has it: red = natural ambient occlusion, blue = the tint palette column.
	auto* Vertex = NewObject<UMaterialExpressionVertexColor>(M);
	Add(Vertex);
	Inputs->AmbientOcclusion.Connect(1, Vertex);
	// Tint (the game's *_tnt shaders): a grey diffuse times the palette's colour, column = vertex blue, row = the
	// entity's tint (see EntityRow), else TintRow (the material's tintPaletteSelector).
	auto* Palette = Sample(TEXT("TintPaletteTex"), White, SAMPLERTYPE_Color, -900, -500);
	auto* Width = Scalar(TEXT("TintPaletteWidth"), 256.0f);
	auto* Height = Scalar(TEXT("TintPaletteHeight"), 4.0f);
	auto* Row = EntityRow(M, Scalar(TEXT("TintRow"), 0.0f));
	auto* Column = NewObject<UMaterialExpressionMultiply>(M); // blue * 255 + 0.5, over the width
	Column->A.Connect(3, Vertex);
	Column->ConstB = 255.0f;
	Add(Column);
	auto* ColumnCentre = NewObject<UMaterialExpressionAdd>(M);
	ColumnCentre->A.Connect(0, Column);
	ColumnCentre->ConstB = 0.5f;
	Add(ColumnCentre);
	auto* U = NewObject<UMaterialExpressionDivide>(M);
	U->A.Connect(0, ColumnCentre);
	U->B.Connect(0, Width);
	Add(U);
	auto* RowCentre = NewObject<UMaterialExpressionAdd>(M);
	RowCentre->A.Connect(0, Row);
	RowCentre->ConstB = 0.5f;
	Add(RowCentre);
	auto* V = NewObject<UMaterialExpressionDivide>(M);
	V->A.Connect(0, RowCentre);
	V->B.Connect(0, Height);
	Add(V);
	auto* Uv = NewObject<UMaterialExpressionAppendVector>(M);
	Uv->A.Connect(0, U);
	Uv->B.Connect(0, V);
	Add(Uv);
	Palette->Coordinates.Connect(0, Uv);
	// Looked up per vertex and the colour interpolated, as the game does: between two vertices the blue sweeps over
	// the columns in between, and palettes keep pure marker colours among their real ones.
	Palette->MipValueMode = TMVM_MipLevel;
	Palette->ConstMipValue = 0;
	auto* PerVertex = NewObject<UMaterialExpressionVertexInterpolator>(M);
	PerVertex->Input.Connect(0, Palette);
	Add(PerVertex);
	auto* Tinted = NewObject<UMaterialExpressionMultiply>(M);
	Tinted->A.Connect(0, Diffuse);
	Tinted->B.Connect(0, PerVertex);
	Add(Tinted);
	// (a scalar, not a static switch: the renderer's materials are made at run time, where switches cannot change)
	auto* UseTint = NewObject<UMaterialExpressionLinearInterpolate>(M);
	UseTint->A.Connect(0, Diffuse);
	UseTint->B.Connect(0, Tinted);
	UseTint->Alpha.Connect(0, Scalar(TEXT("UseTint"), 0.0f));
	Add(UseTint);
	Inputs->BaseColor.Connect(0, UseTint);
	if (Kind == 3)
	{
		// Grass instances carry the ground's colour (custom data 0..2, linear; 1 for everything not instanced): the
		// blades take on the colour of the soil they grow from.
		UMaterialExpressionPerInstanceCustomData* Channel[3];
		for (int32 I = 0; I < 3; ++I)
		{
			Channel[I] = NewObject<UMaterialExpressionPerInstanceCustomData>(M);
			Channel[I]->DataIndex = I;
			Channel[I]->ConstDefaultValue = 1.0f;
			Add(Channel[I]);
		}
		auto* RG = NewObject<UMaterialExpressionAppendVector>(M);
		RG->A.Connect(0, Channel[0]);
		RG->B.Connect(0, Channel[1]);
		Add(RG);
		auto* RGB = NewObject<UMaterialExpressionAppendVector>(M);
		RGB->A.Connect(0, RG);
		RGB->B.Connect(0, Channel[2]);
		Add(RGB);
		auto* Ground = NewObject<UMaterialExpressionMultiply>(M);
		Ground->A.Connect(0, UseTint);
		Ground->B.Connect(0, RGB);
		Add(Ground);
		Inputs->BaseColor.Connect(0, Ground);
	}
	Inputs->Normal.Connect(0, Bump);
	// Specular map: red = intensity. Rough where there is none, smoother where it shines.
	auto* Rough = NewObject<UMaterialExpressionLinearInterpolate>(M);
	Rough->ConstA = 0.85f;
	Rough->ConstB = 0.3f;
	Rough->Alpha.Connect(1, Spec);
	M->GetExpressionCollection().AddExpression(Rough);
	Inputs->Roughness.Connect(0, Rough);
	Inputs->Specular.Connect(1, Spec);
	if (Kind == 3)
	{
		Inputs->OpacityMask.Connect(4, Diffuse);
	}
	else if (Kind == 1)
	{
		// (decals fade with the vertex alpha; many have no alpha in their texture)
		auto* Opacity = NewObject<UMaterialExpressionMultiply>(M);
		Opacity->A.Connect(4, Diffuse);
		Opacity->B.Connect(4, Vertex);
		Add(Opacity);
		Inputs->Opacity.Connect(0, Opacity);
	}
	// Emissive in nits: the game's emissiveMultiplier (0 for everything else) times EmissiveNits (a sign at 4-16 then
	// shines with 150-650 nits, a lit window at 1-4 with 40-160; by day such light is lost in the sun's, at night
	// it is what one sees).
	auto* Glow = NewObject<UMaterialExpressionMultiply>(M);
	Glow->A.Connect(0, UseTint);
	Glow->B.Connect(0, Scalar(TEXT("EmissiveMultiplier"), 0.0f));
	Add(Glow);
	auto* Nits = NewObject<UMaterialExpressionMultiply>(M);
	Nits->A.Connect(0, Glow);
	Nits->B.Connect(0, Scalar(TEXT("EmissiveNits"), 40.0f));
	Add(Nits);
	Inputs->EmissiveColor.Connect(0, Nits);
	if (Kind != 1)
	{
		Wetten(M);
	}
	M->PreEditChange(nullptr);
	M->PostEditChange();
	Package->MarkPackageDirty();
	Dirty.Add(Package);
	return Masters.Add(Kind, M);
}

// The map's lights as seen (the bulbs): unlit, additive, the colour times the intensity per instance (custom data
// 0..2), grown so that a far light stays a pixel wide.
UMaterial* FGTAVMaterials::Corona()
{
	static const TCHAR* PackageName = TEXT("/Game/GTAV/Materials/M_GTAV_Corona");
	if (UMaterial** Found = Masters.Find(kCorona))
	{
		return *Found;
	}
	if (UMaterial* Existing = LoadExisting<UMaterial>(PackageName))
	{
		return Masters.Add(kCorona, Existing);
	}
	UPackage* Package = CreatePackage(PackageName);
	UMaterial* M = NewObject<UMaterial>(Package, TEXT("M_GTAV_Corona"), RF_Public | RF_Standalone);
	M->BlendMode = BLEND_Additive;
	M->SetShadingModel(MSM_Unlit);
	M->bUsedWithInstancedStaticMeshes = true;
	UMaterialEditorOnlyData* Inputs = M->GetEditorOnlyData();
	const auto Add = [&](UMaterialExpression* E) {
		M->GetExpressionCollection().AddExpression(E);
		return E;
	};
	UMaterialExpressionPerInstanceCustomData* Channel[3];
	for (int32 I = 0; I < 3; ++I)
	{
		Channel[I] = NewObject<UMaterialExpressionPerInstanceCustomData>(M);
		Channel[I]->DataIndex = I;
		Add(Channel[I]);
	}
	auto* RG = NewObject<UMaterialExpressionAppendVector>(M);
	RG->A.Connect(0, Channel[0]);
	RG->B.Connect(0, Channel[1]);
	Add(RG);
	auto* RGB = NewObject<UMaterialExpressionAppendVector>(M);
	RGB->A.Connect(0, RG);
	RGB->B.Connect(0, Channel[2]);
	Add(RGB);
	// A bulb of intensity I (candela) seen as a disk of radius R shines with I / (pi R^2) nits: Candela is the intensity
	// of colour 1 (the lights' own), R the sphere's radius as drawn (cm: x 10^4 for m^2).
	auto* Candela = NewObject<UMaterialExpressionScalarParameter>(M);
	Candela->ParameterName = TEXT("Candela");
	Candela->DefaultValue = 4500.0f;
	Add(Candela);
	auto* Glow = NewObject<UMaterialExpressionMultiply>(M);
	Glow->A.Connect(0, RGB);
	Glow->B.Connect(0, Candela);
	Add(Glow);
	auto* PerArea = NewObject<UMaterialExpressionMultiply>(M);
	PerArea->A.Connect(0, Glow);
	PerArea->ConstB = 10000.0f / UE_PI;
	Add(PerArea);
	// Grown so as not to shrink below a pixel (at its distance a pixel spans distance x 2 tan(fov / 2) / width): the
	// same light over the larger disk, dimmer by its area, as a camera sees a light too small to resolve.
	auto* Centre = NewObject<UMaterialExpressionObjectPositionWS>(M);
	Add(Centre);
	auto* Eye = NewObject<UMaterialExpressionCameraPositionWS>(M);
	Add(Eye);
	auto* Far = NewObject<UMaterialExpressionDistance>(M);
	Far->A.Connect(0, Eye);
	Far->B.Connect(0, Centre);
	Add(Far);
	auto* TanHalf = NewObject<UMaterialExpressionViewProperty>(M);
	TanHalf->Property = MEVP_TanHalfFieldOfView;
	Add(TanHalf);
	auto* TanX = NewObject<UMaterialExpressionComponentMask>(M);
	TanX->Input.Connect(0, TanHalf);
	TanX->R = true;
	Add(TanX);
	auto* ViewSize = NewObject<UMaterialExpressionViewProperty>(M);
	ViewSize->Property = MEVP_ViewSize;
	Add(ViewSize);
	auto* Width = NewObject<UMaterialExpressionComponentMask>(M);
	Width->Input.Connect(0, ViewSize);
	Width->R = true;
	Add(Width);
	auto* Span = NewObject<UMaterialExpressionMultiply>(M);
	Span->A.Connect(0, Far);
	Span->B.Connect(0, TanX);
	Add(Span);
	auto* Pixel = NewObject<UMaterialExpressionDivide>(M); // (2 x: the half width's tangent; a radius of one pixel)
	Pixel->A.Connect(0, Span);
	Pixel->B.Connect(0, Width);
	Add(Pixel);
	auto* PixelRadius = NewObject<UMaterialExpressionMultiply>(M);
	PixelRadius->A.Connect(0, Pixel);
	PixelRadius->ConstB = 2.0f;
	Add(PixelRadius);
	auto* Here = NewObject<UMaterialExpressionWorldPosition>(M);
	Add(Here);
	auto* FromCentre = NewObject<UMaterialExpressionSubtract>(M);
	FromCentre->A.Connect(0, Here);
	FromCentre->B.Connect(0, Centre);
	Add(FromCentre);
	auto* Radius = NewObject<UMaterialExpressionDistance>(M); // (the sphere's own, every vertex the same)
	Radius->A.Connect(0, Here);
	Radius->B.Connect(0, Centre);
	Add(Radius);
	auto* SafeRadius = NewObject<UMaterialExpressionMax>(M);
	SafeRadius->A.Connect(0, Radius);
	SafeRadius->ConstB = 0.1f;
	Add(SafeRadius);
	auto* Ratio = NewObject<UMaterialExpressionDivide>(M);
	Ratio->A.Connect(0, PixelRadius);
	Ratio->B.Connect(0, SafeRadius);
	Add(Ratio);
	auto* AtLeast = NewObject<UMaterialExpressionMax>(M);
	AtLeast->A.Connect(0, Ratio);
	AtLeast->ConstB = 1.0f;
	Add(AtLeast);
	auto* Grow = NewObject<UMaterialExpressionSubtract>(M);
	Grow->A.Connect(0, AtLeast);
	Grow->ConstB = 1.0f;
	Add(Grow);
	auto* Offset = NewObject<UMaterialExpressionMultiply>(M);
	Offset->A.Connect(0, FromCentre);
	Offset->B.Connect(0, Grow);
	Add(Offset);
	Inputs->WorldPositionOffset.Connect(0, Offset);
	// The drawn radius from the vertices to the pixels.
	auto* Drawn = NewObject<UMaterialExpressionMultiply>(M);
	Drawn->A.Connect(0, SafeRadius);
	Drawn->B.Connect(0, AtLeast);
	Add(Drawn);
	auto* DrawnArea = NewObject<UMaterialExpressionMultiply>(M);
	DrawnArea->A.Connect(0, Drawn);
	DrawnArea->B.Connect(0, Drawn);
	Add(DrawnArea);
	auto* PerVertex = NewObject<UMaterialExpressionVertexInterpolator>(M);
	PerVertex->Input.Connect(0, DrawnArea);
	Add(PerVertex);
	auto* Spread = NewObject<UMaterialExpressionDivide>(M);
	Spread->A.Connect(0, PerArea);
	Spread->B.Connect(0, PerVertex);
	Add(Spread);
	Inputs->EmissiveColor.Connect(0, Spread);
	M->PreEditChange(nullptr);
	M->PostEditChange();
	Package->MarkPackageDirty();
	Dirty.Add(Package);
	return Masters.Add(kCorona, M);
}

// Light given off by glowing gas and the like (the game's emissive_additive* and decal_emissive*_only shaders, neon
// tubes as flat cards, one-sided as the game draws them: a sign's back has cards of its own): added to what is behind,
// unlit, the diffuse's colour times its alpha and the vertex alpha, times the material's emissiveMultiplier in
// EmissiveNits.
UMaterial* FGTAVMaterials::Glow()
{
	static const TCHAR* PackageName = TEXT("/Game/GTAV/Materials/M_GTAV_Glow");
	if (UMaterial** Found = Masters.Find(kGlow))
	{
		return *Found;
	}
	if (UMaterial* Existing = LoadExisting<UMaterial>(PackageName))
	{
		return Masters.Add(kGlow, Existing);
	}
	UPackage* Package = CreatePackage(PackageName);
	UMaterial* M = NewObject<UMaterial>(Package, TEXT("M_GTAV_Glow"), RF_Public | RF_Standalone);
	M->BlendMode = BLEND_Additive;
	M->SetShadingModel(MSM_Unlit);
	M->bUsedWithNanite = true;
	M->bUsedWithInstancedStaticMeshes = true;
	M->bUseTranslucencyVertexFog = true; // ("Apply Fogging": the air between dims it)
	M->bComputeFogPerPixel = true;
	UMaterialEditorOnlyData* Inputs = M->GetEditorOnlyData();
	const auto Add = [&](UMaterialExpression* E) {
		M->GetExpressionCollection().AddExpression(E);
		return E;
	};
	auto* Diffuse = NewObject<UMaterialExpressionTextureSampleParameter2D>(M);
	Diffuse->ParameterName = TEXT("DiffuseTex");
	Diffuse->Texture = EngineTexture(TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
	Diffuse->SamplerType = SAMPLERTYPE_Color;
	Add(Diffuse);
	auto* Vertex = NewObject<UMaterialExpressionVertexColor>(M);
	Add(Vertex);
	auto* Alpha = NewObject<UMaterialExpressionMultiply>(M);
	Alpha->A.Connect(4, Diffuse);
	Alpha->B.Connect(4, Vertex);
	Add(Alpha);
	auto* Lit = NewObject<UMaterialExpressionMultiply>(M);
	Lit->A.Connect(1, Diffuse); // (RGB)
	Lit->B.Connect(0, Alpha);
	Add(Lit);
	auto* Multiplier = NewObject<UMaterialExpressionScalarParameter>(M);
	Multiplier->ParameterName = TEXT("EmissiveMultiplier");
	Multiplier->DefaultValue = 1.0f;
	Add(Multiplier);
	auto* Nits = NewObject<UMaterialExpressionScalarParameter>(M);
	Nits->ParameterName = TEXT("EmissiveNits");
	Nits->DefaultValue = 40.0f;
	Add(Nits);
	auto* Scale = NewObject<UMaterialExpressionMultiply>(M);
	Scale->A.Connect(0, Multiplier);
	Scale->B.Connect(0, Nits);
	Add(Scale);
	auto* Out = NewObject<UMaterialExpressionMultiply>(M);
	Out->A.Connect(0, Lit);
	Out->B.Connect(0, Scale);
	Add(Out);
	Inputs->EmissiveColor.Connect(0, Out);
	M->PreEditChange(nullptr);
	M->PostEditChange();
	Package->MarkPackageDirty();
	Dirty.Add(Package);
	return Masters.Add(kGlow, M);
}

// The moon (M_GTAV_Moon): a sphere far off in the sky, unlit, its face the game's moon texture (a full moon's disc)
// seen as from the camera, lit by the sun as a sphere is (its phase): albedo x 100,000 lux / pi x cos(sun), about
// 2,500 nits full (the moon's surface; MoonTex's mean scaled to the moon's albedo, 0.12). The sun's direction from
// the collection (SunX/Y/Z, towards it).
UMaterial* FGTAVMaterials::Moon()
{
	static const TCHAR* PackageName = TEXT("/Game/GTAV/Materials/M_GTAV_Moon");
	if (UMaterial** Found = Masters.Find(kMoon))
	{
		return *Found;
	}
	if (UMaterial* Existing = LoadExisting<UMaterial>(PackageName))
	{
		return Masters.Add(kMoon, Existing);
	}
	UPackage* Package = CreatePackage(PackageName);
	UMaterial* M = NewObject<UMaterial>(Package, TEXT("M_GTAV_Moon"), RF_Public | RF_Standalone);
	M->SetShadingModel(MSM_Unlit);
	UMaterialEditorOnlyData* Inputs = M->GetEditorOnlyData();
	const auto Add = [&](UMaterialExpression* E) {
		M->GetExpressionCollection().AddExpression(E);
		return E;
	};
	UMaterialParameterCollection* C = Collection();
	const auto Global = [&](const TCHAR* Name) {
		auto* E = NewObject<UMaterialExpressionCollectionParameter>(M);
		E->Collection = C;
		E->ParameterName = Name;
		E->ParameterId = C->GetParameterId(Name);
		return Add(E);
	};
	auto* Here = Add(NewObject<UMaterialExpressionWorldPosition>(M));
	auto* Centre = Add(NewObject<UMaterialExpressionObjectPositionWS>(M));
	auto* Eye = Add(NewObject<UMaterialExpressionCameraPositionWS>(M));
	const auto Custom = [&](const TCHAR* Description, ECustomMaterialOutputType Type, const TCHAR* Code,
							std::initializer_list<TPair<const TCHAR*, UMaterialExpression*>> Ins) {
		auto* E = NewObject<UMaterialExpressionCustom>(M);
		E->Description = Description;
		E->OutputType = Type;
		E->Code = Code;
		E->Inputs.Reset();
		for (const TPair<const TCHAR*, UMaterialExpression*>& I : Ins)
		{
			FCustomInput& In = E->Inputs.AddDefaulted_GetRef();
			In.InputName = I.Key;
			In.Input.Connect(0, I.Value);
		}
		return Add(E);
	};
	auto* Uv = Custom(TEXT("Moon face"), CMOT_Float2, TEXT(R"HLSL(// The face as seen from the camera, upright (the world's up), on the texture's disc.
float3 F = normalize(Centre - Eye);
float3 P = normalize(Here - Centre);
float3 R = normalize(cross(float3(0, 0, 1), F));
float3 U = cross(F, R);
return float2(dot(P, R), dot(P, U)) * float2(0.5, -0.5) + 0.5;
)HLSL"), {{TEXT("Here"), Here}, {TEXT("Centre"), Centre}, {TEXT("Eye"), Eye}});
	auto* Face = NewObject<UMaterialExpressionTextureSampleParameter2D>(M);
	Face->ParameterName = TEXT("MoonTex");
	Face->Texture = EngineTexture(TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
	Face->SamplerType = SAMPLERTYPE_Color;
	Face->Coordinates.Connect(0, Uv);
	Add(Face);
	auto* Lit = Custom(TEXT("Moon light"), CMOT_Float3, TEXT(R"HLSL(// Sunlit as a sphere: albedo (the texture's mean, about 0.24 linear, as the moon's 0.12) x E / pi x cos.
float3 P = normalize(Here - Centre);
float3 S = normalize(float3(SunX, SunY, SunZ));
return Face * (0.12 / 0.24) * (100000.0 / 3.14159265) * saturate(dot(P, S));
)HLSL"), {{TEXT("Here"), Here}, {TEXT("Centre"), Centre}, {TEXT("Face"), Face}, {TEXT("SunX"), Global(TEXT("SunX"))},
		 {TEXT("SunY"), Global(TEXT("SunY"))}, {TEXT("SunZ"), Global(TEXT("SunZ"))}});
	Inputs->EmissiveColor.Connect(0, Lit);
	M->PreEditChange(nullptr);
	M->PostEditChange();
	Package->MarkPackageDirty();
	Dirty.Add(Package);
	return Masters.Add(kMoon, M);
}

// The stars (M_GTAV_Stars): the game's starfield over a sphere far off around the camera (turned by the hour about the
// celestial pole), added to the sky: a texel's value t as a star of magnitude 6 - 7.5 t (the brightest about -1),
// its light over the texel's solid angle (nits). The city's glowing sky drowns all but the brightest, as it does.
UMaterial* FGTAVMaterials::Stars()
{
	static const TCHAR* PackageName = TEXT("/Game/GTAV/Materials/M_GTAV_Stars");
	if (UMaterial** Found = Masters.Find(kStars))
	{
		return *Found;
	}
	if (UMaterial* Existing = LoadExisting<UMaterial>(PackageName))
	{
		return Masters.Add(kStars, Existing);
	}
	UPackage* Package = CreatePackage(PackageName);
	UMaterial* M = NewObject<UMaterial>(Package, TEXT("M_GTAV_Stars"), RF_Public | RF_Standalone);
	M->BlendMode = BLEND_Additive;
	M->SetShadingModel(MSM_Unlit);
	M->TwoSided = true;
	M->bUseTranslucencyVertexFog = true; // (the air between: haze and the horizon's extinction)
	M->bComputeFogPerPixel = true;
	UMaterialEditorOnlyData* Inputs = M->GetEditorOnlyData();
	const auto Add = [&](UMaterialExpression* E) {
		M->GetExpressionCollection().AddExpression(E);
		return E;
	};
	auto* Local = Add(NewObject<UMaterialExpressionLocalPosition>(M));
	auto* Uv = NewObject<UMaterialExpressionCustom>(M);
	Uv->Description = TEXT("Star map");
	Uv->OutputType = CMOT_Float2;
	Uv->Code = TEXT(R"HLSL(// Longitude, latitude over the texture, 12 x 6 times (a texel about 0.06 degrees: a star a pixel or two).
float3 D = normalize(Local);
return float2(atan2(D.y, D.x) / 6.2831853 * 12.0, acos(clamp(D.z, -1, 1)) / 3.14159265 * 6.0);
)HLSL");
	Uv->Inputs.Reset();
	{
		FCustomInput& In = Uv->Inputs.AddDefaulted_GetRef();
		In.InputName = TEXT("Local");
		In.Input.Connect(0, Local);
	}
	Add(Uv);
	auto* Field = NewObject<UMaterialExpressionTextureSampleParameter2D>(M);
	Field->ParameterName = TEXT("StarTex");
	Field->Texture = EngineTexture(TEXT("/Engine/EngineResources/Black.Black"));
	Field->SamplerType = SAMPLERTYPE_Color;
	Field->MipValueMode = TMVM_MipLevel; // (the longitude's seam would pick the smallest mip)
	Field->ConstMipValue = 0;
	Field->Coordinates.Connect(0, Uv);
	Add(Field);
	auto* Light = NewObject<UMaterialExpressionCustom>(M);
	Light->Description = TEXT("Star light");
	Light->OutputType = CMOT_Float3;
	Light->Code = TEXT(R"HLSL(float t = max(Star.r, max(Star.g, Star.b));
if (t < 0.04) return 0;
float m = 6.0 - 7.5 * t;                        // magnitude
float E = pow(10.0, -0.4 * (m + 14.18));        // lux
return Star / t * E / 1.05e-6;                  // over the texel's solid angle (sr)
)HLSL");
	Light->Inputs.Reset();
	{
		FCustomInput& In = Light->Inputs.AddDefaulted_GetRef();
		In.InputName = TEXT("Star");
		In.Input.Connect(0, Field);
	}
	Add(Light);
	Inputs->EmissiveColor.Connect(0, Light);
	M->PreEditChange(nullptr);
	M->PostEditChange();
	Package->MarkPackageDirty();
	Dirty.Add(Package);
	return Masters.Add(kStars, M);
}

// Foam on the shore (M_GTAV_Foam): white water (foam reflects some 0.6 of the light, diffusely) where the game's foam
// map (waterfoam: its density in green) has it, over the world (8 m across, drifting), faded by the mesh's vertex
// alpha as the game fades it towards the sand.
UMaterial* FGTAVMaterials::Foam()
{
	static const TCHAR* PackageName = TEXT("/Game/GTAV/Materials/M_GTAV_Foam");
	if (UMaterial** Found = Masters.Find(kFoam))
	{
		return *Found;
	}
	if (UMaterial* Existing = LoadExisting<UMaterial>(PackageName))
	{
		return Masters.Add(kFoam, Existing);
	}
	UPackage* Package = CreatePackage(PackageName);
	UMaterial* M = NewObject<UMaterial>(Package, TEXT("M_GTAV_Foam"), RF_Public | RF_Standalone);
	M->BlendMode = BLEND_Translucent;
	M->TranslucencyLightingMode = TLM_SurfacePerPixelLighting;
	M->bUsedWithNanite = true;
	M->bUseTranslucencyVertexFog = true;
	M->bComputeFogPerPixel = true;
	UMaterialEditorOnlyData* Inputs = M->GetEditorOnlyData();
	const auto Add = [&](UMaterialExpression* E) {
		M->GetExpressionCollection().AddExpression(E);
		return E;
	};
	auto* World = Add(NewObject<UMaterialExpressionWorldPosition>(M));
	auto* XY = NewObject<UMaterialExpressionComponentMask>(M);
	XY->Input.Connect(0, World);
	XY->R = XY->G = true;
	Add(XY);
	auto* Scale = NewObject<UMaterialExpressionDivide>(M);
	Scale->A.Connect(0, XY);
	Scale->ConstB = 800.0f;
	Add(Scale);
	auto* Drift = NewObject<UMaterialExpressionPanner>(M);
	Drift->Coordinate.Connect(0, Scale);
	Drift->SpeedX = 0.004f;
	Drift->SpeedY = 0.007f;
	Add(Drift);
	auto* Map = NewObject<UMaterialExpressionTextureSampleParameter2D>(M);
	Map->ParameterName = TEXT("FoamTex");
	Map->Texture = BlackMask();
	Map->SamplerType = SAMPLERTYPE_Masks;
	Map->Coordinates.Connect(0, Drift);
	Add(Map);
	auto* Vertex = Add(NewObject<UMaterialExpressionVertexColor>(M));
	auto* Density = NewObject<UMaterialExpressionMultiply>(M);
	Density->A.Connect(2, Map); // (green)
	Density->B.Connect(4, Vertex); // (alpha)
	Add(Density);
	Inputs->Opacity.Connect(0, Density);
	auto* White = NewObject<UMaterialExpressionConstant3Vector>(M);
	White->Constant = FLinearColor(0.6f, 0.6f, 0.6f);
	Add(White);
	Inputs->BaseColor.Connect(0, White);
	auto* Rough = NewObject<UMaterialExpressionConstant>(M);
	Rough->R = 0.8f;
	Add(Rough);
	Inputs->Roughness.Connect(0, Rough);
	M->PreEditChange(nullptr);
	M->PostEditChange();
	Package->MarkPackageDirty();
	Dirty.Add(Package);
	return Masters.Add(kFoam, M);
}

// Falling rain (M_GTAV_Rain): streaks on cylinders around the camera, lit by the scene's lights (rain shows under a
// street light), the layer's Radius (m) per instance.
UMaterial* FGTAVMaterials::Rain()
{
	static const TCHAR* PackageName = TEXT("/Game/GTAV/Materials/M_GTAV_Rain");
	if (UMaterial** Found = Masters.Find(kRain))
	{
		return *Found;
	}
	if (UMaterial* Existing = LoadExisting<UMaterial>(PackageName))
	{
		return Masters.Add(kRain, Existing);
	}
	UPackage* Package = CreatePackage(PackageName);
	UMaterial* M = NewObject<UMaterial>(Package, TEXT("M_GTAV_Rain"), RF_Public | RF_Standalone);
	M->BlendMode = BLEND_Translucent;
	M->TwoSided = true;
	M->TranslucencyLightingMode = TLM_VolumetricNonDirectional;
	UMaterialEditorOnlyData* Inputs = M->GetEditorOnlyData();
	const auto Add = [&](UMaterialExpression* E) {
		M->GetExpressionCollection().AddExpression(E);
		return E;
	};
	UMaterialParameterCollection* C = Collection();
	const auto Global = [&](const TCHAR* Name) {
		auto* E = NewObject<UMaterialExpressionCollectionParameter>(M);
		E->Collection = C;
		E->ParameterName = Name;
		E->ParameterId = C->GetParameterId(Name);
		return Add(E);
	};
	auto* Radius = NewObject<UMaterialExpressionScalarParameter>(M);
	Radius->ParameterName = TEXT("Radius");
	Radius->DefaultValue = 5.0f;
	Add(Radius);
	auto* Streaks = NewObject<UMaterialExpressionCustom>(M);
	Streaks->OutputType = CMOT_Float1;
	Streaks->Description = TEXT("Rain");
	Streaks->Code = TEXT(R"HLSL(// Rain on a cylinder around the camera: streaks of drops falling at their terminal velocity (about 6.5 m/s for the
// millimetre drops of moderate rain), slanted by the wind, as many as the rain rate gives (Marshall-Palmer: some
// thousand drops in a cubic metre at 8 mm/h; a share of the cells of this layer).
if (RainRate <= 0.01) return 0;
float3 d = (Pos - Cam) * 0.01;
float v = 6.5;
float2 wind = float2(WindX, WindY) * WindSpeed * 0.5;
float ang = atan2(d.y, d.x);
float2 side = float2(-sin(ang), cos(ang));
float u = ang * Radius + dot(wind, side) * (d.z / v);
float h = d.z + v * Time;
float cellW = 0.02 * Radius;
float cellH = 0.5;
float2 g = float2(u / cellW, h / cellH);
float2 cell = floor(g);
float rnd = frac(sin(dot(cell, float2(12.9898, 78.233))) * 43758.5453);
if (rnd > saturate(RainRate / 25.0) * 0.6 + 0.05) return 0;
float2 f = frac(g);
float x = frac(f.x + rnd * 7.31);
float y = frac(f.y + rnd * 3.17);
// (a streak: what a 1/60 s exposure makes of a falling drop, about 10 cm long)
float len = 0.22;
float across = saturate(1 - abs(x - 0.5) * 14);
float along = smoothstep(0, 0.05, y) * (1 - smoothstep(len - 0.06, len, y));
// (no rain right at the camera or far above and below it)
float fade = saturate(1 - abs(d.z) / 12.0);
return across * along * fade;
)HLSL");
	Streaks->Inputs.Reset();
	for (const TPair<const TCHAR*, UMaterialExpression*>& I : {TPair<const TCHAR*, UMaterialExpression*>(TEXT("Pos"), Add(NewObject<UMaterialExpressionWorldPosition>(M))),
			 {TEXT("Cam"), Add(NewObject<UMaterialExpressionCameraPositionWS>(M))}, {TEXT("Time"), Add(NewObject<UMaterialExpressionTime>(M))},
			 {TEXT("RainRate"), Global(TEXT("RainRate"))}, {TEXT("WindX"), Global(TEXT("WindX"))}, {TEXT("WindY"), Global(TEXT("WindY"))},
			 {TEXT("WindSpeed"), Global(TEXT("WindSpeed"))}, {TEXT("Radius"), Radius}})
	{
		FCustomInput& In = Streaks->Inputs.AddDefaulted_GetRef();
		In.InputName = I.Key;
		In.Input.Connect(0, I.Value);
	}
	Add(Streaks);
	auto* Water = NewObject<UMaterialExpressionConstant3Vector>(M);
	Water->Constant = FLinearColor(0.75f, 0.78f, 0.82f);
	Add(Water);
	Inputs->BaseColor.Connect(0, Water);
	auto* Opacity = NewObject<UMaterialExpressionMultiply>(M);
	Opacity->A.Connect(0, Streaks);
	Opacity->ConstB = 0.18f;
	Add(Opacity);
	Inputs->Opacity.Connect(0, Opacity);
	M->PreEditChange(nullptr);
	M->PostEditChange();
	Package->MarkPackageDirty();
	Dirty.Add(Package);
	return Masters.Add(kRain, M);
}

// The renderer's global values for every material: Wetness (0..1, the weather's), the wind (WindX, WindY: the way it
// blows in Unreal's space; WindSpeed m/s), RainRate (mm/h).
UMaterialParameterCollection* FGTAVMaterials::Collection()
{
	static const TCHAR* PackageName = TEXT("/Game/GTAV/Materials/MPC_GTAV");
	if (GlobalParameters)
	{
		return GlobalParameters;
	}
	if (UMaterialParameterCollection* Existing = LoadExisting<UMaterialParameterCollection>(PackageName))
	{
		return GlobalParameters = Existing;
	}
	UPackage* Package = CreatePackage(PackageName);
	UMaterialParameterCollection* C = NewObject<UMaterialParameterCollection>(Package, TEXT("MPC_GTAV"), RF_Public | RF_Standalone);
	for (const TPair<const TCHAR*, float>& P : {TPair<const TCHAR*, float>(TEXT("Wetness"), 0.0f), {TEXT("WindX"), 1.0f}, {TEXT("WindY"), 0.0f},
			 {TEXT("WindSpeed"), 4.0f}, {TEXT("RainRate"), 0.0f}, {TEXT("SunX"), 0.0f}, {TEXT("SunY"), 0.5f}, {TEXT("SunZ"), 0.866f}})
	{
		FCollectionScalarParameter Parameter;
		Parameter.ParameterName = P.Key;
		Parameter.DefaultValue = P.Value;
		C->ScalarParameters.Add(Parameter);
	}
	C->PostEditChange();
	Package->MarkPackageDirty();
	Dirty.Add(Package);
	return GlobalParameters = C;
}

// Wet ground in the rain: surfaces that face up get darker and glossy with the collection's Wetness.
void FGTAVMaterials::Wetten(UMaterial* M)
{
	UMaterialEditorOnlyData* Inputs = M->GetEditorOnlyData();
	const auto Add = [&](UMaterialExpression* E) {
		M->GetExpressionCollection().AddExpression(E);
		return E;
	};
	UMaterialParameterCollection* C = Collection();
	auto* Wetness = NewObject<UMaterialExpressionCollectionParameter>(M);
	Wetness->Collection = C;
	Wetness->ParameterName = TEXT("Wetness");
	Wetness->ParameterId = C->GetParameterId(TEXT("Wetness"));
	Add(Wetness);
	auto* Normal = NewObject<UMaterialExpressionVertexNormalWS>(M);
	Add(Normal);
	auto* Up = NewObject<UMaterialExpressionComponentMask>(M);
	Up->Input.Connect(0, Normal);
	Up->B = true;
	Add(Up);
	auto* Steep = NewObject<UMaterialExpressionMultiply>(M); // (flat ground 1, walls 0)
	Steep->A.Connect(0, Up);
	Steep->ConstB = 2.5f;
	Add(Steep);
	auto* Shifted = NewObject<UMaterialExpressionSubtract>(M);
	Shifted->A.Connect(0, Steep);
	Shifted->ConstB = 1.2f;
	Add(Shifted);
	auto* Facing = NewObject<UMaterialExpressionSaturate>(M);
	Facing->Input.Connect(0, Shifted);
	Add(Facing);
	auto* Wet = NewObject<UMaterialExpressionMultiply>(M);
	Wet->A.Connect(0, Wetness);
	Wet->B.Connect(0, Facing);
	Add(Wet);
	auto* Where = NewObject<UMaterialExpressionWorldPosition>(M);
	Add(Where);
	auto* Puddle = NewObject<UMaterialExpressionCustom>(M);
	Puddle->OutputType = CMOT_Float1;
	Puddle->Description = TEXT("Puddles");
	Puddle->Code = TEXT(R"HLSL(// Standing water: where a two-octave value noise over the ground (features of a few metres) dips under a level
// that rises with the wetness, on surfaces that face up.
if (Wet < 0.05) return 0;
float2 p = Pos.xy * 0.01 / 3.0;
float n = 0, amp = 0.65, norm = 0;
[unroll] for (int o = 0; o < 2; o++)
{
    float2 i = floor(p), f = frac(p);
    float2 s = f * f * (3 - 2 * f);
    float a = frac(sin(dot(i, float2(127.1, 311.7))) * 43758.5453);
    float b = frac(sin(dot(i + float2(1, 0), float2(127.1, 311.7))) * 43758.5453);
    float c = frac(sin(dot(i + float2(0, 1), float2(127.1, 311.7))) * 43758.5453);
    float e = frac(sin(dot(i + float2(1, 1), float2(127.1, 311.7))) * 43758.5453);
    n += amp * lerp(lerp(a, b, s.x), lerp(c, e, s.x), s.y);
    norm += amp;
    p *= 2.7;
    amp *= 0.5;
}
n /= norm;
return saturate((Wet * 0.3 + 0.1 - n) * 12) * Facing;
)HLSL");
	Puddle->Inputs.Reset();
	for (const TPair<const TCHAR*, UMaterialExpression*>& I : {TPair<const TCHAR*, UMaterialExpression*>(TEXT("Pos"), Where), {TEXT("Wet"), Wetness},
			 {TEXT("Facing"), Facing}})
	{
		FCustomInput& In = Puddle->Inputs.AddDefaulted_GetRef();
		In.InputName = I.Key;
		In.Input.Connect(0, I.Value);
	}
	Add(Puddle);
	FExpressionInput Color = Inputs->BaseColor, Rough = Inputs->Roughness;
	auto* Darker = NewObject<UMaterialExpressionMultiply>(M);
	Darker->A = Color;
	Darker->ConstB = 0.55f;
	Add(Darker);
	auto* WetColor = NewObject<UMaterialExpressionLinearInterpolate>(M);
	WetColor->A = Color;
	WetColor->B.Connect(0, Darker);
	WetColor->Alpha.Connect(0, Wet);
	Add(WetColor);
	// (in a puddle: darker still, a mirror)
	auto* Deeper = NewObject<UMaterialExpressionMultiply>(M);
	Deeper->A.Connect(0, WetColor);
	Deeper->ConstB = 0.7f;
	Add(Deeper);
	auto* PuddleColor = NewObject<UMaterialExpressionLinearInterpolate>(M);
	PuddleColor->A.Connect(0, WetColor);
	PuddleColor->B.Connect(0, Deeper);
	PuddleColor->Alpha.Connect(0, Puddle);
	Add(PuddleColor);
	Inputs->BaseColor.Connect(0, PuddleColor);
	auto* WetRough = NewObject<UMaterialExpressionLinearInterpolate>(M);
	WetRough->A = Rough;
	WetRough->ConstB = 0.08f;
	WetRough->Alpha.Connect(0, Wet);
	Add(WetRough);
	auto* PuddleRough = NewObject<UMaterialExpressionLinearInterpolate>(M);
	PuddleRough->A.Connect(0, WetRough);
	PuddleRough->ConstB = 0.02f;
	PuddleRough->Alpha.Connect(0, Puddle);
	Add(PuddleRough);
	Inputs->Roughness.Connect(0, PuddleRough);
}

UTexture2D* FGTAVMaterials::Texture(const FString& Path, bool bLinear, bool bNormalMap, bool bPalette)
{
	if (UTexture2D** Found = Textures.Find(Path))
	{
		return *Found;
	}
	FString Dictionary, Name;
	Path.Split(TEXT("/"), &Dictionary, &Name);
	const FString PackageName = FString::Printf(TEXT("/Game/GTAV/Textures/%s/%s"), *GTAV::AssetName(Dictionary), *GTAV::AssetName(Name));
	if (UTexture2D* Existing = LoadExisting<UTexture2D>(PackageName))
	{
		return Textures.Add(Path, Existing);
	}
	TArray<uint8> Data;
	if (!FFileHelper::LoadFileToArray(Data, *FPaths::Combine(ExportRoot, TEXT("textures"), Dictionary, Name + TEXT(".dds"))))
	{
		++TexturesMissing;
		return Textures.Add(Path, nullptr);
	}
	int32 Width = 0, Height = 0;
	TArray<FColor> Pixels;
	FString Error;
	if (!GTAVDds::Decode(Data, bNormalMap, Width, Height, Pixels, Error))
	{
		UE_LOG(LogGTAVMaterials, Warning, TEXT("%s: %s"), *Path, *Error);
		++TexturesMissing;
		return Textures.Add(Path, nullptr);
	}
	UPackage* Package = CreatePackage(*PackageName);
	UTexture2D* T = NewObject<UTexture2D>(Package, FName(*GTAV::AssetName(Name)), RF_Public | RF_Standalone);
	T->Source.Init(Width, Height, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
	T->MipGenSettings = TMGS_FromTextureGroup;
	if (bPalette)
	{
		// (a lookup table: exact texels, no compression, no mips)
		T->CompressionSettings = TC_VectorDisplacementmap;
		T->Filter = TF_Nearest;
		T->MipGenSettings = TMGS_NoMipmaps;
	}
	else if (bNormalMap)
	{
		T->CompressionSettings = TC_Normalmap;
		T->SRGB = false;
	}
	else if (bLinear)
	{
		T->CompressionSettings = TC_Masks;
		T->SRGB = false;
	}
	T->PostEditChange();
	Package->MarkPackageDirty();
	Dirty.Add(Package);
	++TexturesImported;
	return Textures.Add(Path, T);
}

UMaterialInterface* FGTAVMaterials::Get(const TSharedPtr<FJsonObject>& Material, bool& bOutTranslucent, bool& bOutTerrain)
{
	const int32 Bucket = Material->GetIntegerField(TEXT("bucket"));
	const TSharedPtr<FJsonObject>* TexturesJson = nullptr;
	Material->TryGetObjectField(TEXT("textures"), TexturesJson);
	bOutTerrain = TexturesJson && (*TexturesJson)->HasField(TEXT("DiffuseTexture_layer0"));
	// 0 opaque, 1 translucent, 2 decal (translucent for now), 3 cutout, 6 water; anything else is drawn as opaque.
	const int32 Kind = bOutTerrain ? kTerrain : Bucket == 6 ? kWater : Bucket == 1 || Bucket == 2 ? 1 : Bucket == 3 ? 3 : 0;
	bOutTranslucent = Kind == 1 || Kind == kWater;
	// The textures this master uses.
	TArray<FString> Parameters = {TEXT("DiffuseTex"), TEXT("BumpTex"), TEXT("SpecularTex"), TEXT("TintPaletteTex")};
	if (bOutTerrain)
	{
		Parameters.Reset();
		for (int32 I = 0; I < 4; ++I)
		{
			Parameters.Add(FString::Printf(TEXT("DiffuseTexture_layer%d"), I));
			Parameters.Add(FString::Printf(TEXT("BumpTexture_layer%d"), I));
		}
	}
	TMap<FName, FString> Used;
	for (const FString& Parameter : Parameters)
	{
		FString Path;
		if (TexturesJson && (*TexturesJson)->TryGetStringField(Parameter, Path) && !Path.StartsWith(TEXT("?")))
		{
			Used.Add(FName(*Parameter), Path);
		}
	}
	// The palette row (tintPaletteSelector).
	float TintRow = 0;
	const TSharedPtr<FJsonObject>* ConstantsJson = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Selector = nullptr;
	if (Material->TryGetObjectField(TEXT("constants"), ConstantsJson) && (*ConstantsJson)->TryGetArrayField(TEXT("tintPaletteSelector"), Selector) && Selector->Num())
	{
		TintRow = (*Selector)[0]->AsNumber();
	}
	float Emissive = 0;
	FLinearColor WaterColor(0.65f, 0.76f, 0.78f);
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (ConstantsJson && (*ConstantsJson)->TryGetArrayField(TEXT("emissiveMultiplier"), Values) && Values->Num())
	{
		Emissive = (*Values)[0]->AsNumber();
	}
	if (ConstantsJson && (*ConstantsJson)->TryGetArrayField(TEXT("e9c969fb"), Values) && Values->Num() >= 3) // (the water's colour)
	{
		WaterColor = FLinearColor((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
	}
	FString Key = FString::Printf(TEXT("%d|%g|%g|%s"), Kind, TintRow, Emissive, Kind == kWater ? *WaterColor.ToString() : TEXT(""));
	for (const auto& [Parameter, Path] : Used)
	{
		Key += TEXT("|") + Parameter.ToString() + TEXT("=") + Path;
	}
	if (UMaterialInterface** Found = Instances.Find(Key))
	{
		return *Found;
	}
	UMaterial* Parent = Master(Kind);
	const FString PackageName = FString::Printf(TEXT("/Game/GTAV/Materials/Instances/MI_%08X"), GetTypeHash(Key));
	if (UMaterialInstanceConstant* Existing = LoadExisting<UMaterialInstanceConstant>(PackageName))
	{
		return Instances.Add(Key, Existing);
	}
	UPackage* Package = CreatePackage(*PackageName);
	UMaterialInstanceConstant* MI = NewObject<UMaterialInstanceConstant>(Package, FName(*FPaths::GetBaseFilename(PackageName)), RF_Public | RF_Standalone);
	MI->SetParentEditorOnly(Parent);
	for (const auto& [Parameter, Path] : Used)
	{
		const FString Name = Parameter.ToString();
		const bool bNormal = Name.StartsWith(TEXT("Bump")), bPalette = Name == TEXT("TintPaletteTex");
		UTexture2D* T = Texture(Path, !Name.StartsWith(TEXT("Diffuse")) && !bPalette, bNormal, bPalette);
		if (!T)
		{
			continue;
		}
		MI->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(Parameter), T);
		if (bPalette)
		{
			MI->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("UseTint")), 1.0f);
			MI->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("TintPaletteWidth")), T->Source.GetSizeX());
			MI->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("TintPaletteHeight")), T->Source.GetSizeY());
			MI->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("TintRow")), TintRow);
		}
	}
	if (Emissive > 0)
	{
		MI->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("EmissiveMultiplier")), Emissive);
	}
	if (Kind == kWater)
	{
		MI->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("WaterColor")), WaterColor);
	}
	MI->PostEditChange();
	Package->MarkPackageDirty();
	Dirty.Add(Package);
	return Instances.Add(Key, MI);
}
