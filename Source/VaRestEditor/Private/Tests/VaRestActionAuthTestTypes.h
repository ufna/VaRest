// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestCustomAuthConfig.h"
#include "VaRestGetJsonAction.h"
#include "VaRestFlushQueueAction.h"
#include "VaRestActionAuthTestTypes.generated.h"

class UVaRestHub;

/** Native observer keeps its own values; helpers contain only weak observer callbacks. */
UCLASS(HideDropdown,NotBlueprintable)
class UVaRestActionAuthTestProbe : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY() TObjectPtr<UVaRestAuthRefreshContext> Context;
	UPROPERTY() TObjectPtr<UVaRestGetJsonAction> First;
	UPROPERTY() TObjectPtr<UVaRestGetJsonAction> Second;
	TWeakObjectPtr<UVaRestCustomAuthConfig> RuntimeConfig;
	FString CreationError,ExpectedRefreshId,FirstWireId,SecondWireId,FirstMetaId,SecondMetaId,SecondListenerId;
	int32 Refreshes=0,FirstJson=0,SecondJson=0,SecondListenerJson=0,ResourceJson=0,Failures=0,ResourceValue=0,ProtectedCalls=0;
	bool bBareFirst=false,bBareSecond=false,bRawFirst=false,bRawSecond=false,bSecondListenerRaw=false,bFinishAccepted=false;
	int32 Flushes=0;
	EVaRestFlushQueueOutcome FlushOutcome=EVaRestFlushQueueOutcome::Refused;
	int32 WrapperErrors=0,WrapperErrorsAtFailure=-1;
	bool bWrapperErrorOnGT=true;
	FVaRestError WrapperError;
	TWeakObjectPtr<UVaRestHub> CloseHubOnWrapperError;
	TWeakObjectPtr<UVaRestRequestAction> ProgressAction;
	TWeakObjectPtr<UVaRestHub> CloseHubOnProgress;
	int32 ProgressCalls=0;
	int64 ProgressSent=0,ProgressReceived=0,ProgressUploadTotal=-1,ProgressDownloadTotal=-1;
	uint64 ProgressFrame=0;
	bool bProgressAfterTerminal=false,bCancelOnProgress=false;
	int32 JsonResponses=0;
	FVaRestJson LastJson;
	FVaRestResponseData LastJsonHttp;
	int32 MetadataSuccesses=0,UnavailableEvents=0;
	UFUNCTION() void FirstCompleted(const FVaRestJson& Json,const FVaRestResponseData& Http);
	UFUNCTION() void FirstObservedAgain(const FVaRestJson& Json,const FVaRestResponseData& Http);
	UFUNCTION() void SecondCompleted(const FVaRestJson& Json,const FVaRestResponseData& Http);
	UFUNCTION() void ResourceCompleted(const FVaRestJson& Json,const FVaRestResponseData& Http);
	UFUNCTION() void Failed(const FVaRestError& Error);
	UFUNCTION() void Flushed(EVaRestFlushQueueOutcome Outcome);
	UFUNCTION() void WrapperFailed(const FVaRestError& Error);
	UFUNCTION() void Progressed(int64 BytesSent,int64 BytesReceived,int64 UploadTotal,int64 DownloadTotal);
	UFUNCTION() void JsonCompleted(const FVaRestJson& Json,const FVaRestResponseData& Http);
	UFUNCTION() void MetadataCompleted(const FVaRestResponseData& Http);
	UFUNCTION() void TypedUnavailable(const FVaRestResponseData& Http,const FVaRestError& Error,EVaRestTypedOutputUnavailableReason Reason);
};

/** Exercises the actual weak Custom adapter and per-wrapper duplication; no transport/editor behavior is in the config. */
UCLASS(HideDropdown,NotBlueprintable)
class UVaRestActionAuthTestConfig : public UVaRestCustomAuthConfig
{
	GENERATED_BODY()
public:
	UPROPERTY() TObjectPtr<UVaRestActionAuthTestProbe> Probe;
	virtual bool Apply_Implementation(FVaRestAuthTargetData& Target) const override;
	virtual FName GetAuthName_Implementation() const override { return TEXT("M5ActionFixture"); }
	virtual bool CanRefresh_Implementation(const FVaRestSessionDataView& Session) const override { return !Session.RefreshToken.IsEmpty(); }
	virtual void Refresh_Implementation(UVaRestAuthRefreshContext* Context) override;
};
