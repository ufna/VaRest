// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "VaRestApi.h"
#include "VaRestApiEditorBridge.h"
#include "VaRestHttpSettings.h"

namespace VaRestApiValidationTest
{
void Configure(UVaRestApi* Api)
{
	FVaRestApiEnvironment Environment;
	Environment.Name = TEXT("Local");
	Environment.BaseUrl = TEXT("https://example.invalid/v1");
	Api->Environments.Add(Environment);
	Api->DefaultEnvironment = Environment.Name;
	FVaRestApiEndpoint Endpoint;
	Endpoint.Name = TEXT("GetItem");
	Endpoint.Path = TEXT("/items/{id}");
	FVaRestEndpointParam Param;
	Param.Name = TEXT("id");
	Param.PinName = TEXT("ItemId");
	Param.In = EVaRestEndpointParamLocation::Path;
	Param.Type.Kind = EVaRestTypeKind::String;
	Endpoint.Params.Add(Param);
	Api->Endpoints.Add(Endpoint);
}
} // namespace VaRestApiValidationTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestApiValidationTest, "VaRest.Http.Api.AssetValidation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestApiValidationTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UVaRestApi> Api(NewObject<UVaRestApi>());
	VaRestApiValidationTest::Configure(Api.Get());
	FVaRestApiValidationContext Valid;
	Api->Validate(Valid);
	TestFalse(TEXT("ordinary configured asset is valid"), Valid.HasErrors());
	TestTrue(TEXT("asset retries are enabled by default"), Api->Retry.IsEnabled());
	TestEqual(TEXT("asset retry count is visible as three"), Api->Retry.MaxRetries, 3);
	TestEqual(TEXT("native explicit disabled policy is unchanged"), FVaRestRetryPolicy::Disabled().MaxRetries, 0);
	const FGuid Id = Api->Endpoints[0].Id;
	Api->Endpoints[0].Name = TEXT("RenamedItem");
	TestTrue(TEXT("endpoint rename preserves GUID lookup"), Api->FindEndpoint(Id) && Api->FindEndpoint(Id)->Name == TEXT("RenamedItem"));
	TestNotNull(TEXT("name lookup uses current display identity"), Api->FindEndpoint(TEXT("RenamedItem")));
	TestNull(TEXT("old name does not create another operation"), Api->FindEndpoint(TEXT("GetItem")));
	const FVaRestApiEndpoint DuplicateEndpoint = Api->Endpoints[0];
	Api->Endpoints.Add(DuplicateEndpoint);
	FVaRestApiValidationContext Duplicate;
	Api->Validate(Duplicate);
	TestTrue(TEXT("duplicate operation name/GUID is diagnosed"), Duplicate.HasErrors());
	Api->Endpoints.Pop();
	Api->Endpoints[0].Headers.AddDefaulted_GetRef().Name = TEXT("Bad Header");
	FVaRestApiValidationContext Header;
	Api->Validate(Header);
	TestTrue(TEXT("bad authored header name is rejected"), Header.HasErrors());
	Api->Endpoints[0].Headers.Reset();
	for (const TCHAR* Verb : {TEXT("CONNECT"), TEXT("connect"), TEXT("CoNnEcT")})
	{
		Api->Endpoints[0].Verb = Verb;
		FVaRestApiValidationContext Unsupported;
		Api->Validate(Unsupported);
		TestTrue(TEXT("asset diagnoses unsupported CONNECT at its verb"), Unsupported.Issues.ContainsByPredicate([](const FVaRestApiValidationIssue& Issue) {
			return !Issue.bWarning && Issue.Code == TEXT("Endpoint.UnsupportedVerb") && Issue.PropertyPath == TEXT("Endpoints[0].Verb");
		}));
	}
	Api->Endpoints[0].Verb = TEXT("CONNECTX");
	FVaRestApiValidationContext Custom;
	Api->Validate(Custom);
	TestFalse(TEXT("a different custom token remains valid"), Custom.HasErrors());
	Api->Endpoints[0].Verb = TEXT("GET");
	Api->Endpoints[0].Params[0].Name = TEXT("missing");
	FVaRestApiValidationContext Path;
	Api->Validate(Path);
	TestTrue(TEXT("declared path parameter must occur in the template"), Path.HasErrors());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestApiAuthPathTest, "VaRest.Http.Api.AuthEndpointReference",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestApiAuthPathTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UVaRestApi> Api(NewObject<UVaRestApi>());
	VaRestApiValidationTest::Configure(Api.Get());
	TestFalse(TEXT("GET reference cannot become an auth request"), Api->ResolveEndpointPath(TEXT("GetItem")).HasValue());
	Api->Endpoints[0].Verb = TEXT("POST");
	TestFalse(TEXT("auth reference cannot leave path parameters unresolved"), Api->ResolveEndpointPath(TEXT("GetItem")).HasValue());
	Api->Endpoints[0].Path = TEXT("/token");
	auto Path = Api->ResolveEndpointPath(TEXT("GetItem"));
	if (TestTrue(TEXT("declared parameter-free POST path resolves"), Path.HasValue()))
	{
		TestEqualSensitive(TEXT("reference is exactly the endpoint path"), Path.GetValue(), FString(TEXT("/token")));
	}
	TestFalse(TEXT("unknown references refuse"), Api->ResolveEndpointPath(TEXT("Missing")).HasValue());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestApiBatchEditTest, "VaRest.Http.Api.BatchEditNotification",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestApiBatchEditTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UVaRestApi> Api(NewObject<UVaRestApi>());
	VaRestApiValidationTest::Configure(Api.Get());
	int32 Notifications = 0;
	const FDelegateHandle Handle = FVaRestApiEditorBridge::OnEdited().AddLambda([&](UVaRestApi* Changed) {
		if (Changed == Api.Get())
			++Notifications;
	});
	ON_SCOPE_EXIT
	{
		FVaRestApiEditorBridge::OnEdited().Remove(Handle);
		FVaRestJsonRules::UnregisterOwner(Api.Get());
	};
	const uint64 Before = FVaRestJsonRules::GetDefaultGeneration();
	Api->UserAgent = TEXT("asset-test");
	Api->JsonContentType = TEXT("application/vnd.example+json");
	Api->NotifyEdited();
	TestEqual(TEXT("one complete edit emits one refresh"), Notifications, 1);
	TestEqual(TEXT("one complete edit publishes one rule generation"), FVaRestJsonRules::GetDefaultGeneration(), Before + 1);
	FVaRestApiValidationContext Validation;
	Api->Validate(Validation);
	TestFalse(TEXT("explicit vendor JSON remains valid"), Validation.HasErrors());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestApiUrlAndMediaTest, "VaRest.Http.Api.UrlAndMediaValidation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestApiUrlAndMediaTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UVaRestApi> Api(NewObject<UVaRestApi>());
	VaRestApiValidationTest::Configure(Api.Get());
	auto HasIssue = [](const FVaRestApiValidationContext& Context, FName Code) {
		return Context.Issues.ContainsByPredicate([&](const FVaRestApiValidationIssue& Issue) { return Issue.Code == Code && !Issue.bWarning; });
	};
	const TCHAR* BadUrls[] = {TEXT("https://:443"), TEXT("https://user@"), TEXT("https://[]"), TEXT("https://[::1"), TEXT("https://[1:2]"), TEXT("https://[::g]"), TEXT("https://[192.0.2.1::]"), TEXT("https://[::ffff:192.000.2.1]"), TEXT("https://example.invalid:bad"), TEXT("https://example.invalid:65536"), TEXT("https://example.invalid:"), TEXT("https://example.invalid/v1#fragment"), TEXT("https://example.invalid#fragment")};
	for (const TCHAR* Url : BadUrls)
	{
		Api->Environments[0].BaseUrl = Url;
		FVaRestApiValidationContext Context;
		Api->Validate(Context);
		TestTrue(TEXT("invalid authority is diagnosed at the environment"), HasIssue(Context, TEXT("Environment.Invalid")));
	}
	const TCHAR* GoodUrls[] = {TEXT("https://example.invalid/v1"), TEXT("https://example.invalid/v1?tenant=demo"), TEXT("http://127.0.0.1:8080"), TEXT("https://[::1]:443/v1"), TEXT("https://[::ffff:192.0.2.1]"), TEXT("https://user:password@example.invalid/v1")};
	for (const TCHAR* Url : GoodUrls)
	{
		Api->Environments[0].BaseUrl = Url;
		FVaRestApiValidationContext Context;
		Api->Validate(Context);
		TestFalse(TEXT("valid native authority remains supported"), HasIssue(Context, TEXT("Environment.Invalid")));
	}
	Api->Environments[0].BaseUrl = TEXT("https://example.invalid/v1");
	const TCHAR* BadMedia[] = {TEXT("application/json; charset="), TEXT("application/json; charset"), TEXT("application/json; charset=\"unterminated"), TEXT("application/json; bad name=x"), TEXT("application/json; charset=x trailing")};
	for (const TCHAR* Media : BadMedia)
	{
		Api->JsonContentType = Media;
		Api->Endpoints[0].BodyContentType = Media;
		FVaRestApiValidationContext Context;
		Api->Validate(Context);
		TestTrue(TEXT("asset media parameter grammar is checked"), HasIssue(Context, TEXT("JSON.InvalidMediaType")));
		TestTrue(TEXT("endpoint media parameter grammar is checked"), HasIssue(Context, TEXT("Endpoint.InvalidMediaType")));
	}
	const TCHAR* GoodMedia[] = {TEXT("application/json"), TEXT("application/vnd.example+json"), TEXT("application/json; charset=utf-8"), TEXT("application/json; profile=\"a;b\""), TEXT("application/json; profile=\"a\\\"b\"")};
	for (const TCHAR* Media : GoodMedia)
	{
		Api->JsonContentType = Media;
		Api->Endpoints[0].BodyContentType = Media;
		FVaRestApiValidationContext Context;
		Api->Validate(Context);
		TestFalse(TEXT("legal asset media parameters remain supported"), HasIssue(Context, TEXT("JSON.InvalidMediaType")));
		TestFalse(TEXT("legal endpoint media parameters remain supported"), HasIssue(Context, TEXT("Endpoint.InvalidMediaType")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestApiImplicitPathTest, "VaRest.Http.Api.ImplicitPathParameterContract",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestApiImplicitPathTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UVaRestApi> Api(NewObject<UVaRestApi>());
	VaRestApiValidationTest::Configure(Api.Get());
	Api->Endpoints[0].Params.Reset();
	FVaRestApiValidationContext Implicit;
	Api->Validate(Implicit);
	TestFalse(TEXT("N9/plan10.1 preserves undeclared templates as implicit string pins"), Implicit.HasErrors());
	FVaRestEndpointParam Removed;
	Removed.Name = TEXT("id");
	Removed.PinName = TEXT("ItemId");
	Removed.In = EVaRestEndpointParamLocation::Path;
	Removed.Type.Kind = EVaRestTypeKind::String;
	Removed.bRemoved = true;
	Api->Endpoints[0].Params.Add(Removed);
	FVaRestApiValidationContext Context;
	Api->Validate(Context);
	TestTrue(TEXT("explicitly removed path input cannot satisfy the active template"), Context.Issues.ContainsByPredicate([](const FVaRestApiValidationIssue& Issue) { return Issue.Code == TEXT("Param.RemovedPathName") && !Issue.bWarning; }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestApiPreloadWarningTest, "VaRest.Http.Api.RulePreloadWarning",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestApiPreloadWarningTest::RunTest(const FString& Parameters)
{
	auto* Settings = GetMutableDefault<UVaRestHttpSettings>();
	const auto SavedApis = Settings->Apis;
	ON_SCOPE_EXIT
	{
		Settings->Apis = SavedApis;
	};
	const FString PackageName = TEXT("/Game/VaRestRulePreloadTest_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	TStrongObjectPtr<UPackage> Package(CreatePackage(*PackageName));
	TStrongObjectPtr<UVaRestApi> Api(NewObject<UVaRestApi>(Package.Get(), TEXT("Api"), RF_Public));
	VaRestApiValidationTest::Configure(Api.Get());
	auto& Type = Api->JsonTypes.AddDefaulted_GetRef();
	Type.Type = StaticEnum<EVaRestTypeKind>();
	auto& Rule = Type.Enums.AddDefaulted_GetRef();
	Rule.Enumerator = TEXT("Bool");
	Rule.Json = TEXT("logical");
	const FSoftObjectPath Identity(Api.Get());
	Settings->Apis.RemoveAll([&](const auto& Entry) { return Entry.ToSoftObjectPath() == Identity; });
	const auto HasWarning = [](const FVaRestApiValidationContext& Context) {
		return Context.Issues.ContainsByPredicate([](const auto& Issue) { return Issue.Code == TEXT("JSON.MissingSettingsApi") && Issue.bWarning; });
	};
	FVaRestApiValidationContext Missing;
	Api->Validate(Missing);
	TestTrue(TEXT("known persistent rule asset missing settings preload is a warning"), HasWarning(Missing));
	TestFalse(TEXT("missing preload alone does not invalidate the asset"), Missing.HasErrors());
	Settings->Apis.Add(TSoftObjectPtr<UVaRestApi>(Api.Get()));
	FVaRestApiValidationContext Listed;
	Api->Validate(Listed);
	TestFalse(TEXT("the exact listed soft identity suppresses the warning"), HasWarning(Listed));
	Settings->Apis.RemoveAll([&](const auto& Entry) { return Entry.ToSoftObjectPath() == Identity; });
	Api->SetFlags(RF_Transient);
	FVaRestApiValidationContext Transient;
	Api->Validate(Transient);
	TestFalse(TEXT("transient instances are not diagnosed as missing persistent assets"), HasWarning(Transient));
	Api->ClearFlags(RF_Transient);
	Api->JsonTypes.Reset();
	FVaRestApiValidationContext RuleFree;
	Api->Validate(RuleFree);
	TestFalse(TEXT("a rule-free API does not need a rules preload warning"), HasWarning(RuleFree));
	return true;
}

#endif
