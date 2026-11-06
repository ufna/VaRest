// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "VaRestActionAuthTestTypes.h"
#include "VaRestApi.h"
#include "VaRestApiClient.h"
#include "VaRestHub.h"
#include "VaRestRequestLibrary.h"
#include "VaRestRequestHandle.h"
#include "VaRestFakeTransport.h"
#include "VaRestTrace.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

namespace VaRest::Http::Editor::JsonValueActionTest
{
class FScopeObserver final : public IVaRestTraceSink
{
public:
	uint64 ScopeId=0;
	bool bClosed=false;
	virtual FVaRestTraceDetailSettings GetDetail() const override { FVaRestTraceDetailSettings Detail; Detail.Flags=EVaRestTraceDetail::Metadata; return Detail; }
	virtual void OnAttached(const FVaRestTraceSnapshot&) override {}
	virtual void OnEvent(const FVaRestTraceEvent& Event) override
	{
		if (Event.Kind!=EVaRestTraceKind::Scope) return;
		const auto& Scope=Event.Payload.Get<FVaRestTraceScope>();
		if (ScopeId!=0 && Scope.ScopeId==ScopeId && !Scope.bOpened) bClosed=true;
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
			if (Case==5) return true;
			StartedAt=FPlatformTime::Seconds(); Isolation=MakeUnique<FVaRestTrace::FScopedSinkIsolation>();
			Scope=MakeShared<FScopeObserver,ESPMode::ThreadSafe>(); FVaRestTrace::AddSink(Scope.ToSharedRef()); bAttached=true;
			Probe.Reset(NewObject<UVaRestActionAuthTestProbe>()); Api.Reset(NewObject<UVaRestApi>());
			FVaRestApiEnvironment Environment; Environment.Name=TEXT("Fixture"); Environment.BaseUrl=TEXT("https://json-value.invalid");
			Api->Environments.Add(Environment); Api->DefaultEnvironment=Environment.Name;
			Api->JsonContentType=TEXT("application/vnd.varest-fixture+json"); Api->Retry=FVaRestRetryPolicy::Disabled(); Api->LogLevel=EVaRestLogLevel::None;
			Hub.Reset(NewObject<UVaRestHub>()); FString Error;
			if (!Test.TestTrue(TEXT("real engine-only Hub"),Hub->InitializeOnGT(nullptr,true,Error)) ||
				!Test.TestTrue(TEXT("owned profile"),Hub->SetStorageProfile(FGuid::NewGuid().ToString(EGuidFormats::Digits),Error))) return true;
			Scope->ScopeId=Hub->GetScopeId(); Client.Reset(Hub->GetClient(Api.Get()));
			if (!Test.TestNotNull(TEXT("actual asset wrapper"),Client.Get())) return true;
			Client->OnError.AddDynamic(Probe.Get(),&UVaRestActionAuthTestProbe::WrapperFailed);
			Clock=MakeShared<FVaRestVirtualClock,ESPMode::ThreadSafe>(); Fake=MakeShared<FVaRestFakeTransport,ESPMode::ThreadSafe>(Clock.ToSharedRef()); Fake->bCompleteOnWorker=false;
			auto Native=Client->GetNativeClient();
			Native.WithClock(Clock.ToSharedRef()).WithTransport(Fake.ToSharedRef()).WithConnectivity(FVaRestConnectivity::Make(Clock.ToSharedRef()));
			FVaRestFakeResponse Response=Case==4 ? FVaRestFakeResponse::Bytes(422,{0,255,13,10}) :
				FVaRestFakeResponse::Json(200,FUtf8StringView(UTF8TEXT(" \r\n{\"value\":7}\n")));
			Response.Headers.Add(TEXT("Set-Cookie"),TEXT("first=a; Path=/")); Response.Headers.Add(TEXT("X-Repeat"),TEXT("one"));
			Response.Headers.Add(TEXT("Set-Cookie"),TEXT("second=b; Path=/")); Response.Headers.Add(TEXT("X-Repeat"),TEXT("two"));
			Fake->Enqueue(MoveTemp(Response));
			const FVaRestJson Body=Case==0 || Case==4 ? FVaRestJson::From(false) : Case==1 ? FVaRestJson::From(int64(0)) : Case==2 ? FVaRestJson::Null() : FVaRestJson();
			Action.Reset(UVaRestRequestLibrary::RequestJson(Probe.Get(),Client.Get(),TEXT("POST"),TEXT("/values"),Body,Error));
			if (!Test.TestNotNull(*Error,Action.Get())) return true;
			Action->OnSuccessJson.AddDynamic(Probe.Get(),&UVaRestActionAuthTestProbe::JsonCompleted);
			Action->OnError.AddDynamic(Probe.Get(),&UVaRestActionAuthTestProbe::Failed); Action->Activate(); return false;
		}
		if (FPlatformTime::Seconds()-StartedAt>30) { Test.AddError(TEXT("JSON value action failed its owned terminal/Scope barriers")); return true; }
		Clock->Advance(0);
		if (!bClosing)
		{
			if (!Action->HasTerminalMetadata() || Client->GetNativeClient().GetInFlightCount()!=0 || Fake->NumInFlight()!=0) return false;
			if (!Test.TestEqual(TEXT("one actual wire request"),Fake->NumCalls(),1)) return true;
			const auto Wire=Fake->ReadBody(0); FString WireText;
			if (!Wire.IsEmpty()) { FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Wire.GetData()),Wire.Num()); WireText=FString(Converted.Length(),Converted.Get()); }
			const TCHAR* Expected=Case==0 || Case==4 ? TEXT("false") : Case==1 ? TEXT("0") : Case==2 ? TEXT("null") : TEXT("");
			Test.TestEqual(TEXT("false, zero and null are actual JSON bodies; absent input sends no body"),WireText,FString(Expected));
			if (Case!=3) Test.TestEqual(TEXT("JSON body uses actual asset media"),Fake->GetCall(0).Request.Headers.Get(TEXT("Content-Type")),Api->JsonContentType);
			Test.TestFalse(TEXT("unused progress did not select upload counting"),Fake->GetCall(0).Request.bProgressCallback);
			const auto Http=Action->GetTerminalResponse();
			Test.TestEqual(TEXT("canonical action ID remains actual native wire ID"),Http.RequestId,UnsignedIdText(Fake->GetCall(0).Request.RequestId));
			if (Test.TestEqual(TEXT("every raw response header line remains present"),Http.Headers.Num(),5))
			{
				const TCHAR* Names[]={TEXT("Content-Type"),TEXT("Set-Cookie"),TEXT("X-Repeat"),TEXT("Set-Cookie"),TEXT("X-Repeat")};
				const TCHAR* Values[]={Case==4 ? TEXT("application/octet-stream") : TEXT("application/json"),TEXT("first=a; Path=/"),TEXT("one"),TEXT("second=b; Path=/"),TEXT("two")};
				for (int32 Index=0;Index<5;++Index)
				{
					Test.TestEqual(TEXT("raw header name/order"),Http.Headers[Index].Name,FString(Names[Index]));
					Test.TestEqual(TEXT("raw header value/order; cookies never joined"),Http.Headers[Index].Value,FString(Values[Index]));
				}
			}
			if (Case==4)
			{
				Test.TestEqual(TEXT("non-JSON entity preserves primary HTTP failure"),Action->GetTerminalError().Kind,EVaRestErrorKind::Http);
				Test.TestEqual(TEXT("one action error"),Probe->Failures,1); Test.TestEqual(TEXT("one client error"),Probe->WrapperErrors,1);
				Test.TestTrue(TEXT("raw error body keeps NUL/high byte/CRLF exactly"),Http.Body==TArray<uint8>({0,255,13,10}));
				Test.TestEqual(TEXT("HTTP error never fabricates successful JSON"),Probe->JsonResponses,0);
			}
			else
			{
				Test.TestFalse(TEXT("native primary JSON success"),Action->GetTerminalError().IsError());
				Test.TestEqual(TEXT("one plain JSON result"),Probe->JsonResponses,1);
				Test.TestEqual(TEXT("actual native JSON parsed value"),Probe->LastJson.Get(TEXT("value")).AsInt64(),int64(7));
				Test.TestEqual(TEXT("JSON event retained exact metadata ID"),Probe->LastJsonHttp.RequestId,Http.RequestId);
				Test.TestEqual(TEXT("successful primary has no client error"),Probe->WrapperErrors,0);
			}
			Fake->ReleaseCompletedClock(0); Hub->DeinitializeOnGT(); bClosing=true;
		}
		if (!Scope->bClosed) return false;
		Close(); ++Case; return false;
	}
private:
	void Close()
	{
		if (Hub) Hub->DeinitializeOnGT();
		if (bAttached) { FVaRestTrace::RemoveSink(Scope.ToSharedRef()); bAttached=false; }
		Action.Reset(); Client.Reset(); Hub.Reset(); Api.Reset(); Probe.Reset(); Fake.Reset(); Clock.Reset(); Scope.Reset(); Isolation.Reset(); bClosing=false;
	}
	FAutomationTestBase& Test;
	TStrongObjectPtr<UVaRestActionAuthTestProbe> Probe;
	TStrongObjectPtr<UVaRestApi> Api;
	TStrongObjectPtr<UVaRestHub> Hub;
	TStrongObjectPtr<UVaRestApiClient> Client;
	TStrongObjectPtr<UVaRestGetJsonAction> Action;
	TSharedPtr<FVaRestVirtualClock,ESPMode::ThreadSafe> Clock;
	TSharedPtr<FVaRestFakeTransport,ESPMode::ThreadSafe> Fake;
	TSharedPtr<FScopeObserver,ESPMode::ThreadSafe> Scope;
	TUniquePtr<FVaRestTrace::FScopedSinkIsolation> Isolation;
	int32 Case=0;
	double StartedAt=0;
	bool bAttached=false,bClosing=false;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestRequestJsonValueActionTest,"VaRest.Api.Actions.Json.ValueBodyAndRawMetadata",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestRequestJsonValueActionTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(VaRest::Http::Editor::JsonValueActionTest::FRun(*this)); return true;
}
#endif
