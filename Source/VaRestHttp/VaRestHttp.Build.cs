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

			// PUBLIC: FVaRestError carries an FVaRestJson; bodies and results are FVaRestJson and structs (FVaRestJsonConvert)
			"VaRestJson",

			// PUBLIC: UVaRestHttpSettings derives from UDeveloperSettings (Engine/DeveloperSettings.h)
			"DeveloperSettings",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// The default IVaRestTransport is FHttpModule (spec 0.4: UE's HTTP module is the transport). No public
			// header names an engine HTTP type, so a game that only links VaRestHttp's API does not need HTTP's headers.
			"HTTP",
		});
	}
}
