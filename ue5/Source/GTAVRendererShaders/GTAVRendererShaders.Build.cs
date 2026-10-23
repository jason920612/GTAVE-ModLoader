using UnrealBuildTool;

// The renderer's own shaders (ue5/Shaders): loaded before the engine compiles its global shaders, so a module of its
// own (the game module, with its UObjects, loads later).
public class GTAVRendererShaders : ModuleRules
{
	public GTAVRendererShaders(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "RenderCore", "RHI" });
	}
}
