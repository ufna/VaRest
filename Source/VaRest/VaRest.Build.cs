// Copyright 2014-2019 Vladimir Alyamkin. All Rights Reserved.

using System.IO;

namespace UnrealBuildTool.Rules
{
	public class VaRest : ModuleRules
	{
		public VaRest(ReadOnlyTargetRules Target) : base(Target)
		{
			PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
			PrecompileForTargets = PrecompileTargetsType.Any;
			IncludeOrderVersion = EngineIncludeOrderVersion.Latest;

			PrivateIncludePaths.AddRange(
				new string[] {
					"VaRest/Private",
					// ... add other private include paths required here ...
				});

			PublicDependencyModuleNames.AddRange(
				new string[]
				{
					"Core",
					"CoreUObject",
					"Engine",
					"HTTP",
					"Json",
					"Projects" // Required by IPluginManager etc (used to get plugin information)
					// ... add other public dependencies that you statically link with here ...
				});
		}
	}
}
