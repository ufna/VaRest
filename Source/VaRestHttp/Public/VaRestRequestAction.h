// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "VaRestHttpBlueprintTypes.h"
#include "VaRestRequestAction.generated.h"

class UVaRestApiClient;
class UVaRestApi;
class UVaRestRequestAction;
class UVaRestRequestHandle;
class FVaRestDynamicResult;
template <typename T> class TVaRestRequest;
namespace VaRest::Http { struct FRequestActionState; class FDynamicPrepareToken; }

/** Native-only instrumentation of the actual transfer path; no reflection or frame/property ownership. */
struct FVaRestRequestTransferDiagnostics
{
	uint32 Preparations=0,Copies=0,Confirmations=0,Continuations=0,Unavailable=0,AddressBorrows=0;
	uint64 PreparedFrame=0,DeliveredFrame=0;
};

UENUM(BlueprintType)
enum class EVaRestTypedOutputUnavailableReason : uint8
{
	TypeChangedAfterCompletion, TargetChangedAfterCompletion, OutputSuperseded, OutputNotBound
};
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVaRestActionSuccess,const FVaRestResponseData&,Http);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVaRestActionError,const FVaRestError&,Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVaRestActionQueued,const FString&,QueueId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FVaRestActionProgress,int64,BytesSent,int64,BytesReceived,int64,UploadTotal,int64,DownloadTotal);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FVaRestActionUnavailable,const FVaRestResponseData&,Http,const FVaRestError&,OriginalError,EVaRestTypedOutputUnavailableReason,Reason);

// Fixed internal singlecast signatures. Only the shared compiler expansion builds their functions.
DECLARE_DYNAMIC_DELEGATE_TwoParams(FVaRestPrepareOutput,UVaRestRequestAction*,Action,const FString&,RequestIdText);
DECLARE_DYNAMIC_DELEGATE_FiveParams(FVaRestContinueOutput,UVaRestRequestAction*,Action,const FString&,RequestIdText,bool,bSuccess,const FVaRestResponseData&,Http,const FVaRestError&,Error);
DECLARE_DYNAMIC_DELEGATE_FiveParams(FVaRestUnavailableOutput,UVaRestRequestAction*,Action,const FString&,RequestIdText,const FVaRestResponseData&,Http,const FVaRestError&,OriginalError,EVaRestTypedOutputUnavailableReason,Reason);
DECLARE_DYNAMIC_DELEGATE_ThreeParams(FVaRestQueuedOutput,UVaRestRequestAction*,Action,const FString&,RequestIdText,const FString&,QueueId);

/** Metadata is public; reflected output transfer is private, synchronous, and one-shot before Completed. */
UCLASS(BlueprintType,meta=(HasDedicatedAsyncNode,ExposedAsyncProxy="Action"))
class VARESTHTTP_API UVaRestRequestAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()
public:
	UVaRestRequestAction(const FObjectInitializer& Initializer=FObjectInitializer::Get());
	UVaRestRequestAction(FVTableHelper& Helper);
	virtual ~UVaRestRequestAction() override;
	/** Internal admitted construction. A refusal returns null with Error, before any request is sent. */
	UFUNCTION(BlueprintCallable,Category="VaRest|Internal",meta=(BlueprintInternalUseOnly="true",WorldContext="WorldContextObject"))
	static UVaRestRequestAction* CreateRequest(const UObject* WorldContextObject,UVaRestApiClient* Client,const FString& Verb,const FString& Path,
		const FVaRestTypeRef& ResponseType,const FVaRestTypeRef& ErrorType,FString& Error);
	UFUNCTION(BlueprintCallable,Category="VaRest|Internal",meta=(BlueprintInternalUseOnly="true",WorldContext="WorldContextObject"))
	static UVaRestRequestAction* CreateEndpointRequest(const UObject* WorldContextObject,UVaRestApi* Api,FGuid EndpointId,FName EndpointName,FName Tag,FString& Error);
	UFUNCTION(BlueprintPure,Category="VaRest|Internal",meta=(BlueprintInternalUseOnly="true"))
	static FVaRestError AdmissionError(const FString& Detail,FName Tag,FVaRestResponseData& Http);
	UFUNCTION(BlueprintCallable,Category="VaRest|Internal",meta=(BlueprintInternalUseOnly="true",AutoCreateRefTerm="Query,Headers"))
	void ConfigureRequest(const TMap<FString,FString>& Query,const TArray<FVaRestHeader>& Headers,FName Tag,EVaRestAuthMode AuthMode,bool bQueueWhenOffline);
	UFUNCTION(BlueprintCallable,CustomThunk,Category="VaRest|Internal",meta=(BlueprintInternalUseOnly="true",CustomStructureParam="Value",AutoCreateRefTerm="Value"))
	bool SetParameter(const FString& Name,EVaRestEndpointParamLocation Location,EVaRestQueryStyle Style,const int32& Value);
	DECLARE_FUNCTION(execSetParameter);
	UFUNCTION(BlueprintCallable,CustomThunk,Category="VaRest|Internal",meta=(BlueprintInternalUseOnly="true",CustomStructureParam="Value",AutoCreateRefTerm="Value"))
	bool SetRequestBody(EVaRestEndpointBodyKind Kind,const FString& ContentType,const int32& Value,bool bBytesAsBase64=false);
	DECLARE_FUNCTION(execSetRequestBody);
	UFUNCTION(BlueprintCallable,Category="VaRest|Internal",meta=(BlueprintInternalUseOnly="true"))
	bool BindOutput(FVaRestPrepareOutput Prepare,FVaRestContinueOutput Continue,FVaRestUnavailableOutput Unavailable);
	/** One internal metadata callback carries its own action; overlapping nodes never consult a shared action temporary. */
	UFUNCTION(BlueprintCallable,Category="VaRest|Internal",meta=(BlueprintInternalUseOnly="true"))
	bool BindMetadata(FVaRestContinueOutput Metadata);
	UFUNCTION(BlueprintCallable,Category="VaRest|Internal",meta=(BlueprintInternalUseOnly="true"))
	bool BindQueued(FVaRestQueuedOutput Queued);
	UFUNCTION(BlueprintCallable,CustomThunk,Category="VaRest|Internal",meta=(BlueprintInternalUseOnly="true",CustomStructureParam="Value"))
	bool TakeResponse(int32& Value);
	DECLARE_FUNCTION(execTakeResponse);
	UFUNCTION(BlueprintCallable,CustomThunk,Category="VaRest|Internal",meta=(BlueprintInternalUseOnly="true",CustomStructureParam="Value"))
	bool TakeErrorBody(int32& Value);
	DECLARE_FUNCTION(execTakeErrorBody);
	UFUNCTION(BlueprintCallable,Category="VaRest|Internal",meta=(BlueprintInternalUseOnly="true"))
	bool ConfirmPrepared(const FString& RequestIdText);
	/** The generated continuation calls this with its engine-owned cookie BEFORE entering user execution. */
	UFUNCTION(BlueprintCallable,Category="VaRest|Internal",meta=(BlueprintInternalUseOnly="true"))
	bool AcceptPreparedOutput(const FString& StoredCookie,const FString& RequestIdText);
	UFUNCTION(BlueprintPure,Category="VaRest|Http") UVaRestRequestHandle* GetHandle() const { return Handle; }
	UFUNCTION(BlueprintPure,Category="VaRest|Http") bool HasTerminalMetadata() const;
	UFUNCTION(BlueprintPure,Category="VaRest|Http") FVaRestResponseData GetTerminalResponse() const { return TerminalResponse; }
	UFUNCTION(BlueprintPure,Category="VaRest|Http") FVaRestError GetTerminalError() const { return TerminalError; }
	UFUNCTION(BlueprintPure,Category="VaRest|Http") FVaRestJson GetJson() const;
	UFUNCTION(BlueprintPure,Category="VaRest|Http") FString GetText() const;
	UFUNCTION(BlueprintPure,Category="VaRest|Http") TArray<uint8> GetBytes() const;
	UFUNCTION(BlueprintPure,Category="VaRest|Http") FVaRestJson GetErrorJson() const { return TerminalError.Body; }
	UFUNCTION(BlueprintPure,Category="VaRest|Http") bool HasErrorJson() const { return TerminalError.Body.IsValid(); }
	UFUNCTION(BlueprintPure,Category="VaRest|Http") FString GetErrorText() const;
	UFUNCTION(BlueprintPure,Category="VaRest|Http") TArray<uint8> GetErrorBytes() const { return TerminalResponse.Body; }
	UFUNCTION(BlueprintPure,Category="VaRest|Http") bool HasErrorBody() const { return TerminalError.IsError() && (TerminalError.Body.IsValid() || !TerminalResponse.Body.IsEmpty()); }
	UFUNCTION(BlueprintPure,Category="VaRest|Offline") FString GetQueueId() const { return TerminalQueueId; }
	UFUNCTION(BlueprintPure,Category="VaRest|Http") bool GetTypedOutputUnavailable(EVaRestTypedOutputUnavailableReason& Reason) const;
	UPROPERTY(BlueprintAssignable,Category="VaRest|Http") FVaRestActionSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable,Category="VaRest|Http") FVaRestActionError OnError;
	UPROPERTY(BlueprintAssignable,Category="VaRest|Offline") FVaRestActionQueued OnQueued;
	UPROPERTY(BlueprintAssignable,Category="VaRest|Http") FVaRestActionProgress OnProgress;
	UPROPERTY(BlueprintAssignable,Category="VaRest|Http") FVaRestActionUnavailable OnTypedOutputUnavailable;
	virtual void Activate() override;
	virtual void BeginDestroy() override;
	FVaRestRequestTransferDiagnostics GetTransferDiagnostics() const;
	/** Plain native diagnostic shell only: retaining/releasing it off-GT owns no reflected state or GTK permit. */
	FVaRestDynamicResult GetNativeResult() const;
	FVaRestDynamicResult TakeNativeResultOnGT();
protected:
	virtual void BroadcastSuccessOnGT(const FVaRestDynamicResult& Result);
	bool CanDeliverOnGT() const;
private:
	friend class UVaRestRequestLibrary;
	friend class UVaRestAuthRefreshContext;
	static UVaRestRequestAction* AdoptOnGT(const UObject* WorldContextObject,UVaRestApiClient* Client,
		TVaRestRequest<FVaRestDynamicResult>&& Request,bool bTypedResponse,bool bTypedError,UClass* ActionClass,FString& Error);
	bool TargetCurrentOnGT() const;
	void PrepareOnGT(VaRest::Http::FDynamicPrepareToken& Token);
	bool TakeOnGT(const FProperty* Property,void* Address,bool bError);
	bool SetParameterOnGT(const FString& Name,EVaRestEndpointParamLocation Location,EVaRestQueryStyle Style,const FProperty* Property,const void* Address);
	bool SetRequestBodyOnGT(EVaRestEndpointBodyKind Kind,const FString& ContentType,const FProperty* Property,const void* Address,bool bBytesAsBase64);
	void CompleteOnGT(const TVaRestResult<FVaRestDynamicResult>& Result);
	void QueuedOnGT(const FString& QueueId);
	void ProgressOnGT(const struct FVaRestProgress& Progress);
	void DeliverProgressOnGT();
	void UnavailableOnGT(EVaRestTypedOutputUnavailableReason Reason);
	void ReleaseOnGT();
	UFUNCTION() void HubClosing();
	UPROPERTY(Transient) TObjectPtr<UVaRestRequestHandle> Handle;
	UPROPERTY(Transient) FVaRestResponseData TerminalResponse;
	UPROPERTY(Transient) FVaRestError TerminalError;
	UPROPERTY(Transient) FString TerminalQueueId;
	UPROPERTY(Transient) FVaRestPrepareOutput PrepareDelegate;
	UPROPERTY(Transient) FVaRestContinueOutput ContinueDelegate;
	UPROPERTY(Transient) FVaRestUnavailableOutput UnavailableDelegate;
	UPROPERTY(Transient) FVaRestContinueOutput MetadataDelegate;
	UPROPERTY(Transient) FVaRestQueuedOutput QueuedDelegate;
	TUniquePtr<VaRest::Http::FRequestActionState> State;
};
