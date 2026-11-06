// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "VaRestActionAuthTestTypes.h"
#include "VaRestApi.h"
#include "VaRestApiClient.h"
#include "VaRestHub.h"
#include "VaRestFakeTransport.h"
#include "VaRestTrace.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"

namespace VaRest::Http::Editor::FlushPersistenceTest
{
struct FWriteResult
{
	EVaRestErrorKind Error=EVaRestErrorKind::None;
	int32 Queued=0;
	bool bDone=false;
};
class FScopeObserver final : public IVaRestTraceSink
{
public:
	uint64 Expected=0;
	bool bClosed=false;
	virtual FVaRestTraceDetailSettings GetDetail() const override { FVaRestTraceDetailSettings Detail; Detail.Flags=EVaRestTraceDetail::Metadata; return Detail; }
	virtual void OnAttached(const FVaRestTraceSnapshot&) override {}
	virtual void OnEvent(const FVaRestTraceEvent& Event) override
	{
		if (Event.Kind!=EVaRestTraceKind::Scope) return;
		const auto& Row=Event.Payload.Get<FVaRestTraceScope>();
		if (Expected!=0 && Row.ScopeId==Expected && !Row.bOpened) bClosed=true;
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
			StartedAt=FPlatformTime::Seconds(); Isolation=MakeUnique<FVaRestTrace::FScopedSinkIsolation>();
			Scope=MakeShared<FScopeObserver,ESPMode::ThreadSafe>(); FVaRestTrace::AddSink(Scope.ToSharedRef()); bAttached=true;
			Directory=FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("Automation/VaRestM5Flush"),FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			if (!Test.TestTrue(TEXT("owned fresh persistence root"),IFileManager::Get().MakeDirectory(*Directory,true))) return true;
			Probe.Reset(NewObject<UVaRestActionAuthTestProbe>()); Api.Reset(NewObject<UVaRestApi>());
			FVaRestApiEnvironment Environment; Environment.Name=TEXT("Fixture"); Environment.BaseUrl=TEXT("https://flush-persistence.invalid");
			Api->Environments.Add(Environment); Api->DefaultEnvironment=Environment.Name; Api->LogLevel=EVaRestLogLevel::None;
			Hub.Reset(NewObject<UVaRestHub>()); FString Error;
			if (!Test.TestTrue(TEXT("real engine-only Hub"),Hub->InitializeOnGT(nullptr,true,Error)) ||
				!Test.TestTrue(TEXT("distinct storage profile"),Hub->SetStorageProfile(FGuid::NewGuid().ToString(EGuidFormats::Digits),Error))) return true;
			Scope->Expected=Hub->GetScopeId(); Client.Reset(Hub->GetClient(Api.Get()));
			if (!Test.TestNotNull(TEXT("actual asset client wrapper"),Client.Get())) return true;
			Clock=MakeShared<FVaRestVirtualClock,ESPMode::ThreadSafe>();
			Connectivity=FVaRestConnectivity::Make(Clock.ToSharedRef());
			Fake=MakeShared<FVaRestFakeTransport,ESPMode::ThreadSafe>(Clock.ToSharedRef()); Fake->bCompleteOnWorker=false;
			const FString QueueRoot=FPaths::Combine(Directory,TEXT("Queue"));
			if (Case==2)
			{
				// A real file occupies the directory needed by native queue IO. No test hook supplies the Flush result.
				if (!Test.TestTrue(TEXT("owned regular file blocks queue storage"),FFileHelper::SaveStringToFile(TEXT("queue path occupied"),*QueueRoot))) return true;
				Connectivity->ReportOffline();
			}
			auto Native=Client->GetNativeClient();
			Native.WithClock(Clock.ToSharedRef()).WithTransport(Fake.ToSharedRef()).WithConnectivity(Connectivity.ToSharedRef())
				.WithStorageDirectories(FPaths::Combine(Directory,TEXT("Cache")),QueueRoot).WithLogLevel(EVaRestLogLevel::None);
			if (!Test.TestTrue(TEXT("account selected without activating replay"),Client->SetQueueAccount(TEXT("fixture-account"),Error))) return true;
			Test.TestEqual(TEXT("explicit account selection is still paused"),Client->GetOfflineQueueState(),EVaRestOfflineQueueState::Paused);
			Before.Reset(UVaRestFlushQueueAction::FlushQueue(Probe.Get(),Client.Get(),Error));
			if (!Test.TestNotNull(*Error,Before.Get())) return true;
			Before->Activate(); Test.TestFalse(TEXT("paused queue does not admit native flush"),Before->WasAdmitted());
			Phase=EPhase::BeforeActivation; return false;
		}
		if (FPlatformTime::Seconds()-StartedAt>30) { Test.AddError(TEXT("owned queue flush/retirement failed to reach its actual barrier")); return true; }
		Clock->Advance(0);
		if (Phase==EPhase::BeforeActivation)
		{
			if (!Before->IsFinished()) return false;
			Test.TestTrue(TEXT("paused refusal was delivered"),Before->HasResult());
			Test.TestEqual(TEXT("paused native false maps to Refused"),Before->GetResult(),EVaRestFlushQueueOutcome::Refused);
			Test.TestEqual(TEXT("flush never activates a paused queue"),Client->GetOfflineQueueState(),EVaRestOfflineQueueState::Paused);
			Phase=EPhase::Activation;
		}
		if (Phase==EPhase::Activation)
		{
			const auto Activation=Client->ActivateOfflineQueue(); const auto State=Client->GetOfflineQueueState();
			if (Activation.Outcome==EVaRestOfflineQueueActivation::Refused) { Test.AddError(FString(TEXT("queue activation refused: "))+Activation.Detail); return true; }
			Test.TestTrue(TEXT("reflected activation reports actual active/pending intent"),
				(State==EVaRestOfflineQueueState::Active && Activation.Outcome==EVaRestOfflineQueueActivation::Activated) ||
				(State==EVaRestOfflineQueueState::PendingBusy && Activation.Outcome==EVaRestOfflineQueueActivation::PendingBusy));
			bool bLoaded=false; const int64 Count=Client->GetQueuedCount(bLoaded);
			if (State!=EVaRestOfflineQueueState::Active || !bLoaded) return false;
			Test.TestEqual(TEXT("actual isolated namespace loaded empty"),Count,int64(0));
			if (Case==2 && !Write)
			{
				Write=MakeShared<FWriteResult,ESPMode::ThreadSafe>();
				Client->GetNativeClient().Post<void>(TEXT("/must-persist")).Body(TEXT("safe"))
					.Idempotent().QueueWhenOffline().NoRetry()
					.OnQueued([Result=Write](const FString&) { ++Result->Queued; })
					.OnComplete([Result=Write](const TVaRestResult<void>& ResultValue) { Result->Error=ResultValue.Error.Kind; Result->bDone=true; }).Send();
				return false;
			}
			if (Write)
			{
				if (!Write->bDone || Client->GetNativeClient().GetInFlightCount()!=0) return false;
				Test.TestEqual(TEXT("real unavailable storage refuses a durable acknowledgement"),Write->Error,EVaRestErrorKind::Config);
				Test.TestEqual(TEXT("failed storage never acknowledged Queued"),Write->Queued,0);
				Test.TestEqual(TEXT("offline storage failure made no wire request"),Fake->NumCalls(),0);
			}
			FString Error; Action.Reset(UVaRestFlushQueueAction::FlushQueue(Probe.Get(),Client.Get(),Error));
			if (!Test.TestNotNull(*Error,Action.Get())) return true;
			Action->OnCompleted.AddDynamic(Probe.Get(),&UVaRestActionAuthTestProbe::Flushed); Action->Activate();
			if (!Test.TestTrue(TEXT("real active queue admits FlushQueue"),Action->WasAdmitted())) return true;
			Test.TestFalse(TEXT("accepted flush never delivers on the API stack"),Action->HasResult());
			if (Case==1)
			{
				if (!Test.TestTrue(TEXT("pause revokes the actual captured queue binding"),Client->PauseOfflineQueue(Error))) return true;
				Test.TestEqual(TEXT("actual public state after pause"),Client->GetOfflineQueueState(),EVaRestOfflineQueueState::Paused);
			}
			Phase=EPhase::Flushing; return false;
		}
		if (Phase==EPhase::Flushing)
		{
			if (!Action->IsFinished()) return false;
			if (Case!=1)
			{
				const auto Expected=Case==0 ? EVaRestFlushQueueOutcome::Flushed : EVaRestFlushQueueOutcome::Failed;
				Test.TestTrue(TEXT("actual persistence barrier has a reflected result"),Action->HasResult());
				Test.TestEqual(TEXT("accepted barrier truthfully maps real success or storage failure"),Action->GetResult(),Expected);
				Test.TestEqual(TEXT("one public completion"),Probe->Flushes,1); Test.TestEqual(TEXT("callback outcome matches readback"),Probe->FlushOutcome,Expected);
			}
			else
			{
				Test.TestFalse(TEXT("suppressed original native Done has no fabricated result"),Action->HasResult());
				Test.TestEqual(TEXT("suppressed native callback cleans up silently"),Probe->Flushes,0);
			}
			Phase=EPhase::Closing; Hub->DeinitializeOnGT(); return false;
		}
		if (!Scope->bClosed) return false;
		Close(); ++Case; return false;
	}
private:
	enum class EPhase : uint8 { BeforeActivation,Activation,Flushing,Closing };
	void Close()
	{
		if (Hub) Hub->DeinitializeOnGT();
		if (bAttached) { FVaRestTrace::RemoveSink(Scope.ToSharedRef()); bAttached=false; }
		Before.Reset(); Action.Reset(); Client.Reset(); Hub.Reset(); Api.Reset(); Probe.Reset(); Fake.Reset(); Connectivity.Reset(); Clock.Reset(); Write.Reset();
		if (!Directory.IsEmpty())
		{
			if (Scope && Scope->bClosed) IFileManager::Get().DeleteDirectory(*Directory,false,true);
			else Test.AddInfo(FString::Printf(TEXT("kept unsettled flush fixture directory: %s"),*Directory));
		}
		Directory.Reset(); Scope.Reset(); Isolation.Reset();
	}
	FAutomationTestBase& Test;
	TStrongObjectPtr<UVaRestActionAuthTestProbe> Probe;
	TStrongObjectPtr<UVaRestApi> Api;
	TStrongObjectPtr<UVaRestHub> Hub;
	TStrongObjectPtr<UVaRestApiClient> Client;
	TStrongObjectPtr<UVaRestFlushQueueAction> Before,Action;
	TSharedPtr<FVaRestVirtualClock,ESPMode::ThreadSafe> Clock;
	TSharedPtr<FVaRestConnectivity,ESPMode::ThreadSafe> Connectivity;
	TSharedPtr<FVaRestFakeTransport,ESPMode::ThreadSafe> Fake;
	TSharedPtr<FWriteResult,ESPMode::ThreadSafe> Write;
	TSharedPtr<FScopeObserver,ESPMode::ThreadSafe> Scope;
	TUniquePtr<FVaRestTrace::FScopedSinkIsolation> Isolation;
	FString Directory;
	EPhase Phase=EPhase::BeforeActivation;
	int32 Case=0;
	double StartedAt=0;
	bool bAttached=false;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestFlushQueuePersistenceActionTest,"VaRest.Api.Actions.Flush.AcceptedPersistenceAndPause",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestFlushQueuePersistenceActionTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(VaRest::Http::Editor::FlushPersistenceTest::FRun(*this)); return true;
}
#endif
