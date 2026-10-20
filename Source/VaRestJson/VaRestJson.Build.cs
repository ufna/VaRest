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

			// PUBLIC: VaRestJsonWriterInternal.h hands the RapidJSON writer type to sibling modules
			// (WebSocket/Socket.IO envelopes). Json re-exports it too (Json.Build.cs:9-15); declared, not inherited.
			"RapidJSON",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// Bench.VaRest.Json.TypedConvert compares against FJsonObjectConverter. Engine already depends on
			// JsonUtilities publicly (Engine.Build.cs:88), so every game links it anyway and gains no code from this.
			"JsonUtilities",
		});
	}
}
