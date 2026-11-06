// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "VaRestActionAuthTestTypes.h"
#include "VaRestApiClient.h"
#include "VaRestHub.h"
#include "VaRestRequestLibrary.h"
#include "VaRestFakeTransport.h"
#include "VaRestTrace.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"

namespace VaRest::Http::Editor::FileActionTest
{
class FScopeObserver final : public IVaRestTraceSink
{
public:
	uint64 ExpectedScope=0;
	bool bClosed=false;
	virtual FVaRestTraceDetailSettings GetDetail() const override { FVaRestTraceDetailSettings Detail; Detail.Flags=EVaRestTraceDetail::Metadata; return Detail; }
	virtual void OnAttached(const FVaRestTraceSnapshot&) override {}
	virtual void OnEvent(const FVaRestTraceEvent& Event) override
	{
		if (Event.Kind!=EVaRestTraceKind::Scope) return;
		const auto& Scope=Event.Payload.Get<FVaRestTraceScope>();
		if (ExpectedScope!=0 && Scope.ScopeId==ExpectedScope && !Scope.bOpened) bClosed=true;
	}
};
class FRun final : public IAutomationLatentCommand
{
public:
	explicit FRun(FAutomationTestBase& InTest) : Test(InTest) {}
	virtual ~FRun() override { Close(); }
	virtual bool Update() override
	{
		if (bClosing)
		{
			if (Scope->bClosed) return true;
			if (FPlatformTime::Seconds()-StartedAt>30) { Test.AddError(TEXT("owned file scope did not close after actual physical/input tails")); return true; }
			return false;
		}
		if (!Download)
		{
			StartedAt=FPlatformTime::Seconds(); Isolation=MakeUnique<FVaRestTrace::FScopedSinkIsolation>();
			FVaRestTrace::AddSink(Scope); bAttached=true;
			Directory=FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("Automation/VaRestM5Actions"),FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			if (!Test.TestTrue(TEXT("owned isolated fixture directory"),IFileManager::Get().MakeDirectory(*Directory,true))) return true;
			Input=FPaths::Combine(Directory,TEXT("upload.bin")); Output=FPaths::Combine(Directory,TEXT("download.bin"));
			Payload.SetNumUninitialized(128*1024); for (int32 Index=0;Index<Payload.Num();++Index) Payload[Index]=static_cast<uint8>((Index*17+3)&0xff);
			if (!Test.TestTrue(TEXT("owned input file"),FFileHelper::SaveArrayToFile(Payload,*Input))) return true;
			Probe.Reset(NewObject<UVaRestActionAuthTestProbe>()); Hub.Reset(NewObject<UVaRestHub>()); FString Error;
			if (!Test.TestTrue(TEXT("real engine-only Hub"),Hub->InitializeOnGT(nullptr,true,Error)) ||
				!Test.TestTrue(TEXT("distinct profile"),Hub->SetStorageProfile(FGuid::NewGuid().ToString(EGuidFormats::Digits),Error))) return true;
			Scope->ExpectedScope=Hub->GetScopeId();
			Client.Reset(Hub->MakeClient(TEXT("https://file-action.invalid"))); if (!Test.TestNotNull(TEXT("real managed client"),Client.Get())) return true;
			Clock=MakeShared<FVaRestVirtualClock,ESPMode::ThreadSafe>(); Fake=MakeShared<FVaRestFakeTransport,ESPMode::ThreadSafe>(Clock.ToSharedRef()); Fake->bCompleteOnWorker=false;
			auto Native=Client->GetNativeClient();
			Native.WithClock(Clock.ToSharedRef()).WithTransport(Fake.ToSharedRef()).WithConnectivity(FVaRestConnectivity::Make(Clock.ToSharedRef())).WithLogLevel(EVaRestLogLevel::None).WithMaxJsonBytes(1);
			Fake->Enqueue(FVaRestFakeResponse::Bytes(200,TArray<uint8>(Payload)));
			Download.Reset(UVaRestRequestLibrary::DownloadFile(Probe.Get(),Client.Get(),TEXT("/download"),Output,Error));
			if (!Test.TestNotNull(*Error,Download.Get())) return true;
			bStartedOperations=true; Download->Activate(); Test.TestFalse(TEXT("download does not complete inside Activate"),Download->HasTerminalMetadata()); return false;
		}
		if (FPlatformTime::Seconds()-StartedAt>30) { Test.AddError(TEXT("streaming actions did not reach owned logical terminals")); return true; }
		Clock->Advance(0);
		if (!Download->HasTerminalMetadata()) return false;
		if (!Upload)
		{
			if (!Test.TestFalse(TEXT("download primary result"),Download->GetTerminalError().IsError())) return true;
			TArray<uint8> Actual;
			Test.TestTrue(TEXT("actual native SaveTo rename produced the complete file"),FFileHelper::LoadFileToArray(Actual,*Output) && Actual==Payload);
			Test.TestEqual(TEXT("actual destination in response metadata"),Download->GetTerminalResponse().SavedFile,Output);
			Test.TestTrue(TEXT("streamed file does not become a retained in-memory response"),Download->GetTerminalResponse().Body.IsEmpty() && Download->GetBytes().IsEmpty());
			if (!Test.TestEqual(TEXT("one actual file transport call"),Fake->NumCalls(),1)) return true;
			Test.TestFalse(TEXT("native transport used a file sink"),Fake->GetCall(0).Request.ResponseFile.IsEmpty());
			Test.TestEqual(TEXT("streaming is not capped by in-memory JSON/dynamic body allowance"),Fake->GetCall(0).Request.MaxBodyBytes,int64(0));
			Test.TestFalse(TEXT("no counting progress callback without a reader"),Fake->GetCall(0).Request.bProgressCallback);
			auto Native=Client->GetNativeClient(); Native.WithMaxJsonBytes(1024);
			Fake->Enqueue(FVaRestFakeResponse::Json(200,FUtf8StringView(UTF8TEXT("{\"value\":7}"))));
			FString Error; Upload.Reset(UVaRestRequestLibrary::UploadFile(Probe.Get(),Client.Get(),TEXT("PUT"),TEXT("/upload"),Input,TEXT("application/octet-stream"),Error));
			if (!Test.TestNotNull(*Error,Upload.Get())) return true;
			Upload->OnSuccessJson.AddDynamic(Probe.Get(),&UVaRestActionAuthTestProbe::ResourceCompleted);
			Upload->OnError.AddDynamic(Probe.Get(),&UVaRestActionAuthTestProbe::Failed);
			Upload->Activate(); return false;
		}
		if (!Upload->HasTerminalMetadata() || Client->GetNativeClient().GetInFlightCount()!=0 || Fake->NumInFlight()!=0) return false;
		Test.TestFalse(TEXT("upload primary result"),Upload->GetTerminalError().IsError());
		Test.TestEqual(TEXT("one real upload JSON callback"),Probe->ResourceJson,1); Test.TestEqual(TEXT("actual upload response JSON"),Probe->ResourceValue,7);
		Test.TestEqual(TEXT("no upload callback failure"),Probe->Failures,0);
		if (Test.TestEqual(TEXT("one download and one upload transport call"),Fake->NumCalls(),2))
		{
			Test.TestTrue(TEXT("real BodyFile source streamed exact input bytes"),Fake->ReadBody(1)==Payload);
			Test.TestEqual(TEXT("file body keeps its actual media type"),Fake->GetCall(1).Request.Headers.Get(TEXT("Content-Type")),FString(TEXT("application/octet-stream")));
			Test.TestFalse(TEXT("unused action progress does not select counting upload archive"),Fake->GetCall(1).Request.bProgressCallback);
		}
		for (int32 Index=0;Index<Fake->NumCalls();++Index) Fake->ReleaseCompletedClock(Index); // only after the held logical operations are terminal
		bClosing=true; Hub->DeinitializeOnGT(); return false; // wait for actual Scope Closed, never a fixed-delay physical drain
	}
private:
	void Close()
	{
		if (Hub) Hub->DeinitializeOnGT();
		if (bAttached) FVaRestTrace::RemoveSink(Scope);
		Download.Reset(); Upload.Reset(); Client.Reset(); Hub.Reset(); Probe.Reset(); Fake.Reset(); Clock.Reset(); Isolation.Reset();
		if (!Directory.IsEmpty())
		{
			if (!bStartedOperations || Scope->bClosed) IFileManager::Get().DeleteDirectory(*Directory,false,true);
			else Test.AddInfo(FString::Printf(TEXT("kept unsettled file fixture directory: %s"),*Directory));
		}
	}
	FAutomationTestBase& Test;
	TStrongObjectPtr<UVaRestActionAuthTestProbe> Probe;
	TStrongObjectPtr<UVaRestHub> Hub;
	TStrongObjectPtr<UVaRestApiClient> Client;
	TStrongObjectPtr<UVaRestRequestAction> Download;
	TStrongObjectPtr<UVaRestGetJsonAction> Upload;
	TSharedPtr<FVaRestVirtualClock,ESPMode::ThreadSafe> Clock;
	TSharedPtr<FVaRestFakeTransport,ESPMode::ThreadSafe> Fake;
	TUniquePtr<FVaRestTrace::FScopedSinkIsolation> Isolation;
	TSharedRef<FScopeObserver,ESPMode::ThreadSafe> Scope=MakeShared<FScopeObserver,ESPMode::ThreadSafe>();
	TArray<uint8> Payload;
	FString Directory,Input,Output;
	double StartedAt=0;
	bool bAttached=false,bStartedOperations=false,bClosing=false;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestFileActionTest,"VaRest.Api.Actions.Files.NativeStreaming",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestFileActionTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(VaRest::Http::Editor::FileActionTest::FRun(*this)); return true;
}
#endif
