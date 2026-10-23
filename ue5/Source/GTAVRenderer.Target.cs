using UnrealBuildTool;

public class GTAVRendererTarget : TargetRules
{
	public GTAVRendererTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
		ExtraModuleNames.Add("GTAVRenderer");
	}
}
