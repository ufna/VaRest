// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

// External archive input only. UBT never compiles or downloads this dependency.
public class VaRestPrivateLws : ModuleRules
{
	// Qualification source opt-in only. Default OFF until the required UE/APK/device/lifecycle gates.
	// Both plugin ModuleRules share this source-controlled switch; no Target/global flags or environment input.
	public static bool AndroidQualificationOptIn => false;

	public static bool IsAndroidQualificationTuple(ReadOnlyTargetRules Target)
	{
		return Target.Platform == UnrealTargetPlatform.Android
			&& Target.Architecture == UnrealArch.Arm64
			&& Target.Type == TargetType.Game
			&& (Target.Configuration == UnrealTargetConfiguration.Development || Target.Configuration == UnrealTargetConfiguration.Shipping)
			&& Target.LinkType == TargetLinkType.Monolithic
			&& !Target.bMergeModules;
	}

	public VaRestPrivateLws(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;
		bool bLinuxSupported = Target.Platform == UnrealTargetPlatform.Linux
			&& Target.Architecture == UnrealArch.X64
			&& !Target.bMergeModules
			&& ((Target.Type == TargetType.Editor
				&& Target.Configuration == UnrealTargetConfiguration.Development
				&& Target.LinkType == TargetLinkType.Modular)
				|| (Target.Type == TargetType.Game
					&& (Target.Configuration == UnrealTargetConfiguration.Development || Target.Configuration == UnrealTargetConfiguration.Shipping)
					&& Target.LinkType == TargetLinkType.Monolithic));
		bool bAndroidSupported = AndroidQualificationOptIn && IsAndroidQualificationTuple(Target);
		bool bSupported = bLinuxSupported || bAndroidSupported;
		if (!bSupported)
		{
			throw new BuildException("VaRestPrivateLws accepts the existing Linux x64 source tuple or explicitly opted-in Android Game ARM64 Development/Shipping monolithic, non-merged source qualification; this does not establish UE/device/lifecycle support.");
		}
		if (bUsePrecompiled)
		{
			throw new BuildException("A precompiled private LWS module/package is not part of this proof.");
		}
		// This tranche supplies Release C objects only. Debug/DebugCRT targets never select it.
		string IncludeRoot = bAndroidSupported
			? Path.Combine(ModuleDirectory, "include", "Android", "ARM64")
			: Path.Combine(ModuleDirectory, "include", "Linux", "x86_64-unknown-linux-gnu");
		string Archive = bAndroidSupported
			? Path.Combine(ModuleDirectory, "lib", "Android", "ARM64", "Release", "libvarest_lws3_private.a")
			: Path.Combine(ModuleDirectory, "lib", "Linux", "x86_64-unknown-linux-gnu", "Release", "libvarest_lws3_private.a");
		if (!File.Exists(Archive))
		{
			throw new BuildException("Missing bundled private LWS archive: {0}. Restore the matching VaRest Source/ThirdParty dependency; no SDK fallback.", Archive);
		}
		PublicSystemIncludePaths.Add(IncludeRoot);
		PublicAdditionalLibraries.Add(Archive);
		ExternalDependencies.Add(Path.Combine(IncludeRoot, "VaRestPrivateLws", "libwebsockets.h"));
		ExternalDependencies.Add(Path.Combine(IncludeRoot, "VaRestPrivateLws", "lws_config.h"));
		ExternalDependencies.Add(Path.Combine(IncludeRoot, "VaRestPrivateLws", bAndroidSupported ? "varest_cpp_api.h" : "varest_external_names.h"));
		ExternalDependencies.Add(Path.Combine(IncludeRoot, "VaRestPrivateLws", "varest_numeric.h"));
	}
}
