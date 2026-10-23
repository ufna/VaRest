// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "VaRestAuth.h"
#include "VaRestClassify.h"
#include "VaRestClientState.h"
#include "VaRestExchangeState.h"
#include "VaRestPersistence.h"
#include "VaRestHttpSettings.h"
#include <limits>
#include "VaRestConnectivity.h"
#include "VaRestHttpTesting.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpConnectivitySharedTest, "VaRest.Http.Connectivity.SharedAndIsolated",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestHttpConnectivitySharedTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http::Testing;
    FTestRig Left;
    FTestRig Right;
    const auto Shared = FVaRestConnectivity::Make(Left.Clock);
    Left.Client.WithConnectivity(Shared);
    Right.Client.WithConnectivity(Shared);
    const auto Isolated = FVaRestConnectivity::Make(Right.Clock);
    const auto Seen = MakeShared<TArray<bool>, ESPMode::ThreadSafe>();
    Shared->OnChanged().AddLambda([Seen](bool bOnline) { Seen->Add(bOnline); });
    Shared->ReportOffline();
    Shared->ReportOffline();
    TestFalse(TEXT("shared state offline"), Shared->IsOnline());
    TestTrue(TEXT("other Hub remains online"), Isolated->IsOnline());
    TestEqual(TEXT("edge once"), Seen->Num(), 1);
    const auto Probes = MakeShared<int32, ESPMode::ThreadSafe>(0);
    Shared->RegisterProbe(Left.Client.GetId(), [Probes] { ++*Probes; return false; });
    Left.Clock->Advance(9.999);
    TestEqual(TEXT("no early probe"), *Probes, 0);
    Left.Clock->Advance(0.001);
    TestEqual(TEXT("probe at ten seconds"), *Probes, 1);
    Shared->ReportOnline();
    Left.Clock->Advance(20);
    TestEqual(TEXT("online stops probing"), *Probes, 1);
    Shared->UnregisterProbe(Left.Client.GetId());
    TestTrue(TEXT("drain"), Left.Drain() && Right.Drain());
    return true;
}

namespace VaRest::Http::ConnectivityTestPrivate
{
    void Abandon(Testing::FTestRig& Rig)
    {
        Rig.Client.CancelAll();
        Testing::PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });
        Rig.Drain();
    }
    // HttpTransport copies response status independently from its subsequent timeout failure mapping.
    class FStatusBeforeTimeoutTransport final : public IVaRestTransport
    {
    public:
        explicit FStatusBeforeTimeoutTransport(TSharedRef<FVaRestFakeTransport, ESPMode::ThreadSafe> InFake)
            : Fake(MoveTemp(InFake)) {}
        virtual TSharedRef<IVaRestTransportCall, ESPMode::ThreadSafe> Start(
            FVaRestTransportRequest&& Request, FVaRestTransportComplete&& OnComplete) override
        {
            return Fake->Start(MoveTemp(Request), [Done = MoveTemp(OnComplete)](FVaRestTransportResult&& Result) mutable
            {
                if (Result.Failure == EVaRestTransportFailure::Timeout) Result.Status = 200;
                Done(MoveTemp(Result));
            });
        }
    private:
        TSharedRef<FVaRestFakeTransport, ESPMode::ThreadSafe> Fake;
    };
    class FHoldingAuth final : public IVaRestAuth
    {
    public:
        virtual FName GetAuthName() const override { return TEXT("ConnectivityHeld"); }
        virtual bool Apply(FVaRestAuthTarget&) const override { return true; }
        virtual bool CanRefresh(const FVaRestSessionData&) const override { return true; }
        virtual bool NeedsRefresh(const FVaRestSessionData&, double) const override { return true; }
        virtual void Refresh(const FVaRestAuthRefreshContext&, TUniqueFunction<void(FVaRestAuthRefreshResult&&)>&& Done) override
        { Held = MoveTemp(Done); }
        void Answer(FVaRestAuthRefreshResult&& Result)
        { auto Done = MoveTemp(Held); if (Done) Done(MoveTemp(Result)); }
        TUniqueFunction<void(FVaRestAuthRefreshResult&&)> Held;
    };
    class FCaptureStage final : public IVaRestStage
    {
    public:
        virtual FName GetStageName() const override { return TEXT("ConnectivityCapture"); }
        virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& Done) override
        { Captured = Exchange; Next.Proceed(Exchange, MoveTemp(Done)); }
        TWeakPtr<FVaRestExchange, ESPMode::ThreadSafe> Captured;
    };
    class FSyntheticStage final : public IVaRestStage
    {
    public:
        virtual FName GetStageName() const override { return TEXT("ConnectivitySynthetic"); }
        virtual void Process(const FVaRestExchangeRef&, const FVaRestStageNext&, FVaRestOutcomeCallback&& Done) override
        { FVaRestAttemptOutcome Result; Result.Status = 200; Done(MoveTemp(Result)); }
    };
    struct FScopedForce
    {
        IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("VaRest.Http.ForceOffline"));
        int32 Old = Var ? Var->GetInt() : 0;
        EConsoleVariableFlags OldFlags = Var ? Var->GetFlags() : ECVF_Default;
        ~FScopedForce()
        {
            if (!Var) return;
            Var->Set(Old, ECVF_SetByConsole);
            Var->SetFlags(OldFlags);
            IConsoleManager::Get().CallAllConsoleVariableSinks();
        }
        void Set(int32 Value)
        { if (Var) { Var->Set(Value, ECVF_SetByConsole); IConsoleManager::Get().CallAllConsoleVariableSinks(); } }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpConnectivityObserveTest, "VaRest.Http.Connectivity.PhysicalObservationAndRetry",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestHttpConnectivityObserveTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http::Testing;
    FTestRig Rig;
    ON_SCOPE_EXIT { VaRest::Http::ConnectivityTestPrivate::Abandon(Rig); };
    FScopedTraceRecorder Trace;
    auto Policy = FVaRestRetryPolicy::Exponential(2);
    Policy.Backoff.Jitter = EVaRestJitter::None;
    Rig.Fake->Enqueue(FVaRestFakeResponse::Failed(EVaRestTransportFailure::Connection));
    Rig.Fake->Enqueue(FVaRestFakeResponse::Failed(EVaRestTransportFailure::Connection));
    Rig.Fake->Enqueue(FVaRestFakeResponse::Failed(EVaRestTransportFailure::Connection));
    auto Result = MakeOutcome<void>();
    const auto Handle = Observe(Rig.Client.Get<void>(TEXT("/discover")).Retry(Policy), Result).Send();
    TestTrue(TEXT("first failure settled"), Rig.Drain());
    TestFalse(TEXT("first physical failure marks offline before retry"), Rig.Connectivity->IsOnline());
    TestFalse(TEXT("discovering request is still retrying"), Result->IsDone());
    Rig.Clock->Advance(0.5);
    TestTrue(TEXT("second attempt settles"), Rig.Drain());
    Rig.Clock->Advance(1.0);
    TestTrue(TEXT("final failure"), Rig.Wait(Result));
    TestEqual(TEXT("full configured retry cycle"), Rig.Fake->NumCalls(), 3);
    TestTrue(TEXT("physical Connection retained"), Result->Error.Kind == EVaRestErrorKind::Connection);
    const auto Markers = Trace.Recorder->MarkersOf(Rig.Client.GetId());
    if (TestEqual(TEXT("one offline edge"), Markers.Num(), 1))
        TestEqual(TEXT("physical cause id"), Markers[0].CauseRequestId, Handle.GetId());
    for (int32 Status : {401, 500})
    {
        Rig.Connectivity->ReportOnline();
        Rig.Fake->Enqueue(FVaRestFakeResponse::Bytes(Status, {}));
        auto Http = MakeOutcome<void>();
        const int32 CallsBefore = Rig.Fake->NumCalls();
        Observe(Rig.Client.Get<void>(TEXT("/status")).NoAuth().NoRetry(), Http).Send();
        if (!TestEqual(TEXT("HTTP status target dispatched online"), Rig.Fake->NumCalls(), CallsBefore + 1)) return false;
        Rig.Connectivity->ReportOffline(); // The already-dispatched physical response must restore reachability.
        TestTrue(TEXT("HTTP response completed"), Rig.Wait(Http));
        TestTrue(TEXT("HTTP errors prove online"), Rig.Connectivity->IsOnline());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpConnectivityStatusTimeoutTest, "VaRest.Http.Connectivity.ResponseBeforeWireTimeout",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestHttpConnectivityStatusTimeoutTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    FTestRig Rig;
    ON_SCOPE_EXIT { ConnectivityTestPrivate::Abandon(Rig); };
    const auto Capture = MakeShared<ConnectivityTestPrivate::FCaptureStage, ESPMode::ThreadSafe>();
    Rig.Client.Use(Capture, EVaRestStageSlot::PerAttempt);
    Rig.Client.WithTransport(MakeShared<ConnectivityTestPrivate::FStatusBeforeTimeoutTransport, ESPMode::ThreadSafe>(Rig.Fake))
        .WithRetry(FVaRestRetryPolicy::Disabled().WithDeadline(10.f)).WithTimeout(1.f);
    Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
    auto Result = MakeOutcome<void>();
    Observe(Rig.Client.Get<void>(TEXT("/headers-then-stall")), Result).Send();
    TestFalse(TEXT("Send does not complete inline"), Result->IsDone());
    if (!TestEqual(TEXT("one physical timeout target"), Rig.Fake->NumCalls(), 1)) return false;
    const auto Exchange = Capture->Captured.Pin();
    if (!TestTrue(TEXT("live exchange captured"), Exchange.IsValid())) return false;
    TestEqual(TEXT("wire timeout is shorter than logical budget"), Rig.Fake->GetCall(0).Request.Timeout, 1.f);
    Rig.Connectivity->ReportOffline();
    Rig.Clock->Advance(1.0);
    TestTrue(TEXT("logical deadline is still open"), Rig.Clock->Now() < Internals(*Exchange).DeadlineAt);
    TestFalse(TEXT("physical timeout completion is deferred"), Result->IsDone());
    TestTrue(TEXT("timeout result completes"), Rig.Wait(Result) && Rig.Drain());
    TestTrue(TEXT("received HTTP status proves online despite wire timeout"), Rig.Connectivity->IsOnline());
    TestTrue(TEXT("typed timeout outcome is preserved"), Result->Error.Kind == EVaRestErrorKind::Timeout);
    TestEqual(TEXT("received status is preserved"), Result->Response.Status, 200);
    TestEqual(TEXT("one completion"), Result->CompleteCalls.load(), 1);
    TestEqual(TEXT("no successful result fabricated"), Result->SuccessCalls.load(), 0);
    TestEqual(TEXT("one physical dispatch without retries"), Rig.Fake->NumCalls(), 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpConnectivityIgnoreTest, "VaRest.Http.Connectivity.IgnoresLocalAndStale",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestHttpConnectivityIgnoreTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    FTestRig Rig;
    ON_SCOPE_EXIT { ConnectivityTestPrivate::Abandon(Rig); };
    const auto Capture = MakeShared<ConnectivityTestPrivate::FCaptureStage, ESPMode::ThreadSafe>();
    Rig.Client.Use(Capture, EVaRestStageSlot::PerAttempt);
    // Both directions: local failures neither invent online nor take an online scope offline.
    for (bool bInitiallyOnline : {false, true})
    {
        if (bInitiallyOnline) Rig.Connectivity->ReportOnline(); else Rig.Connectivity->ReportOffline();
        auto Bad = MakeOutcome<void>();
        Observe(Rig.Client.Request<void>(TEXT(""), TEXT("/bad")), Bad).Send();
        TestFalse(TEXT("local completion is deferred"), Bad->IsDone());
        TestTrue(TEXT("Config finishes"), Rig.Wait(Bad) && Bad->Error.Kind == EVaRestErrorKind::Config);
        TestEqual(TEXT("Config leaves state"), Rig.Connectivity->IsOnline(), bInitiallyOnline);
        for (const auto Failure : {EVaRestTransportFailure::Timeout, EVaRestTransportFailure::Canceled})
        {
            Rig.Connectivity->ReportOnline();
            Rig.Fake->Enqueue(FVaRestFakeResponse::Failed(Failure));
            auto Local = MakeOutcome<void>();
            const int32 CallsBefore = Rig.Fake->NumCalls();
            Observe(Rig.Client.Get<void>(TEXT("/local")).NoRetry(), Local).Send();
            if (!TestEqual(TEXT("local-result target dispatched online"), Rig.Fake->NumCalls(), CallsBefore + 1)) return false;
            if (!bInitiallyOnline) Rig.Connectivity->ReportOffline();
            TestTrue(TEXT("local transport end"), Rig.Wait(Local));
            TestEqual(TEXT("local result leaves state"), Rig.Connectivity->IsOnline(), bInitiallyOnline);
        }
    }
    Rig.Connectivity->ReportOnline();
    Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
    auto Held = MakeOutcome<void>();
    const auto Handle = Observe(Rig.Client.Get<void>(TEXT("/held")).NoRetry(), Held).Send();
    const auto Exchange = Capture->Captured.Pin();
    if (!TestTrue(TEXT("captured live dispatch"), Exchange.IsValid())) return false;
    auto& Ex = Internals(*Exchange);
    if (!TestTrue(TEXT("stale target has a current physical dispatch"), Ex.LiveDispatch != 0)) return false;
    Rig.Connectivity->ReportOffline();
    FDispatchTag Stale;
    Stale.Serial = Ex.LiveDispatch + 1;
    Stale.Attempt = Ex.Attempt;
    FVaRestTransportResult Late;
    Late.Status = 200;
    Ex.State->OnAttemptDone(Exchange.ToSharedRef(), MoveTemp(Stale), MoveTemp(Late), [](FVaRestAttemptOutcome&&) {});
    TestFalse(TEXT("stale physical completion cannot mark online"), Rig.Connectivity->IsOnline());
    Handle.Cancel();
    TestTrue(TEXT("cancel drains"), Rig.Wait(Held) && Rig.Drain());
    TestFalse(TEXT("cancel leaves offline"), Rig.Connectivity->IsOnline());
    FTestRig Synthetic;
    Synthetic.Connectivity->ReportOffline();
    Synthetic.Client.Use(MakeShared<ConnectivityTestPrivate::FSyntheticStage, ESPMode::ThreadSafe>(), EVaRestStageSlot::Outer);
    auto FakeSuccess = MakeOutcome<void>();
    Observe(Synthetic.Client.Get<void>(TEXT("/synthetic")), FakeSuccess).Send();
    TestTrue(TEXT("synthetic success delivered"), Synthetic.Wait(FakeSuccess) && Synthetic.Drain());
    TestFalse(TEXT("synthetic success is not reachability"), Synthetic.Connectivity->IsOnline());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpConnectivityClosedTest, "VaRest.Http.Connectivity.IgnoresClosedDeadlineAndOwner",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestHttpConnectivityClosedTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    for (int32 Mode = 0; Mode < 4; ++Mode)
    {
        FTestRig Rig;
        ON_SCOPE_EXIT { ConnectivityTestPrivate::Abandon(Rig); };
        const auto Capture = MakeShared<ConnectivityTestPrivate::FCaptureStage, ESPMode::ThreadSafe>();
        Rig.Client.Use(Capture, EVaRestStageSlot::PerAttempt);
        Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
        UObject* Owner = NewObject<UVaRestHttpSettings>(GetTransientPackage(), NAME_None, RF_Transient);
        auto Result = MakeOutcome<void>();
        const auto Handle = Observe(Rig.Client.Get<void>(TEXT("/late")).Owner(Owner), Result).Send();
        const auto Exchange = Capture->Captured.Pin();
        if (!TestTrue(TEXT("captured"), Exchange.IsValid())) return false;
        auto& Ex = Internals(*Exchange);
        const uint32 Serial = Ex.LiveDispatch;
        if (!TestTrue(TEXT("closed target has a current physical dispatch"), Serial != 0)) return false;
        Rig.Connectivity->ReportOffline();
        if (Mode == 0) { Handle.Cancel(); TestTrue(TEXT("canceled done"), Rig.Wait(Result)); }
        if (Mode == 1) Ex.bCompletionQueued = true;
        if (Mode == 2) { Rig.Clock->Advance(1.0); Ex.DeadlineAt = Rig.Clock->Now(); }
        if (Mode == 3) Owner->MarkAsGarbage();
        FDispatchTag Tag;
        Tag.Serial = Serial;
        Tag.Attempt = Ex.Attempt;
        FVaRestTransportResult Late;
        Late.Status = 200;
        Ex.State->OnAttemptDone(Exchange.ToSharedRef(), MoveTemp(Tag), MoveTemp(Late), [](FVaRestAttemptOutcome&&) {});
        TestFalse(TEXT("closed/accepted/deadline/orphan cannot mark online"), Rig.Connectivity->IsOnline());
        if (Mode == 1) Ex.bCompletionQueued = false;
        // The stimulus consumed the physical slot; complete the actual fake and cancel the exchange for cleanup.
        if (Rig.Fake->NumCalls() > 0) Rig.Fake->Complete(0, FVaRestFakeResponse::Bytes(200, {}));
        Rig.Client.CancelAll();
        TestTrue(TEXT("late dispatch drained"), Rig.Drain());
    }
    FTestRig Dropped;
    ON_SCOPE_EXIT { ConnectivityTestPrivate::Abandon(Dropped); };
    Dropped.Fake->Enqueue(FVaRestFakeResponse::Manual());
    auto Result = MakeOutcome<void>();
    Observe(Dropped.Client.Get<void>(TEXT("/dropped")), Result).Send();
    if (TestEqual(TEXT("one drop target"), Dropped.Fake->NumCalls(), 1)) Dropped.Fake->Drop(0);
    TestTrue(TEXT("drop finishes"), Dropped.Wait(Result));
    TestTrue(TEXT("dropped synthetic completion leaves online"), Dropped.Connectivity->IsOnline());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpConnectivityProbeTest, "VaRest.Http.Connectivity.OneHealthProbeInFlight",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestHttpConnectivityProbeTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    FTestRig Left;
    FTestRig Right;
    ON_SCOPE_EXIT { ConnectivityTestPrivate::Abandon(Left); ConnectivityTestPrivate::Abandon(Right); };
    const auto Shared = Left.Connectivity;
    Shared->SetProbeInterval(1.0);
    Left.Client.WithHealthUrl(TEXT("https://api.test/health")).WithAuth(FVaRestAuth::Bearer()).WithRetry(FVaRestRetryPolicy::Exponential(3));
    const auto Capture = MakeShared<ConnectivityTestPrivate::FCaptureStage, ESPMode::ThreadSafe>();
    Left.Client.Use(Capture, EVaRestStageSlot::PerAttempt);
    Left.Client.SetBearerToken(TEXT("probe-must-not-carry-this-token"));
    Right.Client.WithHealthUrl(TEXT("https://api.test/right"));
    const TWeakPtr<FClientState, ESPMode::ThreadSafe> WeakRight(FClientAccess::State(Right.Client));
    const TWeakPtr<FVaRestConnectivity, ESPMode::ThreadSafe> WeakShared(Shared);
    // Reentrant registration while the first callback is invoking Send, then another registration while it is held.
    Left.Fake->SetResponder([WeakRight, WeakShared](const FVaRestTransportRequest&)
    {
        if (const auto State = WeakRight.Pin())
            if (const auto Scope = WeakShared.Pin()) FClientAccess::Wrap(State).WithConnectivity(Scope.ToSharedRef());
        return FVaRestFakeResponse::Manual();
    });
    Right.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
    Shared->ReportOffline();
    Left.Clock->Advance(1.0);
    if (!TestEqual(TEXT("first health request held"), Left.Fake->NumCalls(), 1)) return false;
    const auto& First = Left.Fake->GetCall(0).Request;
    TestEqual(TEXT("HEAD"), First.Verb, FString(TEXT("HEAD")));
    const auto ProbeExchange = Capture->Captured.Pin();
    if (TestTrue(TEXT("health exchange captured"), ProbeExchange.IsValid()))
        TestTrue(TEXT("core probe flag"), Internals(*ProbeExchange).bConnectivityProbe);
    TestFalse(TEXT("NoAuth"), First.Headers.Contains(TEXT("Authorization")));
    TestTrue(TEXT("bounded timeout"), First.Timeout > 0.f && First.Timeout <= 10.f);
    Right.Client.WithConnectivity(Shared);
    Left.Clock->Advance(1.0);
    TestEqual(TEXT("only one health probe may overlap across owners"), Left.Fake->NumInFlight() + Right.Fake->NumInFlight(), 1);
    TestEqual(TEXT("no second probe before completion"), Left.Fake->NumCalls() + Right.Fake->NumCalls(), 1);
    Left.Fake->Complete(0, FVaRestFakeResponse::Failed(EVaRestTransportFailure::Connection));
    TestTrue(TEXT("failed health completion drains"), Left.Drain());
    TestEqual(TEXT("probe does not retry"), Left.Fake->NumCalls(), 1);
    Left.Clock->Advance(0.999);
    TestEqual(TEXT("next probe waits whole interval"), Left.Fake->NumCalls(), 1);
    Left.Clock->Advance(0.001);
    if (TestEqual(TEXT("completion permits next probe"), Left.Fake->NumCalls(), 2))
        Left.Fake->Complete(1, FVaRestFakeResponse::Bytes(500, {}));
    TestTrue(TEXT("HTTP health completion drains"), Left.Drain());
    TestTrue(TEXT("unhealthy HTTP response proves reachability"), Shared->IsOnline());
    Left.Clock->Advance(5.0);
    TestEqual(TEXT("online stops timer"), Left.Fake->NumCalls(), 2);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpConnectivityProbeLifetimeTest, "VaRest.Http.Connectivity.ProbeLifetimeAndReconfiguration",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestHttpConnectivityProbeLifetimeTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    FTestRig Rig;
    ON_SCOPE_EXIT { ConnectivityTestPrivate::Abandon(Rig); };
    Rig.Connectivity->SetProbeInterval(0.1);
    Rig.Connectivity->ReportOffline();
    Rig.Client.WithHealthUrl(TEXT("https://foreign.test/health"));
    Rig.Clock->Advance(0.1);
    TestEqual(TEXT("cross-origin health skipped"), Rig.Fake->NumCalls(), 0);
    Rig.Client.WithHealthUrl(TEXT("https://api.test/health"));
    Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
    Rig.Clock->Advance(0.1);
    if (!TestEqual(TEXT("current health config read"), Rig.Fake->NumCalls(), 1)) return false;
    const auto Old = Rig.Connectivity;
    const auto New = FVaRestConnectivity::Make(Rig.Clock);
    New->SetProbeInterval(0.1);
    Rig.Client.WithConnectivity(New);
    Rig.Client.CancelAll();
    TestTrue(TEXT("cancel releases old probe"), Rig.Drain());
    Rig.Clock->Advance(0.2);
    TestEqual(TEXT("old owner unregistered"), Rig.Fake->NumCalls(), 1);
    New->ReportOffline();
    Rig.Clock->Advance(0.1);
    TestTrue(TEXT("new owner probes"), Rig.Drain());
    TestEqual(TEXT("new connectivity selected"), Rig.Fake->NumCalls(), 2);
    TestTrue(TEXT("new state online"), New->IsOnline());
    TestFalse(TEXT("old state still offline"), Old->IsOnline());
    // Sole registry-held capture remains alive through self-unregistration.
    const auto Observed = MakeShared<TArray<bool>, ESPMode::ThreadSafe>();
    TSharedPtr<int32, ESPMode::ThreadSafe> Capture = MakeShared<int32, ESPMode::ThreadSafe>(42);
    TWeakPtr<int32, ESPMode::ThreadSafe> WeakCapture(Capture);
    const TWeakPtr<FVaRestConnectivity, ESPMode::ThreadSafe> WeakOld(Old);
    Old->RegisterProbe(999999, [Capture, WeakCapture, WeakOld, Observed]
    {
        if (const auto Scope = WeakOld.Pin()) Scope->UnregisterProbe(999999);
        Observed->Add(WeakCapture.IsValid() && *Capture == 42);
        return false;
    });
    Capture.Reset();
    Rig.Clock->Advance(0.1);
    TestTrue(TEXT("owned callback survives unregister"), Observed->Num() == 1 && (*Observed)[0]);
    TestFalse(TEXT("capture released after invocation"), WeakCapture.IsValid());
    // A refused health Send also releases the running slot through its deferred completion.
    Rig.Client.WithHealthUrl(TEXT("https://api.test/{missing}"));
    New->ReportOffline();
    Rig.Clock->Advance(0.1);
    TestTrue(TEXT("local probe refusal settles"), Rig.Drain());
    Rig.Client.WithHealthUrl(TEXT("https://api.test/health"));
    Rig.Clock->Advance(0.1);
    TestTrue(TEXT("probe after local refusal settles"), Rig.Drain());
    TestEqual(TEXT("local refusal did not wedge running slot"), Rig.Fake->NumCalls(), 3);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpConnectivityEventsTest, "VaRest.Http.Connectivity.EventsFifoAndDeferredRequest",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestHttpConnectivityEventsTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    FTestRig Rig;
    auto Holder = MakeShared<FVaRestClient, ESPMode::ThreadSafe>(Rig.Client);
    const TWeakPtr<FClientState, ESPMode::ThreadSafe> WeakClient(FClientAccess::State(Rig.Client));
    Rig.Client = FVaRestClient();
    const auto Scope = Rig.Connectivity;
    const auto Seen = MakeShared<TArray<bool>, ESPMode::ThreadSafe>();
    const auto Depth = MakeShared<int32, ESPMode::ThreadSafe>(0);
    const auto MaxDepth = MakeShared<int32, ESPMode::ThreadSafe>(0);
    const auto Result = MakeOutcome<void>();
    const auto Inline = MakeShared<bool, ESPMode::ThreadSafe>(false);
    const TWeakPtr<FVaRestConnectivity, ESPMode::ThreadSafe> WeakScope(Scope);
    const auto Handle = Scope->OnChanged().AddLambda([WeakScope, Holder, Seen, Depth, MaxDepth, Result, Inline](bool bOnline)
    {
        ++*Depth; *MaxDepth = FMath::Max(*MaxDepth, *Depth);
        Seen->Add(bOnline);
        if (Seen->Num() == 1)
        {
            Observe(Holder->Request<void>(TEXT(""), TEXT("/local")), Result).Send();
            *Inline = Result->IsDone();
            Holder->WithConnectivity(FVaRestConnectivity::Make(FVaRestRealClock::Get()));
            *Holder = FVaRestClient();
            if (const auto Pinned = WeakScope.Pin()) { Pinned->ReportOnline(); Pinned->ReportOffline(); }
        }
        --*Depth;
    });
    Scope->ReportOffline();
    TestFalse(TEXT("listener Send remains deferred through transition return"), Result->IsDone() || *Inline);
    TestEqual(TEXT("nested listener never recurses"), *MaxDepth, 1);
    if (TestEqual(TEXT("all nested edges delivered synchronously"), Seen->Num(), 3))
        TestTrue(TEXT("FIFO offline online offline"), !(*Seen)[0] && (*Seen)[1] && !(*Seen)[2]);
    TestFalse(TEXT("final state reconciled"), Scope->IsOnline());
    TestTrue(TEXT("local request eventually completes once"), Rig.Wait(Result) && Result->CompleteCalls.load() == 1 && Rig.Drain());
    Scope->OnChanged().Remove(Handle);
    TestFalse(TEXT("listener dropped the last client; in-flight ownership drained"), WeakClient.IsValid());
    // The event may release the very last external scope owner; delivery itself pins the object.
    auto Last = MakeShared<TSharedPtr<FVaRestConnectivity, ESPMode::ThreadSafe>, ESPMode::ThreadSafe>(FVaRestConnectivity::Make(Rig.Clock));
    const TWeakPtr<FVaRestConnectivity, ESPMode::ThreadSafe> WeakLast(*Last);
    const auto Alive = MakeShared<bool, ESPMode::ThreadSafe>(false);
    (*Last)->OnChanged().AddLambda([Last, WeakLast, Alive](bool) { Last->Reset(); *Alive = WeakLast.IsValid(); });
    (*Last)->ReportOffline();
    TestTrue(TEXT("strong self during callbacks"), *Alive);
    TestFalse(TEXT("dies after callback delivery"), WeakLast.IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpConnectivityIntervalsTest, "VaRest.Http.Connectivity.IntervalsHintsAndClock",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestHttpConnectivityIntervalsTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http::Testing;
    FTestRig Rig;
    const auto Previous = GetConnectivityHint();
    ON_SCOPE_EXIT { SetConnectivityHint(Previous); };
    const auto Count = MakeShared<int32, ESPMode::ThreadSafe>(0);
    Rig.Connectivity->RegisterProbe(Rig.Client.GetId(), [Count] { ++*Count; return false; });
    Rig.Connectivity->ReportOffline();
    Rig.Connectivity->SetProbeInterval(-1.0);
    SetConnectivityHint(ENetworkConnectionType::None);
    Rig.Clock->Advance(0.2);
    TestEqual(TEXT("None suppresses"), *Count, 0);
    SetConnectivityHint(ENetworkConnectionType::AirplaneMode);
    Rig.Clock->Advance(0.1);
    TestEqual(TEXT("airplane suppresses"), *Count, 0);
    SetConnectivityHint(ENetworkConnectionType::Unknown);
    Rig.Clock->Advance(0.1);
    TestEqual(TEXT("Unknown permits and bad interval clamps without spin"), *Count, 1);
    SetConnectivityHint(ENetworkConnectionType::WiFi);
    Rig.Clock->Advance(0.1);
    TestEqual(TEXT("WiFi permits"), *Count, 2);
    TestFalse(TEXT("hint never claims online"), Rig.Connectivity->IsOnline());
    Rig.Connectivity->SetProbeInterval(std::numeric_limits<double>::quiet_NaN());
    Rig.Clock->Advance(9.999);
    TestEqual(TEXT("NaN falls back to ten seconds"), *Count, 2);
    Rig.Clock->Advance(0.001);
    TestEqual(TEXT("NaN finite fallback fires"), *Count, 3);
    Rig.Connectivity->SetProbeInterval(std::numeric_limits<double>::infinity());
    Rig.Clock->Advance(10);
    TestEqual(TEXT("infinity also finite fallback"), *Count, 4);
    Rig.Connectivity->SetProbeInterval(10000);
    Rig.Clock->Advance(3599.9);
    TestEqual(TEXT("upper clamp not early"), *Count, 4);
    Rig.Clock->Advance(0.1);
    TestEqual(TEXT("upper clamp at hour"), *Count, 5);
    Rig.Connectivity->SetProbeInterval(1.0);
    auto OtherClock = MakeShared<FVaRestVirtualClock, ESPMode::ThreadSafe>();
    Rig.Client.WithClock(OtherClock);
    OtherClock->Advance(5);
    TestEqual(TEXT("WithClock does not retime connectivity"), *Count, 5);
    Rig.Clock->Advance(1);
    TestEqual(TEXT("original connectivity clock still owns timer"), *Count, 6);
    Rig.Connectivity->UnregisterProbe(Rig.Client.GetId());
    TestTrue(TEXT("drain"), Rig.Drain());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpConnectivityForceTest, "VaRest.Http.Connectivity.ForceOffline",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestHttpConnectivityForceTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    ConnectivityTestPrivate::FScopedForce Override;
    if (!TestNotNull(TEXT("nonshipping cvar registered"), Override.Var)) return false;
    Override.Set(0);
    FTestRig Rig;
    ON_SCOPE_EXIT { ConnectivityTestPrivate::Abandon(Rig); };
    FScopedTraceRecorder Trace;
    Rig.Connectivity->SetProbeInterval(1);
    Rig.Client.WithHealthUrl(TEXT("https://api.test/health"));
    Rig.Connectivity->ReportOffline();
    Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
    Rig.Clock->Advance(1);
    if (!TestEqual(TEXT("old probe in flight"), Rig.Fake->NumCalls(), 1)) return false;
    Override.Set(1);
    Rig.Fake->Complete(0, FVaRestFakeResponse::Bytes(200, {}));
    TestTrue(TEXT("old response drained"), Rig.Drain());
    TestFalse(TEXT("override masks observed online"), Rig.Connectivity->IsOnline());
    Rig.Clock->Advance(5);
    TestEqual(TEXT("force prevents further health"), Rig.Fake->NumCalls(), 1);
    Rig.Client.WithAuth(FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig()));
    Rig.Client.SetSession(TEXT("access-value"), TEXT("refresh-value"));
    auto Answer = MakeShared<TArray<EVaRestErrorKind>, ESPMode::ThreadSafe>();
    Rig.Client.RefreshAuth([Answer](EVaRestAuthRefreshOutcome, EVaRestErrorKind Kind) { Answer->Add(Kind); });
    TestEqual(TEXT("refresh callback deferred"), Answer->Num(), 0);
    TestTrue(TEXT("forced refresh finishes"), PumpUntil([Answer] { return Answer->Num() == 1; }));
    if (TestEqual(TEXT("one refresh answer"), Answer->Num(), 1)) TestTrue(TEXT("forced refresh Offline"), (*Answer)[0] == EVaRestErrorKind::Offline);
    TestEqual(TEXT("forced refresh never on wire"), Rig.Fake->NumCalls(), 1);
    Override.Set(0);
    TestTrue(TEXT("removal reveals latest observed online"), Rig.Connectivity->IsOnline());
    const auto Markers = Trace.Recorder->MarkersOf(Rig.Client.GetId());
    TestTrue(TEXT("OverrideRemoved marker"), Markers.ContainsByPredicate([](const FVaRestTraceMarker& Marker)
    { return Marker.Kind == EVaRestTraceMarkerKind::Online && Marker.Text == TEXT("OverrideRemoved"); }));
    Rig.Clock->Advance(2);
    TestEqual(TEXT("online removal does not probe"), Rig.Fake->NumCalls(), 1);
    Override.Set(1);
    Rig.Connectivity->ReportOffline();
    Override.Set(0);
    TestFalse(TEXT("removal reveals latest observed offline"), Rig.Connectivity->IsOnline());
    Rig.Clock->Advance(0.999);
    TestEqual(TEXT("offline removal waits interval"), Rig.Fake->NumCalls(), 1);
    Rig.Clock->Advance(0.001);
    TestTrue(TEXT("offline removal probe completes"), Rig.Drain());
    TestEqual(TEXT("offline removal schedules probe"), Rig.Fake->NumCalls(), 2);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpConnectivityRefreshTest, "VaRest.Http.Connectivity.RefreshOfflineAndFallback",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestHttpConnectivityRefreshTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    // Actual built-in refresh request goes through Enter's independent refusal and the actual coordinator.
    FTestRig Builtin;
    ON_SCOPE_EXIT { ConnectivityTestPrivate::Abandon(Builtin); };
    Builtin.Client.WithAuth(FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig()));
    Builtin.Client.SetSession(TEXT("valid-access"), TEXT("valid-refresh"), 100);
    Builtin.Clock->Advance(80);
    Builtin.Connectivity->ReportOffline();
    auto Original = MakeOutcome<void>();
    Observe(Builtin.Client.Get<void>(TEXT("/original")), Original).Send();
    TestFalse(TEXT("offline request completion deferred"), Original->IsDone());
    TestTrue(TEXT("proactive refresh finishes Offline"), Builtin.Wait(Original));
    TestTrue(TEXT("offline does not fall back to valid token"), Original->Error.Kind == EVaRestErrorKind::Offline && !Original->Error.bRetryable);
    TestEqual(TEXT("neither refresh nor original sent"), Builtin.Fake->NumCalls(), 0);
    TestTrue(TEXT("session retained"), Builtin.Client.GetSession().HasAccessToken());
    // Both public construction routes, with an actually parked request and a RefreshAuth waiter.
    for (bool bByHand : {false, true})
    {
        FTestRig Rig;
        ON_SCOPE_EXIT { ConnectivityTestPrivate::Abandon(Rig); };
        auto Auth = MakeShared<ConnectivityTestPrivate::FHoldingAuth, ESPMode::ThreadSafe>();
        Rig.Client.WithAuth(Auth);
        Rig.Client.SetSession(TEXT("retained-access"), TEXT("retained-refresh"));
        const FGuid Identity = Rig.Client.GetSession().IdentityId;
        auto Answer = MakeShared<TArray<EVaRestErrorKind>, ESPMode::ThreadSafe>();
        Rig.Client.RefreshAuth([Answer](EVaRestAuthRefreshOutcome, EVaRestErrorKind Kind) { Answer->Add(Kind); });
        auto Request = MakeOutcome<void>();
        Observe(Rig.Client.Get<void>(TEXT("/parked")), Request).Send();
        FVaRestAuthRefreshResult Failure;
        if (bByHand) { Failure.Outcome = EVaRestAuthRefreshOutcome::Failed; Failure.FailedKind = EVaRestErrorKind::Offline; }
        else Failure = FVaRestAuthRefreshResult::Failed(EVaRestErrorKind::Offline, TEXT("offline"));
        Auth->Answer(MoveTemp(Failure));
        TestFalse(TEXT("auth answer asynchronous"), Request->IsDone());
        TestTrue(TEXT("parked request delivered"), Rig.Wait(Request) && Rig.Drain());
        TestTrue(TEXT("Offline survives actual auth gate"), Request->Error.Kind == EVaRestErrorKind::Offline && !Request->Error.bRetryable);
        if (TestEqual(TEXT("RefreshAuth receives one answer"), Answer->Num(), 1))
            TestTrue(TEXT("Offline survives FinishRefresh"), (*Answer)[0] == EVaRestErrorKind::Offline);
        TestEqual(TEXT("offline failure does not dispatch"), Rig.Fake->NumCalls(), 0);
        TestTrue(TEXT("Offline retains exact session and identity"), Rig.Client.GetSession().AccessToken == TEXT("retained-access")
            && Rig.Client.GetSession().RefreshToken == TEXT("retained-refresh") && Rig.Client.GetSession().IdentityId == Identity);
    }
    // A held proactive failure may fall back only when connectivity permits it at the eventual handback.
    for (bool bRecover : {false, true})
    {
        FTestRig Rig;
        ON_SCOPE_EXIT { ConnectivityTestPrivate::Abandon(Rig); };
        auto Auth = MakeShared<ConnectivityTestPrivate::FHoldingAuth, ESPMode::ThreadSafe>();
        Rig.Client.WithAuth(Auth);
        Rig.Client.SetSession(TEXT("still-valid-access"), TEXT("still-valid-refresh"), 100);
        auto Request = MakeOutcome<void>();
        Observe(Rig.Client.Get<void>(TEXT("/fallback")), Request).Send();
        Rig.Connectivity->ReportOffline();
        Auth->Answer(FVaRestAuthRefreshResult::Failed(EVaRestErrorKind::Connection, TEXT("failed")));
        if (bRecover) Rig.Connectivity->ReportOnline();
        TestTrue(TEXT("fallback decision completes"), Rig.Wait(Request));
        TestEqual(TEXT("only now-online may dispatch"), Rig.Fake->NumCalls(), bRecover ? 1 : 0);
        TestTrue(TEXT("fallback result"), bRecover ? Request->SuccessCalls.load() == 1 : Request->Error.Kind == EVaRestErrorKind::Offline);
        TestTrue(TEXT("fallback keeps session"), Rig.Client.GetSession().HasAccessToken());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpConnectivityStorageTest, "VaRest.Http.Connectivity.StorageSnapshotsAndClassification",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestHttpConnectivityStorageTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    FTestRig Rig;
    const auto State = FClientAccess::State(Rig.Client);
    if (!TestTrue(TEXT("state"), State.IsValid())) return false;
    const auto Defaults = State->GetConfig();
    TestTrue(TEXT("default absolute roots"), !FPaths::IsRelative(Defaults->CacheRoot) && !FPaths::IsRelative(Defaults->QueueRoot));
    TestTrue(TEXT("default suffixes"), Defaults->CacheRoot.EndsWith(TEXT("VaRest/Cache")) && Defaults->QueueRoot.EndsWith(TEXT("VaRest/Queue")));
    const FString Root = FPaths::ProjectSavedDir() / TEXT("ConnectivityPathOnly") / FGuid::NewGuid().ToString();
    const uint64 Launched = PersistenceCounters().Launched.load();
    Rig.Client.WithStorageDirectories(Root / TEXT("a/../cache"), Root / TEXT("queue"));
    const auto Good = State->GetConfig();
    TestTrue(TEXT("canonical explicit roots"), Good->StorageConfigError.IsEmpty() && !Good->CacheRoot.Contains(TEXT("..")) && !FPaths::IsRelative(Good->CacheRoot));
    TestFalse(TEXT("configuration creates no directory"), IFileManager::Get().DirectoryExists(*Root));
    TestEqual(TEXT("configuration launches no filesystem worker"), PersistenceCounters().Launched.load(), Launched);
    TestTrue(TEXT("previous snapshot unchanged"), Defaults->CacheRoot != Good->CacheRoot && Defaults->StorageConfigError.IsEmpty());
    Rig.Client.WithStorageDirectories(TEXT(""), Root);
    const auto Invalid = State->GetConfig();
    TestFalse(TEXT("invalid storage records error"), Invalid->StorageConfigError.IsEmpty());
    TestEqual(TEXT("invalid preserves previous canonical root"), Invalid->CacheRoot, Good->CacheRoot);
    auto Plain = MakeOutcome<void>();
    Observe(Rig.Client.Get<void>(TEXT("/unused-storage")), Plain).Send();
    TestTrue(TEXT("ordinary HTTP unaffected by unused storage error"), Rig.Wait(Plain) && Plain->SuccessCalls.load() == 1);
    Rig.Client.WithStorageDirectories(Root, Root);
    TestTrue(TEXT("valid update clears error"), State->GetConfig()->StorageConfigError.IsEmpty());
    for (const TCHAR* Detail : {TEXT("Offline"), TEXT("QueueExpired"), TEXT("AuthRefreshOffline"), TEXT("PersistenceStopped"),
        TEXT("QueueWriteFailed"), TEXT("QueueFull"), TEXT("PersistenceCredentials"), TEXT("QueueConfig"), TEXT("QueueOrigin"), TEXT("QueueIdentity")})
    {
        FVaRestAttemptOutcome Outcome;
        Outcome.Failure = EVaRestTransportFailure::Other;
        Outcome.FailureDetail = Detail;
        const auto Classified = ClassifyAttempt(Outcome);
        const FString Name(Detail);
        const auto Expected = Name == TEXT("QueueIdentity") ? EVaRestErrorKind::Auth
            : (Name == TEXT("Offline") || Name == TEXT("QueueExpired") || Name == TEXT("AuthRefreshOffline")) ? EVaRestErrorKind::Offline : EVaRestErrorKind::Config;
        TestTrue(Detail, Classified.Kind == Expected && !Classified.bRetryable);
    }
    TestTrue(TEXT("offline factory"), ClassifyAttempt(MakeOfflineOutcome()).Kind == EVaRestErrorKind::Offline);
    TestTrue(TEXT("drain"), Rig.Drain());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpConnectivityBoundedTest, "VaRest.Http.Connectivity.HealthDeadlineIncludesLimiter",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestHttpConnectivityBoundedTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    FTestRig Rig;
    ON_SCOPE_EXIT { ConnectivityTestPrivate::Abandon(Rig); };
    FScopedTraceRecorder Trace;
    Rig.Client.WithMaxConcurrent(1).WithHealthUrl(TEXT("https://api.test/health"));
    Rig.Connectivity->SetProbeInterval(1);
    Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
    auto Blocker = MakeOutcome<void>();
    const auto Handle = Observe(Rig.Client.Get<void>(TEXT("/held")).NoRetry(), Blocker).Send();
    if (!TestEqual(TEXT("limiter occupied"), Rig.Fake->NumCalls(), 1)) return false;
    Rig.Connectivity->ReportOffline();
    Rig.Clock->Advance(1);
    TestEqual(TEXT("probe waits outside transport"), Rig.Fake->NumCalls(), 1);
    uint64 ProbeId = 0;
    const auto State = FClientAccess::State(Rig.Client);
    if (!TestTrue(TEXT("client alive"), State.IsValid())) return false;
    State->ForEachInFlight([&ProbeId](FExchangeState& Ex) { if (Ex.bConnectivityProbe) ProbeId = Ex.RequestId; });
    TestTrue(TEXT("queued probe identified"), ProbeId != 0);
    Rig.Clock->Advance(10);
    TestTrue(TEXT("deadline handback drains"), Rig.Drain());
    TestEqual(TEXT("blocked probe expires while ordinary request remains held"), Rig.Client.GetInFlightCount(), 1);
    TestTrue(TEXT("probe receives Timeout through existing limiter deadline"), Trace.Recorder->For(ProbeId).ContainsByPredicate([](const FVaRestTraceEvent& Event)
    { return Event.Kind == EVaRestTraceKind::Completed && Event.Payload.Get<FVaRestTraceCompleted>().Error.Kind == EVaRestErrorKind::Timeout; }));
    TestFalse(TEXT("local deadline is not online evidence"), Rig.Connectivity->IsOnline());
    Handle.Cancel();
    TestTrue(TEXT("ordinary hold released"), Rig.Wait(Blocker) && Rig.Drain());
    Rig.Clock->Advance(1);
    TestTrue(TEXT("next scheduled probe drains"), Rig.Drain());
    TestEqual(TEXT("ProbeFinished rearms after limiter expiry"), Rig.Fake->NumCalls(), 2);
    TestTrue(TEXT("later health restores online"), Rig.Connectivity->IsOnline());
    return true;
}
#endif
