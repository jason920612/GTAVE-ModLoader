#include "GTAVBridgeShaders.h"

#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"

IMPLEMENT_GLOBAL_SHADER(FGTAVBridgePS, "/GTAVRenderer/GTAVBridge.usf", "MainPS", SF_Pixel);

class FGTAVRendererShadersModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// (the project's Shaders folder, beside its Source)
		AddShaderSourceDirectoryMapping(TEXT("/GTAVRenderer"), FPaths::Combine(FPaths::ProjectDir(), TEXT("Shaders")));
	}
};

IMPLEMENT_MODULE(FGTAVRendererShadersModule, GTAVRendererShaders);
