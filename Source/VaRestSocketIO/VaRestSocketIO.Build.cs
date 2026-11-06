// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

using UnrealBuildTool;

public class VaRestSocketIO : ModuleRules
{
	public VaRestSocketIO(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "DeveloperSettings", "VaRestJson", "VaRestHttp", "VaRestWebSocket" });
	}
}
