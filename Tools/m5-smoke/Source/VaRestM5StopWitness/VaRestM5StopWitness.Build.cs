// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
using UnrealBuildTool;
using System.IO;

public class VaRestM5StopWitness : ModuleRules
{
 public VaRestM5StopWitness(ReadOnlyTargetRules Target) : base(Target)
 {
  PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
  PrivateDependencyModuleNames.Add("Core");
  // Resolve the registered product module in both repository and ordinary Plugins layouts.
  // Text contract only. No VaRestHttp/Engine/CoreUObject import can change startup ordering.
  PrivateIncludePaths.Add(Path.Combine(GetModuleDirectory("VaRestHttp"), "Public"));
 }
}
