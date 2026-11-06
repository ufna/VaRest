// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestActionAuthTestTypes.h"
#include "VaRestRequestLibrary.h"
#include "VaRestRequestHandle.h"
#include "VaRestHub.h"
#include "CoreGlobals.h"
#include "UObject/StrongObjectPtr.h"

void UVaRestActionAuthTestProbe::FirstCompleted(const FVaRestJson& Json,const FVaRestResponseData& Http)
{
	check(IsInGameThread()); ++FirstJson; FirstMetaId=Http.RequestId;
	bRawFirst=Json.Get(TEXT("access_token")).AsString()==TEXT("fixture-fresh") && !Json.Has(TEXT("data"));
	if (Context) bFinishAccepted=Context->FinishRefresh(EVaRestCustomRefreshOutcome::Refreshed,Json.Get(TEXT("access_token")).AsString(),
		Json.Get(TEXT("refresh_token")).AsString(),600,EVaRestErrorKind::None,TEXT(""));
}
void UVaRestActionAuthTestProbe::FirstObservedAgain(const FVaRestJson& Json,const FVaRestResponseData& Http)
{
	check(IsInGameThread()); ++SecondListenerJson; SecondListenerId=Http.RequestId;
	bSecondListenerRaw=Json.Get(TEXT("access_token")).AsString()==TEXT("fixture-fresh") && !Json.Has(TEXT("data"));
}
void UVaRestActionAuthTestProbe::SecondCompleted(const FVaRestJson& Json,const FVaRestResponseData& Http)
{
	check(IsInGameThread()); ++SecondJson; SecondMetaId=Http.RequestId;
	bRawSecond=Json.Get(TEXT("access_token")).AsString()==TEXT("fixture-fresh") && !Json.Has(TEXT("data"));
}
void UVaRestActionAuthTestProbe::ResourceCompleted(const FVaRestJson& Json,const FVaRestResponseData&)
{
	check(IsInGameThread()); ++ResourceJson; ResourceValue=static_cast<int32>(Json.Get(TEXT("value")).AsInt64());
}
void UVaRestActionAuthTestProbe::Failed(const FVaRestError& Error)
{
	check(IsInGameThread()); ++Failures; WrapperErrorsAtFailure=WrapperErrors;
	if (Context && Context->IsActive()) Context->FinishRefresh(EVaRestCustomRefreshOutcome::Failed,TEXT(""),TEXT(""),0,Error.Kind,Error.Message);
}
void UVaRestActionAuthTestProbe::Flushed(EVaRestFlushQueueOutcome Outcome)
{
	check(IsInGameThread()); ++Flushes; FlushOutcome=Outcome;
}
void UVaRestActionAuthTestProbe::WrapperFailed(const FVaRestError& Error)
{
	bWrapperErrorOnGT=bWrapperErrorOnGT && IsInGameThread(); ++WrapperErrors; WrapperError=Error;
	if (auto* Hub=CloseHubOnWrapperError.Get()) Hub->DeinitializeOnGT();
}
void UVaRestActionAuthTestProbe::Progressed(int64 BytesSent,int64 BytesReceived,int64 UploadTotal,int64 DownloadTotal)
{
	check(IsInGameThread()); ++ProgressCalls; ProgressFrame=GFrameCounter;
	ProgressSent=BytesSent; ProgressReceived=BytesReceived; ProgressUploadTotal=UploadTotal; ProgressDownloadTotal=DownloadTotal;
	if (auto* Action=ProgressAction.Get())
	{
		bProgressAfterTerminal=bProgressAfterTerminal || Action->HasTerminalMetadata();
		if (bCancelOnProgress) Action->GetHandle()->Cancel();
	}
	if (auto* Hub=CloseHubOnProgress.Get()) Hub->DeinitializeOnGT();
}
void UVaRestActionAuthTestProbe::JsonCompleted(const FVaRestJson& Json,const FVaRestResponseData& Http)
{
	check(IsInGameThread()); ++JsonResponses; LastJson=Json; LastJsonHttp=Http;
}
void UVaRestActionAuthTestProbe::MetadataCompleted(const FVaRestResponseData&)
{
	check(IsInGameThread()); ++MetadataSuccesses;
}
void UVaRestActionAuthTestProbe::TypedUnavailable(const FVaRestResponseData&,const FVaRestError&,EVaRestTypedOutputUnavailableReason)
{
	check(IsInGameThread()); ++UnavailableEvents;
}
bool UVaRestActionAuthTestConfig::Apply_Implementation(FVaRestAuthTargetData& Target) const
{
	if (Target.Session.AccessToken.IsEmpty()) return false;
	Target.Headers.Add({TEXT("Authorization"),FString(TEXT("Bearer "))+Target.Session.AccessToken}); return true;
}
void UVaRestActionAuthTestConfig::Refresh_Implementation(UVaRestAuthRefreshContext* InContext)
{
	check(IsInGameThread());
	if (!IsValid(Probe)) { InContext->FinishRefresh(EVaRestCustomRefreshOutcome::Failed,TEXT(""),TEXT(""),0,EVaRestErrorKind::Config,TEXT("fixture observer unavailable")); return; }
	const TStrongObjectPtr<UVaRestActionAuthTestProbe> Observer(Probe.Get());
	++Probe->Refreshes; Probe->RuntimeConfig=this; Probe->Context=InContext; Probe->ExpectedRefreshId=InContext->GetRefreshId();
	const FVaRestJson Body=FVaRestJson::Object();
	Probe->First=UVaRestRequestLibrary::RequestRefreshJson(Probe.Get(),InContext,TEXT("POST"),TEXT("/refresh"),Body,Probe->CreationError);
	if (Probe->First) Probe->Second=UVaRestRequestLibrary::RequestRefreshJson(Probe.Get(),InContext,TEXT("POST"),TEXT("/aux"),Body,Probe->CreationError);
	if (!Probe->First || !Probe->Second)
	{
		InContext->FinishRefresh(EVaRestCustomRefreshOutcome::Failed,TEXT(""),TEXT(""),0,EVaRestErrorKind::Config,Probe->CreationError); return;
	}
	Probe->First->OnSuccessJson.AddDynamic(Probe.Get(),&UVaRestActionAuthTestProbe::FirstCompleted);
	Probe->First->OnSuccessJson.AddDynamic(Probe.Get(),&UVaRestActionAuthTestProbe::FirstObservedAgain);
	Probe->First->OnError.AddDynamic(Probe.Get(),&UVaRestActionAuthTestProbe::Failed);
	Probe->Second->OnSuccessJson.AddDynamic(Probe.Get(),&UVaRestActionAuthTestProbe::SecondCompleted);
	Probe->Second->OnError.AddDynamic(Probe.Get(),&UVaRestActionAuthTestProbe::Failed);
	Probe->First->Activate(); Probe->Second->Activate(); // one preset shared by both builders; later helper gets a fresh ID
}
