// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
using UnrealBuildTool;

public class VaRestOpenApi : ModuleRules
{
    public VaRestOpenApi(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        DefaultBuildSettings = BuildSettingsVersion.V5;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "VaRestJson", "UnrealEd", "DeveloperSettings", "DataValidation" });
        PrivateDependencyModuleNames.AddRange(new[] { "Json", "VaRestHttp", "BlueprintGraph", "AssetTools", "AssetRegistry", "MessageLog", "Slate", "SlateCore", "PropertyEditor", "InputCore", "ToolMenus", "ContentBrowser", "ContentBrowserData", "DesktopPlatform", "HTTP", "SSL" });
        // Direct static curl users need its complete link set with precompiled modular SDKs.
        AddEngineThirdPartyPrivateStaticDependencies(Target, "libcurl", "nghttp2", "OpenSSL", "zlib");
    }
}
