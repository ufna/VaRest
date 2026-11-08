// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "VaRestManagedOperation.h"
#include "VaRestNativeOwnershipFixture.h"

namespace VaRest::Http
{
struct FManagedCompletionTestAccess
{
	static bool IsPublished(const FClientState& State) { return State.bClientPublished; }
	static void Complete(const FVaRestExchangeRef& Exchange, FFinish&& Finish)
	{
		Internals(*Exchange).State->Complete(Exchange, MoveTemp(Finish)); // REAL private caller entry, never a copied completion
	}
};
} // namespace VaRest::Http

namespace VaRest::Http::Testing::ManagedCompletionCalloutPrivate
{
struct FState
{
	int32 NowCalls = 0;
	int32 StopAtNow = 0;
	int32 Errors = 0;
	int32 HandlerDeaths = 0;
	int32 Wakes = 0;
	int32 PhysicalCallbacks = 0;
	bool StopOnClear = false;
	bool WrongThread = false;
};
class FClock final : public IVaRestClock
{
public:
	FClock(TSharedRef<FState, ESPMode::ThreadSafe> InState, TWeakPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> InControl,
		TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> InClock)
		: State(MoveTemp(InState))
		, Control(MoveTemp(InControl))
		, Clock(MoveTemp(InClock))
	{
	}
	double Now() const override
	{
		const double At = Clock->Now();
		if (++State->NowCalls == State->StopAtNow)
		{
			State->StopAtNow = 0;
			if (auto Kept = Control.Pin())
				Kept->StopInputsOnGT();
		}
		return At;
	}
	FVaRestTimerHandle SetTimer(double Delay, TUniqueFunction<void()>&& Callback) override { return Clock->SetTimer(Delay, MoveTemp(Callback)); }
	void ClearTimer(FVaRestTimerHandle& Handle) override
	{
		Clock->ClearTimer(Handle);
		if (State->StopOnClear)
		{
			State->StopOnClear = false;
			if (auto Kept = Control.Pin())
				Kept->StopInputsOnGT();
		}
	}

private:
	TSharedRef<FState, ESPMode::ThreadSafe> State;
	TWeakPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> Control;
	TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> Clock;
};
struct FCapture
{
	explicit FCapture(TSharedRef<FState, ESPMode::ThreadSafe> InState)
		: State(MoveTemp(InState))
	{
	}
	~FCapture()
	{
		++State->HandlerDeaths;
		State->WrongThread |= !IsInGameThread();
	}
	TSharedRef<FState, ESPMode::ThreadSafe> State;
};
/** Only this fixture's fake record and native lease, disposed BEFORE the exact owned native fixture sweeps on every return. */
struct FRecordCleanup
{
	explicit FRecordCleanup(TSharedRef<FVaRestFakeTransport, ESPMode::ThreadSafe> InFake)
		: Fake(MoveTemp(InFake))
	{
		Fake->bCompleteOnWorker = false;
	}
	~FRecordCleanup() { Release(); }
	void Release()
	{
		if (Fake->NumCalls() == 1)
		{
			if (!Fake->GetCall(0).bCompleted)
				Fake->Drop(0); // no invented normal result; actual uncaught native physical callback ownership ends here
			Fake->ReleaseCompletedClock(0);
		}
	}
	TSharedRef<FVaRestFakeTransport, ESPMode::ThreadSafe> Fake;
};
struct FRecorder
{
	FRecorder() { FVaRestTrace::AddSink(Sink); }
	~FRecorder() { FVaRestTrace::RemoveSink(Sink); }
	TSharedRef<FTraceRecorder, ESPMode::ThreadSafe> Sink = MakeShared<FTraceRecorder, ESPMode::ThreadSafe>(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Bodies);
};
} // namespace VaRest::Http::Testing::ManagedCompletionCalloutPrivate

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestManagedCompletionCalloutTest, "VaRest.Http.Managed.CompletionActualCalloutsAndType",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestManagedCompletionCalloutTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	using namespace ManagedCompletionCalloutPrivate;
	for (int32 Mode = 0; Mode < 4; ++Mode)
	{
#if !VAREST_WITH_TRACE_EXCERPTS
		if (Mode == 2)
		{
			AddInfo(TEXT("discarded-excerpt clock case requires compiled excerpt support"));
			continue;
		}
#endif
		FVaRestTrace::FScopedSinkIsolation Isolation;
		NativeOwnership::FFixture F(true);
		FRecordCleanup Record(F.Rig.Fake);
		FRecorder Trace;
		if (!TestNotNull(TEXT("actual Control-owned unpublished operation store"), F.Store))
			return false;
		// The real managed kernel requires the SAME immutable client registration as its actual operation.
		// Keep this a private unpublished state: no factory CommitClient or Created publication is claimed.
		FClientConfig Initial(*FClientAccess::State(F.Rig.Client)->GetConfig());
		Initial.ScopeLifetime = F.Scope;
		const auto State = FClientState::Create(MoveTemp(Initial), F.Client.InitialRegistration(), false);
		ON_SCOPE_EXIT { F.Control->StopInputsOnGT(); }; // before State's affine teardown, on every return
		const auto& Registration = State->GetManagedLifetime();
		const auto& Reserved = F.Client.InitialRegistration();
		if (!TestTrue(TEXT("actual private state has the same complete native registration"), Registration.IsValid() &&
			Registration.Control.Get() == F.Control.operator->() && Registration.Gate == F.Gate && Registration.Operations == F.Store &&
			Registration.Client.Index == Reserved.Client.Index && Registration.Client.Generation == Reserved.Client.Generation) ||
			!TestTrue(TEXT("the client reservation remains unpublished"), F.Client.IsValid()) ||
			!TestFalse(TEXT("private completion fixture publishes no Created client"), FManagedCompletionTestAccess::IsPublished(State.Get())))
			return false;
		auto Input = FRequestSpecAccess::MakeNativeOwnershipPayload(State, {});
		F.Sender = F.Control->AdoptInput(Input);
		if (!TestTrue(TEXT("real gate admission"), F.Gate->TryAcquire(F.Work)) ||
			!TestTrue(TEXT("real two-tail cell claim"), F.Store->TryClaim(F.Work, F.Sender, F.Claim, 2)))
			return false;
		auto* Owner = F.Claim.CreateManagedPayload();
		if (!TestNotNull(TEXT("actual unique affine payload"), Owner))
			return false;
		auto Seen = MakeShared<FState, ESPMode::ThreadSafe>();
		TSharedPtr<FClientConfig, ESPMode::ThreadSafe> Config = MakeShared<FClientConfig, ESPMode::ThreadSafe>(*State->GetConfig());
		Config->Clock = MakeShared<FClock, ESPMode::ThreadSafe>(Seen, F.Control, F.Rig.Clock);
		Owner->SendConfig = Config;
		Owner->Exchange = MakeShared<FVaRestExchange, ESPMode::ThreadSafe>();
		auto& Ex = Internals(*Owner->Exchange);
		Ex.Managed = Owner->Reference();
		Ex.State = State;
		Ex.Config = Config;
		Config.Reset(); // actual operation/config owners retain the ORIGINAL hook; no extra fixture clock keepalive
		Ex.RequestId = FVaRestTrace::NewRequestId();
		Ex.Phase = EVaRestExchangePhase::InChain;
		Ex.Url = TEXT("https://api.test/completion-entry");
		Ex.RawPath = TEXT("/completion-entry");
		auto Handler = MakeUnique<TResultHandler<FString>>();
		Handler->OnError = [Seen, Capture = MakeUnique<FCapture>(Seen)](const FVaRestError&) { ++Seen->Errors; };
		Ex.Handler = MoveTemp(Handler);
		const uint64 Id = Ex.RequestId;
		F.Handoff = F.Claim.PublishManaged(); // real Send publication form binds the SAME native Completion later in Begin
		if (!TestTrue(TEXT("real initial publication"), F.Handoff.IsHeld()))
			return false;
		if (Mode == 3)
		{
			auto Physical = F.Store->TryAddDispatchOnGT(F.Handoff.Reference());
			if (!TestTrue(TEXT("real physical settlement BEFORE fake Start"), Physical.IsHeld()))
				return false;
			FVaRestTransportRequest Request;
			Request.Verb = TEXT("GET");
			Request.Url = Ex.Url;
			Request.Clock = Ex.Config->Clock;
			F.Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
			Ex.TransportCall = F.Rig.Fake->Start(MoveTemp(Request), [Held = MoveTemp(Physical), Seen](FVaRestTransportResult&&) { ++Seen->PhysicalCallbacks; });
			Ex.ProgressTimer = F.Rig.Clock->SetTimer(60.0, [] {});
			Ex.Interrupt = [Seen] { ++Seen->Wakes; return true; };
			Seen->StopOnClear = true;
		}
		if (Mode == 2)
		{
			if (!TestTrue(TEXT("actual body excerpt mask is enabled"), FVaRestTrace::Wants(EVaRestTraceDetail::Bodies)))
				return false;
			Ex.Attempt = Ex.ExcerptOnDecodeAttempt = 1;
			Ex.ExcerptOnDecodeDispatchId = 1;
			Ex.ExcerptOnDecodeBytes = 16;
		}
		Seen->NowCalls = 0;
		Seen->StopAtNow = Mode == 1 || Mode == 2 ? 2 : 0; // elapsed read first; EXACT later Completed/excerpt read stops
		FFinish Finish;
		Finish.Response.RequestId = Id;
		Finish.Clock = Ex.Config->Clock;
		Finish.Error.Kind = EVaRestErrorKind::Config;
		Finish.Error.Message = TEXT("actual typed completion-entry fixture");
		F.Handoff.Reset(); // no hidden initial stack tail protects the actual completion call after selection
		FManagedCompletionTestAccess::Complete(Owner->Exchange.ToSharedRef(), MoveTemp(Finish));
		int32 Completed = 0, Excerpts = 0;
		FName Type;
		for (const auto& Event : Trace.Sink->For(Id))
		{
			if (Event.Kind == EVaRestTraceKind::Completed)
			{
				++Completed;
				Type = Event.Payload.Get<FVaRestTraceCompleted>().ResultType;
			}
			if (Event.Kind == EVaRestTraceKind::Excerpt)
				++Excerpts;
		}
		if (Mode == 0)
		{
			TestEqual(TEXT("actual typed error has one real Completed event"), Completed, 1);
			TestEqual(TEXT("actual frame-owned typed handler reaches real Completed"), Type, FName(TEXT("String")));
			TestEqual(TEXT("actual typed error handler delivers once"), Seen->Errors, 1);
		}
		else
		{
			TestTrue(TEXT("actual nested clock hook stopped the SAME native Control"), F.Control->IsStopped());
			TestEqual(TEXT("no normal Completed after nested Stop"), Completed, 0);
			TestEqual(TEXT("no user error after nested Stop"), Seen->Errors, 0);
			if (Mode == 2)
				TestEqual(TEXT("discarded excerpt also suppressed after its exact original Now stopped"), Excerpts, 0);
			if (Mode == 3)
			{
				TestFalse(TEXT("real fake call was NOT canceled after ClearTimer stopped"), F.Rig.Fake->GetCall(0).bCanceled);
				TestEqual(TEXT("later real stage interrupt was NOT invoked after Stop"), Seen->Wakes, 0);
				TestEqual(TEXT("no manufactured physical callback after Stop"), Seen->PhysicalCallbacks, 0);
			}
		}
		TestEqual(TEXT("actual frame-owned handler capture destroyed GTK exactly once"), Seen->HandlerDeaths, 1);
		TestFalse(TEXT("actual handler capture destruction stayed GTK"), Seen->WrongThread);
		Record.Release(); // exact fake-only native resource disposal; BEFORE the owned core fixture sweeps/reclaims
		F.Store->SweepOnGT(FNativeOperationStore::Capacity);
		F.Control->CollectInputsOnGT();
		TestEqual(TEXT("actual cell reclaimed after real callout method returned"), F.Store->NumOccupied(), uint32(0));
		TestEqual(TEXT("actual consumed input reclaimed"), F.Control->NumPendingInputs(), uint64(0));
	}
	// Direct real private completion-entry fixtures, NOT a public managed factory/Send/worker/full-lifecycle qualification.
	return true;
}

#endif
