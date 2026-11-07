// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

// External archive input only. UBT never compiles or downloads this dependency.
public class VaRestPrivateLws : ModuleRules
{
	public VaRestPrivateLws(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;
		bool bSupported = Target.Platform == UnrealTargetPlatform.Linux
			&& Target.Architecture == UnrealArch.X64
			&& !Target.bMergeModules
			&& ((Target.Type == TargetType.Editor
				&& Target.Configuration == UnrealTargetConfiguration.Development
				&& Target.LinkType == TargetLinkType.Modular)
				|| (Target.Type == TargetType.Game
					&& (Target.Configuration == UnrealTargetConfiguration.Development || Target.Configuration == UnrealTargetConfiguration.Shipping)
					&& Target.LinkType == TargetLinkType.Monolithic));
		if (!bSupported)
		{
			throw new BuildException("VaRestPrivateLws supports Linux x64 non-merged source builds: Editor Development modular or Game Development/Shipping monolithic; each requires its actual qualification.");
		}
		if (bUsePrecompiled)
		{
			throw new BuildException("A precompiled private LWS module/package is not part of this proof.");
		}
		// This tranche supplies Release C objects only. Debug/DebugCRT targets never select it.
		string IncludeRoot = Path.Combine(ModuleDirectory, "include", "Linux", "x86_64-unknown-linux-gnu");
		string Archive = Path.Combine(ModuleDirectory, "lib", "Linux", "x86_64-unknown-linux-gnu", "Release", "libvarest_lws3_private.a");
		if (!File.Exists(Archive))
		{
			throw new BuildException("Missing bundled private LWS archive: {0}. Restore the matching VaRest Source/ThirdParty dependency; no SDK fallback.", Archive);
		}
		PublicSystemIncludePaths.Add(IncludeRoot);
		PublicAdditionalLibraries.Add(Archive);
		ExternalDependencies.Add(Path.Combine(IncludeRoot, "VaRestPrivateLws", "libwebsockets.h"));
		ExternalDependencies.Add(Path.Combine(IncludeRoot, "VaRestPrivateLws", "lws_config.h"));
		ExternalDependencies.Add(Path.Combine(IncludeRoot, "VaRestPrivateLws", "varest_external_names.h"));
	}
}
