// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"
#include "VaRestClientState.h"
#include "VaRestHttpTesting.h"
#include "VaRestM5PolicyTestTypes.h"
#include "VaRestNativeScope.h"
#include "VaRestTracePoints.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpM5InitialScopeTest, "VaRest.Http.TraceSurface.M5InitialScope",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpM5InitialScopeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	TestEqual(TEXT("ordinary native client retains Process scope"), Rig.Client.GetScopeId(), FVaRestTrace::ProcessScopeId);
	FScopedTraceRecorder Capture;
	// Capture is already attached: a wrong early Process Created cannot be hidden by a later snapshot upsert.
	FVaRestTraceScope Info;
	Info.Label = TEXT("fixture-world");
	Info.PIEInstance = 17;
	auto Lifetime = FNativeScopeLifetime::Open(Info, Rig.Clock->Now());
	if (!TestTrue(TEXT("native scope is allocated before its client"), Lifetime.IsValid() && Lifetime->GetId() != 0))
		return false;
	FClientConfig Initial = FClientAccess::InitialConfig(TEXT("https://api.test"));
	const uint64 Scope = Lifetime->GetId();
	Initial.ScopeLifetime = Lifetime;
	Initial.DisplayName = TEXT("owned-scope-client");
	Initial.Transport = Rig.Fake;
	Initial.Clock = Rig.Clock;
	Initial.Connectivity = Rig.Connectivity;
	Initial.LogLevel = EVaRestLogLevel::None;
	FVaRestClient Client = FClientAccess::Create(MoveTemp(Initial));
	const uint64 ClientId = Client.GetId();
	const auto ClientEvents = [&] {
		return Capture.Recorder->For(0).FilterByPredicate([&](const FVaRestTraceEvent& Event) {
			return Event.Kind == EVaRestTraceKind::Client && Event.Payload.Get<FVaRestTraceClient>().ClientId == ClientId;
		});
	};
	const auto Created = ClientEvents();
	if (!TestEqual(TEXT("exactly one initial client event"), Created.Num(), 1))
		return false;
	TestEqual(TEXT("first Created already has its final scope"), Created[0].Payload.Get<FVaRestTraceClient>().ScopeId, Scope);
	TestEqual(TEXT("first Created already has its final display name"), Created[0].Payload.Get<FVaRestTraceClient>().DisplayName, FString(TEXT("owned-scope-client")));
	const auto Before = Capture.Recorder->For(0);
	const FVaRestTraceEvent* Opened = Before.FindByPredicate([&](const FVaRestTraceEvent& Event) {
		return Event.Kind == EVaRestTraceKind::Scope && Event.Payload.Get<FVaRestTraceScope>().ScopeId == Scope;
	});
	TestTrue(TEXT("Opened precedes the first client publication"), Opened && Opened->Payload.Get<FVaRestTraceScope>().bOpened && Opened->Seq < Created[0].Seq);
	TestEqual(TEXT("public scope accessor matches"), Client.GetScopeId(), Scope);
	FClientAccess::State(Client)->EditConfig([](FClientConfig& Config) { Config.ScopeId = 999; });
	TestEqual(TEXT("ordinary config edits cannot move a client scope"), FClientAccess::State(Client)->GetConfig()->ScopeId, Scope);
	const FVaRestTraceSnapshot Snapshot = FVaRestTrace::BuildSnapshot();
	const FVaRestTraceClient* Entry = Snapshot.Clients.FindByPredicate([&](const FVaRestTraceClient& Item) { return Item.ClientId == ClientId; });
	TestTrue(TEXT("late snapshot preserves scope"), Entry && Entry->ScopeId == Scope);
	TracePoints::Marker(*FClientAccess::State(Client), EVaRestTraceMarkerKind::SessionRefreshed, 0, TEXT("fixture"));
	const auto Markers = Capture.Recorder->MarkersOf(ClientId);
	TestTrue(TEXT("marker uses the client's scope"), Markers.Num() == 1 && Markers[0].ScopeId == Scope);
	auto Outcome = MakeOutcome<FVaRestJson>();
	const auto Handle = Observe(Client.Get(TEXT("/a")).Endpoint(TEXT("ListItems")), Outcome).Send();
	if (!TestTrue(TEXT("scoped request completes"), Rig.Wait(Outcome)))
		return false;
	const auto Events = Capture.Recorder->For(Handle.GetId());
	const FVaRestTraceEvent* Started = Events.FindByPredicate([](const FVaRestTraceEvent& Event) { return Event.Kind == EVaRestTraceKind::Started; });
	TestTrue(TEXT("Started carries scope and endpoint"), Started && Started->Payload.Get<FVaRestTraceStarted>().ScopeId == Scope && Started->Payload.Get<FVaRestTraceStarted>().Endpoint == FName(TEXT("ListItems")));
	if (!TestTrue(TEXT("owned work settles before the scope close fixture"), Rig.Drain()))
		return false;
	Lifetime->SetLabel(TEXT("frozen-world-label"));
	Lifetime->Close(Rig.Clock->Now());
	Lifetime->Close(Rig.Clock->Now());
	TestFalse(TEXT("closed scope label cannot be replaced"), Lifetime->SetLabel(TEXT("wrong")));
	const auto Closed = FVaRestTrace::BuildSnapshot();
	const auto* ClosedInfo = Closed.Scopes.FindByPredicate([&](const FVaRestTraceScope& Row) { return Row.ScopeId == Scope; });
	TestTrue(TEXT("closed scope metadata survives its retained client shell"), ClosedInfo && !ClosedInfo->bOpened && ClosedInfo->PIEInstance == 17 && ClosedInfo->Label.Equals(TEXT("frozen-world-label"), ESearchCase::CaseSensitive));
	TestFalse(TEXT("closed-scope shell is not an active client snapshot row"), Closed.Clients.ContainsByPredicate([&](const FVaRestTraceClient& Row) { return Row.ClientId == ClientId; }));
	int32 CloseCount = 0;
	for (const auto& Event : Capture.Recorder->For(0))
		if (Event.Kind == EVaRestTraceKind::Scope && Event.Payload.Get<FVaRestTraceScope>().ScopeId == Scope && !Event.Payload.Get<FVaRestTraceScope>().bOpened)
			++CloseCount;
	TestEqual(TEXT("scope Close emits once"), CloseCount, 1);
	Client = FVaRestClient();
	TestTrue(TEXT("final native shell destruction keeps the original scope"), PumpUntil([&] {
		return ClientEvents().ContainsByPredicate([&](const FVaRestTraceEvent& Event) {
			const auto& Payload = Event.Payload.Get<FVaRestTraceClient>();
			return !Payload.bCreated && Payload.ScopeId == Scope;
		});
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpM5StrictDefaultTest, "VaRest.Http.M5.StrictDecodeDefault",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpM5StrictDefaultTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	const auto Answer = [&] { Rig.Fake->Enqueue(FVaRestFakeResponse::Json(200, UTF8TEXT("{\"x\":\"2\",\"y\":0,\"z\":0}"))); };
	Answer();
	auto Ordinary = MakeOutcome<FVector>();
	Observe(Rig.Client.Get<FVector>(TEXT("/ordinary")), Ordinary).Send();
	if (!TestTrue(TEXT("ordinary default conversion completes"), Rig.Wait(Ordinary)))
		return false;
	TestTrue(TEXT("code-first native default is still lenient"), Ordinary->Value.IsSet() && Ordinary->Value->X == 2.0);
	auto BuiltBeforeChange = Rig.Client.Get<FVector>(TEXT("/strict"));
	Rig.Client.WithStrictDecode();
	Answer();
	auto Strict = MakeOutcome<FVector>();
	Observe(MoveTemp(BuiltBeforeChange), Strict).Send();
	if (!TestTrue(TEXT("strict client default request completes"), Rig.Wait(Strict)))
		return false;
	TestTrue(TEXT("unset override resolves the immutable Send-time client default"), Strict->Error.Kind == EVaRestErrorKind::Decode && !Strict->Value.IsSet());
	Answer();
	auto Override = MakeOutcome<FVector>();
	Observe(Rig.Client.Get<FVector>(TEXT("/override")).StrictDecode(false), Override).Send();
	if (!TestTrue(TEXT("explicit per-request false completes"), Rig.Wait(Override)))
		return false;
	TestTrue(TEXT("explicit false overrides a strict client"), Override->Value.IsSet() && Override->Value->X == 2.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpM5ScopeOwnershipTest, "VaRest.Http.TraceSurface.M5ScopeOwnership",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpM5ScopeOwnershipTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	FVaRestScopedSinkIsolation Quiet;
	const uint64 Before = FVaRestTrace::Counters().EventsBuilt.load();
	FVaRestTraceScope Info;
	Info.Label = TEXT("quiet-world");
	auto Scope = FNativeScopeLifetime::Open(Info, 0);
	if (!TestTrue(TEXT("scope identity exists with tracing off"), Scope.IsValid() && Scope->GetId() != 0))
		return false;
	const uint64 Id = Scope->GetId();
	TestEqual(TEXT("no trace event is built for mask None"), FVaRestTrace::Counters().EventsBuilt.load(), Before);
	FScopedTraceRecorder Capture;
	const auto Snapshots = Capture.Recorder->GetSnapshots();
	TestTrue(TEXT("late capture learns the already-open scope"), !Snapshots.IsEmpty() && Snapshots.Last().Scopes.ContainsByPredicate([&](const FVaRestTraceScope& Row) {
		return Row.ScopeId == Id && Row.bOpened && Row.Label.Equals(TEXT("quiet-world"), ESearchCase::CaseSensitive);
	}));
	Scope->Close(0); // no client/work exists; no retirement fence is being simulated
	TWeakPtr<FNativeScopeLifetime, ESPMode::ThreadSafe> Weak = Scope;
	Scope.Reset();
	TestFalse(TEXT("registry never retains a strong scope or host owner"), Weak.IsValid());
	const auto After = FVaRestTrace::BuildSnapshot();
	TestFalse(TEXT("expired records are removed rather than appended as history"), After.Scopes.ContainsByPredicate([&](const FVaRestTraceScope& Row) { return Row.ScopeId == Id; }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpM5BodyPolicyTest, "VaRest.Http.M5.JsonBodyPolicy",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpM5BodyPolicyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	Rig.Client.WithHeader(TEXT("Content-Type"), TEXT("text/plain"))
		.WithJsonContentType(TEXT("application/vnd.test+json; profile=one"));
	const auto Send = [&](TVaRestRequest<FVaRestJson>&& Request, const TCHAR* Expected) {
		const int32 Index = Rig.Fake->NumCalls();
		auto Outcome = MakeOutcome<FVaRestJson>();
		Observe(MoveTemp(Request), Outcome).Send();
		if (!TestTrue(TEXT("body policy request completes"), Rig.Wait(Outcome)) || !TestEqual(TEXT("one native wire attempt"), Rig.Fake->NumCalls(), Index + 1))
			return false;
		return TestEqualSensitive(TEXT("actual outgoing content type follows body/request precedence"),
			Rig.Fake->GetCall(Index).Request.Headers.Get(TEXT("Content-Type")), FString(Expected));
	};
	auto Built = Rig.Client.Post(TEXT("/built")).Body(FVaRestJson::Object());
	Rig.Client.WithJsonContentType(TEXT("application/json"));
	Send(MoveTemp(Built), TEXT("application/vnd.test+json; profile=one"));
	Send(Rig.Client.Post(TEXT("/explicit")).Body(FVaRestJson::Object()).Header(TEXT("Content-Type"), TEXT("application/explicit+json")), TEXT("application/explicit+json"));
	Send(Rig.Client.Post(TEXT("/text")).Body(TEXTVIEW("hello")), TEXT("text/plain; charset=utf-8"));
	Send(Rig.Client.Post(TEXT("/form")).Body(FVaRestForm().Add(TEXT("k"), TEXT("v"))), TEXT("application/x-www-form-urlencoded"));
	Send(Rig.Client.Post(TEXT("/bytes")).Body(TArray<uint8>{1}), TEXT("application/octet-stream"));
	const TCHAR* QuotedMedia = TEXT("application/vnd.test+json ; profile=\"one two\"");
	Rig.Client.WithJsonContentType(QuotedMedia);
	Send(Rig.Client.Post(TEXT("/quoted")).Body(FVaRestJson::Object()), QuotedMedia);
	AddExpectedError(TEXT("invalid JSON media type"), EAutomationExpectedErrorFlags::Contains, 5);
	Rig.Client.WithJsonContentType(TEXT("application/json; charset="));
	Rig.Client.WithJsonContentType(TEXT("application/json\t"));
	Rig.Client.WithJsonContentType(TEXT("application/json; charset =utf-8"));
	Rig.Client.WithJsonContentType(TEXT("application/json; charset= utf-8"));
	Rig.Client.WithJsonContentType(TEXT("application/json; charset = utf-8"));
	TestEqualSensitive(TEXT("invalid media never replaces the valid default"), FClientAccess::State(Rig.Client)->GetConfig()->JsonContentType, FString(QuotedMedia));
	Send(Rig.Client.Post(TEXT("/after-refusal")).Body(FVaRestJson::Object()), QuotedMedia);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpM5RulesSnapshotTest, "VaRest.Http.M5.JsonRulesSnapshots",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpM5RulesSnapshotTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	TStrongObjectPtr<UObject> Owner(NewObject<UObject>());
	ON_SCOPE_EXIT
	{
		FVaRestJsonRules::UnregisterOwner(Owner.Get());
	};
	FVaRestJsonFieldRule Rule;
	Rule.Field = FGuid(0, 0, 0, 1);
	Rule.Json = TEXT("buildName");
	TArray<FVaRestJsonFieldRule> Rules{Rule};
	FString Error;
	if (!TestTrue(TEXT("body rules register"), FVaRestJsonRules::RegisterFields(Owner.Get(), FVaRestM5PolicyPayload::StaticStruct(), Rules, &Error)))
		return false;
	Rig.Client.WithJsonRules(FVaRestJsonRules::Snapshot(Owner.Get()));
	FVaRestM5PolicyPayload Input;
	Input.Value_1_00000000000000000000000000000001 = 17;
	auto Built = Rig.Client.Post<FVaRestM5PolicyPayload>(TEXT("/rules")).Body(Input);
	Rules[0].Json = TEXT("responseName");
	if (!TestTrue(TEXT("response rules register"), FVaRestJsonRules::RegisterFields(Owner.Get(), FVaRestM5PolicyPayload::StaticStruct(), Rules, &Error)))
		return false;
	Rig.Client.WithJsonRules(FVaRestJsonRules::Snapshot(Owner.Get()));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Json(200, UTF8TEXT("{\"responseName\":42}")));
	auto Outcome = MakeOutcome<FVaRestM5PolicyPayload>();
	Observe(MoveTemp(Built), Outcome).Send();
	if (!TestTrue(TEXT("real typed request completes"), Rig.Wait(Outcome)))
		return false;
	TestTrue(TEXT("response uses its Send-time immutable rules"), Outcome->Value.IsSet() && Outcome->Value->Value_1_00000000000000000000000000000001 == 42);
	const TArray<uint8> Body = Rig.Fake->ReadBody(0);
	auto Parsed = FVaRestJson::Parse(TConstArrayView<uint8>(Body));
	TestTrue(TEXT("body keeps its build-time rules across the later config change"), Parsed.HasValue() && Parsed.GetValue().Get(TEXT("buildName")).AsInt64() == 17 && !Parsed.GetValue().Get(TEXT("responseName")).IsValid());
	return true;
}

#endif
