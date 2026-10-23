#include "GTAVMaterials.h"

#include "Dom/JsonObject.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
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

UMaterial* FGTAVMaterials::Master(int32 Bucket)
{
	// 0 opaque, 1 translucent, 2 decal (translucent for now), 3 cutout; anything else is drawn as opaque.
	const int32 Kind = Bucket == 1 || Bucket == 2 ? 1 : Bucket == 3 ? 3 : 0;
	if (UMaterial** Found = Masters.Find(Kind))
	{
		return *Found;
	}
	static const TCHAR* Names[] = {TEXT("M_GTAV_Opaque"), TEXT("M_GTAV_Translucent"), nullptr, TEXT("M_GTAV_Cutout")};
	const FString PackageName = FString(TEXT("/Game/GTAV/Materials/")) + Names[Kind];
	if (UMaterial* Existing = LoadExisting<UMaterial>(PackageName))
	{
		return Masters.Add(Kind, Existing);
	}
	UPackage* Package = CreatePackage(*PackageName);
	UMaterial* M = NewObject<UMaterial>(Package, Names[Kind], RF_Public | RF_Standalone);
	M->BlendMode = Kind == 1 ? BLEND_Translucent : Kind == 3 ? BLEND_Masked : BLEND_Opaque;
	M->TwoSided = Kind == 3; // (foliage, fences)
	if (Kind == 1)
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
	auto* Diffuse = Sample(TEXT("DiffuseTex"), EngineTexture(TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture")), SAMPLERTYPE_Color, -600, -200);
	auto* Bump = Sample(TEXT("BumpTex"), EngineTexture(TEXT("/Engine/EngineMaterials/DefaultNormal.DefaultNormal")), SAMPLERTYPE_Normal, -600, 100);
	auto* Spec = Sample(TEXT("SpecularTex"), EngineTexture(TEXT("/Engine/EngineResources/Black.Black")), SAMPLERTYPE_LinearColor, -600, 400);
	Inputs->BaseColor.Connect(0, Diffuse);
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
		Inputs->Opacity.Connect(4, Diffuse);
	}
	M->PreEditChange(nullptr);
	M->PostEditChange();
	Package->MarkPackageDirty();
	Dirty.Add(Package);
	return Masters.Add(Kind, M);
}

UTexture2D* FGTAVMaterials::Texture(const FString& Path, bool bLinear, bool bNormalMap)
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
	if (bNormalMap)
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

UMaterialInterface* FGTAVMaterials::Get(const TSharedPtr<FJsonObject>& Material, bool& bOutTranslucent)
{
	const int32 Bucket = Material->GetIntegerField(TEXT("bucket"));
	bOutTranslucent = Bucket == 1 || Bucket == 2;
	const TSharedPtr<FJsonObject>* TexturesJson = nullptr;
	Material->TryGetObjectField(TEXT("textures"), TexturesJson);
	// The textures this master uses.
	TMap<FName, FString> Used;
	for (const TCHAR* Parameter : {TEXT("DiffuseTex"), TEXT("BumpTex"), TEXT("SpecularTex")})
	{
		FString Path;
		if (TexturesJson && (*TexturesJson)->TryGetStringField(Parameter, Path) && !Path.StartsWith(TEXT("?")))
		{
			Used.Add(Parameter, Path);
		}
	}
	FString Key = FString::Printf(TEXT("%d"), Bucket);
	for (const auto& [Parameter, Path] : Used)
	{
		Key += TEXT("|") + Parameter.ToString() + TEXT("=") + Path;
	}
	if (UMaterialInterface** Found = Instances.Find(Key))
	{
		return *Found;
	}
	UMaterial* Parent = Master(Bucket);
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
		const bool bNormal = Parameter == FName(TEXT("BumpTex"));
		if (UTexture2D* T = Texture(Path, Parameter != FName(TEXT("DiffuseTex")), bNormal))
		{
			MI->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(Parameter), T);
		}
	}
	MI->PostEditChange();
	Package->MarkPackageDirty();
	Dirty.Add(Package);
	return Instances.Add(Key, MI);
}
