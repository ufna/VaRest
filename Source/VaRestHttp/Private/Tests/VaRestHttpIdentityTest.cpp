// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestAuth.h"
#include "VaRestHttpTesting.h"
#include "VaRestIdentity.h"
#include "VaRestJson.h"
#include "Misc/DateTime.h"
#include "Misc/Timespan.h"

namespace VaRest::Http::IdentityTestPrivate
{
	FString Key(const FVaRestSessionData& Data)
	{
		return CacheKey(TEXT("GET"), TEXT("https://example.test/items"), SessionScope(Data), {}, Data.Headers);
	}

	class FUnknownAuth : public IVaRestAuth
	{
	public:
		virtual FName GetAuthName() const override { return TEXT("Unknown"); }
		virtual bool Apply(FVaRestAuthTarget& Target) const override { ++ApplyCalls; return true; }
		mutable int32 ApplyCalls = 0;
	};

	class FInvalidAuth final : public FUnknownAuth
	{
	public:
		virtual FString GetConfigError() const override { return TEXT("invalid configuration"); }
	};

	class FDirtyIdentityAuth final : public FUnknownAuth
	{
	public:
		virtual bool GetIdentity(const FVaRestSessionData& Session, FString& OutIdentity) const override
		{
			OutIdentity = TEXT("must not escape a failed query");
			return false;
		}
	};
	class FHeldRefreshAuth final : public FUnknownAuth
	{
	public:
		virtual bool CanRefresh(const FVaRestSessionData& Session) const override { return Session.HasRefreshToken(); }
		virtual void Refresh(const FVaRestAuthRefreshContext& Context, TUniqueFunction<void(FVaRestAuthRefreshResult&&)>&& OnDone) override
		{
			Pending = MoveTemp(OnDone);
		}
		TUniqueFunction<void(FVaRestAuthRefreshResult&&)> Pending;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpIdentityImportTest, "VaRest.Http.Identity.ImportBinding",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpIdentityImportTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::IdentityTestPrivate;
	FTestRig First;
	First.Client.WithAuth(FVaRestAuth::Bearer());
	First.Client.SetSession(TEXT("account-A-token"), TEXT("account-A-refresh"));
	First.Client.SetSessionHeader(TEXT("X-Tenant"), TEXT("tenant-A"));
	const FGuid Original = First.Client.GetSession().IdentityId;
	const FString OriginalScope = SessionScope(First.Client.GetSession());
	const FString OriginalKey = Key(First.Client.GetSession());
	const FString Exported = First.Client.ExportSession();
	FTestRig Restored;
	Restored.Client.WithAuth(FVaRestAuth::Bearer());
	if (!TestTrue(TEXT("valid export imports"), Restored.Client.ImportSession(Exported))) return false;
	TestTrue(TEXT("restart keeps identity"), Restored.Client.GetSession().IdentityId == Original);
	TestTrue(TEXT("restart: scope"), SessionScope(Restored.Client.GetSession()) == OriginalScope);
	TestTrue(TEXT("restart: cache key"), Key(Restored.Client.GetSession()) == OriginalKey);
	TestTrue(TEXT("repeat import succeeds"), Restored.Client.ImportSession(Exported));
	TestTrue(TEXT("unchanged tokens keep restored identity"), Restored.Client.GetSession().IdentityId == Original);
	TestTrue(TEXT("repeat: scope"), SessionScope(Restored.Client.GetSession()) == OriginalScope);
	TestTrue(TEXT("repeat: cache key"), Key(Restored.Client.GetSession()) == OriginalKey);
	Restored.Client.SetBearerToken(TEXT("account-A-rotated"), true);
	TestTrue(TEXT("refresh keeps identity"), Restored.Client.GetSession().IdentityId == Original);
	TestTrue(TEXT("refresh: scope"), SessionScope(Restored.Client.GetSession()) == OriginalScope);
	TestTrue(TEXT("refresh: cache key"), Key(Restored.Client.GetSession()) == OriginalKey);
	Restored.Client.SetBearerToken(TEXT("account-B-token"));
	TestTrue(TEXT("replacement separates"), Restored.Client.GetSession().IdentityId != Original);
	TestTrue(TEXT("replacement: scope"), SessionScope(Restored.Client.GetSession()) != OriginalScope);
	TestTrue(TEXT("replacement: cache key"), Key(Restored.Client.GetSession()) != OriginalKey);
	const auto Parsed = FVaRestJson::Parse(Exported);
	if (!TestTrue(TEXT("export JSON"), Parsed.HasValue())) return false;
	FVaRestJson Mixed = Parsed.GetValue().Clone();
	Mixed.Set(TEXT("accessToken"), TEXT("account-B-token"));
	TestTrue(TEXT("valid credentials, stale binding still imports"), Restored.Client.ImportSession(Mixed.ToString()));
	TestTrue(TEXT("stale binding never restores A"), Restored.Client.GetSession().IdentityId != Original);
	TestTrue(TEXT("mixed tokens: scope"), SessionScope(Restored.Client.GetSession()) != OriginalScope);
	TestTrue(TEXT("mixed tokens: cache key"), Key(Restored.Client.GetSession()) != OriginalKey);
	Mixed.Set(TEXT("accessToken"), 42);
	const FVaRestSessionData BeforeInvalid = Restored.Client.GetSession();
	TestFalse(TEXT("bad token type rejected"), Restored.Client.ImportSession(Mixed.ToString()));
	TestTrue(TEXT("invalid import did not mutate identity"), Restored.Client.GetSession().IdentityId == BeforeInvalid.IdentityId);
	TestEqual(TEXT("invalid import did not mutate token"), Restored.Client.GetSession().AccessToken, BeforeInvalid.AccessToken);
	TestEqual(TEXT("invalid import retains scope"), SessionScope(Restored.Client.GetSession()), SessionScope(BeforeInvalid));
	TestEqual(TEXT("invalid import retains cache key"), Key(Restored.Client.GetSession()), Key(BeforeInvalid));
	const FGuid LiveBeforeTrusted=Restored.Client.GetSession().IdentityId;
	TestTrue(TEXT("trusted same identity with different valid export"),Restored.Client.ImportSession(Exported,true));
	TestTrue(TEXT("explicit live assertion overrides export ID"),Restored.Client.GetSession().IdentityId==LiveBeforeTrusted);
	Restored.Client.ClearSession();
	TestTrue(TEXT("same flag without live session restores valid export"),Restored.Client.ImportSession(Exported,true));
	TestTrue(TEXT("no-live restores A"),Restored.Client.GetSession().IdentityId==Original);
	TestTrue(TEXT("no live restore: scope"), SessionScope(Restored.Client.GetSession()) == OriginalScope);
	TestTrue(TEXT("no live restore: cache key"), Key(Restored.Client.GetSession()) == OriginalKey);
	FVaRestJson ChangedHeaders=Parsed.GetValue().Clone();
	ChangedHeaders.Set(TEXT("headers"),FVaRestJson::Array().Add(FVaRestJson::Object().Set(TEXT("name"),TEXT("X-Tenant")).Set(TEXT("value"),TEXT("tenant-B"))));
	TestTrue(TEXT("header edit remains valid legacy input"),Restored.Client.ImportSession(ChangedHeaders.ToString()));
	TestTrue(TEXT("changed tenant invalidates export binding"),Restored.Client.GetSession().IdentityId!=Original);
	TestTrue(TEXT("tenant change: scope"), SessionScope(Restored.Client.GetSession()) != OriginalScope);
	TestTrue(TEXT("tenant change: cache key"), Key(Restored.Client.GetSession()) != OriginalKey);
	FVaRestJson Legacy=Parsed.GetValue().Clone();
	Legacy.Remove(TEXT("cacheIdentity")); Legacy.Remove(TEXT("cacheBinding"));
	TestTrue(TEXT("old export imports"),Restored.Client.ImportSession(Legacy.ToString()));
	TestTrue(TEXT("old export gets fresh namespace"),Restored.Client.GetSession().IdentityId!=Original);
	TestTrue(TEXT("legacy: scope"), SessionScope(Restored.Client.GetSession()) != OriginalScope);
	TestTrue(TEXT("legacy: cache key"), Key(Restored.Client.GetSession()) != OriginalKey);
	TestTrue(TEXT("framing distinguishes ambiguous concatenation"),
	    HashFields(TEXT("test"), TArray<FString>{TEXT("ab"), TEXT("c")}) != HashFields(TEXT("test"), TArray<FString>{TEXT("a"), TEXT("bc")}));
	TestTrue(TEXT("domains differ"), HashFields(TEXT("one"), TArray<FString>{TEXT("x")}) != HashFields(TEXT("two"), TArray<FString>{TEXT("x")}));
	TestTrue(TEXT("drains"), First.Drain() && Restored.Drain());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpIdentityImportEdgesTest, "VaRest.Http.Identity.ImportEdges",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpIdentityImportEdgesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::IdentityTestPrivate;
	FTestRig Source;
	Source.Client.SetSession(TEXT("account-token"), TEXT("refresh-token"));
	Source.Client.SetSessionHeader(TEXT("X-Tenant"), TEXT("tenant-A"));
	const auto Parsed = FVaRestJson::Parse(Source.Client.ExportSession());
	if (!TestTrue(TEXT("export parses"), Parsed.HasValue())) return false;
	const FVaRestJson Baseline = Parsed.GetValue();
	const FString OriginalScope = SessionScope(Source.Client.GetSession());
	const FString OriginalKey = Key(Source.Client.GetSession());
	FTestRig Target;
	for (int32 Variant = 0; Variant < 8; ++Variant)
	{
		FVaRestJson Edited = Baseline.Clone();
		switch (Variant)
		{
		case 0: Edited.Remove(TEXT("cacheIdentity")); Edited.Remove(TEXT("cacheBinding")); break;
		case 1: Edited.Set(TEXT("cacheIdentity"), TEXT("not-a-guid")); break;
		case 2: Edited.Set(TEXT("cacheBinding"), TEXT("bad-hash")); break;
		case 3: Edited.Set(TEXT("cacheIdentity"), 12); break;
		case 4: Edited.Set(TEXT("cacheBinding"), 12); break;
		case 5: Edited.Set(TEXT("cacheBinding"), Baseline[TEXT("cacheBinding")].AsString().ToUpper()); break;
		case 6: Edited.Set(TEXT("accessToken"), TEXT("different-account")); break;
		case 7: Edited.Set(TEXT("headers"), FVaRestJson::Array().Add(FVaRestJson::Object()
			.Set(TEXT("name"), TEXT("X-Tenant")).Set(TEXT("value"), TEXT("tenant-B")))); break;
		}
		for (const bool bSame : {false, true})
		{
			Target.Client.ClearSession();
			if (!TestTrue(TEXT("malformed additive fields still import without live state"), Target.Client.ImportSession(Edited.ToString(), bSame))) return false;
			TestTrue(TEXT("unbound import has fresh scope"), SessionScope(Target.Client.GetSession()) != OriginalScope);
			TestTrue(TEXT("unbound import has fresh key"), Key(Target.Client.GetSession()) != OriginalKey);
			if (!TestTrue(TEXT("restore baseline before live import"), Target.Client.ImportSession(Baseline.ToString()))) return false;
			if (!TestTrue(TEXT("live edited import"), Target.Client.ImportSession(Edited.ToString(), bSame))) return false;
			TestTrue(TEXT("live caller assertion preserves ID only when requested"),
				(Target.Client.GetSession().IdentityId == Source.Client.GetSession().IdentityId) == bSame);
			const bool bSameScope = bSame && Variant != 7;
			TestTrue(TEXT("live scope follows assertion and tenant"), (SessionScope(Target.Client.GetSession()) == OriginalScope) == bSameScope);
			TestTrue(TEXT("live key follows assertion and tenant"), (Key(Target.Client.GetSession()) == OriginalKey) == bSameScope);
		}
	}
	FVaRestJson ExpiryOnly = Baseline.Clone();
	ExpiryOnly.Set(TEXT("expiresAt"), (FDateTime::UtcNow() + FTimespan::FromHours(1)).ToIso8601());
	if (!TestTrue(TEXT("expiry is not account identity"), Target.Client.ImportSession(ExpiryOnly.ToString()))) return false;
	TestEqual(TEXT("expiry-only scope stable"), SessionScope(Target.Client.GetSession()), OriginalScope);
	TestEqual(TEXT("expiry-only key stable"), Key(Target.Client.GetSession()), OriginalKey);
	for (int32 Variant = 0; Variant < 4; ++Variant)
	{
		FVaRestJson Invalid = Baseline.Clone();
		if (Variant == 0) Invalid.Set(TEXT("refreshToken"), 42);
		if (Variant == 1) Invalid.Set(TEXT("expiresAt"), TEXT("invalid-time"));
		if (Variant == 2) Invalid.Set(TEXT("headers"), 42);
		if (Variant == 3) Invalid.Set(TEXT("headers"), FVaRestJson::Array().Add(FVaRestJson::Object()
			.Set(TEXT("name"), TEXT("invalid name")).Set(TEXT("value"), TEXT("value"))));
		const FVaRestSessionData BeforeData = Target.Client.GetSession();
		TestFalse(TEXT("invalid original field rejected"), Target.Client.ImportSession(Invalid.ToString(), true));
		TestEqual(TEXT("invalid original field leaves scope"), SessionScope(Target.Client.GetSession()), SessionScope(BeforeData));
		TestEqual(TEXT("invalid original field leaves key"), Key(Target.Client.GetSession()), Key(BeforeData));
		TestEqual(TEXT("invalid original field leaves generation"), Target.Client.GetSession().Generation, BeforeData.Generation);
		TestEqual(TEXT("invalid original field leaves expiry"), Target.Client.GetSession().ExpiresAt, BeforeData.ExpiresAt);
		TestEqual(TEXT("invalid original field leaves tokens"), Target.Client.GetSession().RefreshToken, BeforeData.RefreshToken);
	}
	FVaRestJson Logout = Baseline.Clone();
	Logout.Set(TEXT("accessToken"), TEXT("")); Logout.Set(TEXT("refreshToken"), TEXT(""));
	if (!TestTrue(TEXT("empty tokens are logout even with same flag"), Target.Client.ImportSession(Logout.ToString(), true))) return false;
	TestTrue(TEXT("logout clears headers too"), Target.Client.GetSession().Headers.IsEmpty());
	TestTrue(TEXT("logout separates scope"), SessionScope(Target.Client.GetSession()) != OriginalScope);
	TestTrue(TEXT("logout separates key"), Key(Target.Client.GetSession()) != OriginalKey);
	TestTrue(TEXT("source drains"), Source.Drain());
	TestTrue(TEXT("target drains"), Target.Drain());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpIdentityAuthTest, "VaRest.Http.Identity.AuthScopes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpIdentityAuthTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::IdentityTestPrivate;
	FVaRestSessionData First;
	First.AccessToken = TEXT("token");
	FVaRestSessionData Restart;
	Restart.AccessToken = First.AccessToken;
	const auto Bearer = FVaRestAuth::Bearer();
	FString BearerIdentity;
	if (!TestTrue(TEXT("bearer identity"), AuthIdentity(&Bearer.Get(), EVaRestAuthMode::Required, First, BearerIdentity))) return false;
	FString Anonymous;
	if (!TestTrue(TEXT("NoAuth identity"), AuthIdentity(&Bearer.Get(), EVaRestAuthMode::None, First, Anonymous))) return false;
	const auto RequestKey = [](const FString& Identity, const FVaRestHeaders& Headers = FVaRestHeaders())
	{
		return CacheKey(TEXT("GET"), TEXT("https://example.test/items"), Identity, {}, Headers);
	};
	TestTrue(TEXT("NoAuth and bearer cache namespaces differ"), RequestKey(Anonymous) != RequestKey(BearerIdentity));
	FString Plain;
	TestTrue(TEXT("no provider identity"), AuthIdentity(nullptr, EVaRestAuthMode::Required, First, Plain));
	TestEqual(TEXT("no provider no headers is anonymous"), RequestKey(Plain), RequestKey(Anonymous));
	for (int32 Kind = 0; Kind < 3; ++Kind)
	{
		const auto MakeAuth = [Kind](const TCHAR* Secret)
		{
			return Kind == 0 ? FVaRestAuth::Basic(TEXT("user"), Secret) :
				FVaRestAuth::ApiKey(Kind == 1 ? EVaRestApiKeyPlacement::Header : EVaRestApiKeyPlacement::Query, TEXT("Api-Key"), Secret);
		};
		const auto OriginalAuth = MakeAuth(TEXT("secret"));
		const auto RecreatedAuth = MakeAuth(TEXT("secret"));
		const auto OtherAuth = MakeAuth(TEXT("other-secret"));
		FString OriginalIdentity, RestartIdentity, OtherIdentity;
		if (!TestTrue(TEXT("built-in identity"), AuthIdentity(&OriginalAuth.Get(), EVaRestAuthMode::Required, First, OriginalIdentity))) return false;
		if (!TestTrue(TEXT("recreated identity"), AuthIdentity(&RecreatedAuth.Get(), EVaRestAuthMode::Required, Restart, RestartIdentity))) return false;
		if (!TestTrue(TEXT("other credential identity"), AuthIdentity(&OtherAuth.Get(), EVaRestAuthMode::Required, Restart, OtherIdentity))) return false;
		TestEqual(TEXT("credential identity independent of session GUID"), RequestKey(OriginalIdentity), RequestKey(RestartIdentity));
		TestTrue(TEXT("new credential separates key"), RequestKey(OtherIdentity) != RequestKey(OriginalIdentity));
		FVaRestSessionData Tenant = Restart;
		Tenant.Headers.Set(TEXT("X-Tenant"), TEXT("other"));
		FString TenantIdentity;
		if (!TestTrue(TEXT("tenant identity"), AuthIdentity(&RecreatedAuth.Get(), EVaRestAuthMode::Required, Tenant, TenantIdentity))) return false;
		TestTrue(TEXT("tenant changes auth scope and key"), TenantIdentity != RestartIdentity && RequestKey(TenantIdentity) != RequestKey(RestartIdentity));
	}
	FVaRestHeaders Built;
	Built.Set(TEXT("X-Tenant"), TEXT("built"));
	Built.Set(TEXT("Accept"), TEXT("application/json"));
	FVaRestHeaders Live;
	Live.Set(TEXT("x-tenant"), TEXT("live"));
	FVaRestHeaders Canonical;
	Canonical.Set(TEXT("accept"), TEXT("application/json"));
	Canonical.Set(TEXT("X-TENANT"), TEXT("live"));
	TestEqual(TEXT("key canonicalizes names/order, overlays session and strips fragment"),
		CacheKey(TEXT("GET"), TEXT("https://example.test/items#fragment"), Anonymous, Built, Live),
		CacheKey(TEXT("GET"), TEXT("https://example.test/items"), Anonymous, Canonical, {}));
	TestTrue(TEXT("verb separates keys"), CacheKey(TEXT("HEAD"), TEXT("https://example.test/items"), Anonymous, {}, {}) != RequestKey(Anonymous));
	const auto HeaderUpper = FVaRestAuth::ApiKey(EVaRestApiKeyPlacement::Header, TEXT("X-API"), TEXT("key"));
	const auto HeaderLower = FVaRestAuth::ApiKey(EVaRestApiKeyPlacement::Header, TEXT("x-api"), TEXT("key"));
	const auto QueryUpper = FVaRestAuth::ApiKey(EVaRestApiKeyPlacement::Query, TEXT("X-API"), TEXT("key"));
	const auto QueryLower = FVaRestAuth::ApiKey(EVaRestApiKeyPlacement::Query, TEXT("x-api"), TEXT("key"));
	FString HeaderUpperId, HeaderLowerId, QueryUpperId, QueryLowerId;
	HeaderUpper->GetIdentity(First, HeaderUpperId); HeaderLower->GetIdentity(First, HeaderLowerId);
	QueryUpper->GetIdentity(First, QueryUpperId); QueryLower->GetIdentity(First, QueryLowerId);
	TestEqual(TEXT("API header names fold case"), RequestKey(HeaderUpperId), RequestKey(HeaderLowerId));
	TestTrue(TEXT("API query names retain case"), RequestKey(QueryUpperId) != RequestKey(QueryLowerId));
	TestTrue(TEXT("API placement separates key"), RequestKey(QueryUpperId) != RequestKey(HeaderUpperId));
	AddExpectedError(TEXT("the user holds ':'"), EAutomationExpectedErrorFlags::Contains, 1);
	const auto InvalidBasic = FVaRestAuth::Basic(TEXT("bad:user"), TEXT("password"));
	FString InvalidOutput = TEXT("seeded-output");
	TestFalse(TEXT("invalid built-in auth has no scope"), AuthIdentity(&InvalidBasic.Get(), EVaRestAuthMode::Required, First, InvalidOutput));
	TestTrue(TEXT("invalid built-in query clears seeded output"), InvalidOutput.IsEmpty());
	FUnknownAuth Unknown;
	FInvalidAuth Invalid;
	FDirtyIdentityAuth Dirty;
	for (const IVaRestAuth* Provider : {static_cast<const IVaRestAuth*>(&Unknown), static_cast<const IVaRestAuth*>(&Invalid), static_cast<const IVaRestAuth*>(&Dirty)})
	{
		FString Output = TEXT("seeded-output");
		TestFalse(TEXT("unknown/invalid identity bypasses cache"), AuthIdentity(Provider, EVaRestAuthMode::Required, First, Output));
		TestTrue(TEXT("every false path resets output"), Output.IsEmpty());
		TestTrue(TEXT("failed identity cannot supply a reusable key"), RequestKey(Output) != RequestKey(BearerIdentity));
	}
	TestEqual(TEXT("custom auth never applied speculatively"), Unknown.ApplyCalls + Invalid.ApplyCalls + Dirty.ApplyCalls, 0);
	FVaRestSessionData Empty;
	FString EmptyIdentity = TEXT("seeded-output");
	TestFalse(TEXT("empty bearer has no identity"), AuthIdentity(&Bearer.Get(), EVaRestAuthMode::Required, Empty, EmptyIdentity));
	TestTrue(TEXT("empty bearer clears identity"), EmptyIdentity.IsEmpty());
	const auto RefreshAuth = FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig());
	Empty.RefreshToken = TEXT("refresh-only");
	FString RefreshIdentity;
	TestTrue(TEXT("refresh-only credential identity"), AuthIdentity(&RefreshAuth.Get(), EVaRestAuthMode::Required, Empty, RefreshIdentity));
	FString PlainBearerIdentity;
	TestTrue(TEXT("bearer also recognizes refresh-only identity"), AuthIdentity(&Bearer.Get(), EVaRestAuthMode::Required, Empty, PlainBearerIdentity));
	TestEqual(TEXT("bearer variants share account key"), RequestKey(RefreshIdentity), RequestKey(PlainBearerIdentity));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpIdentityTransitionsTest, "VaRest.Http.Identity.Transitions",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpIdentityTransitionsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::IdentityTestPrivate;
	FTestRig Rig;
	Rig.Client.WithAuth(FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig()));
	Rig.Client.SetSession(TEXT("original-token"), TEXT("original-refresh"));
	const FString OriginalScope = SessionScope(Rig.Client.GetSession());
	const FString OriginalKey = Key(Rig.Client.GetSession());
	Rig.Client.SetSession(TEXT("original-token"), TEXT("original-refresh"));
	TestEqual(TEXT("identical setter scope"), SessionScope(Rig.Client.GetSession()), OriginalScope);
	TestEqual(TEXT("identical setter key"), Key(Rig.Client.GetSession()), OriginalKey);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&)
	{
		return FVaRestFakeResponse::Json(200, FUtf8StringView(UTF8TEXT("{\"accessToken\":\"rotated-token\",\"refreshToken\":\"rotated-refresh\"}")));
	});
	const auto Done = MakeShared<bool, ESPMode::ThreadSafe>(false);
	const auto Refreshed = MakeShared<bool, ESPMode::ThreadSafe>(false);
	Rig.Client.RefreshAuth([Done, Refreshed](EVaRestAuthRefreshOutcome Outcome, EVaRestErrorKind)
	{
		*Refreshed = Outcome == EVaRestAuthRefreshOutcome::Refreshed;
		*Done = true;
	});
	if (!TestTrue(TEXT("refresh finishes"), PumpUntil([Done] { return *Done; }))) return false;
	TestTrue(TEXT("refresh succeeded"), *Refreshed);
	TestEqual(TEXT("actual refresh preserves scope"), SessionScope(Rig.Client.GetSession()), OriginalScope);
	TestEqual(TEXT("actual refresh preserves key"), Key(Rig.Client.GetSession()), OriginalKey);
	Rig.Client.SetSessionHeader(TEXT("X-Tenant"), TEXT("other-tenant"));
	const FString TenantScope = SessionScope(Rig.Client.GetSession());
	const FString TenantKey = Key(Rig.Client.GetSession());
	TestTrue(TEXT("live header changes scope"), TenantScope != OriginalScope);
	TestTrue(TEXT("live header changes key"), TenantKey != OriginalKey);
	Rig.Client.NotifySessionExpired();
	TestTrue(TEXT("expiry changes scope with headers retained"), SessionScope(Rig.Client.GetSession()) != TenantScope);
	TestTrue(TEXT("expiry changes key with headers retained"), Key(Rig.Client.GetSession()) != TenantKey);
	const FString ExpiredScope = SessionScope(Rig.Client.GetSession());
	Rig.Client.NotifySessionExpired();
	TestEqual(TEXT("repeat expiry no-op"), SessionScope(Rig.Client.GetSession()), ExpiredScope);
	Rig.Client.ClearSession();
	const FString ClearedScope = SessionScope(Rig.Client.GetSession());
	const FString ClearedKey = Key(Rig.Client.GetSession());
	TestTrue(TEXT("clear changes expired header scope"), ClearedScope != ExpiredScope);
	Rig.Client.ClearSession();
	TestEqual(TEXT("empty clear keeps scope"), SessionScope(Rig.Client.GetSession()), ClearedScope);
	TestEqual(TEXT("empty clear keeps key"), Key(Rig.Client.GetSession()), ClearedKey);
	TestTrue(TEXT("drain"), Rig.Drain());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpIdentityRefreshWaiterTest, "VaRest.Http.Identity.ImportBeforeRefreshWaiter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpIdentityRefreshWaiterTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::IdentityTestPrivate;
	FTestRig Saved;
	Saved.Client.SetSession(TEXT("restored-access"), TEXT("restored-refresh"));
	Saved.Client.SetSessionHeader(TEXT("X-Tenant"), TEXT("restored-tenant"));
	const FString ExpectedScope = SessionScope(Saved.Client.GetSession());
	const FString ExpectedKey = Key(Saved.Client.GetSession());
	FTestRig Live;
	const auto Auth = MakeShared<FHeldRefreshAuth, ESPMode::ThreadSafe>();
	Live.Client.WithAuth(Auth);
	Live.Client.SetSession(TEXT("old-access"), TEXT("old-refresh"));
	const auto SeenScope = MakeShared<FString, ESPMode::ThreadSafe>();
	const auto SeenKey = MakeShared<FString, ESPMode::ThreadSafe>();
	const auto Done = MakeShared<bool, ESPMode::ThreadSafe>(false);
	Live.Client.RefreshAuth([SeenScope, SeenKey, Done, Client = Live.Client](EVaRestAuthRefreshOutcome, EVaRestErrorKind)
	{
		*SeenScope = SessionScope(Client.GetSession());
		*SeenKey = Key(Client.GetSession());
		*Done = true;
	});
	if (!TestTrue(TEXT("refresh is held"), static_cast<bool>(Auth->Pending))) return false;
	TestTrue(TEXT("validated import while refresh waits"), Live.Client.ImportSession(Saved.Client.ExportSession()));
	TestFalse(TEXT("refresh waiter not called on import stack"), *Done);
	const bool bDelivered = PumpUntil([Done] { return *Done; });
	TestTrue(TEXT("import releases refresh waiter"), bDelivered);
	TestEqual(TEXT("waiter sees imported account scope"), *SeenScope, ExpectedScope);
	TestEqual(TEXT("waiter sees imported cache key"), *SeenKey, ExpectedKey);
	// The old provider still owns its result; its late completion must not replace the imported account.
	TUniqueFunction<void(FVaRestAuthRefreshResult&&)> Late = MoveTemp(Auth->Pending);
	Late(FVaRestAuthRefreshResult::Refreshed(TEXT("stale-access"), TEXT("stale-refresh")));
	TestTrue(TEXT("live rig drains late result"), Live.Drain());
	TestEqual(TEXT("late refresh keeps imported scope"), SessionScope(Live.Client.GetSession()), ExpectedScope);
	TestEqual(TEXT("late refresh keeps imported key"), Key(Live.Client.GetSession()), ExpectedKey);
	TestTrue(TEXT("saved rig drains"), Saved.Drain());
	return true;
}

#endif
