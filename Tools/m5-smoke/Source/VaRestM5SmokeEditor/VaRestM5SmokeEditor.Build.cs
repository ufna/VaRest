// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
using UnrealBuildTool;
public class VaRestM5SmokeEditor : ModuleRules
{
    public VaRestM5SmokeEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new string[] {
            "Core", "CoreUObject", "Engine", "UnrealEd", "BlueprintGraph", "AssetRegistry",
            "VaRestHttp", "VaRestJson", "VaRestM5Smoke"
        });
    }
}
