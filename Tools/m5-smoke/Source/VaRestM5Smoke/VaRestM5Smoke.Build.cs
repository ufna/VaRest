// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
using UnrealBuildTool;
public class VaRestM5Smoke : ModuleRules
{
 public VaRestM5Smoke(ReadOnlyTargetRules Target) : base(Target)
 {
  PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
  PrivateDependencyModuleNames.Add("Core");
 }
}
