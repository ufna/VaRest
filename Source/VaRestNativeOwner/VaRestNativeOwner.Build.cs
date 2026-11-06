// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

using UnrealBuildTool;

public class VaRestNativeOwner : ModuleRules
{
	public VaRestNativeOwner(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.CPlusPlus;
		PCHUsage = PCHUsageMode.NoPCHs;
		bUseUnity = false;
		PrivateDependencyModuleNames.Add("Core");
	}
}
