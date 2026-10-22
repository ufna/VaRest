// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

using UnrealBuildTool;

// WebSocket spec 1.1: VaRest's own WebSocket transport on the libwebsockets 3.0 the engine bundles (ruling R1), and from W2
// on the socket, its stages and its Blueprint surface. Dependencies mirror the engine's WebSockets module
// (WebSockets.Build.cs:92-120); that module itself is never one.
public class VaRestWebSocket : ModuleRules
{
	public VaRestWebSocket(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			// PUBLIC: VaRestSocketTypes.h declares UENUMs (W3-W5 hand them to Blueprints)
			"CoreUObject",
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
		AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL", "libWebSockets", "zlib");
	}
}
