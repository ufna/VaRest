// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

using UnrealBuildTool;
using System.IO;
using System;

// External archive input only. UBT never compiles or downloads this dependency.
public class VaRestPrivateLws : ModuleRules
{
	public VaRestPrivateLws(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;
        bool bNativeHostProfile = Target.Name == "VaRestHost" && Target.Type == TargetType.Game
            && Target.Platform == UnrealTargetPlatform.Linux && Target.Configuration == UnrealTargetConfiguration.Development
            && Target.ProjectFile != null
            && String.Equals(Path.GetFullPath(Target.ProjectFile.FullName), "/w/Unreal/VaRestHost/VaRestHost.uproject", StringComparison.Ordinal);
        if (bNativeHostProfile && (Target.Architectures.bIsMultiArch || Target.Architecture != UnrealArch.X64
            || Target.LinkType != TargetLinkType.Modular || Target.bMergeModules))
            throw new BuildException("VaRestHost native frame profile requires single x64 Linux Development modular, non-merged source.");
        bool bEditorProfile = Target.Platform == UnrealTargetPlatform.Linux
            && !Target.Architectures.bIsMultiArch && Target.Architecture == UnrealArch.X64
            && Target.Type == TargetType.Editor && Target.Configuration == UnrealTargetConfiguration.Development
            && Target.LinkType == TargetLinkType.Modular && !Target.bMergeModules;
        if (!bEditorProfile && !bNativeHostProfile)
            throw new BuildException("VaRestPrivateLws selects only Linux x64 Editor or exact VaRestHost Game Development modular, non-merged source profiles.");
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
