// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "UObject/Package.h"
#include "VaRestApi.h"
#include "VaRestAuth.h"
#include "VaRestBuiltinAuthConfigs.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestBuiltinAuthConfigTest, "VaRest.Http.Auth.Config.NativeBindings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestBuiltinAuthConfigTest::RunTest(const FString& Parameters)
{
	FVaRestSessionData Session;
	Session.AccessToken = TEXT("access-token-test");
	FVaRestHeaders Headers;
	TArray<TPair<FString, FString>> Query;
	FVaRestAuthTarget Target(Headers, Query, Session);
	auto* Bearer = NewObject<UVaRestBearerAuthConfig>();
	TestTrue(TEXT("Bearer config creates the actual native provider"), Bearer->CreateAuth()->Apply(Target));
	TestEqualSensitive(TEXT("session bearer applied"), Headers.Get(TEXT("Authorization")), FString(TEXT("Bearer access-token-test")));
	auto* Key = NewObject<UVaRestApiKeyAuthConfig>();
	Key->Name = TEXT("X-Game-Key");
	Key->Key = TEXT("test-key-123456");
	TestTrue(TEXT("API key config creates the actual native provider"), Key->CreateAuth()->Apply(Target));
	TestEqualSensitive(TEXT("configured header applied"), Headers.Get(TEXT("X-Game-Key")), Key->Key);
	Key->Placement = EVaRestApiKeyPlacement::Query;
	Key->Name = TEXT("api_key");
	TestTrue(TEXT("query placement uses the native ordered query target"), Key->CreateAuth()->Apply(Target));
	TestTrue(TEXT("query pair preserved"), Query.Num() == 1 && Query[0].Key == Key->Name && Query[0].Value == Key->Key);
	auto* Basic = NewObject<UVaRestBasicAuthConfig>();
	Basic->User = TEXT("game");
	Basic->Password = TEXT("test-password");
	TestTrue(TEXT("Basic config creates the actual native provider"), Basic->CreateAuth()->Apply(Target));
	FString Pair;
	TestTrue(TEXT("Basic credential decodes"), FBase64::Decode(Headers.Get(TEXT("Authorization")).Mid(6), Pair));
	TestEqualSensitive(TEXT("ordinary Basic preserves its existing RFC7617 contract"), Pair, FString(TEXT("game:test-password")));
	FVaRestApiValidationContext Context;
	Basic->User = TEXT("bad:user");
	Basic->Validate(Context);
	TestTrue(TEXT("Basic validation refuses ambiguous user separator"), Context.HasErrors());
	Context = {};
	Key->Key.Reset();
	Key->Validate(Context);
	TestFalse(TEXT("development secrets are optional in a cooked asset"), Context.HasErrors());
	TestTrue(TEXT("runtime credential requirement remains visible"), Context.Issues.ContainsByPredicate([](const auto& Issue) { return Issue.bWarning && Issue.Code == TEXT("Auth.RuntimeCredentials"); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestAuthConfigEndpointTest, "VaRest.Http.Auth.Config.PerClientEndpointResolution",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestAuthConfigEndpointTest::RunTest(const FString& Parameters)
{
	auto* Api = NewObject<UVaRestApi>();
	FVaRestApiEndpoint Endpoint;
	Endpoint.Name = TEXT("RefreshToken");
	Endpoint.Verb = TEXT("POST");
	Endpoint.Path = TEXT("/tokens/refresh");
	Endpoint.AuthMode = EVaRestAuthMode::Required;
	Endpoint.Headers.Add({TEXT("X-Ordinary-Policy"), TEXT("not-copied")});
	Api->Endpoints.Add(Endpoint);
	auto* Original = NewObject<UVaRestBearerRefreshAuthConfig>(Api);
	Original->RefreshEndpoint = Endpoint.Name;
	auto* A = DuplicateObject<UVaRestBearerRefreshAuthConfig>(Original, GetTransientPackage());
	auto* B = DuplicateObject<UVaRestBearerRefreshAuthConfig>(Original, GetTransientPackage());
	FString Error;
	TestTrue(TEXT("first per-client copy resolves the actual POST endpoint"), A->ResolveEndpoint(*Api, Error));
	TestTrue(TEXT("second per-client copy resolves independently"), B->ResolveEndpoint(*Api, Error));
	TestEqualSensitive(TEXT("resolved refresh path"), A->Config.RefreshPath, Endpoint.Path);
	TestEqualSensitive(TEXT("asset config was not mutated"), Original->Config.RefreshPath, FString(TEXT("/auth/refresh")));
	A->Config.RefreshPath = TEXT("/client-a-only");
	TestEqualSensitive(TEXT("another client's config stays independent"), B->Config.RefreshPath, Endpoint.Path);
	TestTrue(TEXT("resolved provider is constructed before any live client is needed"), B->CreateAuth().IsValid());
	Api->Endpoints[0].Verb = TEXT("GET");
	TestFalse(TEXT("a normal GET endpoint cannot be relabeled as a token POST"), B->ResolveEndpoint(*Api, Error));
	Api->Endpoints[0].Verb = TEXT("POST");
	Api->Endpoints[0].Path = TEXT("/tokens/{id}");
	TestFalse(TEXT("an unresolved template is not a grant URL"), B->ResolveEndpoint(*Api, Error));

	auto* ClientCredentials = NewObject<UVaRestClientCredentialsAuthConfig>(Api);
	FVaRestApiValidationContext Context;
	ClientCredentials->Validate(Context);
	TestFalse(TEXT("client-credentials config can await runtime credentials without embedding a secret"), Context.HasErrors());
	TestTrue(TEXT("runtime credentials requirement is reported"), Context.Issues.ContainsByPredicate([](const auto& Issue) { return Issue.bWarning && Issue.Code == TEXT("Auth.RuntimeCredentials"); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestAuthConfigResponsePathsTest, "VaRest.Http.Auth.Config.ResponsePaths",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestAuthConfigResponsePathsTest::RunTest(const FString& Parameters)
{
	auto* Api = NewObject<UVaRestApi>();
	FVaRestApiEnvironment Environment;
	Environment.Name = TEXT("Local");
	Environment.BaseUrl = TEXT("https://api.invalid");
	Api->Environments = { Environment };
	Api->DefaultEnvironment = Environment.Name;
	Api->ShippingEnvironment = NAME_None;
	auto* Auth = NewObject<UVaRestBearerRefreshAuthConfig>(Api);
	Api->Auth = Auth;
	Auth->Config.AccessTokenPath = TEXT("data..access");
	Auth->Config.RefreshTokenPath = TEXT("/data/~2");
	Auth->Config.ExpiresInPath = TEXT("data[01]");
	FVaRestApiValidationContext Context;
	Api->Validate(Context);
	for (const TCHAR* Code : { TEXT("Auth.AccessTokenPath.Syntax"), TEXT("Auth.RefreshTokenPath.Syntax"), TEXT("Auth.ExpiresInPath.Syntax") })
	{
		TestTrue(Code, Context.Issues.ContainsByPredicate([&](const auto& Issue) { return !Issue.bWarning && Issue.Code == Code; }));
	}
	Auth->Config.AccessTokenPath = TEXT("/data/access_token");
	Auth->Config.RefreshTokenPath.Reset();
	Auth->Config.ExpiresInPath.Reset();
	Context = {};
	Api->Validate(Context);
	TestFalse(TEXT("a valid access path and absent optional paths validate"), Context.HasErrors());
	return true;
}

#endif
