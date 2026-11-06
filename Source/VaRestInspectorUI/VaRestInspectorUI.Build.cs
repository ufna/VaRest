// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
using UnrealBuildTool;

public class VaRestInspectorUI : ModuleRules
{
	public VaRestInspectorUI(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new[] { "Core", "VaRestInspector" });
		PrivateDependencyModuleNames.AddRange(new[] { "CoreUObject", "Engine", "VaRestJson", "VaRestHttp", "Slate", "SlateCore", "InputCore", "ApplicationCore", "DesktopPlatform" });
		if (Target.bBuildEditor)
			PrivateDependencyModuleNames.AddRange(new[] { "UnrealEd", "WorkspaceMenuStructure", "ToolMenus", "Settings" });
	}
}
