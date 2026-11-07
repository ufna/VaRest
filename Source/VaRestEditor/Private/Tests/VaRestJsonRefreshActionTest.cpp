// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "VaRestActionAuthTestTypes.h"
#include "VaRestApi.h"
#include "VaRestAuth.h"
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

namespace VaRest::Http::Editor::JsonRefreshTest
{
class FRun final : public IAutomationLatentCommand
{
public:
	explicit FRun(FAutomationTestBase& InTest,bool bInOwnerLoss=false) : Test(InTest),bOwnerLoss(bInOwnerLoss) {}
	virtual ~FRun() override { Close(); }
	virtual bool Update() override
	{
		if (!Original)
		{
			Isolation=MakeUnique<FVaRestTrace::FScopedSinkIsolation>(); StartedAt=FPlatformTime::Seconds(); SentFrame=GFrameCounter;
			Probe.Reset(NewObject<UVaRestActionAuthTestProbe>()); Api.Reset(NewObject<UVaRestApi>());
			FVaRestApiEnvironment Environment; Environment.Name=TEXT("Fixture"); Environment.BaseUrl=TEXT("https://refresh-action.invalid");
			Api->Environments.Add(Environment); Api->DefaultEnvironment=Environment.Name; Api->LogLevel=EVaRestLogLevel::None;
			Api->Envelope=FVaRestEnvelope::DataAt(TEXTVIEW("data")).SuccessWhen(TEXTVIEW("ok"),{TEXT("true")});
			AssetAuth.Reset(NewObject<UVaRestActionAuthTestConfig>(Api.Get())); AssetAuth->Probe=Probe.Get(); Api->Auth=AssetAuth.Get();
			Hub.Reset(NewObject<UVaRestHub>()); FString Error;
			if (!Test.TestTrue(TEXT("real engine-only Hub"),Hub->InitializeOnGT(nullptr,true,Error)) ||
				!Test.TestTrue(TEXT("distinct fixture profile"),Hub->SetStorageProfile(FGuid::NewGuid().ToString(EGuidFormats::Digits),Error))) return true;
			Client.Reset(Hub->GetClient(Api.Get())); if (!Test.TestNotNull(TEXT("real per-asset wrapper"),Client.Get())) return true;
			Client->OnError.AddDynamic(Probe.Get(),&UVaRestActionAuthTestProbe::WrapperFailed);
			Clock=MakeShared<FVaRestVirtualClock,ESPMode::ThreadSafe>(); Fake=MakeShared<FVaRestFakeTransport,ESPMode::ThreadSafe>(Clock.ToSharedRef()); Fake->bCompleteOnWorker=false;
			const TWeakObjectPtr<UVaRestActionAuthTestProbe> Weak(Probe.Get());
			Fake->SetResponder([Weak,bDelayRefresh=bOwnerLoss](const FVaRestTransportRequest& Request) {
				check(IsInGameThread()); auto* Observer=Weak.Get();
				if (Request.Url.EndsWith(TEXT("/refresh")) || Request.Url.EndsWith(TEXT("/aux")))
				{
					const bool bFirst=Request.Url.EndsWith(TEXT("/refresh"));
					const bool bBare=!Request.Headers.Find(TEXT("Authorization")) && !Request.Headers.Find(TEXT("X-Session"));
					if (Observer)
					{
						if (bFirst) { Observer->FirstWireId=UnsignedIdText(Request.RequestId); Observer->bBareFirst=bBare; }
						else { Observer->SecondWireId=UnsignedIdText(Request.RequestId); Observer->bBareSecond=bBare; }
					}
					auto Response=FVaRestFakeResponse::Json(200,FUtf8StringView(UTF8TEXT("{\"access_token\":\"fixture-fresh\",\"refresh_token\":\"fixture-next\"}")));
					if (bDelayRefresh) Response.Delay=0.25;
					return Response;
				}
				if (Observer) ++Observer->ProtectedCalls;
				return Request.Headers.Get(TEXT("Authorization"))==TEXT("Bearer fixture-fresh") ?
					FVaRestFakeResponse::Json(200,FUtf8StringView(UTF8TEXT("{\"ok\":true,\"data\":{\"value\":7}}"))) : FVaRestFakeResponse::Json(401,FUtf8StringView(UTF8TEXT("{}")));
			});
			auto Native=Client->GetNativeClient();
			Native.WithClock(Clock.ToSharedRef()).WithTransport(Fake.ToSharedRef()).WithConnectivity(FVaRestConnectivity::Make(Clock.ToSharedRef())).WithLogLevel(EVaRestLogLevel::None);
			Client->SetSession(TEXT("fixture-old"),TEXT("fixture-refresh"),3600); Client->SetSessionHeader(TEXT("X-Session"),TEXT("fixture-session"));
			if (bOwnerLoss) OriginalOwner.Reset(NewObject<UVaRestActionAuthTestProbe>());
			Original.Reset(UVaRestRequestLibrary::GetJson(bOwnerLoss?OriginalOwner.Get():Probe.Get(),Client.Get(),TEXT("/protected"),Error));
			if (!Test.TestNotNull(*Error,Original.Get())) return true;
			Original->OnSuccessJson.AddDynamic(Probe.Get(),&UVaRestActionAuthTestProbe::ResourceCompleted);
			Original->OnError.AddDynamic(Probe.Get(),&UVaRestActionAuthTestProbe::Failed); Original->Activate();
			Test.TestFalse(TEXT("initial action capture does not deliver in its Send frame"),Original->HasTerminalMetadata()); return false;
		}
		if (FPlatformTime::Seconds()-StartedAt>30) { Test.AddError(TEXT("actual custom-auth JSON actions did not reach owned logical terminals")); return true; }
		Clock->Advance(0);
		if (bOwnerLoss && !bOwnerInvalidated)
		{
			if (!Probe->CreationError.IsEmpty()) { Test.AddError(Probe->CreationError); return true; }
			if (Probe->Refreshes!=1 || !Probe->Context || !Probe->First || !Probe->Second || Fake->NumCalls()!=3) return false;
			Test.TestTrue(TEXT("managed original and both refresh helpers are held"),Original->GetHandle()->IsActive() && Client->GetNativeClient().GetInFlightCount()==3 && Probe->Context->IsActive());
			UObject* Owner=OriginalOwner.Get(); LostOwner=Owner; OriginalOwner.Reset(); Owner->MarkAsGarbage();
			bOwnerInvalidated=true; Clock->Advance(0.1); return false;
		}
		if (bOwnerLoss && !bOwnerRetired)
		{
			if (Original->GetHandle()->IsActive() || Client->GetNativeClient().GetInFlightCount()!=2) return false;
			Test.TestFalse(TEXT("the real original owner is invalid"),LostOwner.IsValid());
			Test.TestTrue(TEXT("the actual custom refresh remains active after owned retirement"),Probe->Context && Probe->Context->IsActive());
			Test.TestFalse(TEXT("the delayed first helper is still held"),Probe->First->HasTerminalMetadata());
			Test.TestFalse(TEXT("the delayed second helper is still held"),Probe->Second->HasTerminalMetadata());
			Test.TestFalse(TEXT("owner suppression fabricates no action terminal metadata"),Original->HasTerminalMetadata());
			Test.TestEqual(TEXT("no original callback before held Done is released"),Probe->ResourceJson+Probe->Failures+Probe->WrapperErrors,0);
			bOwnerRetired=true; Clock->Advance(0.2); return false;
		}
		const bool bHelpersDone=!Probe->CreationError.IsEmpty() || (Probe->First && Probe->Second && Probe->First->HasTerminalMetadata() && Probe->Second->HasTerminalMetadata());
		if ((!bOwnerLoss && !Original->HasTerminalMetadata()) || !bHelpersDone || Client->GetNativeClient().GetInFlightCount()!=0 || Fake->NumInFlight()!=0) return false;
		Test.TestTrue(TEXT("completion used real later engine frames"),GFrameCounter>SentFrame);
		Test.TestEqual(TEXT("one live native refresh invocation"),Probe->Refreshes,1);
		Test.TestTrue(TEXT("actual provider runs on the per-wrapper duplicate"),Probe->RuntimeConfig.IsValid() && Probe->RuntimeConfig.Get()!=AssetAuth.Get());
		Test.TestTrue(TEXT("all factories admitted without a config error"),Probe->CreationError.IsEmpty());
		Test.TestEqual(TEXT("no action failed"),Probe->Failures,0);
		Test.TestEqual(TEXT("a retried 401 attempt is not forwarded as a primary client error"),Probe->WrapperErrors,0);
		Test.TestEqual(TEXT("first helper completed once"),Probe->FirstJson,1); Test.TestEqual(TEXT("another helper completed once"),Probe->SecondJson,1);
		Test.TestEqual(TEXT("second JSON subscriber received the same plain document"),Probe->SecondListenerJson,1);
		Test.TestTrue(TEXT("refresh helpers bypass Apply/session headers"),Probe->bBareFirst && Probe->bBareSecond);
		Test.TestTrue(TEXT("both helpers and JSON subscriber bypass the ordinary envelope"),Probe->bRawFirst && Probe->bRawSecond && Probe->bSecondListenerRaw);
		Test.TestEqual(TEXT("prepared first builder keeps the real refresh preset"),Probe->FirstWireId,Probe->ExpectedRefreshId);
		Test.TestEqual(TEXT("first action metadata retains that exact ID"),Probe->FirstMetaId,Probe->ExpectedRefreshId);
		Test.TestEqual(TEXT("another plain JSON listener sees the same ID"),Probe->SecondListenerId,Probe->ExpectedRefreshId);
		Test.TestTrue(TEXT("later helper gets a fresh nonzero ID instead of consuming preset twice"),!Probe->SecondWireId.IsEmpty() && Probe->SecondWireId!=TEXT("0") && Probe->SecondWireId!=Probe->ExpectedRefreshId);
		Test.TestEqual(TEXT("second action metadata is its actual new ID"),Probe->SecondMetaId,Probe->SecondWireId);
		Test.TestTrue(TEXT("exact original Done accepted the custom refresh"),Probe->bFinishAccepted);
		Test.TestEqual(TEXT("only a surviving original is replayed"),Probe->ProtectedCalls,bOwnerLoss?1:2); Test.TestEqual(TEXT("no helper was retried"),Fake->NumCalls(),bOwnerLoss?3:4);
		for (int32 Index=0;Index<Fake->NumCalls();++Index)
			Test.TestFalse(TEXT("no native progress callback without an action progress subscriber"),Fake->GetCall(Index).Request.bProgressCallback);
		if (bOwnerLoss)
		{
			Test.TestTrue(TEXT("managed owner retirement was observed before helper completion"),bOwnerRetired);
			Test.TestFalse(TEXT("the retired original remains inactive"),Original->GetHandle()->IsActive());
			Test.TestFalse(TEXT("late refresh does not fabricate original metadata"),Original->HasTerminalMetadata());
			Test.TestEqual(TEXT("late refresh never invokes the original callback"),Probe->ResourceJson,0);
		}
		else
		{
			Test.TestEqual(TEXT("ordinary JSON action applied its actual envelope"),Probe->ResourceValue,7); Test.TestEqual(TEXT("ordinary success once"),Probe->ResourceJson,1);
		}
		if (Test.TestNotNull(TEXT("saved context exists only as an ended invocation"),Probe->Context.Get()))
		{
			Test.TestFalse(TEXT("saved context is no longer active"),Probe->Context->IsActive());
			Test.AddExpectedMessage(TEXT("VaRest ignored a stale custom-auth refresh finish"),ELogVerbosity::Warning,EAutomationExpectedMessageFlags::Contains,1,false);
			Test.TestFalse(TEXT("duplicate finish cannot affect the new session"),Probe->Context->FinishRefresh(EVaRestCustomRefreshOutcome::Refreshed,TEXT("ignored"),TEXT(""),1,EVaRestErrorKind::None,TEXT("")));
			FString Error; Test.TestNull(TEXT("ended context cannot create another refresh helper"),Probe->Context->RequestJson(Probe.Get(),TEXT("POST"),TEXT("/late"),FVaRestJson::Object(),Error));
			Test.TestEqual(TEXT("stale helper refusal is explicit"),Error,FString(TEXT("StaleRefreshContext")));
		}
		Test.TestEqual(TEXT("late finish preserved the actual refreshed token"),Client->GetNativeClient().GetSession().AccessToken,FString(TEXT("fixture-fresh")));
		for (int32 Index=0;Index<Fake->NumCalls();++Index) Fake->ReleaseCompletedClock(Index); // terminal before physical tail release
		return true;
	}
private:
	void Close()
	{
		if (Hub) Hub->DeinitializeOnGT();
		if (Probe) { Probe->Context=nullptr; Probe->First=nullptr; Probe->Second=nullptr; }
		OriginalOwner.Reset(); Original.Reset(); Client.Reset(); Hub.Reset(); AssetAuth.Reset(); Api.Reset(); Probe.Reset(); Fake.Reset(); Clock.Reset(); Isolation.Reset();
	}
	FAutomationTestBase& Test;
	TStrongObjectPtr<UVaRestActionAuthTestProbe> Probe;
	TStrongObjectPtr<UVaRestApi> Api;
	TStrongObjectPtr<UVaRestActionAuthTestConfig> AssetAuth;
	TStrongObjectPtr<UVaRestHub> Hub;
	TStrongObjectPtr<UVaRestApiClient> Client;
	TStrongObjectPtr<UVaRestGetJsonAction> Original;
	TStrongObjectPtr<UObject> OriginalOwner;
	TWeakObjectPtr<UObject> LostOwner;
	TSharedPtr<FVaRestVirtualClock,ESPMode::ThreadSafe> Clock;
	TSharedPtr<FVaRestFakeTransport,ESPMode::ThreadSafe> Fake;
	TUniquePtr<FVaRestTrace::FScopedSinkIsolation> Isolation;
	uint64 SentFrame=0;
	double StartedAt=0;
	bool bOwnerLoss=false,bOwnerInvalidated=false,bOwnerRetired=false;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonRefreshActionTest,"VaRest.Api.Actions.Json.CustomRefreshPreparedBuilder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestJsonRefreshActionTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(VaRest::Http::Editor::JsonRefreshTest::FRun(*this)); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonRefreshOwnerRetirementTest,"VaRest.Api.Actions.Json.CustomRefreshOwnerRetirement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestJsonRefreshOwnerRetirementTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(VaRest::Http::Editor::JsonRefreshTest::FRun(*this,true)); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCustomAuthUnicodeAdapterTest,"VaRest.Api.Actions.Auth.UnicodeAdapter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestCustomAuthUnicodeAdapterTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UVaRestActionAuthTestConfig> Config(NewObject<UVaRestActionAuthTestConfig>());
	auto Native=Config->CreateAuth();
	if (!TestTrue(TEXT("real public custom-auth adapter exists"),Native.IsValid())) return false;
	const TCHAR UnicodeUnits[]={TCHAR(0x00d6),TCHAR(0x0416),TCHAR(0xd83d),TCHAR(0xde80)};
	const FString Unicode=FString::ConstructFromPtrSize(UnicodeUnits,UE_ARRAY_COUNT(UnicodeUnits));
	const TCHAR SurrogateUnits[]={TCHAR(0xd800)};
	const FString InvalidUnicode=FString::ConstructFromPtrSize(SurrogateUnits,UE_ARRAY_COUNT(SurrogateUnits));
	const auto Check=[&](const FString& Token,const FString& QueryName,const FString& QueryValue,bool Expected)
	{
		FVaRestHeaders Headers; Headers.Set(TEXT("X-Keep"),TEXT("unchanged"));
		TArray<TPair<FString,FString>> Query; Query.Emplace(QueryName,QueryValue);
		FVaRestSessionData Session; Session.AccessToken=Token;
		FVaRestAuthTarget Target(Headers,Query,Session);
		TestEqual(TEXT("actual custom adapter accepts only valid configured text"),Native->Apply(Target),Expected);
		TestEqualSensitive(TEXT("unrelated header survives adapter admission/refusal"),Headers.Get(TEXT("X-Keep")),FString(TEXT("unchanged")));
		TestEqual(TEXT("query count stays exact"),Query.Num(),1);
		if (Query.Num()==1)
		{
			TestEqualSensitive(TEXT("query name is preserved exactly"),Query[0].Key,QueryName);
			TestEqualSensitive(TEXT("query value is preserved exactly"),Query[0].Value,QueryValue);
		}
		if (Expected)
		{
			TestTrue(TEXT("valid text has no adapter configuration error"),Target.ConfigError.IsEmpty());
			TestEqualSensitive(TEXT("real reflected provider header preserves complete Unicode"),Headers.Get(TEXT("Authorization")),FString(TEXT("Bearer "))+Token);
		}
		else
		{
			TestFalse(TEXT("malformed text produces a real adapter error"),Target.ConfigError.IsEmpty());
			TestFalse(TEXT("refused reflected edits are not partially published"),Headers.Find(TEXT("Authorization"))!=nullptr);
		}
	};
	Check(TEXT("plain-token"),TEXT("key"),TEXT("value"),true);
	Check(Unicode,TEXT("key"),TEXT("value"),true);
	Check(TEXT("plain-token"),FString(TEXT("key-"))+Unicode,FString(TEXT("value-"))+Unicode,true);
	Check(InvalidUnicode,TEXT("key"),TEXT("value"),false);
	Check(TEXT("plain-token"),FString(TEXT("key-"))+InvalidUnicode,TEXT("value"),false);
	Check(TEXT("plain-token"),TEXT("key"),FString(TEXT("value-"))+InvalidUnicode,false);
	Check(TEXT("bad\r\nheader"),TEXT("key"),TEXT("value"),false);
	const TCHAR NullUnits[]={TEXT('a'),TCHAR(0),TEXT('b')};
	Check(FString::ConstructFromPtrSize(NullUnits,UE_ARRAY_COUNT(NullUnits)),TEXT("key"),TEXT("value"),false);
	Config->RetireOnGT();
	FVaRestHeaders Headers; TArray<TPair<FString,FString>> Query; FVaRestSessionData Session; Session.AccessToken=Unicode;
	FVaRestAuthTarget Target(Headers,Query,Session);
	TestFalse(TEXT("retired real adapter remains unavailable"),Native->Apply(Target));
	TestFalse(TEXT("retired adapter publishes no authorization header"),Headers.Find(TEXT("Authorization"))!=nullptr);
	Native.Reset(); Config.Reset();
	return true;
}
#endif
