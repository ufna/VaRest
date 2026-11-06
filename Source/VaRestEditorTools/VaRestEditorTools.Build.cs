// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

using UnrealBuildTool;

public class VaRestEditorTools : ModuleRules
{
	public VaRestEditorTools(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "UnrealEd" });
		PrivateDependencyModuleNames.AddRange(new[] { "Engine", "VaRestHttp", "VaRestWebSocket", "VaRestSocketIO", "PropertyEditor", "AssetTools", "ToolMenus", "Slate", "SlateCore" });
	}
}
