// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

using UnrealBuildTool;

public class VaRestHttp : ModuleRules
{
	public VaRestHttp(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",

			// PUBLIC: M5 API assets, subsystem hosts and Blueprint actions expose Engine runtime types.
			"Engine",

			// PUBLIC: FVaRestError carries an FVaRestJson; bodies and results are FVaRestJson and structs (FVaRestJsonConvert)
			"VaRestJson",

			// PUBLIC: UVaRestHttpSettings derives from UDeveloperSettings (Engine/DeveloperSettings.h)
			"DeveloperSettings",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// The default transport uses FHttpModule. The optional legacy adapter only forward-declares IHttpRequest,
			// so ordinary VaRestHttp consumers do not need to include the engine HTTP headers.
			"HTTP",
			"Projects", // version metadata, without depending on the legacy VaRest module
		});
	}
}
