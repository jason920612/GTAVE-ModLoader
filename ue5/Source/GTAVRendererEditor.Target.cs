using UnrealBuildTool;

public class GTAVRendererEditorTarget : TargetRules
{
	public GTAVRendererEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
		ExtraModuleNames.Add("GTAVRenderer");
	}
}
