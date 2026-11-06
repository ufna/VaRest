// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

using UnrealBuildTool;

public class VaRestJson : ModuleRules
{
	public VaRestJson(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Direct OS entropy for the module-owned SipHash key; never use a GUID fallback.
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.Add("bcrypt.lib");
		}
		else if (Target.Platform == UnrealTargetPlatform.Mac || Target.Platform == UnrealTargetPlatform.IOS)
		{
			PublicFrameworks.Add("Security");
		}

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

			// PUBLIC: VaRestJsonLibrary.h derives from UBlueprintFunctionLibrary (Engine/Classes/Kismet/
			// BlueprintFunctionLibrary.h). Every game links Engine; v1's VaRest module needs it at PreDefault too.
			"Engine",
		});
	}
}
