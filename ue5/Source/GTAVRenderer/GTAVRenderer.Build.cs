using UnrealBuildTool;

public class GTAVRenderer : ModuleRules
{
	public GTAVRenderer(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "RenderCore", "RHI", "Renderer", "D3D12RHI", "MeshDescription", "StaticMeshDescription", "GTAVRendererShaders" });
		// (the bridge's shared texture and fence: D3D12 itself)
		AddEngineThirdPartyPrivateStaticDependencies(Target, "DX12");
		PublicSystemLibraries.Add("dxgi.lib"); // (the GPU memory Windows gives the renderer)
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("UnrealEd");
		}
	}
}
