// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Tasks/Task.h"
#include "UObject/Package.h"
#include "VaRestClientState.h"
#include "VaRestHttpTesting.h"
#include "VaRestHttpSettings.h"
#include "VaRestRedactor.h"
#include "VaRestPersistenceGuard.h"
#include "VaRestIdentity.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPersistenceShortTest, "VaRest.Http.PersistenceGuard.ShortBinaryLate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPersistenceShortTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	FPersistenceGuard Guard;
	Guard.AddSecret(TEXT("1234567")); // below the log redactor's minimum
	TArray<uint8> Body;
	Body.Init(static_cast<uint8>('x'), 8192);
	const ANSICHAR* Token = "1234567";
	Body.Append(reinterpret_cast<const uint8*>(Token), 7);
	TestTrue(TEXT("scans beyond excerpt cap"), Guard.ContainsSecret(Body));
	TestFalse(TEXT("binary is scanned, not a descriptor"), Guard.AllowsBody(EVaRestBodyKind::Binary, TEXT("application/octet-stream"), Body));
	const FTCHARToUTF8 Json(TEXT("{\"padding\":\"safe\",\"password\":\"unknown-value\"}"));
	TestFalse(TEXT("secret JSON key rejects unknown value"), Guard.AllowsBody(EVaRestBodyKind::Json, TEXT("application/json"),
	    MakeArrayView(reinterpret_cast<const uint8*>(Json.Get()), Json.Length())));
	TestFalse(TEXT("query token key"), Guard.AllowsUrl(TEXT("https://a.test/write?access_token=unknown")));
	TestFalse(TEXT("userinfo"), Guard.AllowsUrl(TEXT("https://user:pw@a.test/write")));
	FString Origin;
	TestTrue(TEXT("default port canonical"), CanonicalOrigin(TEXT("HTTPS://A.TEST:443/x"), Origin));
	TestEqual(TEXT("canonical result"), Origin, FString(TEXT("https://a.test:443")));
	TestFalse(TEXT("scheme is part of origin"), SameOrigin(TEXT("http://a.test/x"), TEXT("https://a.test/x")));
	TestFalse(TEXT("port is part of origin"), SameOrigin(TEXT("https://a.test:444/x"), TEXT("https://a.test/x")));
	TestFalse(TEXT("userinfo refused even with equal host"), SameOrigin(TEXT("https://a.test"), TEXT("https://u@a.test/x")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPersistencePrefixTest, "VaRest.Http.PersistenceGuard.SharedPrefixBound",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPersistencePrefixTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	TArray<uint8> Prefix; Prefix.Init(static_cast<uint8>('a'),8191); Prefix.Add(static_cast<uint8>('b'));
	TArray<uint8> Body; Body.Init(static_cast<uint8>('a'),256*1024);
	FPersistenceGuard Linear;
	Linear.Needles.Add(Prefix); // exercise one exact raw KMP needle, independent of encoding expansion
	const int64 Before=Linear.RemainingWork;
	TestFalse(TEXT("shared prefix absent"),Linear.ContainsSecret(Body));
	TestTrue(TEXT("linear comparisons"),Before-Linear.RemainingWork<=2LL*(Body.Num()+Prefix.Num()));
	Body.Append(Prefix);
	TestTrue(TEXT("late shared-prefix match"),Linear.ContainsSecret(Body));
	FPersistenceGuard Budget;
	Budget.Needles.Add(TArray<uint8>{static_cast<uint8>('a'),static_cast<uint8>('b')});
	Budget.RemainingWork=4096;
	TestTrue(TEXT("bounded refusal"),Budget.ContainsSecret(Body));
	TestTrue(TEXT("budget not silently reset"),Budget.RemainingWork<=0);
	TestTrue(TEXT("metadata cannot reset exhausted body budget"),Budget.ContainsSecret(TArray<uint8>{120}));
	FPersistenceGuard Canceled;
	Canceled.AbortSignal=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);
	const int64 Untouched=Canceled.RemainingWork;
	TestTrue(TEXT("canceled refuses immediately"),Canceled.ContainsSecret(Body));
	TestEqual(TEXT("no scan after cancel"),Canceled.RemainingWork,Untouched);
	FPersistenceGuard Many;
	for (int32 Index=0;Index<65;++Index) Many.AddSecret(FString::Printf(TEXT("value-%03d"),Index));
	TestTrue(TEXT("too many encoded needles refuse, never discard"),Many.bRefusePersistence);
	return true;
}

namespace VaRest::Http::PersistenceTestPrivate
{
	TArray<uint8> Bytes(FStringView Text)
	{
		const FTCHARToUTF8 Encoded(Text.GetData(), Text.Len());
		return TArray<uint8>(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
	}

	class FExchangeProbe final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("PersistenceProbe"); }
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			Seen.Add(Exchange->GetRequestId(), Exchange);
			Next.Proceed(Exchange, MoveTemp(OnOutcome));
		}
		TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> Pin(uint64 Id) const
		{
			const auto* Found = Seen.Find(Id);
			return Found ? Found->Pin() : nullptr;
		}
		TMap<uint64, TWeakPtr<FVaRestExchange, ESPMode::ThreadSafe>> Seen;
	};

	class FCountingAuth final : public IVaRestAuth
	{
	public:
		virtual FName GetAuthName() const override { return TEXT("CountBearer"); }
		virtual bool Apply(FVaRestAuthTarget& Target) const override
		{
			++Calls;
			Target.Headers.Set(TEXT("Authorization"), TEXT("Bearer ") + Target.Session.AccessToken);
			return Target.Session.HasAccessToken();
		}
		virtual void GetRedactedKeys(TArray<FString>& Out) const override { Out.Add(TEXT("X-Custom-Alias")); }
		virtual void GetSecretValues(TArray<TPair<FName, FString>>& Out) const override { Out.Emplace(TEXT("tiny"), TEXT("c!")); }
		mutable int32 Calls = 0;
	};

	/** An owned input gate only: no executor, filesystem, or persistence acceptance is simulated. */
	struct FGuardGate
	{
		TSharedPtr<const FPersistenceGuard, ESPMode::ThreadSafe> Snapshot;
		TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> Abort;
		std::atomic<bool> Done{false};
		bool RefusedA = false;
		bool RefusedB = false;
		bool Safe = false;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPersistenceFormsTest, "VaRest.Http.PersistenceGuard.FormsStructureHeaders",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPersistenceFormsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::PersistenceTestPrivate;
	FPersistenceGuard Guard;
	Guard.AddSecret(TEXT("q /\"é"));
	const TArray<FString> Forms{TEXT("q /\"é"), TEXT("q%20%2F%22%C3%A9"), TEXT("q%20%2f%22%c3%a9"),
		TEXT("q+%2F%22%C3%A9"), TEXT("q+%2f%22%c3%a9"), TEXT("q /\\\"é"), TEXT("71202f22c3a9"), TEXT("71202F22C3A9")};
	for (const FString& Form : Forms) TestTrue(TEXT("wire form refused"), Guard.ContainsSecret(Bytes(Form)));
	TArray<uint8> Binary{0, 255, 0, 128};
	Binary.Append(Bytes(TEXT("q /\"é"))); Binary.Add(0);
	TestFalse(TEXT("embedded NUL binary scanned"), Guard.AllowsBody(EVaRestBodyKind::Binary, TEXT("application/octet-stream"), Binary));
	const TArray<uint8> Plain{0, 255, 37, 0, 128, 42};
	const TArray<uint8> Before = Plain;
	TestTrue(TEXT("unregistered binary safe"), Guard.AllowsBody(EVaRestBodyKind::Binary, TEXT("application/octet-stream"), Plain));
	TestTrue(TEXT("accepted bytes unchanged"), Plain == Before);
	TestFalse(TEXT("stream not persistable"), Guard.AllowsBody(EVaRestBodyKind::Stream, TEXT("application/octet-stream"), Plain));
	TestFalse(TEXT("JSON-declared stream never accepted from a partial buffer"), Guard.AllowsBody(EVaRestBodyKind::Stream, TEXT("application/json"), Bytes(TEXT("{\"score\":12}"))));
	TestFalse(TEXT("form-declared stream never accepted from a partial buffer"), Guard.AllowsBody(EVaRestBodyKind::Stream, TEXT("application/x-www-form-urlencoded"), Bytes(TEXT("score=12"))));
	TestFalse(TEXT("unknown stream never accepted from a partial buffer"), Guard.AllowsBody(EVaRestBodyKind::Stream, TEXT("application/unknown"), Plain));
	TestFalse(TEXT("JSON secret name under binary kind/type"), Guard.AllowsBody(EVaRestBodyKind::Binary, TEXT("application/problem+json"), Bytes(TEXT("{\"access_token\":\"unregistered\"}"))));
	TestFalse(TEXT("malformed JSON refused"), Guard.AllowsBody(EVaRestBodyKind::Json, TEXT("application/json"), Bytes(TEXT("{\"safe\":"))));
	TestTrue(TEXT("safe complete JSON"), Guard.AllowsBody(EVaRestBodyKind::Json, TEXT("application/json"), Bytes(TEXT("{\"score\":12}"))));
	TestFalse(TEXT("percent decoded form key"), Guard.AllowsBody(EVaRestBodyKind::Form, TEXT("application/x-www-form-urlencoded"), Bytes(TEXT("safe=1&pass%77ord=unregistered"))));
	TestTrue(TEXT("safe form"), Guard.AllowsBody(EVaRestBodyKind::Form, TEXT("application/x-www-form-urlencoded"), Bytes(TEXT("score=12"))));
	for (const EVaRestBodyKind RawKind : {EVaRestBodyKind::Text, EVaRestBodyKind::Binary})
	{
		const FString Declared = TEXT("  Application/X-Www-Form-Urlencoded ; charset=UTF-8");
		TestFalse(TEXT("declared form on raw builder refuses unknown credential value"), Guard.AllowsBody(RawKind, Declared, Bytes(TEXT("password=unregistered"))));
		TestFalse(TEXT("declared form on raw builder decodes percent key"), Guard.AllowsBody(RawKind, Declared, Bytes(TEXT("pass%77ord=unregistered"))));
		const TArray<uint8> Harmless = Bytes(TEXT("score=12&label=plain"));
		const TArray<uint8> Original = Harmless;
		TestTrue(TEXT("harmless declared form allowed"), Guard.AllowsBody(RawKind, Declared, Harmless));
		TestTrue(TEXT("harmless declared form unchanged"), Harmless == Original);
		TestTrue(TEXT("unknown media type is raw only"), Guard.AllowsBody(RawKind, TEXT("application/x-www-form-urlencoded-other"), Bytes(TEXT("password=unregistered"))));
	}
	TestFalse(TEXT("bounded form key conversion"), Guard.AllowsBody(EVaRestBodyKind::Form, TEXT("application/x-www-form-urlencoded"), Bytes(FString::ChrN(8193, TEXT('x')) + TEXT("=1"))));
	Guard.AddKey(TEXT("X-Tenant-Alias"));
	TestTrue(TEXT("multipart descriptor-name primitive"), Guard.Rules.ShouldRedact(FStringView(TEXT("X-Tenant-Alias")), EVaRestRedactKeyKind::Part));
	TestFalse(TEXT("custom URL key"), Guard.AllowsUrl(TEXT("https://a.test/path?X-Tenant-Alias=unregistered")));
	TestFalse(TEXT("fragment key"), Guard.AllowsUrl(TEXT("https://a.test/path#access_token=unregistered")));
	TestFalse(TEXT("raw secret in URL path"), Guard.AllowsUrl(TEXT("https://a.test/q%20%2F%22%C3%A9")));
	TestTrue(TEXT("safe URL"), Guard.AllowsUrl(TEXT("https://a.test/score?value=12")));
	FVaRestHeaders Input;
	Input.Add(TEXT("Authorization"), TEXT("unknown")); Input.Add(TEXT("Cookie"), TEXT("unknown"));
	Input.Add(TEXT("Set-Cookie"), TEXT("unknown")); Input.Add(TEXT("X-Tenant-Alias"), TEXT("unknown"));
	Input.Add(TEXT("Connection"), TEXT("X-Hop, keep-alive")); Input.Add(TEXT("X-Hop"), TEXT("unknown"));
	Input.Add(TEXT("Transfer-Encoding"), TEXT("chunked")); Input.Add(TEXT("X-Value"), TEXT("q /\"é"));
	Input.Add(TEXT("Idempotency-Key"), TEXT("operation-123")); Input.Add(TEXT("Content-Type"), TEXT("application/octet-stream"));
	const FVaRestHeaders Safe = Guard.SafeHeaders(Input);
	TestEqual(TEXT("only safe headers retained"), Safe.Num(), 2);
	TestTrue(TEXT("idempotency key preserved"), Safe.Find(TEXT("Idempotency-Key")) != nullptr);
	TestTrue(TEXT("content type preserved"), Safe.Find(TEXT("Content-Type")) != nullptr);
	TestEqual(TEXT("binary reservation includes worker copy"), PersistenceGuardReservation(EVaRestBodyKind::Binary, TEXT("application/octet-stream"), 32LL*1024*1024), 34LL*1024*1024);
	TestEqual(TEXT("JSON reservation includes structural transients"), PersistenceGuardReservation(EVaRestBodyKind::Json, TEXT("application/json"), 1024), 8LL*1024+2LL*1024*1024);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPersistenceLimitsTest, "VaRest.Http.PersistenceGuard.CurrentCaseAndCaps",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPersistenceLimitsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::PersistenceTestPrivate;
	FVaRestSecretSet Slots;
	Slots.Set(TEXT("one"), TEXT("abcdefg"), 0); Slots.Set(TEXT("two"), TEXT("ABCDEFG"), 0);
	Slots.Set(TEXT("retired"), TEXT("retired-only"), 0); Slots.Set(TEXT("retired"), TEXT(""), 1);
	const TArray<FString> Current = Slots.GetCurrentForPersistence();
	TestEqual(TEXT("all current case-distinct short values only"), Current.Num(), 2);
	FPersistenceGuard Guard;
	for (const FString& Value : Current) Guard.AddSecret(Value);
	TestTrue(TEXT("lowercase slot retained"), Guard.ContainsSecret(Bytes(TEXT("abcdefg"))));
	TestTrue(TEXT("uppercase slot retained"), Guard.ContainsSecret(Bytes(TEXT("ABCDEFG"))));
	TestFalse(TEXT("retired history excluded"), Guard.ContainsSecret(Bytes(TEXT("retired-only"))));
	const int32 NeedleCount = Guard.Needles.Num(); Guard.AddSecret(TEXT("abcdefg")); Guard.AddSecret(TEXT(""));
	TestEqual(TEXT("byte-identical/empty dedup"), Guard.Needles.Num(), NeedleCount);
	FPersistenceGuard Other; Other.AddSecret(TEXT("!")); Guard.UnionWith(Other);
	TestTrue(TEXT("single-byte secret"), Guard.ContainsSecret(Bytes(TEXT("!"))));
	FPersistenceGuard Oversize; Oversize.AddSecret(FString::ChrN(6000, TCHAR(0x00e9)));
	TestTrue(TEXT("oversize encoded needle refuses"), Oversize.bRefusePersistence);
	FPersistenceGuard Aggregate;
	for (int32 Index = 0; Index < 8; ++Index) Aggregate.AddSecret(FString::ChrN(4096, static_cast<TCHAR>('a'+Index)));
	TestTrue(TEXT("aggregate needle cap refuses"), Aggregate.bRefusePersistence);
	FPersistenceGuard Left, Right;
	for (int32 Index = 0; Index < 12; ++Index)
	{
		Left.AddSecret(FString::Printf(TEXT("left-%d"), Index));
		Right.AddSecret(FString::Printf(TEXT("right-%d"), Index));
	}
	TestFalse(TEXT("left separately within cap"), Left.bRefusePersistence);
	TestFalse(TEXT("right separately within cap"), Right.bRefusePersistence);
	Left.UnionWith(Right);
	TestTrue(TEXT("union capacity refuses rather than dropping values"), Left.bRefusePersistence);
	FPersistenceGuard Keys;
	for (int32 Index = 0; Index < 129; ++Index) Keys.AddKey(FString::Printf(TEXT("field%d"), Index));
	TestTrue(TEXT("extra key count refuses"), Keys.bRefusePersistence);
	FPersistenceGuard KeyBytes;
	KeyBytes.AddKey(FString::ChrN(16385, TEXT('x')));
	TestTrue(TEXT("extra key text refuses"), KeyBytes.bRefusePersistence);
	FPersistenceGuard BadKey; BadKey.AddKey(TEXT("bad&key"));
	TestTrue(TEXT("unrepresentable rule refuses"), BadKey.bRefusePersistence);
	FPersistenceGuard Exhausted; Exhausted.Needles.Add(Bytes(TEXT("absent"))); Exhausted.RemainingWork = 2;
	TestTrue(TEXT("body exhausts"), Exhausted.ContainsSecret(Bytes(TEXT("boring payload"))));
	TestTrue(TEXT("empty metadata cannot clear exhaustion"), Exhausted.ContainsSecret({}));
	TestTrue(TEXT("exhaustion latched"), Exhausted.bWorkExhausted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPersistenceOriginTest, "VaRest.Http.PersistenceGuard.Origin",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPersistenceOriginTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	for (const FString& Bad : TArray<FString>{TEXT("https://"), TEXT("ftp://a.test"), TEXT("https://u@a.test"), TEXT("https://a%2etest"),
		TEXT("https://a\\b"), TEXT("https://a b"), TEXT("https://a\tb"), TEXT("https://é.test"), TEXT("https://:443"),
		TEXT("https://a:"), TEXT("https://a:0"), TEXT("https://a:65536"), TEXT("https://a:-1"), TEXT("https://a:1x"),
		TEXT("https://a:9999999999999999999999999"), TEXT("https://::1"), TEXT("https://[::1"), TEXT("https://[]"),
		TEXT("https://[gg::1]"), TEXT("https://[::1]suffix"), TEXT("https://[::1]:"), TEXT("https://a]"), TEXT("https://a:4:4")})
	{
		FString Origin = TEXT("must clear");
		TestFalse(TEXT("malformed authority refused"), CanonicalOrigin(Bad, Origin));
		TestTrue(TEXT("failure clears output"), Origin.IsEmpty());
	}
	TestTrue(TEXT("HTTP defaults"), SameOrigin(TEXT("http://A.TEST/path"), TEXT("http://a.test:80?x")));
	TestTrue(TEXT("HTTPS defaults"), SameOrigin(TEXT("https://A.TEST"), TEXT("HTTPS://a.test:00443#x")));
	TestTrue(TEXT("bracketed IPv6 lowercase"), SameOrigin(TEXT("https://[AB::1]/"), TEXT("https://[ab::1]:443")));
	TestFalse(TEXT("IPv6 spellings conservative"), SameOrigin(TEXT("https://[::1]"), TEXT("https://[0:0:0:0:0:0:0:1]")));
	TestFalse(TEXT("DNS trailing dot significant"), SameOrigin(TEXT("https://a.test"), TEXT("https://a.test.")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPersistenceLifetimeTest, "VaRest.Http.PersistenceGuard.CapturedLifetime",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPersistenceLifetimeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::PersistenceTestPrivate;
	FTestRig Rig;
	const auto Probe = MakeShared<FExchangeProbe, ESPMode::ThreadSafe>();
	const auto Auth = MakeShared<FCountingAuth, ESPMode::ThreadSafe>();
	Rig.Client.WithAuth(Auth).WithMaxConcurrent(1).Use(Probe, EVaRestStageSlot::Outer);
	Rig.Client.SetBearerToken(TEXT("short-A"));
	Rig.Client.SetSessionHeader(TEXT("X-Ticket-Alias"), TEXT("t!"));
	Rig.Client.SetSecretValue(TEXT("game"), TEXT("g!"));
	TestEqual(TEXT("publishing never applies auth"), Auth->Calls, 0);
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual()); Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	const auto BlockedOutcome = MakeOutcome<void>(); const auto CapturedOutcome = MakeOutcome<void>();
	const auto Blocker = Observe(Rig.Client.Get<void>(TEXT("/block")), BlockedOutcome).Send();
	const auto Captured = Observe(Rig.Client.Get<void>(TEXT("/capture")), CapturedOutcome).Send();
	const auto Exchange = Probe->Pin(Captured.GetId());
	if (!TestTrue(TEXT("actual held exchange captured"), Exchange.IsValid())) { Rig.Client.CancelAll(); Rig.Drain(); return false; }
	FExchangeState& Ex = Internals(*Exchange);
	if (!TestTrue(TEXT("Send captured immutable guard"), Ex.PersistenceGuard.IsValid())) { Rig.Client.CancelAll(); Rig.Drain(); return false; }
	const auto SendSnapshot = Ex.PersistenceGuard;
	TestTrue(TEXT("waiting in actual limiter"), Ex.bInLimiterQueue);
	TestEqual(TEXT("only blocker applied auth"), Auth->Calls, 1);
	Rig.Client.SetBearerToken(TEXT("short-B"));
	if (!TestEqual(TEXT("blocker really dispatched"), Rig.Fake->NumCalls(), 1)) { Rig.Client.CancelAll(); Rig.Drain(); return false; }
	Rig.Fake->Complete(0, FVaRestFakeResponse::Text(200, TEXT("")));
	if (!TestTrue(TEXT("B attempt launched"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; }))) { Rig.Client.CancelAll(); Rig.Drain(); return false; }
	TestEqual(TEXT("one Apply per real attempt"), Auth->Calls, 2);
	const FString* Applied = Rig.Fake->GetCall(1).Request.Headers.Find(TEXT("Authorization"));
	TestTrue(TEXT("actual attempt used B"), Applied && *Applied == TEXT("Bearer short-B"));
	const auto Gate = MakeShared<FGuardGate, ESPMode::ThreadSafe>();
	Gate->Snapshot = Ex.PersistenceGuard; Gate->Abort = Ex.PersistenceAbort;
	Rig.Client.ClearSession(); Rig.Client.ClearSecretValues();
	const auto State = FClientAccess::State(Rig.Client);
	if (!TestTrue(TEXT("client state"), State.IsValid())) { Rig.Client.CancelAll(); Rig.Drain(); return false; }
	const auto Current = State->GetConfig()->PersistenceGuard;
	if (!TestTrue(TEXT("current guard"), Current.IsValid())) { Rig.Client.CancelAll(); Rig.Drain(); return false; }
	FPersistenceGuard Initial = *SendSnapshot;
	TestTrue(TEXT("Send remembers A"), Initial.ContainsSecret(Bytes(TEXT("short-A"))));
	TestFalse(TEXT("Send snapshot not changed to B"), Initial.ContainsSecret(Bytes(TEXT("short-B"))));
	TestTrue(TEXT("session raw value protected"), Initial.ContainsSecret(Bytes(TEXT("t!"))));
	TestTrue(TEXT("game raw value protected"), Initial.ContainsSecret(Bytes(TEXT("g!"))));
	TestTrue(TEXT("auth raw value protected"), Initial.ContainsSecret(Bytes(TEXT("c!"))));
	TestTrue(TEXT("session alias structure protected"), Initial.Rules.ShouldRedact(FStringView(TEXT("X-Ticket-Alias")), EVaRestRedactKeyKind::Header));
	TestTrue(TEXT("auth alias structure protected"), Initial.Rules.ShouldRedact(FStringView(TEXT("X-Custom-Alias")), EVaRestRedactKeyKind::Header));
	const int64 SnapshotBudget = Gate->Snapshot->RemainingWork;
	UE::Tasks::Launch(TEXT("VaRest.TestGuardGate"), [Gate, Current]
	{
		FPersistenceGuard Candidate = *Gate->Snapshot;
		Candidate.UnionWith(*Current);
		Candidate.AbortSignal = Gate->Abort;
		Gate->RefusedA = Candidate.ContainsSecret(Bytes(TEXT("short-A")));
		Gate->RefusedB = Candidate.ContainsSecret(Bytes(TEXT("short-B")));
		Gate->Safe = Candidate.AllowsBody(EVaRestBodyKind::Binary, TEXT("application/octet-stream"), Bytes(TEXT("plain payload")));
		Gate->Done.store(true);
	});
	if (TestTrue(TEXT("owned guard gate released"), PumpUntil([Gate] { return Gate->Done.load(); })))
	{
		TestTrue(TEXT("captured A survives rotation and clear"), Gate->RefusedA);
		TestTrue(TEXT("applied B survives clear"), Gate->RefusedB);
		TestTrue(TEXT("safe payload preserved by gate"), Gate->Safe);
	}
	TestEqual(TEXT("worker never consumes shared snapshot budget"), Gate->Snapshot->RemainingWork, SnapshotBudget);
	FPersistenceGuard Fresh = *Current;
	TestFalse(TEXT("new config has no A history"), Fresh.ContainsSecret(Bytes(TEXT("short-A"))));
	TestFalse(TEXT("new config has no B history"), Fresh.ContainsSecret(Bytes(TEXT("short-B"))));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	const auto NewOutcome = MakeOutcome<void>();
	const auto NewRequest = Observe(Rig.Client.Get<void>(TEXT("/new")).NoAuth(), NewOutcome).Send();
	const auto NewExchange = Probe->Pin(NewRequest.GetId());
	if (TestTrue(TEXT("new request captured"), NewExchange.IsValid()))
		TestTrue(TEXT("new request uses current immutable snapshot"), Internals(*NewExchange).PersistenceGuard == Current);
	Rig.Fake->Complete(1, FVaRestFakeResponse::Text(200, TEXT("")));
	TestTrue(TEXT("captured live response succeeds"), Rig.Wait(CapturedOutcome));
	TestFalse(TEXT("normal success keeps best effort disk signal live"), Ex.PersistenceAbort->load());
	Captured.Cancel();
	TestFalse(TEXT("finished cancel is no-op"), Ex.PersistenceAbort->load());
	NewRequest.Cancel();
	TestTrue(TEXT("drains"), Rig.Drain());
	TestEqual(TEXT("no speculative auth"), Auth->Calls, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPersistenceCancelTest, "VaRest.Http.PersistenceGuard.CancellationSignals",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPersistenceCancelTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::PersistenceTestPrivate;
	for (int32 Mode = 0; Mode < 4; ++Mode)
	{
		FTestRig Rig;
		const auto Probe = MakeShared<FExchangeProbe, ESPMode::ThreadSafe>();
		Rig.Client.Use(Probe, EVaRestStageSlot::Outer);
		Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
		const auto Outcome = MakeOutcome<void>();
		UObject* Owner = NewObject<UVaRestHttpSettings>(GetTransientPackage(), NAME_None, RF_Transient);
		const auto Handle = Observe(Rig.Client.Get<void>(TEXT("/signal")).Owner(Owner), Outcome).Send();
		const auto Exchange = Probe->Pin(Handle.GetId());
		if (!TestTrue(TEXT("held signal exchange"), Exchange.IsValid())) { Rig.Client.CancelAll(); Rig.Drain(); return false; }
		FExchangeState& Ex = Internals(*Exchange);
		TestFalse(TEXT("signal starts clear"), Ex.PersistenceAbort->load());
		if (Mode == 0) Handle.Cancel();
		else if (Mode == 1)
		{
			const auto Published = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
			UE::Tasks::Launch(TEXT("VaRest.TestCancel"), [Handle, Published] { Handle.Cancel(); Published->store(true); });
			TestTrue(TEXT("any-thread cancel returned"), PumpUntil([Published] { return Published->load(); }));
		}
		else if (Mode == 2)
		{
			Owner->MarkAsGarbage();
			if (!TestEqual(TEXT("owner request dispatched"), Rig.Fake->NumCalls(), 1)) { Rig.Client.CancelAll(); Rig.Drain(); return false; }
			Rig.Fake->Complete(0, FVaRestFakeResponse::Text(200, TEXT("")));
			TestTrue(TEXT("owner loss observed at held outcome"), Rig.Drain());
		}
		else Ex.State->CancelAll(NAME_None, EVaRestCancelReason::Shutdown);
		TestTrue(TEXT("accepted cancellation publishes worker signal"), Ex.PersistenceAbort->load());
		FPersistenceGuard Candidate;
		Candidate.AbortSignal = Ex.PersistenceAbort;
		TestTrue(TEXT("worker-safe signal refuses even empty candidate"), Candidate.ContainsSecret({}));
		TestTrue(TEXT("cancellation drains"), Rig.Drain());
		TestEqual(TEXT("silent reasons suppress callbacks"), Outcome->CompleteCalls.load(), Mode < 2 ? 1 : 0);
	}
	// An Enter after global shutdown publishes at the initial shutdown owner before any stage can run.
	FTestRig AfterExit;
	FClientState::CancelForShutdown();
	ON_SCOPE_EXIT { FClientState::ResetShutdownInternal(); };
	const auto Exchange = MakeShared<FVaRestExchange, ESPMode::ThreadSafe>();
	FExchangeState& Ex = Internals(*Exchange);
	Ex.State = FClientAccess::State(AfterExit.Client);
	if (!TestTrue(TEXT("post shutdown state"), Ex.State.IsValid())) return false;
	Ex.RequestId = FVaRestTrace::NewRequestId(); Ex.ClientId = AfterExit.Client.GetId(); Ex.Verb = TEXT("GET"); Ex.RawPath = TEXT("/after-exit");
	Ex.State->Enter(Exchange);
	TestTrue(TEXT("initial shutdown Enter publishes signal"), Ex.PersistenceAbort->load());
	TestTrue(TEXT("post shutdown drains"), AfterExit.Drain());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPersistenceConfiguredKeyTest, "VaRest.Http.PersistenceGuard.ConfiguredKeyRepresentation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPersistenceConfiguredKeyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::PersistenceTestPrivate;
	for (const FString& Key : TArray<FString>{TEXT("_"), TEXT("-"), TEXT("."), TEXT("_-.-_")})
	{
		FPersistenceGuard Guard;
		Guard.AddKey(Key);
		TestTrue(TEXT("nonempty unusable configured key refuses persistence"), Guard.bRefusePersistence);
		TestTrue(TEXT("pure key regression has no registered values to mask it"), Guard.Needles.IsEmpty());
		const TArray<uint8> Json = Bytes(TEXT("{\"") + Key + TEXT("\":\"unknown-value\"}"));
		const TArray<uint8> Form = Bytes(Key + TEXT("=unknown-value"));
		FVaRestHeaders Header; Header.Add(Key, TEXT("unknown-value"));
		TestFalse(TEXT("unusable configured JSON key cannot leak unknown value"), Guard.AllowsBody(EVaRestBodyKind::Json, TEXT("application/json"), Json));
		TestFalse(TEXT("unusable configured form key cannot leak unknown value"), Guard.AllowsBody(EVaRestBodyKind::Form, TEXT("application/x-www-form-urlencoded"), Form));
		TestTrue(TEXT("unusable configured header cannot leak unknown value"), Guard.SafeHeaders(Header).IsEmpty());
		FPersistenceGuard IntoClean;
		IntoClean.AddKey(TEXT("X-Game-Ticket"));
		IntoClean.UnionWith(Guard);
		TestTrue(TEXT("refused source propagates through union"), IntoClean.bRefusePersistence);
		TestFalse(TEXT("refused source union cannot persist JSON"), IntoClean.AllowsBody(EVaRestBodyKind::Json, TEXT("application/json"), Json));
		FPersistenceGuard Clean;
		Clean.AddKey(TEXT("X-Game-Ticket"));
		FPersistenceGuard IntoRefused = Guard;
		IntoRefused.UnionWith(Clean);
		TestTrue(TEXT("refused destination stays refused through union"), IntoRefused.bRefusePersistence);
		TestFalse(TEXT("refused destination union cannot persist form"), IntoRefused.AllowsBody(EVaRestBodyKind::Form, TEXT("application/x-www-form-urlencoded"), Form));
	}
	FPersistenceGuard Representable;
	Representable.AddKey(TEXT("X-Game-Ticket"));
	TestFalse(TEXT("ordinary configured key remains usable"), Representable.bRefusePersistence);
	TestTrue(TEXT("ordinary configured key protects structure"), Representable.Rules.ShouldRedact(FStringView(TEXT("X-Game-Ticket")), EVaRestRedactKeyKind::Json));
	TestTrue(TEXT("ordinary rule permits harmless JSON"), Representable.AllowsBody(EVaRestBodyKind::Json, TEXT("application/json"), Bytes(TEXT("{\"score\":12}"))));
	TestTrue(TEXT("ordinary rule permits harmless form"), Representable.AllowsBody(EVaRestBodyKind::Form, TEXT("application/x-www-form-urlencoded"), Bytes(TEXT("score=12"))));
	FVaRestHeaders Harmless; Harmless.Add(TEXT("X-Score"), TEXT("12"));
	TestEqual(TEXT("ordinary rule permits harmless header"), Representable.SafeHeaders(Harmless).Num(), 1);

	for (int32 ConfigMode = 0; ConfigMode < 2; ++ConfigMode)
	{
		FTestRig Rig;
		const FString Key = ConfigMode == 0 ? TEXT("_") : TEXT(".");
		if (ConfigMode == 0) Rig.Client.SetSessionHeader(Key, TEXT("registered-session-value"));
		else Rig.Client.WithRedactedKeys(TArray<FString>{Key});
		const auto State = FClientAccess::State(Rig.Client);
		if (!TestTrue(TEXT("published client state exists"), State.IsValid())) return false;
		const auto Snapshot = State->GetConfig()->PersistenceGuard;
		if (!TestTrue(TEXT("published persistence guard exists"), Snapshot.IsValid())) return false;
		FPersistenceGuard Candidate = *Snapshot;
		TestTrue(TEXT("published unusable alias refuses persistence"), Candidate.bRefusePersistence);
		const FString Unknown = TEXT("unregistered-body-value");
		// Deliberately distinct from the actual session header value: the raw needle must not hide a lost key rule.
		TestFalse(TEXT("published alias protects unknown JSON value"), Candidate.AllowsBody(EVaRestBodyKind::Json, TEXT("application/json"), Bytes(TEXT("{\"") + Key + TEXT("\":\"") + Unknown + TEXT("\"}"))));
		TestFalse(TEXT("published alias protects unknown form value"), Candidate.AllowsBody(EVaRestBodyKind::Form, TEXT("application/x-www-form-urlencoded"), Bytes(Key + TEXT("=") + Unknown)));
		FVaRestHeaders UnknownHeader; UnknownHeader.Add(Key, Unknown);
		TestTrue(TEXT("published alias protects unknown header value"), Candidate.SafeHeaders(UnknownHeader).IsEmpty());
		const auto Outcome = MakeOutcome<void>();
		Observe(Rig.Client.Get<void>(TEXT("/ordinary-live-request")), Outcome).Send();
		TestTrue(TEXT("live HTTP still completes"), Rig.Wait(Outcome));
		TestEqual(TEXT("persistence refusal does not fail live HTTP"), Outcome->SuccessCalls.load(), 1);
		TestEqual(TEXT("live HTTP request still dispatched"), Rig.Fake->NumCalls(), 1);
		if (ConfigMode == 0 && Rig.Fake->NumCalls() == 1)
		{
			const FString* Applied = Rig.Fake->GetCall(0).Request.Headers.Find(Key);
			TestTrue(TEXT("live session alias still applied"), Applied && Applied->Equals(TEXT("registered-session-value"), ESearchCase::CaseSensitive));
		}
		TestTrue(TEXT("published alias rig drains"), Rig.Drain());
	}
	return true;
}

#endif
