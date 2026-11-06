// Copyright 2014-2019 Vladimir Alyamkin. All Rights Reserved.

using UnrealBuildTool;

public class VaRestEditor : ModuleRules
{
	public VaRestEditor(ReadOnlyTargetRules Target) : base(Target)
	{
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        DefaultBuildSettings = BuildSettingsVersion.V5;

		PrivateIncludePaths.AddRange(
			new string[] {
				"VaRestEditor/Private",

				// ... add other private include paths required here ...
			});


		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"BlueprintGraph", // exported spawner/provider contract
				"VaRestHttp", // exported API/socket entry types
                "VaRest"

				// ... add other public dependencies that you statically link with here ...
			});


		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
                "Slate",
                "SlateCore",
                "InputCore",
                "AssetTools",
                "AssetRegistry",
                "UnrealEd",     // for FAssetEditorManager
                "KismetWidgets",
                "KismetCompiler",
                "GraphEditor",
                "Kismet",       // for FWorkflowCentricApplication
                "PropertyEditor",
                "EditorStyle",
                "Sequencer",
                "DetailCustomizations",
                "Settings",
                "RenderCore",
                // VaRest 2: the v2 Make Json node's "Remove key" context menu (UToolMenu)
                "ToolMenus",
                // VaRest 2: struct <-> JSON cache invalidation, the v2 Make Json node, their tests
                "VaRestJson",
                "VaRestWebSocket",
                "VaRestSocketIO"
			});


		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			});
	}
}
