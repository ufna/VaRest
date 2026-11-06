// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "VaRestActionAuthTestTypes.h"
#include "VaRestApiClient.h"
#include "VaRestHub.h"
#include "VaRestRequestLibrary.h"
#include "VaRestRequestHandle.h"
#include "VaRestFakeTransport.h"
#include "VaRestTrace.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"
#include <atomic>

namespace VaRest::Http::Editor::ActionErrorTest
{
struct FNativeResult
{
	std::atomic<bool> bDone{false};
	FVaRestError Error;
	bool bOnGT=true;
};
class FObserver final : public IVaRestTraceSink
{
public:
	uint64 ScopeId=0,RequestId=0;
	int32 Completed=0;
	FVaRestTraceError Error;
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
		else if (Event.Kind==EVaRestTraceKind::Completed)
		{
			const auto& Result=Event.Payload.Get<FVaRestTraceCompleted>();
			if (RequestId!=0 && Result.RequestId==RequestId) { ++Completed; Error=Result.Error; }
		}
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
			if (Case==4) return true;
			Test.AddInfo(FString::Printf(TEXT("PrimaryErrorCase %d"),Case));
			StartedAt=FPlatformTime::Seconds(); Isolation=MakeUnique<FVaRestTrace::FScopedSinkIsolation>();
			Observer=MakeShared<FObserver,ESPMode::ThreadSafe>(); FVaRestTrace::AddSink(Observer.ToSharedRef()); bAttached=true;
			Probe.Reset(NewObject<UVaRestActionAuthTestProbe>()); Hub.Reset(NewObject<UVaRestHub>()); FString Error;
			if (!Test.TestTrue(TEXT("real engine-only Hub"),Hub->InitializeOnGT(nullptr,true,Error)) ||
				!Test.TestTrue(TEXT("owned profile"),Hub->SetStorageProfile(FGuid::NewGuid().ToString(EGuidFormats::Digits),Error))) return true;
			Observer->ScopeId=Hub->GetScopeId(); Client.Reset(Hub->MakeClient(TEXT("https://action-error.invalid")));
			if (!Test.TestNotNull(TEXT("actual managed wrapper"),Client.Get())) return true;
			Client->OnError.AddDynamic(Probe.Get(),&UVaRestActionAuthTestProbe::WrapperFailed);
			if (Case>=2) Probe->CloseHubOnWrapperError=Hub.Get();
			Clock=MakeShared<FVaRestVirtualClock,ESPMode::ThreadSafe>(); Fake=MakeShared<FVaRestFakeTransport,ESPMode::ThreadSafe>(Clock.ToSharedRef()); Fake->bCompleteOnWorker=false;
			auto Native=Client->GetNativeClient();
			Native.WithClock(Clock.ToSharedRef()).WithTransport(Fake.ToSharedRef()).WithConnectivity(FVaRestConnectivity::Make(Clock.ToSharedRef()))
				.WithRetry(FVaRestRetryPolicy::Disabled()).WithLogLevel(EVaRestLogLevel::None);
			Fake->Enqueue(FVaRestFakeResponse::Json(503,FUtf8StringView(UTF8TEXT("{\"message\":\"fixture unavailable\"}"))));
			if (Case==1 || Case==3)
			{
				NativeResult=MakeShared<FNativeResult,ESPMode::ThreadSafe>();
				NativeHandle=Native.Get<void>(TEXT("/primary")).OnHttpThread().OnComplete([Result=NativeResult](const TVaRestResult<void>& Value) {
					Result->Error=Value.Error; Result->bOnGT=IsInGameThread(); Result->bDone.store(true,std::memory_order_release);
				}).Send();
				Observer->RequestId=NativeHandle.GetId();
			}
			else
			{
				Action.Reset(UVaRestRequestLibrary::GetJson(Probe.Get(),Client.Get(),TEXT("/primary"),Error));
				if (!Test.TestNotNull(*Error,Action.Get())) return true;
				Action->OnError.AddDynamic(Probe.Get(),&UVaRestActionAuthTestProbe::Failed); Action->Activate();
				Observer->RequestId=Action->GetHandle()->GetId();
				Test.TestFalse(TEXT("no primary wrapper error inside Activate"),Probe->WrapperErrors!=0);
			}
			return false;
		}
		if (FPlatformTime::Seconds()-StartedAt>30)
		{
			Test.AddError(FString::Printf(TEXT("primary error barriers: case=%d closing=%d completed=%d calls=%d inFlight=%d wrapperErrors=%d scopeClosed=%d workerDone=%d"),
				Case,bClosing,Observer->Completed,Fake->NumCalls(),Fake->NumInFlight(),Probe->WrapperErrors,Observer->bClosed,
				NativeResult && NativeResult->bDone.load(std::memory_order_acquire))); return true;
		}
		Clock->Advance(0);
		if (!bClosing)
		{
			if (Observer->Completed!=1 || Fake->NumInFlight()!=0) return false;
			if (Case==1 && !NativeResult->bDone.load(std::memory_order_acquire)) return false;
			if (Case==0 && !Action->HasTerminalMetadata()) return false;
			if (Case>=2 && Probe->WrapperErrors==0) return false;
			Test.TestEqual(TEXT("primary error forwarded once"),Probe->WrapperErrors,1);
			Test.TestTrue(TEXT("wrapper event stays GTK even for native OnHttpThread"),Probe->bWrapperErrorOnGT);
			Test.TestEqual(TEXT("accepted trace primary kind"),Observer->Error.Kind,EVaRestErrorKind::Http);
			Test.TestEqual(TEXT("forwarded primary kind"),Probe->WrapperError.Kind,Observer->Error.Kind);
			Test.TestEqual(TEXT("forwarded primary HTTP status"),Probe->WrapperError.HttpStatus,503);
			Test.TestEqual(TEXT("one real wire attempt"),Fake->NumCalls(),1);
			if (Case==0)
			{
				Test.TestEqual(TEXT("individual action error once"),Probe->Failures,1);
				Test.TestEqual(TEXT("wrapper error precedes the individual action handler"),Probe->WrapperErrorsAtFailure,1);
				Test.TestEqual(TEXT("action primary remains HTTP"),Action->GetTerminalError().Kind,EVaRestErrorKind::Http);
			}
			else if (Case==1)
			{
				Test.TestFalse(TEXT("borrowed native callback actually runs on its selected worker"),NativeResult->bOnGT);
				Test.TestEqual(TEXT("borrowed native primary remains HTTP"),NativeResult->Error.Kind,EVaRestErrorKind::Http);
			}
			else if (Case==2)
			{
				Test.TestEqual(TEXT("wrapper's reentrant Hub close suppresses individual action handler"),Probe->Failures,0);
				Test.TestFalse(TEXT("suppression does not fabricate action terminal metadata"),Action->HasTerminalMetadata());
			}
			else Test.TestFalse(TEXT("reentrant wrapper close prevents OnHttpThread callback launch"),NativeResult->bDone.load(std::memory_order_acquire));
			// Logical completion is established above. The fake still retains its
			// managed dispatch clock; Scope Closed must follow that view's release.
			Test.TestTrue(TEXT("release the completed fake's managed clock"),Fake->ReleaseCompletedClock(0));
			bClosing=true; Hub->DeinitializeOnGT();
		}
		if (!Observer->bClosed) return false;
		Test.TestEqual(TEXT("retirement did not forward the primary error twice"),Probe->WrapperErrors,1);
		if (Case==3) Test.TestFalse(TEXT("no suppressed worker callback appears before actual retirement"),NativeResult->bDone.load(std::memory_order_acquire));
		Close(); ++Case; return false;
	}
private:
	void Close()
	{
		if (Hub) Hub->DeinitializeOnGT();
		if (bAttached) { FVaRestTrace::RemoveSink(Observer.ToSharedRef()); bAttached=false; }
		Action.Reset(); Client.Reset(); Hub.Reset(); Probe.Reset(); NativeHandle={}; NativeResult.Reset(); Fake.Reset(); Clock.Reset(); Observer.Reset(); Isolation.Reset(); bClosing=false;
	}
	FAutomationTestBase& Test;
	TStrongObjectPtr<UVaRestActionAuthTestProbe> Probe;
	TStrongObjectPtr<UVaRestHub> Hub;
	TStrongObjectPtr<UVaRestApiClient> Client;
	TStrongObjectPtr<UVaRestGetJsonAction> Action;
	TSharedPtr<FVaRestVirtualClock,ESPMode::ThreadSafe> Clock;
	TSharedPtr<FVaRestFakeTransport,ESPMode::ThreadSafe> Fake;
	TSharedPtr<FNativeResult,ESPMode::ThreadSafe> NativeResult;
	TSharedPtr<FObserver,ESPMode::ThreadSafe> Observer;
	TUniquePtr<FVaRestTrace::FScopedSinkIsolation> Isolation;
	FVaRestRequestHandle NativeHandle;
	int32 Case=0;
	double StartedAt=0;
	bool bAttached=false,bClosing=false;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestRequestActionErrorTest,"VaRest.Api.Actions.PrimaryErrorForwarding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestRequestActionErrorTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(VaRest::Http::Editor::ActionErrorTest::FRun(*this)); return true;
}
#endif
