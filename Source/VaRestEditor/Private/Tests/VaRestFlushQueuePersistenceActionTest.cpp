// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "VaRestActionAuthTestTypes.h"
#include "VaRestApi.h"
#include "VaRestApiClient.h"
#include "VaRestHub.h"
#include "VaRestFakeTransport.h"
#include "VaRestTrace.h"
#include "VaRestBuiltinAuthConfigs.h"
#include "VaRestJson.h"
#include "VaRestSha256.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Internationalization/Internationalization.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/Package.h"

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
// Run Produce and Consume individually in DIFFERENT processes, in that order. The fixed owned
// root deliberately survives Produce and every failed/unsettled run; never erase acknowledged work to retry.
namespace VaRest::Http::Editor::DurableRestartTest
{
FString FoldAscii(FString Text)
{
	for (TCHAR& Character:Text) if (Character>=0x41 && Character<=0x5a) Character=static_cast<TCHAR>(Character+0x20);
	return Text;
}
FString PackageText() { return FString(TEXT("/Temp/VaRestDurableI_61F4B879"))+TCHAR(0x00d6); }
FString ObjectText() { return TEXT("VaRestDurableApiI_61F4B879_47"); }
FString EnvironmentText() { return FString(TEXT("VaRestDurableEnvI_61F4B879"))+TCHAR(0x00d6)+TEXT("_19"); }
FString RootDirectory()
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("Automation/VaRestM5DurableRestart_61F4B879")));
}
FString ProcessToken()
{
	// One marker for this loaded test module, not a fabricated queue identity. Root supplies real fresh processes.
	static const FString Token=FGuid::NewGuid().ToString(EGuidFormats::DigitsLower); return Token;
}
TArray<uint8> Payload(int32 Index) { return {uint8('M'),uint8('5'),uint8(0),uint8(0xff),uint8('I'),uint8(Index)}; }
FString Operation(int32 Index) { return FString::Printf(TEXT("m5-durable-61f4b879-%d"),Index); }
FString ExpectedDirectory(const FString& Root,const FString& Profile,const FString& Namespace,const FString& Account)
{
	// Literal version/canonical fields are an independent directory oracle around ACTUAL committed queue IO.
	FString Name(TEXT("M5/v1"));
	for (const FString& Field:TArray<FString>{FoldAscii(PackageText()+TEXT(".")+ObjectText()),Profile,Namespace,Account})
	{
		const FTCHARToUTF8 Encoded(*Field); Name+=FString::Printf(TEXT("%d:"),Encoded.Length()); Name+=Field;
	}
	FSha256 Hash;
	for (const FString& Field:TArray<FString>{TEXT("VaRest/queue-name/v1"),Name})
	{
		const FTCHARToUTF8 Encoded(*Field); const uint64 Size=static_cast<uint64>(Encoded.Length()); uint8 Length[8];
		for (int32 Index=0;Index<8;++Index) Length[Index]=static_cast<uint8>(Size>>(Index*8));
		Hash.Update(Length,8); Hash.Update(Encoded.Get(),Encoded.Length());
	}
	return FPaths::Combine(Root,TEXT("Queue"),Hash.Finalize().ToHex());
}
struct FObserved
{
	FString QueueId;
	int32 Acknowledgements=0,Completions=0;
	TArray<FVaRestQueuedResult> Replays;
	TArray<FVaRestTraceStarted> Starts;
};
class FObserver final : public IVaRestTraceSink
{
public:
	uint64 ScopeId=0;
	bool bClosed=false;
	TSharedPtr<FObserved,ESPMode::ThreadSafe> Observed;
	virtual FVaRestTraceDetailSettings GetDetail() const override { FVaRestTraceDetailSettings Detail; Detail.Flags=EVaRestTraceDetail::Metadata; return Detail; }
	virtual void OnAttached(const FVaRestTraceSnapshot&) override {}
	virtual void OnEvent(const FVaRestTraceEvent& Event) override
	{
		if (Event.Kind==EVaRestTraceKind::Scope)
		{
			const auto& Row=Event.Payload.Get<FVaRestTraceScope>(); if (ScopeId!=0 && Row.ScopeId==ScopeId && !Row.bOpened) bClosed=true;
		}
		if (Event.Kind==EVaRestTraceKind::Started)
		{
			const auto& Row=Event.Payload.Get<FVaRestTraceStarted>(); if (ScopeId!=0 && Row.ScopeId==ScopeId) Observed->Starts.Add(Row);
		}
	}
};
class FRun final : public IAutomationLatentCommand
{
public:
	FRun(FAutomationTestBase& InTest,bool bInProduce) : Test(InTest),bProduce(bInProduce) {}
	virtual ~FRun() override
	{
		if (Hub) Hub->DeinitializeOnGT();
		Client.Reset(); Hub.Reset(); Api.Reset(); Package.Reset(); Fake.Reset(); Connectivity.Reset(); Clock.Reset();
		if (bAttached) FVaRestTrace::RemoveSink(Observer.ToSharedRef());
		Observer.Reset(); Isolation.Reset();
		if (bSavedCulture)
		{
			FInternationalization::Get().RestoreCultureState(SavedCulture);
			FInternationalization::FCultureStateSnapshot Restored; FInternationalization::Get().BackupCultureState(Restored);
			Test.TestTrue(TEXT("restore original language/locale/asset-group culture state"),Restored.Language.Equals(SavedCulture.Language,ESearchCase::CaseSensitive) &&
				Restored.Locale.Equals(SavedCulture.Locale,ESearchCase::CaseSensitive) && Restored.AssetGroups==SavedCulture.AssetGroups);
		}
		if (!bFinished) Test.AddInfo(FString(TEXT("retained unsettled restart fixture (no cleanup certificate): "))+Directory);
	}
	virtual bool Update() override
	{
		if (!bInitialized)
		{
			bInitialized=true; Directory=RootDirectory();
			if (!Initialize()) return true;
		}
		if (Clock) Clock->Advance(0);
		// Later latent GT turn: inline fake completion has fully returned. Its retained managed clock view
		// must end before native settlement can emit the queued replay result that this fixture awaits.
		ReleaseCompletedCallClocks();
		RecordProgress();
		if (Phase!=EPhase::Setup && FPlatformTime::Seconds()-StartedAt>30)
		{
			Test.AddError(FString(TEXT("restart fixture missed its actual load/ack/Flush/replay/scope-close barrier; retained evidence: "))+LastProgress);
			return true;
		}
		if (Phase==EPhase::Setup)
		{
			StartedAt=FPlatformTime::Seconds();
			if (!Setup()) { bFailed=true; BeginClose(); }
			else Phase=EPhase::Loading;
			return false;
		}
		if (Phase==EPhase::Loading)
		{
			bool bLoaded=false; const int64 Count=Client->GetQueuedCount(bLoaded);
			if (Client->GetOfflineQueueState()==EVaRestOfflineQueueState::Unavailable) return Fail(TEXT("selected real queue became unavailable"));
			if (!bLoaded || Client->GetOfflineQueueState()!=EVaRestOfflineQueueState::Active) return false;
			if (!Test.TestEqual(TEXT("selected partition loaded actual expected work"),Count,int64(bProduce || !bReplay ? 0 : 1))) return Fail(TEXT("restart partition count differs"));
			if (bProduce)
			{
				const auto Kept=Observed;
				Request=Client->GetNativeClient().Post<void>(TEXT("/durable-restart")).Body(Payload(RecordIndex))
					.Header(TEXT("Idempotency-Key"),Operation(RecordIndex)).Idempotent().QueueWhenOffline().NoRetry()
					.OnQueued([Kept](const FString& Id) { Kept->QueueId=Id; ++Kept->Acknowledgements; })
					.OnComplete([Kept](const TVaRestResult<void>&) { ++Kept->Completions; }).Send();
				if (!Test.TestEqual(TEXT("durable acknowledgement is not on Send stack"),Observed->Acknowledgements,0)) return Fail(TEXT("reentrant Queued callback"));
				Phase=EPhase::Acknowledging; return false;
			}
			if (bReplay && !VerifyCommittedRecord(false)) return Fail(TEXT("consumer did not find exact original persisted head"));
			Connectivity->ReportOnline(); // Only this isolated per-client reachability changes.
			Phase=EPhase::Replaying; return false;
		}
		if (Phase==EPhase::Acknowledging)
		{
			if (Observed->Completions!=0) return Fail(TEXT("queue send completed instead of acknowledging durable work"));
			if (Observed->Acknowledgements==0 || Request.IsActive() || Client->GetNativeClient().GetInFlightCount()!=0) return false;
			if (!Test.TestEqual(TEXT("one genuine Queued acknowledgement"),Observed->Acknowledgements,1) ||
				!Test.TestEqual(TEXT("offline producer made no transport call"),Fake->NumCalls(),0)) return Fail(TEXT("producer acknowledgement invariant"));
			bool bLoaded=false;
			if (!Test.TestEqual(TEXT("acknowledged real queue retains one head"),Client->GetQueuedCount(bLoaded),int64(1)) ||
				!Test.TestTrue(TEXT("acknowledged queue remains loaded"),bLoaded)) return Fail(TEXT("acknowledged head not loaded"));
			return StartFlush();
		}
		if (Phase==EPhase::Replaying)
		{
			if (bReplay && Observed->Replays.IsEmpty()) return false;
			if (Client->GetNativeClient().GetInFlightCount()!=0) return false;
			bool bLoaded=false; const int64 Count=Client->GetQueuedCount(bLoaded); if (!bLoaded || Count!=0) return false;
			if (!CheckReplay()) return Fail(TEXT("actual replay payload/key/link/count invariant"));
			return StartFlush();
		}
		if (Phase==EPhase::Flushing)
		{
			if (!Flushed->IsSet()) return false;
			if (!Test.TestTrue(TEXT("actual asynchronous persistence Flush succeeded"),Flushed->GetValue())) return Fail(TEXT("Flush failed"));
			if (bProduce && !VerifyCommittedRecord(true)) return Fail(TEXT("producer committed head receipt invariant"));
			if (!bProduce && !CheckReplay()) return Fail(TEXT("post-Flush replay count changed"));
			BeginClose(); return false;
		}
		if (Phase==EPhase::Closing)
		{
			if (Observer && Observer->ScopeId!=0 && !Observer->bClosed) return false;
			if (!bProduce && !bFailed && !CheckReplay()) bFailed=true;
			ReleaseCase();
			if (bFailed) return true;
			++Case;
			if (Case!=(bProduce ? 2 : 9)) { Phase=EPhase::Setup; return false; }
			if (bProduce)
			{
				if (!Test.TestTrue(TEXT("persist cross-process receipt after BOTH actual scope closures"),
					FFileHelper::SaveStringToFile(Receipt.ToString(),*FPaths::Combine(Directory,TEXT("receipt.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return true;
				Test.AddInfo(FString(TEXT("producer acknowledged work retained for fresh-process Consume: "))+Directory);
			}
			else if (!Test.TestTrue(TEXT("delete ONLY after all consumer Flush/scope-close barriers"),IFileManager::Get().DeleteDirectory(*Directory,false,true))) return true;
			bFinished=true; return true;
		}
		return false;
	}
private:
	enum class EPhase : uint8 { Setup,Loading,Acknowledging,Replaying,Flushing,Closing };
	bool Initialize()
	{
		if (bProduce)
		{
			if (!Test.TestFalse(TEXT("producer refuses existing evidence root"),IFileManager::Get().DirectoryExists(*Directory) || IFileManager::Get().FileExists(*Directory)) ||
				!Test.TestTrue(TEXT("create owned restart root"),IFileManager::Get().MakeDirectory(*Directory,true))) return false;
			Receipt=FVaRestJson::Object().Set(TEXT("version"),1).Set(TEXT("process"),ProcessToken()).Set(TEXT("records"),FVaRestJson::Array());
		}
		else
		{
			TArray<uint8> Bytes;
			const FString ReceiptPath=FPaths::Combine(Directory,TEXT("receipt.json")); const int64 Size=IFileManager::Get().FileSize(*ReceiptPath);
			if (!Test.TestTrue(TEXT("bounded actual producer receipt"),Size>0 && Size<=65536) ||
				!Test.TestTrue(TEXT("read actual producer receipt"),FFileHelper::LoadFileToArray(Bytes,*ReceiptPath))) return false;
			const auto Parsed=FVaRestJson::Parse(Bytes);
			if (!Test.TestTrue(TEXT("receipt parses"),Parsed.HasValue())) return false;
			Receipt=Parsed.GetValue();
			if (!Test.TestTrue(TEXT("exact two-record receipt format"),Receipt.IsObject() && Receipt[TEXT("version")].AsInt()==1 && Receipt[TEXT("records")].IsArray() && Receipt[TEXT("records")].Num()==2) ||
				!Test.TestFalse(TEXT("Consume requires a fresh process/module lifetime"),Receipt[TEXT("process")].AsString().Equals(ProcessToken(),ESearchCase::CaseSensitive))) return false;
		}
		// Probe the unsuffixed pool strings before first interning. With outlined numbers disabled,
		// FNAME_Find of a missing numbered name can retain its nonzero Number and fail IsNone().
		for (const FString& BaseName:TArray<FString>{PackageText(),ObjectText().LeftChop(3),EnvironmentText().LeftChop(3)})
			if (!Test.TestTrue(*FString::Printf(TEXT("fixture pool string has not been first-interned already: %s"),*BaseName),FName(*BaseName,FNAME_Find).IsNone())) return false;
		for (const FString& Name:TArray<FString>{PackageText(),ObjectText(),EnvironmentText()})
		{
			const FName Seed(*(bProduce ? Name : FoldAscii(Name)));
			if (!Test.TestFalse(TEXT("actual first intern succeeded"),Seed.IsNone())) return false;
		}
		FInternationalization::Get().BackupCultureState(SavedCulture); bSavedCulture=true;
		const FString Culture=bProduce ? TEXT("en-US") : TEXT("tr-TR");
		if (!Test.TestTrue(TEXT("set opposite culture for actual producer/consumer work"),FInternationalization::Get().SetCurrentCulture(Culture))) return false;
		FInternationalization::FCultureStateSnapshot ActualCulture; FInternationalization::Get().BackupCultureState(ActualCulture);
		if (!Test.TestTrue(TEXT("actual language and locale match selected fixture culture"),ActualCulture.Language.Equals(Culture,ESearchCase::CaseSensitive) && ActualCulture.Locale.Equals(Culture,ESearchCase::CaseSensitive))) return false;
		Receipt.Set(bProduce ? TEXT("producerCulture") : TEXT("consumerCulture"),Culture);
		Package.Reset(CreatePackage(*(bProduce ? PackageText() : FoldAscii(PackageText()))));
		if (!Test.TestNotNull(TEXT("owned named package"),Package.Get())) return false;
		Api.Reset(NewObject<UVaRestApi>(Package.Get(),FName(*(bProduce ? ObjectText() : FoldAscii(ObjectText()))),RF_Transient));
		if (!Test.TestNotNull(TEXT("owned named real API asset"),Api.Get())) return false;
		Api->Auth=NewObject<UVaRestBearerAuthConfig>(Api.Get()); Api->LogLevel=EVaRestLogLevel::None;
		const FString Name=bProduce ? EnvironmentText() : FoldAscii(EnvironmentText());
		for (int32 Index=0;Index<4;++Index)
		{
			auto& Environment=Api->Environments.AddDefaulted_GetRef();
			FString Text=Name;
			if (Index==1) Text=Text.LeftChop(2)+TEXT("20");
			if (Index==2) Text.ReplaceCharInline(TCHAR(0x00d6),TCHAR(0x00f6),ESearchCase::CaseSensitive);
			if (Index==3) { Text=TEXT("VaRestExplicit_61F4B879"); Environment.QueueNamespace=TEXT("ExactNamespaceI"); }
			Environment.Name=FName(*Text); Environment.BaseUrl=TEXT("https://durable-restart.invalid");
		}
		Api->DefaultEnvironment=Api->Environments[0].Name;
		Isolation=MakeUnique<FVaRestTrace::FScopedSinkIsolation>();
		return true;
	}
	bool Setup()
	{
		RecordIndex=bProduce ? Case : (((Case>=2 && Case<=4) || Case==6 || Case==8) ? 1 : 0);
		bReplay=!bProduce && (Case==5 || Case==6);
		Profile=TEXT("ExactProfileI"); Account=TEXT("ExactAccountI");
		int32 Environment=RecordIndex==0 ? 0 : 3;
		if (!bProduce && Case==0) Environment=1; // Full numeric suffix remains part of fallback identity.
		if (!bProduce && Case==1) Environment=2; // Non-ASCII U+00D6 is not ASCII-folded into U+00F6.
		if (!bProduce && Case==2) Api->Environments[3].QueueNamespace=TEXT("exactnamespacei");
		else Api->Environments[3].QueueNamespace=TEXT("ExactNamespaceI");
		if (!bProduce && Case==3) Account=TEXT("exactaccounti");
		if (!bProduce && Case==4) Profile=TEXT("exactprofilei");
		Api->DefaultEnvironment=Api->Environments[Environment].Name;
		Namespace=Api->Environments[Environment].QueueNamespace;
		if (Namespace.IsEmpty()) Namespace=FoldAscii(Api->Environments[Environment].Name.ToString());
		ExpectedPath=ExpectedDirectory(Directory,Profile,Namespace,Account);
		Observed=MakeShared<FObserved,ESPMode::ThreadSafe>(); Observer=MakeShared<FObserver,ESPMode::ThreadSafe>(); Observer->Observed=Observed;
		FVaRestTrace::AddSink(Observer.ToSharedRef()); bAttached=true;
		Hub.Reset(NewObject<UVaRestHub>()); FString Error;
		if (!Test.TestTrue(TEXT("real engine-only restart Hub"),Hub->InitializeOnGT(nullptr,true,Error))) return false;
		Observer->ScopeId=Hub->GetScopeId();
		if (!Test.TestTrue(TEXT("stable exact storage profile before admission"),Hub->SetStorageProfile(Profile,Error))) return false;
		FVaRestApiValidationContext PreClientValidation; Api->Validate(PreClientValidation);
		Test.AddInfo(FString::Printf(TEXT("DurableIdentityPreGetClient produce=%d case=%d api=%s defaultEnvironment=%s environments=%d issues=%d hasErrors=%d"),
			bProduce ? 1 : 0,Case,*Api->GetPathName(),*Api->DefaultEnvironment.ToString(),Api->Environments.Num(),PreClientValidation.Issues.Num(),PreClientValidation.HasErrors() ? 1 : 0));
		for (const auto& Issue:PreClientValidation.Issues)
			Test.AddInfo(FString::Printf(TEXT("DurableIdentityApiIssue warning=%d code=%s property=%s message=%s"),
				Issue.bWarning ? 1 : 0,*Issue.Code.ToString(),*Issue.PropertyPath,*Issue.Message.ToString()));
		Client.Reset(Hub->GetClient(Api.Get())); if (!Test.TestNotNull(TEXT("real asset wrapper"),Client.Get())) return false;
		Clock=MakeShared<FVaRestVirtualClock,ESPMode::ThreadSafe>(); Connectivity=FVaRestConnectivity::Make(Clock.ToSharedRef()); Connectivity->ReportOffline();
		Fake=MakeShared<FVaRestFakeTransport,ESPMode::ThreadSafe>(Clock.ToSharedRef()); Fake->bCompleteOnWorker=false;
		Fake->Enqueue(FVaRestFakeResponse::Json(200,FUtf8StringView(UTF8TEXT("{\"replayed\":true}"))));
		auto Native=Client->GetNativeClient();
		Native.WithClock(Clock.ToSharedRef()).WithTransport(Fake.ToSharedRef()).WithConnectivity(Connectivity.ToSharedRef())
			.WithStorageDirectories(FPaths::Combine(Directory,TEXT("Cache")),FPaths::Combine(Directory,TEXT("Queue"))).WithLogLevel(EVaRestLogLevel::None);
		if (!Test.TestTrue(TEXT("explicit account selection before activation"),Client->SetQueueAccount(Account,Error))) return false;
		if (bProduce) { Client->SetBearerToken(TEXT("durable-owned-fixture-token")); Session=Client->ExportSession(); }
		else
		{
			Session=Receipt[TEXT("records")][RecordIndex][TEXT("session")].AsString();
			if (!Test.TestTrue(TEXT("restore trusted fixture session before replay activation"),!Session.IsEmpty() && Client->ImportSession(Session))) return false;
		}
		const auto Kept=Observed; Native.OnQueuedResult().AddLambda([Kept](const FVaRestQueuedResult& Result) { Kept->Replays.Add(Result); });
		if (!Test.TestEqual(TEXT("account/session do not activate replay"),Client->GetOfflineQueueState(),EVaRestOfflineQueueState::Paused)) return false;
		const auto Activation=Client->ActivateOfflineQueue();
		return Test.TestTrue(*FString::Printf(TEXT("actual activation admitted: %s"),*Activation.Detail),Activation.Outcome!=EVaRestOfflineQueueActivation::Refused);
	}
	bool VerifyCommittedRecord(bool bRecord)
	{
		TArray<FString> Files; IFileManager::Get().FindFiles(Files,*FPaths::Combine(ExpectedPath,TEXT("*.json")),true,false);
		if (!Test.TestEqual(TEXT("literal M5/v1 identity selects exactly one real persisted head"),Files.Num(),1)) return false;
		const FString Path=FPaths::Combine(ExpectedPath,Files[0]); TArray<uint8> Bytes;
		const int64 Size=IFileManager::Get().FileSize(*Path);
		if (!Test.TestTrue(TEXT("bounded committed queue record"),Size>0 && Size<=65536) ||
			!Test.TestTrue(TEXT("read committed queue record"),FFileHelper::LoadFileToArray(Bytes,*Path))) return false;
		const auto Parsed=FVaRestJson::Parse(Bytes); if (!Test.TestTrue(TEXT("real native record parses"),Parsed.HasValue())) return false;
		const auto Record=Parsed.GetValue(); const FString Id=Record[TEXT("queueId")].AsString();
		if (!Test.TestEqual(TEXT("persisted FIFO sequence"),Record[TEXT("sequence")].AsString(),FString(TEXT("1"))) ||
			!Test.TestEqual(TEXT("persisted exact operation key"),Record[TEXT("idempotencyKey")].AsString(),Operation(RecordIndex)) ||
			!Test.TestEqual(TEXT("persisted exact binary body including NUL/ff"),Record[TEXT("body")].AsString(),FBase64::Encode(Payload(RecordIndex)))) return false;
		if (bRecord)
		{
			if (!Test.TestEqual(TEXT("actual acknowledgement matches committed queue ID"),Id,Observed->QueueId)) return false;
			const auto Row=FVaRestJson::Object().Set(TEXT("path"),Path).Set(TEXT("record"),FBase64::Encode(Bytes)).Set(TEXT("queueId"),Id)
				.Set(TEXT("session"),Session).Set(TEXT("apiPath"),Api->GetPathName());
			Receipt[TEXT("records")].Add(Row);
		}
		else
		{
			const auto Row=Receipt[TEXT("records")][RecordIndex];
			if (!Test.TestTrue(TEXT("fresh identity reopened exact original directory and head filename"),Path.Equals(Row[TEXT("path")].AsString(),ESearchCase::CaseSensitive)) ||
				!Test.TestEqual(TEXT("record bytes unchanged across process restart"),FBase64::Encode(Bytes),Row[TEXT("record")].AsString()) ||
				!Test.TestEqual(TEXT("head queue ID survives restart"),Id,Row[TEXT("queueId")].AsString())) return false;
		}
		return true;
	}
	bool CheckReplay()
	{
		const int32 Count=bReplay ? 1 : 0;
		if (!Test.TestEqual(TEXT("exact replay result count (empty control/reopen never replays)"),Observed->Replays.Num(),Count) ||
			!Test.TestEqual(TEXT("exact real transport call count"),Fake->NumCalls(),Count)) return false;
		if (!bReplay) return Test.TestEqual(TEXT("empty partition has no Started operation"),Observed->Starts.Num(),0);
		const auto Row=Receipt[TEXT("records")][RecordIndex]; const auto& Result=Observed->Replays[0];
		if (!Test.TestTrue(TEXT("actual replay succeeds"),Result.bSuccess) ||
			!Test.TestEqual(TEXT("actual replay200"),Result.Response.Status,200) ||
			!Test.TestEqual(TEXT("replayed original queue ID"),Result.QueueId,Row[TEXT("queueId")].AsString()) ||
			!Test.TestEqual(TEXT("one actual replay Started"),Observed->Starts.Num(),1)) return false;
		const auto& Start=Observed->Starts[0]; const auto& Wire=Fake->GetCall(0);
		return Test.TestEqual(TEXT("replay trace links durable original queue ID"),Start.QueueReplayOf.QueueId,Result.QueueId) &&
			Test.TestEqual(TEXT("fresh-process replay has no same-run request link"),Start.QueueReplayOf.OriginalRequestId,uint64(0)) &&
			Test.TestEqual(TEXT("replay method"),Wire.Request.Verb,FString(TEXT("POST"))) &&
			Test.TestEqual(TEXT("replay original URL"),Wire.Request.Url,FString(TEXT("https://durable-restart.invalid/durable-restart"))) &&
			Test.TestEqual(TEXT("replay original idempotency key"),Wire.Request.Headers.Get(TEXT("Idempotency-Key")),Operation(RecordIndex)) &&
			Test.TestTrue(TEXT("actual transport received exact original payload once"),Fake->ReadBody(0)==Payload(RecordIndex)) &&
			Test.TestTrue(TEXT("actual transport call completed"),Wire.bCompleted);
	}
	bool StartFlush()
	{
		Flushed=MakeShared<TOptional<bool>,ESPMode::ThreadSafe>(); const auto Kept=Flushed;
		if (!Test.TestTrue(TEXT("active real queue admits persistence barrier"),Client->GetNativeClient().FlushQueue([Kept](bool bOk) { *Kept=bOk; }))) return Fail(TEXT("Flush admission false: no callback promised"));
		if (!Test.TestFalse(TEXT("Flush callback is not inline"),Flushed->IsSet())) return Fail(TEXT("reentrant Flush callback"));
		Phase=EPhase::Flushing; return false;
	}
	void ReleaseCompletedCallClocks()
	{
		if (!Fake) return;
		for (int32 Index=0;Index<Fake->NumCalls();++Index)
		{
			const auto& Call=Fake->GetCall(Index);
			if (Call.bCompleted && Call.Request.Clock.IsValid())
			{
				const bool bReleased=Fake->ReleaseCompletedClock(Index);
				Test.TestTrue(TEXT("release only the completed fake call's actual retained clock view"),bReleased);
				Test.AddInfo(FString::Printf(TEXT("DurableIdentityReleasedCompletedClock case=%d phase=%s call=%d released=%d"),Case,PhaseName(),Index,bReleased ? 1 : 0));
			}
		}
	}
	const TCHAR* PhaseName() const
	{
		switch (Phase)
		{
		case EPhase::Setup:return TEXT("Setup");
		case EPhase::Loading:return TEXT("Loading");
		case EPhase::Acknowledging:return TEXT("Acknowledging");
		case EPhase::Replaying:return TEXT("Replaying");
		case EPhase::Flushing:return TEXT("Flushing");
		case EPhase::Closing:return TEXT("Closing");
		default:return TEXT("Invalid");
		}
	}
	void RecordProgress()
	{
		bool bLoaded=false;
		const int64 Count=Client ? Client->GetQueuedCount(bLoaded) : 0;
		const int32 QueueState=Client ? static_cast<int32>(Client->GetOfflineQueueState()) : -1;
		const FString QueueError=Client ? Client->GetOfflineQueueError() : FString(TEXT("NoClient"));
		int32 InFlight=0; bool bNative=false;
		{
			const FVaRestClient Native=Client ? Client->GetNativeClient() : FVaRestClient();
			bNative=Native.IsValid(); InFlight=Native.GetInFlightCount();
		} // no diagnostic alias survives the snapshot, Flush or scope-close wait
		int32 Completed=0,RetainedClocks=0;
		if (Fake) for (int32 Index=0;Index<Fake->NumCalls();++Index)
		{
			const auto& Call=Fake->GetCall(Index);
			Completed+=Call.bCompleted ? 1 : 0; RetainedClocks+=Call.Request.Clock.IsValid() ? 1 : 0;
		}
		const TCHAR* Flush=!Flushed ? TEXT("None") : !Flushed->IsSet() ? TEXT("Pending") : Flushed->GetValue() ? TEXT("Succeeded") : TEXT("Failed");
		const FString Snapshot=FString::Printf(TEXT("DurableIdentityProgress produce=%d case=%d record=%d phase=%s replay=%d queueState=%d loaded=%d count=%lld queueError=%s native=%d inFlight=%d fakeCalls=%d fakeInFlight=%d completed=%d retainedClocks=%d acks=%d completions=%d starts=%d replays=%d flush=%s scope=%llu scopeClosed=%d virtualTime=%.6f"),
			bProduce ? 1 : 0,Case,RecordIndex,PhaseName(),bReplay ? 1 : 0,QueueState,bLoaded ? 1 : 0,static_cast<long long>(Count),*QueueError,bNative ? 1 : 0,InFlight,
			Fake ? Fake->NumCalls() : 0,Fake ? Fake->NumInFlight() : 0,Completed,RetainedClocks,Observed ? Observed->Acknowledgements : 0,Observed ? Observed->Completions : 0,
			Observed ? Observed->Starts.Num() : 0,Observed ? Observed->Replays.Num() : 0,Flush,static_cast<unsigned long long>(Observer ? Observer->ScopeId : 0),Observer && Observer->bClosed ? 1 : 0,Clock ? Clock->Now() : 0.0);
		if (!Snapshot.Equals(LastProgress,ESearchCase::CaseSensitive)) { LastProgress=Snapshot; Test.AddInfo(Snapshot); }
	}
	bool Fail(const TCHAR* Message) { Test.AddError(Message); bFailed=true; BeginClose(); return false; }
	void BeginClose()
	{
		ReleaseCompletedCallClocks();
		if (Hub) Hub->DeinitializeOnGT(); Phase=EPhase::Closing;
	}
	void ReleaseCase()
	{
		Client.Reset(); Hub.Reset(); Fake.Reset(); Connectivity.Reset(); Clock.Reset(); Flushed.Reset(); Request=FVaRestRequestHandle();
		if (bAttached) { FVaRestTrace::RemoveSink(Observer.ToSharedRef()); bAttached=false; }
		Observer.Reset(); Observed.Reset();
	}
	FAutomationTestBase& Test;
	const bool bProduce;
	bool bInitialized=false,bAttached=false,bFailed=false,bFinished=false,bReplay=false,bSavedCulture=false;
	int32 Case=0,RecordIndex=0;
	double StartedAt=0;
	EPhase Phase=EPhase::Setup;
	FString Directory,Profile,Account,Namespace,ExpectedPath,Session,LastProgress;
	FInternationalization::FCultureStateSnapshot SavedCulture;
	FVaRestJson Receipt;
	FVaRestRequestHandle Request;
	TStrongObjectPtr<UPackage> Package;
	TStrongObjectPtr<UVaRestApi> Api;
	TStrongObjectPtr<UVaRestHub> Hub;
	TStrongObjectPtr<UVaRestApiClient> Client;
	TSharedPtr<FVaRestVirtualClock,ESPMode::ThreadSafe> Clock;
	TSharedPtr<FVaRestConnectivity,ESPMode::ThreadSafe> Connectivity;
	TSharedPtr<FVaRestFakeTransport,ESPMode::ThreadSafe> Fake;
	TSharedPtr<FObserved,ESPMode::ThreadSafe> Observed;
	TSharedPtr<FObserver,ESPMode::ThreadSafe> Observer;
	TSharedPtr<TOptional<bool>,ESPMode::ThreadSafe> Flushed;
	TUniquePtr<FVaRestTrace::FScopedSinkIsolation> Isolation;
};
}
// Each phase needs a separate fresh process. Ordinary suite discovery must not run
// both halves or consume a receipt before its producer has committed real work.
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FVaRestDurableIdentityRestartTest,"VaRest.Api.Actions.Flush.DurableIdentityRestart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
void FVaRestDurableIdentityRestartTest::GetTests(TArray<FString>& Names,TArray<FString>& Commands) const
{
	FString Phase;
	if (FParse::Value(FCommandLine::Get(),TEXT("VaRestM5DurablePhase="),Phase) &&
		(Phase.Equals(TEXT("Produce"),ESearchCase::CaseSensitive) || Phase.Equals(TEXT("Consume"),ESearchCase::CaseSensitive)))
	{
		Names.Add(Phase); Commands.Add(Phase);
	}
}
bool FVaRestDurableIdentityRestartTest::RunTest(const FString& Parameters)
{
	FString Phase;
	if (!FParse::Value(FCommandLine::Get(),TEXT("VaRestM5DurablePhase="),Phase) ||
		!Parameters.Equals(Phase,ESearchCase::CaseSensitive) ||
		(!Phase.Equals(TEXT("Produce"),ESearchCase::CaseSensitive) && !Phase.Equals(TEXT("Consume"),ESearchCase::CaseSensitive)))
	{
		AddError(TEXT("durable restart phase requires its matching explicit fresh-process command-line selection")); return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(VaRest::Http::Editor::DurableRestartTest::FRun(*this,Phase.Equals(TEXT("Produce"),ESearchCase::CaseSensitive))); return true;
}
#endif
