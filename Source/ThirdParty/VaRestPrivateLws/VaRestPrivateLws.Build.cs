// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

// External archive input only. UBT never compiles or downloads this dependency.
public class VaRestPrivateLws : ModuleRules
{
	public VaRestPrivateLws(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;
		if (Target.Platform != UnrealTargetPlatform.Linux
			|| Target.Architecture != UnrealArch.X64
			|| Target.Type != TargetType.Editor
			|| Target.Configuration != UnrealTargetConfiguration.Development
			|| Target.LinkType != TargetLinkType.Modular
			|| Target.bMergeModules)
		{
			throw new BuildException("VaRestPrivateLws is qualified only for Linux x64 Editor Development modular, non-merged source builds.");
		}
		if (bUsePrecompiled)
		{
			throw new BuildException("A precompiled private LWS module/package is not part of this proof.");
		}
		// This tranche supplies Release C objects only. Debug/DebugCRT targets never select it.
		string IncludeRoot = Path.Combine(ModuleDirectory, "include", "Linux", "x86_64-unknown-linux-gnu");
		string Archive = Path.Combine(PluginDirectory, "Intermediate", "VaRestPrivateLws", "Linux", "x86_64-unknown-linux-gnu", "Release", "006", "libvarest_lws3_private.a");
		if (!File.Exists(Archive))
		{
			throw new BuildException("Missing reviewed private LWS archive: {0}. Root must build it before the fresh UBT graph; no SDK fallback.", Archive);
		}
		PublicSystemIncludePaths.Add(IncludeRoot);
		PublicAdditionalLibraries.Add(Archive);
		ExternalDependencies.Add(Path.Combine(IncludeRoot, "VaRestPrivateLws", "libwebsockets.h"));
		ExternalDependencies.Add(Path.Combine(IncludeRoot, "VaRestPrivateLws", "lws_config.h"));
	}
}
