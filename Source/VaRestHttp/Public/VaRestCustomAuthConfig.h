// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestAuthConfig.h"
#include "VaRestAuth.h"
#include "VaRestHttpBlueprintTypes.h"
#include "VaRestDynamicRequest.h"
#include "VaRestCustomAuthConfig.generated.h"

class UVaRestCustomAuthConfig;
class UVaRestGetJsonAction;
namespace VaRest::Http { class FCustomAuthAdapter; struct FCustomRefreshContextData; struct FCustomRefreshPending; }

UENUM(BlueprintType)
enum class EVaRestCustomRefreshOutcome : uint8 { Refreshed, Rejected, Failed };
USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestAuthSecretValue
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere,BlueprintReadWrite,Category="VaRest|Auth") FName Slot;
	UPROPERTY(EditAnywhere,BlueprintReadWrite,Category="VaRest|Auth",meta=(PasswordField="true")) FString Value;
};

/** One refresh invocation. Saved contexts cannot finish a later invocation or consume another refresh preset ID. */
UCLASS(BlueprintType)
class VARESTHTTP_API UVaRestAuthRefreshContext : public UObject
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintPure,Category="VaRest|Auth") FVaRestSessionDataView GetSession() const;
	UFUNCTION(BlueprintPure,Category="VaRest|Auth") FString GetRefreshId() const;
	UFUNCTION(BlueprintPure,Category="VaRest|Auth") bool IsActive() const;
	/** Creates the same one-time preset refresh request used by the Request Refresh Json async node. */
	UFUNCTION(BlueprintCallable,Category="VaRest|Auth",meta=(WorldContext="WorldContextObject",BlueprintInternalUseOnly="true"))
	UVaRestGetJsonAction* RequestJson(const UObject* WorldContextObject,const FString& Verb,const FString& Path,const FVaRestJson& Body,FString& Error);
	UFUNCTION(BlueprintCallable,Category="VaRest|Auth") bool FinishRefresh(EVaRestCustomRefreshOutcome Outcome,const FString& AccessToken,
		const FString& RefreshToken,double ExpiresInSeconds,EVaRestErrorKind FailedKind,const FString& Message);
	/** Native sibling used by the later real BP action factory; it retains the core one-time preset contract. */
	TVaRestRequest<FVaRestDynamicResult> RequestDynamic(FStringView Verb,FStringView Path,FVaRestDynamicResultConfig&& Config) const;
	virtual void BeginDestroy() override;
private:
	friend class UVaRestCustomAuthConfig;
	void InitializeOnGT(UVaRestCustomAuthConfig* Owner,const FVaRestAuthRefreshContext& Native);
	void EndOnGT();
	TWeakObjectPtr<UVaRestCustomAuthConfig> Owner;
	TSharedPtr<const VaRest::Http::FCustomRefreshContextData,ESPMode::ThreadSafe> Data;
	uint64 RefreshId=0;
	bool bActive=false;
	bool bWarnedStaleFinish=false;
};

/** Blueprint implementation lives on the per-wrapper duplicate. Native providers keep this UObject weakly. */
UCLASS(BlueprintType,Blueprintable,EditInlineNew,DefaultToInstanced,meta=(DisplayName="Custom Blueprint Auth"))
class VARESTHTTP_API UVaRestCustomAuthConfig : public UVaRestAuthConfig
{
	GENERATED_BODY()
public:
	UVaRestCustomAuthConfig(const FObjectInitializer& Initializer=FObjectInitializer::Get());
	UVaRestCustomAuthConfig(FVTableHelper& Helper);
	virtual ~UVaRestCustomAuthConfig() override;
	virtual TSharedPtr<IVaRestAuth,ESPMode::ThreadSafe> CreateAuth() const override;
	virtual void Validate(FVaRestApiValidationContext& Context) const override;
	virtual void RetireOnGT() override;
	virtual void BeginDestroy() override;
	UFUNCTION(BlueprintNativeEvent,Category="VaRest|Auth") bool Apply(UPARAM(ref) FVaRestAuthTargetData& Target) const;
	virtual bool Apply_Implementation(FVaRestAuthTargetData& Target) const;
	UFUNCTION(BlueprintNativeEvent,BlueprintPure,Category="VaRest|Auth") FName GetAuthName() const;
	virtual FName GetAuthName_Implementation() const;
	UFUNCTION(BlueprintNativeEvent,BlueprintPure,Category="VaRest|Auth") FName GetIdentityContractName() const;
	virtual FName GetIdentityContractName_Implementation() const;
	UFUNCTION(BlueprintNativeEvent,BlueprintPure,Category="VaRest|Auth") bool GetIdentity(const FVaRestSessionDataView& Session,FString& Identity) const;
	virtual bool GetIdentity_Implementation(const FVaRestSessionDataView& Session,FString& Identity) const;
	UFUNCTION(BlueprintNativeEvent,BlueprintPure,Category="VaRest|Auth") bool CanRefresh(const FVaRestSessionDataView& Session) const;
	virtual bool CanRefresh_Implementation(const FVaRestSessionDataView& Session) const;
	UFUNCTION(BlueprintNativeEvent,BlueprintPure,Category="VaRest|Auth") bool NeedsRefresh(const FVaRestSessionDataView& Session,double Now) const;
	virtual bool NeedsRefresh_Implementation(const FVaRestSessionDataView& Session,double Now) const;
	UFUNCTION(BlueprintNativeEvent,BlueprintPure,Category="VaRest|Auth") bool IsAuthFailure(int32 Status) const;
	virtual bool IsAuthFailure_Implementation(int32 Status) const;
	UFUNCTION(BlueprintNativeEvent,BlueprintPure,Category="VaRest|Auth") TArray<FString> GetRedactedKeys() const;
	virtual TArray<FString> GetRedactedKeys_Implementation() const;
	UFUNCTION(BlueprintNativeEvent,BlueprintPure,Category="VaRest|Auth") TArray<FVaRestAuthSecretValue> GetSecretValues() const;
	virtual TArray<FVaRestAuthSecretValue> GetSecretValues_Implementation() const;
	UFUNCTION(BlueprintNativeEvent,Category="VaRest|Auth") void Refresh(UVaRestAuthRefreshContext* Context);
	virtual void Refresh_Implementation(UVaRestAuthRefreshContext* Context);
private:
	friend class VaRest::Http::FCustomAuthAdapter;
	friend class UVaRestAuthRefreshContext;
	void BeginRefreshOnGT(const FVaRestAuthRefreshContext& Context,TUniqueFunction<void(FVaRestAuthRefreshResult&&)>&& Done);
	bool FinishOnGT(UVaRestAuthRefreshContext* Context,FVaRestAuthRefreshResult&& Result);
	void AbandonContextOnGT(UVaRestAuthRefreshContext* Context);
	UPROPERTY(Transient) TObjectPtr<UVaRestAuthRefreshContext> PendingContext;
	TUniquePtr<VaRest::Http::FCustomRefreshPending> Pending;
	bool bRetired=false;
};
