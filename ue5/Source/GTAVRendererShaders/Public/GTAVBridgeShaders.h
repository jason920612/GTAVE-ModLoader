#pragma once

#include "GlobalShader.h"
#include "SceneView.h"
#include "ShaderParameterStruct.h"

// The picture the game gets (GTAVBridgeSubsystem): the scene colour, and in alpha the scene's depth in metres along the
// view (the sky 65504, as far as a half float goes), for the game to keep its peds and vehicles only where nothing of
// the renderer's world is in front of them. Negative where a screen a script draws into is (custom stencil 1: a TV's,
// a monitor's): the game shows its own picture there, what the script drew.
class GTAVRENDERERSHADERS_API FGTAVBridgePS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGTAVBridgePS);
	SHADER_USE_PARAMETER_STRUCT(FGTAVBridgePS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColor)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepth)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>, CustomStencil)
		SHADER_PARAMETER(FIntPoint, ColorMin)
		SHADER_PARAMETER(FIntPoint, ColorSize)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};
