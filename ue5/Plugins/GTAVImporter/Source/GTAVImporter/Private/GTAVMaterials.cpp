#include "GTAVMaterials.h"

#include "Dom/JsonObject.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexColor.h"
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
		TEXT("M_GTAV_Water")};
	const FString PackageName = FString(TEXT("/Game/GTAV/Materials/")) + Names[Kind];
	if (UMaterial* Existing = LoadExisting<UMaterial>(PackageName))
	{
		return Masters.Add(Kind, Existing);
	}
	UPackage* Package = CreatePackage(*PackageName);
	UMaterial* M = NewObject<UMaterial>(Package, Names[Kind], RF_Public | RF_Standalone);
	M->BlendMode = Kind == 1 || Kind == kWater ? BLEND_Translucent : Kind == 3 ? BLEND_Masked : BLEND_Opaque;
	M->TwoSided = Kind == 3; // (foliage, fences)
	// (usage flags cannot be added in the game: the material would fall back to the default one)
	M->bUsedWithNanite = true;
	M->bUsedWithInstancedStaticMeshes = true;
	if (Kind == 1 || Kind == kWater)
	{
		M->TranslucencyLightingMode = TLM_SurfacePerPixelLighting;
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
	if (Kind == kWater)
	{
		// Water (draw bucket 6): its colour, smooth and reflective, see-through.
		auto* Color = NewObject<UMaterialExpressionVectorParameter>(M);
		Color->ParameterName = TEXT("WaterColor");
		Color->DefaultValue = FLinearColor(0.65f, 0.76f, 0.78f);
		M->GetExpressionCollection().AddExpression(Color);
		// (the game's water is mostly what it reflects: its colour is the depth fog, kept dark here)
		auto* Dark = NewObject<UMaterialExpressionMultiply>(M);
		Dark->A.Connect(0, Color);
		Dark->ConstB = 0.12f;
		M->GetExpressionCollection().AddExpression(Dark);
		Inputs->BaseColor.Connect(0, Dark);
		auto* Specular = NewObject<UMaterialExpressionConstant>(M);
		Specular->R = 1.0f;
		M->GetExpressionCollection().AddExpression(Specular);
		Inputs->Specular.Connect(0, Specular);
		auto* Opacity = NewObject<UMaterialExpressionConstant>(M);
		Opacity->R = 0.4f;
		M->GetExpressionCollection().AddExpression(Opacity);
		Inputs->Opacity.Connect(0, Opacity);
		auto* Rough = NewObject<UMaterialExpressionConstant>(M);
		Rough->R = 0.03f;
		M->GetExpressionCollection().AddExpression(Rough);
		Inputs->Roughness.Connect(0, Rough);
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
	auto* Tinted = NewObject<UMaterialExpressionMultiply>(M);
	Tinted->A.Connect(0, Diffuse);
	Tinted->B.Connect(0, Palette);
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
	// Emissive (the game's emissiveMultiplier; 0 for everything else).
	auto* Glow = NewObject<UMaterialExpressionMultiply>(M);
	Glow->A.Connect(0, UseTint);
	Glow->B.Connect(0, Scalar(TEXT("EmissiveMultiplier"), 0.0f));
	Add(Glow);
	Inputs->EmissiveColor.Connect(0, Glow);
	M->PreEditChange(nullptr);
	M->PostEditChange();
	Package->MarkPackageDirty();
	Dirty.Add(Package);
	return Masters.Add(Kind, M);
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
