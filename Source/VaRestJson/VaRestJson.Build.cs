// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

using UnrealBuildTool;

public class VaRestJson : ModuleRules
{
	public VaRestJson(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",

			// PUBLIC: VaRestJson.h exposes TSharedPtr<FJsonValue>/<FJsonObject> bridges, so a
			// consumer calling them needs Json's include path.
			"Json",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// RapidJSON headers. Json re-exports them publicly (Json.Build.cs), but the
			// dependency is ours, so it is declared rather than inherited.
			"RapidJSON",
		});
	}
}
