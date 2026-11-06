// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "VaRestClient.h"
#include "VaRestHttpBlueprintTypes.h"
#include "VaRestApiClient.generated.h"

class UVaRestApi;
class UVaRestAuthConfig;
class UVaRestHub;
namespace VaRest::Http { class FHubRuntime; struct FClientConfig; }

UENUM(BlueprintType)
enum class EVaRestOfflineQueueState : uint8 { Paused, PendingBusy, Active, Unavailable };
UENUM(BlueprintType)
enum class EVaRestOfflineQueueActivation : uint8 { Activated, PendingBusy, Refused };
USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestOfflineQueueActivationResult
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Offline")
	EVaRestOfflineQueueActivation Outcome=EVaRestOfflineQueueActivation::Refused;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Offline")
	FString Detail;
};
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVaRestApiSessionChanged,EVaRestSessionChange,Change);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FVaRestApiSessionExpired);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVaRestApiQueuedResult,const FVaRestQueuedResultData&,Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVaRestApiError,const FVaRestError&,Error);

/** Per-Hub asset client. Runtime credentials and native scope never live on the shared API asset. */
UCLASS(BlueprintType)
class VARESTHTTP_API UVaRestApiClient : public UObject
{
	GENERATED_BODY()
public:
	FVaRestClient GetNativeClient() const { check(IsInGameThread()); return Native; }
	UFUNCTION(BlueprintPure,Category="VaRest|API") UVaRestApi* GetApi() const { return Asset; }
	UFUNCTION(BlueprintPure,Category="VaRest|API") FName GetEnvironment() const { return Environment; }
	UFUNCTION(BlueprintPure,Category="VaRest|API") FString GetBaseUrl() const;
	UFUNCTION(BlueprintCallable,Category="VaRest|API") bool SetEnvironment(FName Name,FString& Error);
	UFUNCTION(BlueprintCallable,Category="VaRest|Offline") bool SetQueueAccount(const FString& Account,FString& Error);
	UFUNCTION(BlueprintCallable,Category="VaRest|Offline") FVaRestOfflineQueueActivationResult ActivateOfflineQueue();
	UFUNCTION(BlueprintCallable,Category="VaRest|Offline") bool PauseOfflineQueue(FString& Error);
	UFUNCTION(BlueprintPure,Category="VaRest|Offline") EVaRestOfflineQueueState GetOfflineQueueState() const;
	UFUNCTION(BlueprintPure,Category="VaRest|Offline") FString GetOfflineQueueError() const;
	UFUNCTION(BlueprintPure,Category="VaRest|Offline") int64 GetQueuedCount(bool& bLoaded) const;
	UFUNCTION(BlueprintCallable,Category="VaRest|Auth") void SetBearerToken(const FString& Token,bool bSameIdentity=false);
	UFUNCTION(BlueprintCallable,Category="VaRest|Auth") void SetSession(const FString& AccessToken,const FString& RefreshToken,double ExpiresInSeconds=0,bool bSameIdentity=false);
	UFUNCTION(BlueprintCallable,Category="VaRest|Auth") bool ImportSession(const FString& Json,bool bSameIdentity=false);
	UFUNCTION(BlueprintCallable,Category="VaRest|Auth") FString ExportSession() const;
	UFUNCTION(BlueprintCallable,Category="VaRest|Auth") void ClearSession();
	UFUNCTION(BlueprintCallable,Category="VaRest|Auth") bool SetApiKey(const FString& Key,FString& Error);
	UFUNCTION(BlueprintCallable,Category="VaRest|Auth") bool SetBasicCredentials(const FString& User,const FString& Password,FString& Error);
	UFUNCTION(BlueprintCallable,Category="VaRest|Auth") bool SetClientCredentials(const FString& ClientId,const FString& ClientSecret,const FString& Scope,FString& Error);
	UFUNCTION(BlueprintCallable,Category="VaRest|Auth") void SetSessionHeader(const FString& Name,const FString& Value);
	UFUNCTION(BlueprintCallable,Category="VaRest|Auth") void RemoveSessionHeader(const FString& Name);
	UFUNCTION(BlueprintCallable,Category="VaRest|Http") void SetHeader(const FString& Name,const FString& Value);
	UFUNCTION(BlueprintCallable,Category="VaRest|Http") void RemoveHeader(const FString& Name);
	UFUNCTION(BlueprintCallable,Category="VaRest|Http") void CancelAll(FName Tag=NAME_None);
	UFUNCTION(BlueprintCallable,Category="VaRest|Cache") void ClearCache();
	UPROPERTY(BlueprintAssignable,Category="VaRest|Auth") FVaRestApiSessionChanged OnSessionChanged;
	UPROPERTY(BlueprintAssignable,Category="VaRest|Auth") FVaRestApiSessionExpired OnSessionExpired;
	UPROPERTY(BlueprintAssignable,Category="VaRest|Offline") FVaRestApiQueuedResult OnQueuedResult;
	/** Once per delivered primary request error, before its individual handler; typed availability has its own event. */
	UPROPERTY(BlueprintAssignable,Category="VaRest|Http") FVaRestApiError OnError;
	virtual void BeginDestroy() override;
private:
	friend class UVaRestHub;
	friend class UVaRestRequestAction;
	friend class UVaRestFlushQueueAction;
	friend class UVaRestRequestLibrary;
	friend class UVaRestAuthRefreshContext;
	bool InitializeOnGT(UVaRestHub* Owner,UVaRestApi* Api,TSharedRef<VaRest::Http::FHubRuntime,ESPMode::ThreadSafe> Runtime,FString& Error,FStringView BaseUrl={});
	bool BuildTargetOnGT(FName Name,FStringView Account,VaRest::Http::FClientConfig& Config,FString& Error) const;
	void BindNativeOnGT(const FVaRestClient& Client);
	bool ReplaceAuthOnGT(UVaRestAuthConfig* Candidate,FString& Error);
	void DetachOnGT();
	UPROPERTY(Transient) TObjectPtr<UVaRestApi> Asset;
	UPROPERTY(Transient) TObjectPtr<UVaRestAuthConfig> Auth;
	TWeakObjectPtr<UVaRestHub> Hub;
	TWeakPtr<VaRest::Http::FHubRuntime,ESPMode::ThreadSafe> Runtime;
	FVaRestClient Native;
	FName Environment;
	FString QueueAccount;
	FDelegateHandle SessionChangedHandle,SessionExpiredHandle,QueuedHandle,ErrorHandle;
	bool bDetached=false;
	bool bPublishingTarget=false;
};
