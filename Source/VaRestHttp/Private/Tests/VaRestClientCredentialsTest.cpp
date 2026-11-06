// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "VaRestClientCredentials.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/ScopeExit.h"
#include "VaRestAuth.h"
#include "VaRestClientState.h"
#include "VaRestHttpTesting.h"

namespace VaRest::Http::ClientCredentialsTest
{
FVaRestFakeResponse Json(int32 Status, const char* Text)
{
	return FVaRestFakeResponse::Json(Status, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text)));
}
FVaRestClientCredentialsConfig Config()
{
	FVaRestClientCredentialsConfig Out;
	Out.ClientId = TEXT("game: id");
	Out.ClientSecret = TEXT("s: +&/秘密");
	Out.Scope = TEXT("read write");
	return Out;
}
FVaRestFakeResponse Tokens()
{
	return Json(200, "{\"access_token\":\"cc-access-123456\",\"token_type\":\"Bearer\",\"expires_in\":20}");
}
FString Body(const TArray<uint8>& Bytes)
{
	const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
	return FString(Text.Length(), Text.Get());
}
} // namespace VaRest::Http::ClientCredentialsTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestClientCredentialsWireTest, "VaRest.Http.Auth.ClientCredentials.WireAndExpiry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestClientCredentialsWireTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	for (const auto Method : {EVaRestOAuthClientAuthMethod::Basic, EVaRestOAuthClientAuthMethod::Form})
	{
		FTestRig Rig;
		ON_SCOPE_EXIT
		{
			Rig.Client.CancelAll();
			Rig.Drain();
		};
		auto Config = ClientCredentialsTest::Config();
		Config.ClientAuthMethod = Method;
		Config.TraceEndpoint = TEXT("TokenGrant");
		FScopedTraceRecorder Trace(EVaRestTraceDetail::Metadata);
		Rig.Client.WithAuth(FVaRestAuth::ClientCredentials(Config));
		Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request) {
			return Request.Url.EndsWith(TEXT("/oauth/token")) ? ClientCredentialsTest::Tokens() : ClientCredentialsTest::Json(200, "{}");
		});
		const auto First = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/resource")), First).Send();
		if (!TestTrue(TEXT("a client with no refresh token obtains its first access token"), Rig.Wait(First)))
			return false;
		if (!TestEqual(TEXT("token POST then protected resource"), Rig.Fake->NumCalls(), 2))
			return false;
		const auto& Token = Rig.Fake->GetCall(0).Request;
		TestEqualSensitive(TEXT("grant method"), Token.Verb, FString(TEXT("POST")));
		TestEqualSensitive(TEXT("grant media type"), Token.Headers.Get(TEXT("Content-Type")), FString(TEXT("application/x-www-form-urlencoded")));
		TestEqualSensitive(TEXT("request a JSON token response"), Token.Headers.Get(TEXT("Accept")), FString(TEXT("application/json")));
		const FString Form = ClientCredentialsTest::Body(Rig.Fake->ReadBody(0));
		if (Method == EVaRestOAuthClientAuthMethod::Basic)
		{
			TestEqualSensitive(TEXT("Basic body contains no client secret"), Form, FString(TEXT("grant_type=client_credentials&scope=read+write")));
			const FString Header = Token.Headers.Get(TEXT("Authorization"));
			TestTrue(TEXT("Basic uses the token request's explicit credentials"), Header.StartsWith(TEXT("Basic ")));
			FString Pair;
			TestTrue(TEXT("Basic credential is base64"), FBase64::Decode(Header.Mid(6), Pair));
			TestEqualSensitive(TEXT("RFC client Basic form-encodes both UTF-8 components first"), Pair,
				FString(TEXT("game%3A+id:s%3A+%2B%26%2F%E7%A7%98%E5%AF%86")));
		}
		else
		{
			TestFalse(TEXT("Form does not also use Basic"), Token.Headers.Contains(TEXT("Authorization")));
			TestEqualSensitive(TEXT("Form carries each credential exactly once"), Form,
				FString(TEXT("grant_type=client_credentials&scope=read+write&client_id=game%3A+id&client_secret=s%3A+%2B%26%2F%E7%A7%98%E5%AF%86")));
		}
		TestEqualSensitive(TEXT("ordinary request receives the acquired Bearer"), Rig.Fake->GetCall(1).Request.Headers.Get(TEXT("Authorization")), FString(TEXT("Bearer cc-access-123456")));
		const auto Session = Rig.Client.GetSession();
		TestTrue(TEXT("snake-case expires_in sets known lifetime/expiry without a refresh token"), Session.Lifetime == 20.0 && Session.ExpiresAt == 20.0 && Session.RefreshToken.IsEmpty());
		const uint64 GrantId = Token.RequestId;
		TestTrue(TEXT("the grant records only its authored endpoint metadata"), Trace.Recorder->For(GrantId).ContainsByPredicate([&](const FVaRestTraceEvent& Event) {
			return Event.Kind == EVaRestTraceKind::Started && Event.Payload.Get<FVaRestTraceStarted>().Endpoint == Config.TraceEndpoint && EnumHasAnyFlags(Event.Payload.Get<FVaRestTraceStarted>().Flags, EVaRestTraceRequestFlags::AuthRefresh);
		}));
		Rig.Clock->Advance(10.0);
		const auto Second = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/resource")), Second).Send();
		TestTrue(TEXT("short lifetime proactively refreshes at half lifetime"), Rig.Wait(Second));
		TestEqual(TEXT("one more grant and resource after proactive lead"), Rig.Fake->NumCalls(), 4);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestClientCredentialsSingleFlightTest, "VaRest.Http.Auth.ClientCredentials.SingleFlightAndReactive",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestClientCredentialsSingleFlightTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	ON_SCOPE_EXIT
	{
		Rig.Client.CancelAll();
		Rig.Drain();
	};
	Rig.Client.WithAuth(FVaRestAuth::ClientCredentials(ClientCredentialsTest::Config()));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	const auto A = MakeOutcome<FVaRestJson>(), B = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a")), A).Send();
	Observe(Rig.Client.Get(TEXT("/b")), B).Send();
	if (!TestTrue(TEXT("both requests wait behind one real grant"), PumpUntil([&] { return Rig.Fake->NumCalls() == 1 && FClientAccess::State(Rig.Client)->NumRefreshWaitersInternal() == 2; })))
		return false;
	TestTrue(TEXT("neither protected operation completes before the grant"), !A->IsDone() && !B->IsDone());
	Rig.Fake->Complete(0, ClientCredentialsTest::Tokens());
	TestTrue(TEXT("first waiter completes"), Rig.Wait(A));
	TestTrue(TEXT("second waiter completes"), Rig.Wait(B));
	TestEqual(TEXT("one grant plus two resources"), Rig.Fake->NumCalls(), 3);
	int32 Protected = 0;
	Rig.Fake->SetResponder([&Protected](const FVaRestTransportRequest& Request) {
		if (Request.Url.EndsWith(TEXT("/oauth/token")))
			return ClientCredentialsTest::Tokens();
		return ClientCredentialsTest::Json(++Protected == 1 ? 401 : 200, "{}");
	});
	const auto Reactive = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/protected")), Reactive).Send();
	TestTrue(TEXT("401 refreshes without a refresh token and replays once"), Rig.Wait(Reactive));
	TestEqual(TEXT("reactive protected attempt, grant and replay"), Rig.Fake->NumCalls(), 6);
	TestTrue(TEXT("reactive result succeeds after exactly two protected attempts"), Reactive->SuccessCalls == 1 && Reactive->Response.Attempts == 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestClientCredentialsIdentityTest, "VaRest.Http.Auth.ClientCredentials.IdentityAndValidation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestClientCredentialsIdentityTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	auto Config = ClientCredentialsTest::Config();
	const auto Provider = FVaRestAuth::ClientCredentials(Config);
	FVaRestSessionData Session;
	FString Identity, Rotated, Other;
	TestTrue(TEXT("valid confidential client has a pure identity before a token"), Provider->GetIdentity(Session, Identity));
	Session.AccessToken = TEXT("rotated-access-token");
	TestTrue(TEXT("token rotation retains provider identity"), Provider->GetIdentity(Session, Rotated) && Identity == Rotated);
	TestFalse(TEXT("opaque identity contains no client secret"), Identity.Contains(Config.ClientSecret));
	Config.ClientSecret += TEXT("-replacement");
	TestTrue(TEXT("secret replacement cannot assert old cache/queue continuity"), FVaRestAuth::ClientCredentials(Config)->GetIdentity(Session, Other) && Identity != Other);
	Config = ClientCredentialsTest::Config();
	Config.Scope = TEXT("admin");
	TestTrue(TEXT("different scope is another identity"), FVaRestAuth::ClientCredentials(Config)->GetIdentity(Session, Other) && Identity != Other);
	Config = ClientCredentialsTest::Config();
	Config.TokenPath = TEXT("https://API.TEST:443/token");
	FVaRestAuth::ClientCredentials(Config)->GetIdentity(Session, Identity);
	Config.TokenPath = TEXT("https://api.test/token");
	TestTrue(TEXT("absolute endpoint identity normalizes scheme/host/default port"), FVaRestAuth::ClientCredentials(Config)->GetIdentity(Session, Other) && Identity == Other);

	FString Error;
	const TCHAR* BadPaths[] = {TEXT(""), TEXT("https://:443/token"), TEXT("https://[1:2]/token"), TEXT("https://[::ffff:192.000.2.1]/token"), TEXT("ftp://api.test/token"), TEXT("/token/{id}"), TEXT("/token#fragment"), TEXT(" /token"), TEXT("/token\n")};
	for (const TCHAR* Path : BadPaths)
	{
		Config = ClientCredentialsTest::Config();
		Config.TokenPath = Path;
		TestFalse(TEXT("unusable grant URL/path is refused before a request"), Config.Validate(Error));
		TestFalse(TEXT("validation error does not quote credentials"), Error.Contains(Config.ClientSecret));
	}
	for (const TCHAR* Path : {TEXT("tokens..access"), TEXT("tokens[01]"), TEXT("tokens."), TEXT("/a~2b"), TEXT("/"), TEXT("[0]")})
	{
		Config = ClientCredentialsTest::Config();
		Config.AccessTokenPath = Path;
		TestFalse(TEXT("invalid token response path is refused"), Config.Validate(Error));
	}
	Config = ClientCredentialsTest::Config();
	Config.AccessTokenPath = TEXT("/tokens/a~1b");
	TestTrue(TEXT("valid pointer escaping is accepted"), Config.Validate(Error));
	Config.ClientSecret.Reset();
	TestFalse(TEXT("runtime provider requires credentials"), Config.Validate(Error));
	TestTrue(TEXT("asset structural validation permits later runtime credentials"), Config.Validate(Error, false));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestClientCredentialsResponseTest, "VaRest.Http.Auth.ClientCredentials.ResponseOutcomes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestClientCredentialsResponseTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	for (int32 Status : {200, 400, 408, 429, 500})
	{
		FTestRig Rig;
		ON_SCOPE_EXIT
		{
			Rig.Client.CancelAll();
			Rig.Drain();
		};
		Rig.Client.WithAuth(FVaRestAuth::ClientCredentials(ClientCredentialsTest::Config()));
		Rig.Client.SetSession(TEXT("kept-access-123456"), TEXT(""));
		Rig.Fake->Enqueue(ClientCredentialsTest::Json(Status, "{}"));
		using FAnswer = TOptional<TPair<EVaRestAuthRefreshOutcome, EVaRestErrorKind>>;
		const auto Answer = MakeShared<FAnswer, ESPMode::ThreadSafe>();
		Rig.Client.RefreshAuth([Answer](EVaRestAuthRefreshOutcome Outcome, EVaRestErrorKind Kind) { *Answer = TPair<EVaRestAuthRefreshOutcome, EVaRestErrorKind>(Outcome, Kind); });
		if (!TestTrue(TEXT("the real grant reports its result"), PumpUntil([Answer] { return Answer->IsSet(); })))
			return false;
		const bool bRejected = Status == 200 || Status == 400;
		TestEqual(TEXT("only endpoint refusal or malformed success expires the session"), Answer->GetValue().Key,
			bRejected ? EVaRestAuthRefreshOutcome::Rejected : EVaRestAuthRefreshOutcome::Failed);
		TestEqualSensitive(TEXT("transient failure retains the access token"), Rig.Client.GetSession().AccessToken,
			bRejected ? FString() : FString(TEXT("kept-access-123456")));
		if (!bRejected)
			TestEqual(TEXT("HTTP transient grant failure remains retryable Connection"), Answer->GetValue().Value, EVaRestErrorKind::Connection);
	}
	FTestRig Rig;
	ON_SCOPE_EXIT
	{
		Rig.Client.CancelAll();
		Rig.Drain();
	};
	auto Config = ClientCredentialsTest::Config();
	Config.AccessTokenPath = TEXT("data.credential");
	Config.ExpiresInPath = TEXT("data.ttl");
	Rig.Client.WithAuth(FVaRestAuth::ClientCredentials(Config));
	Rig.Fake->Enqueue(ClientCredentialsTest::Json(200, "{\"data\":{\"credential\":\"custom-access-123456\",\"ttl\":\"90\"},\"refresh_token\":\"not-a-refresh-grant\"}"));
	const auto Answer = MakeShared<TOptional<EVaRestAuthRefreshOutcome>, ESPMode::ThreadSafe>();
	Rig.Client.RefreshAuth([Answer](EVaRestAuthRefreshOutcome Outcome, EVaRestErrorKind) { *Answer = Outcome; });
	TestTrue(TEXT("custom response paths finish"), PumpUntil([Answer] { return Answer->IsSet(); }));
	const auto Session = Rig.Client.GetSession();
	TestTrue(TEXT("custom token and expiry are used, without importing a refresh token"), Session.AccessToken == TEXT("custom-access-123456") && Session.Lifetime == 90.0 && Session.RefreshToken.IsEmpty());
	TArray<FString> Keys;
	FVaRestAuth::ClientCredentials(Config)->GetRedactedKeys(Keys);
	TestTrue(TEXT("custom response leaf joins protected keys before delivery"), Keys.Contains(TEXT("credential")) && Keys.Contains(TEXT("client_secret")));
	TArray<TPair<FName, FString>> Secrets;
	FVaRestAuth::ClientCredentials(Config)->GetSecretValues(Secrets);
	const auto Redactor = FClientAccess::State(Rig.Client)->GetRedactor();
	for (const auto& Secret : Secrets)
		TestFalse(TEXT("raw and computed credentials are installed in the real client redactor"), Redactor->ScrubText(Secret.Value).Contains(Secret.Value, ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestClientCredentialsRefusalTest, "VaRest.Http.Auth.ClientCredentials.PreWireValidation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestClientCredentialsRefusalTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	struct FBadConfig
	{
		const TCHAR* Url;
		const TCHAR* TokenPath;
	};
	const FBadConfig Bad[] = {
		{TEXT("/oauth/token"), TEXT("/data/a&b")},
		{TEXT("/oauth/token"), TEXT("/data/a?b")},
		{TEXT("/oauth/token"), TEXT("/data/a;b")},
		{TEXT("/oauth/token"), TEXT("/data/a#b")},
		{TEXT("/oauth/token"), TEXT("/data/a=b")},
		{TEXT("/oauth/token"), TEXT("/data/._-")},
		{TEXT("/oauth/token"), TEXT("/0")},
		{TEXT("/oauth/token"), TEXT("/0/123")},
		{TEXT("https://[1:2]/token"), TEXT("access_token")},
		{TEXT("https://[::ffff:192.000.2.1]/token"), TEXT("access_token")}};
	for (const auto& Case : Bad)
	{
		FTestRig Rig;
		ON_SCOPE_EXIT
		{
			Rig.Client.CancelAll();
			Rig.Drain();
		};
		auto Config = ClientCredentialsTest::Config();
		Config.TokenPath = Case.Url;
		Config.AccessTokenPath = Case.TokenPath;
		FString Error;
		TestFalse(TEXT("unprotectable token keys and malformed token authorities fail configuration"), Config.Validate(Error));
		AddExpectedErrorPlain(TEXT("FVaRestAuth::ClientCredentials:"), EAutomationExpectedErrorFlags::Contains, 1);
		const auto Auth = FVaRestAuth::ClientCredentials(Config);
		TestFalse(TEXT("the real provider retains its Config refusal"), Auth->GetConfigError().IsEmpty());
		Rig.Client.WithAuth(Auth);
		const auto Outcome = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/protected")), Outcome).Send();
		TestTrue(TEXT("the rejected provider completes the logical request"), Rig.Wait(Outcome));
		TestEqual(TEXT("refusal uses the normal Config outcome"), Outcome->Error.Kind, EVaRestErrorKind::Config);
		TestEqual(TEXT("no grant or protected request reaches transport"), Rig.Fake->NumCalls(), 0);
	}
	for (const TCHAR* Url : {TEXT("https://[::1]:443/token"), TEXT("https://[::ffff:192.0.2.1]/token"), TEXT("https://example.invalid/oauth/token")})
	{
		auto Config = ClientCredentialsTest::Config();
		Config.TokenPath = Url;
		FString Error;
		TestTrue(TEXT("valid compressed, embedded-IPv4 and ordinary authorities remain supported"), Config.Validate(Error));
	}
	for (const TCHAR* Path : {TEXT("/tokens/0"), TEXT("/items/0"), TEXT("/0/access"), TEXT("/data/a~1b"), TEXT("0"), TEXT("[0].0")})
	{
		auto Config = ClientCredentialsTest::Config();
		Config.AccessTokenPath = Path;
		FString Error;
		TestTrue(TEXT("paths with an actual named object anchor remain supported"), Config.Validate(Error));
	}
	FTestRig Rig;
	auto Config = ClientCredentialsTest::Config();
	Config.AccessTokenPath = TEXT("/data/a~1b");
	FString Error;
	TestTrue(TEXT("safe pointer escaping remains a supported custom token path"), Config.Validate(Error));
	Rig.Client.WithAuth(FVaRestAuth::ClientCredentials(Config));
	const auto BeforeAnyGrant = FClientAccess::State(Rig.Client)->GetRedactor();
	TestTrue(TEXT("the real published redactor protects the unescaped JSON key before any token installation"), BeforeAnyGrant->GetRules().ShouldRedact(TEXTVIEW("a/b"), EVaRestRedactKeyKind::Json));
	TestFalse(TEXT("the future response value is not a previously registered value"), BeforeAnyGrant->GetSecrets().Contains(TEXT("fresh-opaque-value-123456")));
	Config.AccessTokenPath = TEXT("/items/0");
	Rig.Client.WithAuth(FVaRestAuth::ClientCredentials(Config));
	TestTrue(TEXT("an enclosing nonsecret object key protects its selected array element before the grant"), FClientAccess::State(Rig.Client)->GetRedactor()->GetRules().ShouldRedact(TEXTVIEW("items"), EVaRestRedactKeyKind::Json));
	return true;
}

#endif
