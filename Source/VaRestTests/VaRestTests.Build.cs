// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

using UnrealBuildTool;

// Tests that need reflected test types, the integration suite and the benchmarks. UncookedOnly: built for the editor
// and uncooked runs only, so its USTRUCTs never reach a game's reflection data (VaRest-rfd, decision 15).
public class VaRestTests : ModuleRules
{
	public VaRestTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"VaRestJson",
			"VaRestHttp",
			"VaRest", // v1, for Bench.VaRest.Http.RequestPath (its HTTP and Json dependencies are public, VaRest.Build.cs:21-31)
		});
	}
}
