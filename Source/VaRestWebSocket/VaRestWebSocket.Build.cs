// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

using UnrealBuildTool;

// WebSocket spec 1.1: VaRest's own WebSocket transport on the libwebsockets 3.0 the engine bundles (ruling R1), and from W2
// on the socket, its stages and its Blueprint surface. Dependencies mirror the engine's WebSockets module
// (WebSockets.Build.cs:92-120). Development Game automation code additionally links WebSockets for the opt-in fixture below.
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
		//419 external C labels isolate this archive from the Engine LWS copy even in monolithic Game.
		// EditorDevelopment modular is qualified; ordinary Game Dev/Shipping qualification is a separate gate.
		// Multi-architecture, merged, other platforms/configurations and precompiled WS remain outside this tranche.
		bool bPrivateLws = Target.Platform == UnrealTargetPlatform.Linux
			&& Target.Architecture == UnrealArch.X64
			&& !Target.bMergeModules
			&& ((Target.Type == TargetType.Editor
				&& Target.Configuration == UnrealTargetConfiguration.Development
				&& Target.LinkType == TargetLinkType.Modular)
				|| (Target.Type == TargetType.Game
					&& (Target.Configuration == UnrealTargetConfiguration.Development || Target.Configuration == UnrealTargetConfiguration.Shipping)
					&& Target.LinkType == TargetLinkType.Monolithic));
		PrivateDefinitions.Add("VAREST_USE_PRIVATE_LWS3=" + (bPrivateLws ? "1" : "0"));
		if (bPrivateLws)
		{
			if (bUsePrecompiled)
			{
				throw new BuildException("VaRest private LWS requires a fresh source-built WS module; precompiled WS is not qualified.");
			}
			PrivateDependencyModuleNames.Add("VaRestPrivateLws");
			AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL", "zlib");
		}
		else
		{
			// Existing selection only; these targets have no private RxFlow repair claim.
			AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL", "libWebSockets", "zlib");
		}

		// WITH_DEV_AUTOMATION_TESTS is normally enabled in Development. Discovery still requires a dedicated
		// -VaRestCookedCoexistFixture -unattended -nowrite process; Shipping and Editor do not gain this dependency.
		bool bCookedCoexistTests = bPrivateLws && Target.Type == TargetType.Game
			&& Target.Configuration == UnrealTargetConfiguration.Development && !Target.bForceDisableAutomationTests;
		PrivateDefinitions.Add("VAREST_COOKED_COEXIST_TESTS=" + (bCookedCoexistTests ? "1" : "0"));
		if (bCookedCoexistTests)
		{
			PrivateIncludePathModuleNames.Add("WebSockets");
			DynamicallyLoadedModuleNames.Add("WebSockets");
		}
	}
}
