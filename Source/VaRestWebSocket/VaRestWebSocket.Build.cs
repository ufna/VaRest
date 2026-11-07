// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

using UnrealBuildTool;
using System;
using System.IO;

// WebSocket spec 1.1: VaRest's own WebSocket transport on the libwebsockets 3.0 the engine bundles (ruling R1), and from W2
// on the socket, its stages and its Blueprint surface. Dependencies mirror the engine's WebSockets module
// (WebSockets.Build.cs:92-120); that module itself is never one.
public class VaRestWebSocket : ModuleRules
{
	public VaRestWebSocket(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		if (Target.Platform == UnrealTargetPlatform.Linux)
		{
			PrivateIncludePathModuleNames.Add("VaRestNativeOwner");
			DynamicallyLoadedModuleNames.Add("VaRestNativeOwner");
		}

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			// PUBLIC: VaRestSocketTypes.h declares UENUMs (W3-W5 hand them to Blueprints)
			"CoreUObject",
			"Engine",
			"DeveloperSettings",
			// PUBLIC: W2-W5's socket API takes FVaRestJson and FVaRestError; W1 uses the UTF-8 validator (VaRestJsonUtf8.h)
			"VaRestJson",
			"VaRestHttp",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// FPlatformHttp::GetConfiguredProxyAddress (GenericPlatformHttp.h:193), as LwsWebSocketsManager.cpp:196-203
			"HTTP",
			// The SSL module's context (monolithic and merged builds) and the certificate manager (roots, pinned keys), as
			// WebSockets.Build.cs:119
			"SSL",
		});

		// The engine's own link of libwebsockets (WebSockets.Build.cs:118; ModuleRules.cs:1601-1607 makes them private
		// dependencies): static OpenSSL, libwebsockets and zlib, one copy per module in modular builds (spec 1.2)
		// Existing Editor and exact named Host Game profiles select the private archive; H remains separate.
		// Architecture is the public SingleArchitecture getter: a multi-architecture target fails closed.
        bool bNativeHostProfile = Target.Name == "VaRestHost" && Target.Type == TargetType.Game
            && Target.Platform == UnrealTargetPlatform.Linux && Target.Configuration == UnrealTargetConfiguration.Development
            && Target.ProjectFile != null
            && String.Equals(Path.GetFullPath(Target.ProjectFile.FullName), "/w/Unreal/VaRestHost/VaRestHost.uproject", StringComparison.Ordinal);
        if (bNativeHostProfile && (Target.Architectures.bIsMultiArch || Target.Architecture != UnrealArch.X64
            || Target.LinkType != TargetLinkType.Modular || Target.bMergeModules))
            throw new BuildException("VaRestHost native frame profile requires single x64 Linux Development modular, non-merged source.");
		bool bPrivateLwsProof = (Target.Platform == UnrealTargetPlatform.Linux
			&& Target.Architecture == UnrealArch.X64
			&& Target.Type == TargetType.Editor
			&& Target.Configuration == UnrealTargetConfiguration.Development
			&& Target.LinkType == TargetLinkType.Modular
			&& !Target.bMergeModules) || bNativeHostProfile;
		PrivateDefinitions.Add("VAREST_USE_PRIVATE_LWS3=" + (bPrivateLwsProof ? "1" : "0"));
		if (bPrivateLwsProof)
		{
			if (bUsePrecompiled)
			{
				throw new BuildException("VaRest private LWS proof requires a fresh source-built WS module; precompiled WS is not qualified.");
			}
			PrivateDependencyModuleNames.Add("VaRestPrivateLws");
			AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL", "zlib");
		}
		else
		{
			// Existing selection only; these targets have no private RxFlow repair claim.
			AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL", "libWebSockets", "zlib");
		}
	}
}
