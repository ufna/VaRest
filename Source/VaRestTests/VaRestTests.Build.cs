// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

// Tests that need reflected test types, the integration suite and the benchmarks. UncookedOnly: built for the editor
// and uncooked runs only, so its USTRUCTs never reach a game's reflection data (VaRest-rfd, decision 15).
public class VaRestTests : ModuleRules
{
	public VaRestTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		if (Target.Platform == UnrealTargetPlatform.Linux)
		{
			PrivateIncludePathModuleNames.Add("VaRestNativeOwner");
			DynamicallyLoadedModuleNames.Add("VaRestNativeOwner");
		}

		// Own reflected JSON fixtures here; private codec assertions keep a one-way dependency on VaRestJson.
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "VaRestJson", "Private"));

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"VaRestJson",
			"Json", // The typed benchmark directly uses the engine JSON DOM/serializer.
			"JsonUtilities", // Bench.VaRest.Json.TypedConvert's engine converter comparand.
			"VaRestHttp",
			"VaRestInspector",
			// WebSocket W1: the transport's integration suite (VaRest.Integration.WebSocket.Transport.*) and its test hooks
			// (VaRestSocketTesting.h)
			"VaRestWebSocket",
			// WebSocket TLS: IPluginManager locates the test server certificate.
			"Projects",
			"VaRest", // v1, for Bench.VaRest.Http.RequestPath (its HTTP and Json dependencies are public, VaRest.Build.cs:21-31)
			// WebSocket W0 probe: the SSL module's context and certificate manager, which the engine's transport uses
			// (WebSockets.Build.cs:118-119)
			"SSL",
			// WebSocket W0 probe: FGenericPlatformHttp::UrlEncode for query values (GenericPlatformHttp.h:99, HTTP_API). VaRest's
			// public HTTP dependency gives this module the header, not the link
			"HTTP",
		});

		// WebSocket W0 probe (Probe.VaRest.Lws.*): the engine's IWebSocket for the coexistence check, reached only through
		// FWebSocketsModule's virtuals. Include paths, no link: this module keeps its own static libwebsockets, as
		// VaRestWebSocket will (WebSocket spec 1.1: the engine module WebSockets is not a dependency).
		PrivateIncludePathModuleNames.Add("WebSockets");

		// WebSocket W0 probe: raw libwebsockets 3.0 as the engine bundles it, linked the way the engine's WebSockets module
		// links it (WebSockets.Build.cs:118; ModuleRules.cs:1601-1607 adds them as private dependencies). Test-only: this
		// module is UncookedOnly, never in a game target (ModuleDescriptor.cs:792-793, TargetRules.cs:1190-1193).
		AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL", "libWebSockets", "zlib");
	}
}
