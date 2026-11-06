// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "VaRestActionAuthTestTypes.h"
#include "VaRestApiClient.h"
#include "VaRestHub.h"
#include "VaRestRequestLibrary.h"
#include "VaRestRequestHandle.h"
#include "VaRestFakeTransport.h"
#include "VaRestTrace.h"
#include "CoreGlobals.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

namespace VaRest::Http::Editor::ActionProgressTest
{
class FScopeObserver final : public IVaRestTraceSink
{
public:
	uint64 ScopeId=0,RequestId=0;
	int32 Completed=0;
	bool bClosed=false;
	virtual FVaRestTraceDetailSettings GetDetail() const override { FVaRestTraceDetailSettings Detail; Detail.Flags=EVaRestTraceDetail::Metadata; return Detail; }
	virtual void OnAttached(const FVaRestTraceSnapshot&) override {}
	virtual void OnEvent(const FVaRestTraceEvent& Event) override
	{
		if (Event.Kind==EVaRestTraceKind::Scope)
		{
			const auto& Scope=Event.Payload.Get<FVaRestTraceScope>();
			if (ScopeId!=0 && Scope.ScopeId==ScopeId && !Scope.bOpened) bClosed=true;
		}
		else if (Event.Kind==EVaRestTraceKind::Completed && Event.Payload.Get<FVaRestTraceCompleted>().RequestId==RequestId) ++Completed;
	}
};
class FRun final : public IAutomationLatentCommand
{
public:
	explicit FRun(FAutomationTestBase& InTest) : Test(InTest) {}
	virtual ~FRun() override { Close(); }
	virtual bool Update() override
	{
		if (!Client)
		{
			if (Case==3) return true;
			Test.AddInfo(FString::Printf(TEXT("ProgressCase %d"),Case));
			StartedAt=FPlatformTime::Seconds(); Isolation=MakeUnique<FVaRestTrace::FScopedSinkIsolation>();
			Scope=MakeShared<FScopeObserver,ESPMode::ThreadSafe>(); FVaRestTrace::AddSink(Scope.ToSharedRef()); bAttached=true;
			Probe.Reset(NewObject<UVaRestActionAuthTestProbe>()); Hub.Reset(NewObject<UVaRestHub>()); FString Error;
			if (!Test.TestTrue(TEXT("real engine-only Hub"),Hub->InitializeOnGT(nullptr,true,Error)) ||
				!Test.TestTrue(TEXT("owned profile"),Hub->SetStorageProfile(FGuid::NewGuid().ToString(EGuidFormats::Digits),Error))) return true;
			Scope->ScopeId=Hub->GetScopeId(); Client.Reset(Hub->MakeClient(TEXT("https://action-progress.invalid")));
			if (!Test.TestNotNull(TEXT("actual managed wrapper"),Client.Get())) return true;
			Client->OnError.AddDynamic(Probe.Get(),&UVaRestActionAuthTestProbe::WrapperFailed);
			Clock=MakeShared<FVaRestVirtualClock,ESPMode::ThreadSafe>(); Fake=MakeShared<FVaRestFakeTransport,ESPMode::ThreadSafe>(Clock.ToSharedRef()); Fake->bCompleteOnWorker=false;
			auto Native=Client->GetNativeClient();
			Native.WithClock(Clock.ToSharedRef()).WithTransport(Fake.ToSharedRef()).WithConnectivity(FVaRestConnectivity::Make(Clock.ToSharedRef()))
				.WithRetry(FVaRestRetryPolicy::Disabled()).WithLogLevel(EVaRestLogLevel::None);
			Fake->Enqueue(FVaRestFakeResponse::Manual());
			Action.Reset(UVaRestRequestLibrary::GetJson(Probe.Get(),Client.Get(),TEXT("/held-progress"),Error));
			if (!Test.TestNotNull(*Error,Action.Get())) return true;
			Probe->ProgressAction=Action.Get(); Probe->bCancelOnProgress=Case==1;
			if (Case==2) Probe->CloseHubOnProgress=Hub.Get();
			Action->OnProgress.AddDynamic(Probe.Get(),&UVaRestActionAuthTestProbe::Progressed);
			Action->OnError.AddDynamic(Probe.Get(),&UVaRestActionAuthTestProbe::Failed); Action->Activate();
			Scope->RequestId=Action->GetHandle()->GetId();
			Phase=EPhase::Wire; return false;
		}
		if (FPlatformTime::Seconds()-StartedAt>30)
		{
			Test.AddError(FString::Printf(TEXT("progress barriers: case=%d phase=%d calls=%d inFlight=%d progress=%d completed=%d metadata=%d scopeClosed=%d"),
				Case,int32(Phase),Fake->NumCalls(),Fake->NumInFlight(),Probe->ProgressCalls,Scope->Completed,Action->HasTerminalMetadata(),Scope->bClosed)); return true;
		}
		Clock->Advance(0);
		if (Phase==EPhase::Wire)
		{
			if (Fake->NumCalls()!=1) return false;
			Test.TestTrue(TEXT("a bound public subscriber selects native progress"),Fake->GetCall(0).Request.bProgressCallback);
			ReportedFrame=GFrameCounter;
			Fake->ReportProgress(0,12,24,100); Clock->Advance(0.11);
			Fake->ReportProgress(0,LargeSent,64,100); Clock->Advance(0.11);
			Test.TestEqual(TEXT("two real native poll samples never deliver action progress in their API frame"),Probe->ProgressCalls,0);
			Phase=EPhase::Progress; return false;
		}
		if (Phase==EPhase::Progress)
		{
			if (Probe->ProgressCalls==0) return false;
			Test.TestEqual(TEXT("native samples coalesced to one actual action notification"),Probe->ProgressCalls,1);
			Test.TestTrue(TEXT("notification crossed a real engine frame"),Probe->ProgressFrame>ReportedFrame);
			Test.TestEqual(TEXT("signed 64-bit sent counter survived"),Probe->ProgressSent,LargeSent);
			Test.TestEqual(TEXT("coalescing kept latest native bytes"),Probe->ProgressReceived,int64(64));
			Test.TestEqual(TEXT("actual known native total"),Probe->ProgressDownloadTotal,int64(100));
			Test.TestFalse(TEXT("progress is not a terminal signal"),Probe->bProgressAfterTerminal);
			if (Case==0) Fake->Complete(0,FVaRestFakeResponse::Json(200,FUtf8StringView(UTF8TEXT("{\"value\":7}"))));
			// Other cases use actual Cancel/Hub close from the public progress listener, ending this held operation.
			Phase=EPhase::Terminal; return false;
		}
		if (Phase==EPhase::Terminal)
		{
			if (Scope->Completed!=1 || Fake->NumInFlight()!=0) return false;
			if (Case==2)
			{
				Test.TestFalse(TEXT("Hub close suppressed the action terminal"),Action->HasTerminalMetadata());
				Test.TestEqual(TEXT("Hub close suppressed primary client error"),Probe->WrapperErrors,0);
			}
			else
			{
				if (!Action->HasTerminalMetadata() || Client->GetNativeClient().GetInFlightCount()!=0 || Fake->NumInFlight()!=0) return false;
				Test.TestEqual(TEXT("truthful terminal after listener action"),Action->GetTerminalError().Kind,Case==0 ? EVaRestErrorKind::None : EVaRestErrorKind::Canceled);
				Test.TestEqual(TEXT("cancel is delivered once; success has no primary error"),Probe->WrapperErrors,Case==0 ? 0 : 1);
			}
			Test.TestFalse(TEXT("no progress followed logical terminal"),Probe->bProgressAfterTerminal);
			AtTerminal=Probe->ProgressCalls;
			Fake->ReportProgress(0,LargeSent+1,65,100); Clock->Advance(0.5); // a late physical probe write cannot revive delivery
			// The inspected fake record owns a managed dispatch-clock view. Drop
			// that view after logical completion, before awaiting Scope Closed.
			Test.TestTrue(TEXT("release the completed fake's managed clock"),Fake->ReleaseCompletedClock(0));
			Hub->DeinitializeOnGT(); Phase=EPhase::Closing; return false;
		}
		if (!Scope->bClosed) return false;
		Test.TestEqual(TEXT("actual owned retirement had no later public progress"),Probe->ProgressCalls,AtTerminal);
		Close(); ++Case; return false;
	}
private:
	enum class EPhase : uint8 { Wire,Progress,Terminal,Closing };
	void Close()
	{
		if (Hub) Hub->DeinitializeOnGT();
		if (bAttached) { FVaRestTrace::RemoveSink(Scope.ToSharedRef()); bAttached=false; }
		Action.Reset(); Client.Reset(); Hub.Reset(); Probe.Reset(); Fake.Reset(); Clock.Reset(); Scope.Reset(); Isolation.Reset();
	}
	FAutomationTestBase& Test;
	TStrongObjectPtr<UVaRestActionAuthTestProbe> Probe;
	TStrongObjectPtr<UVaRestHub> Hub;
	TStrongObjectPtr<UVaRestApiClient> Client;
	TStrongObjectPtr<UVaRestGetJsonAction> Action;
	TSharedPtr<FVaRestVirtualClock,ESPMode::ThreadSafe> Clock;
	TSharedPtr<FVaRestFakeTransport,ESPMode::ThreadSafe> Fake;
	TSharedPtr<FScopeObserver,ESPMode::ThreadSafe> Scope;
	TUniquePtr<FVaRestTrace::FScopedSinkIsolation> Isolation;
	static constexpr int64 LargeSent=int64(1)<<33;
	EPhase Phase=EPhase::Wire;
	uint64 ReportedFrame=0;
	int32 Case=0,AtTerminal=0;
	double StartedAt=0;
	bool bAttached=false;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestRequestActionProgressTest,"VaRest.Api.Actions.Progress.CoalescedAndReentrant",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestRequestActionProgressTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(VaRest::Http::Editor::ActionProgressTest::FRun(*this)); return true;
}
#endif
