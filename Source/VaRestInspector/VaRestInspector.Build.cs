// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

using UnrealBuildTool;
public class VaRestInspector : ModuleRules
{
    public VaRestInspector(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core", "CoreUObject", "Engine", "DeveloperSettings", "VaRestHttp"
        });
        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "VaRestJson"
        });
    }
}
