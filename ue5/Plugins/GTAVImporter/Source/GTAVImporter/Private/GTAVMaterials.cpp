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
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionSceneDepth.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
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
		const auto Ripples = [&](float Size, float SpeedX, float SpeedY, int32 Y) {
			auto* Scale = NewObject<UMaterialExpressionDivide>(M);
			Scale->A.Connect(0, XY);
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
		auto* Sum = NewObject<UMaterialExpressionAdd>(M);
		Sum->A.Connect(0, Ripples(bSea ? 900.0f : 200.0f, 0.012f, 0.007f, -200));
		Sum->B.Connect(0, Ripples(bSea ? 2300.0f : 450.0f, -0.006f, 0.010f, 100));
		Add(Sum);
		auto* Flatter = NewObject<UMaterialExpressionMultiply>(M); // (xy halved, z kept: gentler waves)
		Flatter->A.Connect(0, Sum);
		auto* Weights = NewObject<UMaterialExpressionConstant3Vector>(M);
		Weights->Constant = FLinearColor(0.5f, 0.5f, 1.0f);
		Add(Weights);
		Flatter->B.Connect(0, Weights);
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
		auto* Specular = NewObject<UMaterialExpressionConstant>(M);
		Specular->R = 1.0f;
		Add(Specular);
		Inputs->Specular.Connect(0, Specular);
		auto* Rough = NewObject<UMaterialExpressionConstant>(M);
		Rough->R = 0.04f;
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
		auto* Sigma = NewObject<UMaterialExpressionConstant>(M);
		Sigma->R = bSea ? 0.35f : 0.6f;
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
		// Four layers blended by the vertex colour (the mesh carries the game's second colour set there):
		// green between layers 0 / 1 and 2 / 3, blue between those two.
		auto* Weights = NewObject<UMaterialExpressionVertexColor>(M);
		M->GetExpressionCollection().AddExpression(Weights);
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
				Pair[I]->Alpha.Connect(2, Weights);
				M->GetExpressionCollection().AddExpression(Pair[I]);
			}
			auto* Out = NewObject<UMaterialExpressionLinearInterpolate>(M);
			Out->A.Connect(0, Pair[0]);
			Out->B.Connect(0, Pair[1]);
			Out->Alpha.Connect(3, Weights);
			M->GetExpressionCollection().AddExpression(Out);
			return Out;
		};
		Inputs->BaseColor.Connect(0, Blend(TEXT("DiffuseTexture_layer"), White, SAMPLERTYPE_Color, -1200));
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
	// Tint (the game's *_tnt shaders): a grey diffuse times the palette's colour, column = vertex blue, row =
	// TintRow (the material's tintPaletteSelector).
	auto* Palette = Sample(TEXT("TintPaletteTex"), White, SAMPLERTYPE_Color, -900, -500);
	auto* Width = Scalar(TEXT("TintPaletteWidth"), 256.0f);
	auto* Height = Scalar(TEXT("TintPaletteHeight"), 4.0f);
	auto* Row = Scalar(TEXT("TintRow"), 0.0f);
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

// The map's distant lights at night: unlit, additive, the colour per instance (custom data 0..2), grown with the
// distance so that a far light stays a few pixels wide.
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
	auto* Brightness = NewObject<UMaterialExpressionScalarParameter>(M);
	Brightness->ParameterName = TEXT("Brightness");
	Brightness->DefaultValue = 20000.0f; // nits: a lamp's glowing housing
	Add(Brightness);
	auto* Glow = NewObject<UMaterialExpressionMultiply>(M);
	Glow->A.Connect(0, RGB);
	Glow->B.Connect(0, Brightness);
	Add(Glow);
	// Grown with the distance (below): the glow spreads over the larger sphere, dimmer in step with its size.
	auto* Spread = NewObject<UMaterialExpressionDivide>(M);
	Spread->A.Connect(0, Glow);
	Add(Spread);
	Inputs->EmissiveColor.Connect(0, Spread);
	// Grown with the distance: (vertex - centre) * (max(distance / 30000, 1) - 1) (the mesh's radius is 30 cm).
	auto* Centre = NewObject<UMaterialExpressionObjectPositionWS>(M);
	Add(Centre);
	auto* Eye = NewObject<UMaterialExpressionCameraPositionWS>(M);
	Add(Eye);
	auto* Far = NewObject<UMaterialExpressionDistance>(M);
	Far->A.Connect(0, Eye);
	Far->B.Connect(0, Centre);
	Add(Far);
	auto* Scaled = NewObject<UMaterialExpressionMultiply>(M);
	Scaled->A.Connect(0, Far);
	Scaled->ConstB = 1.0f / 30000.0f;
	Add(Scaled);
	auto* AtLeast = NewObject<UMaterialExpressionMax>(M);
	AtLeast->A.Connect(0, Scaled);
	AtLeast->ConstB = 1.0f;
	Add(AtLeast);
	Spread->B.Connect(0, AtLeast);
	auto* Grow = NewObject<UMaterialExpressionSubtract>(M);
	Grow->A.Connect(0, AtLeast);
	Grow->ConstB = 1.0f;
	Add(Grow);
	auto* Here = NewObject<UMaterialExpressionWorldPosition>(M);
	Add(Here);
	auto* FromCentre = NewObject<UMaterialExpressionSubtract>(M);
	FromCentre->A.Connect(0, Here);
	FromCentre->B.Connect(0, Centre);
	Add(FromCentre);
	auto* Offset = NewObject<UMaterialExpressionMultiply>(M);
	Offset->A.Connect(0, FromCentre);
	Offset->B.Connect(0, Grow);
	Add(Offset);
	Inputs->WorldPositionOffset.Connect(0, Offset);
	M->PreEditChange(nullptr);
	M->PostEditChange();
	Package->MarkPackageDirty();
	Dirty.Add(Package);
	return Masters.Add(kCorona, M);
}

// The renderer's global values for every material: Wetness (0..1, the weather's), the wind (WindX, WindY: the way it
// blows in Unreal's space; WindSpeed m/s).
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
			 {TEXT("WindSpeed"), 4.0f}})
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
	Inputs->BaseColor.Connect(0, WetColor);
	auto* WetRough = NewObject<UMaterialExpressionLinearInterpolate>(M);
	WetRough->A = Rough;
	WetRough->ConstB = 0.08f;
	WetRough->Alpha.Connect(0, Wet);
	Add(WetRough);
	Inputs->Roughness.Connect(0, WetRough);
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
