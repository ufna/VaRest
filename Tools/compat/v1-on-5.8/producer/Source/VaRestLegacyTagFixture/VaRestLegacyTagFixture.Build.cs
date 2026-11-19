using UnrealBuildTool;

public class VaRestLegacyTagFixture : ModuleRules
{
    public VaRestLegacyTagFixture(ReadOnlyTargetRules Target) : base(Target)
    {
        if (Target.Type != TargetType.Editor)
        {
            throw new BuildException("VaRestLegacyTagFixture is an isolated Editor-only original-tag producer.");
        }
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        DefaultBuildSettings = BuildSettingsVersion.V5;
        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Core", "CoreUObject", "Engine", "Projects",
            "UnrealEd", "BlueprintGraph", "Kismet", "KismetCompiler", "GraphEditor",
            "VaRest", "VaRestEditor"
        });
    }
}
