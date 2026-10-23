using UnrealBuildTool;

public class GTAVImporter : ModuleRules
{
	public GTAVImporter(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PrivateDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject", "Engine", "UnrealEd", "AssetTools", "AssetRegistry",
			"MeshDescription", "StaticMeshDescription", "Json", "JsonUtilities", "RenderCore", "RHI", "MaterialEditor"
		});
	}
}
