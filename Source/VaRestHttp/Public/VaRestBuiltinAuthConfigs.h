// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestAuthConfig.h"
#include "VaRestAuthTypes.h"
#include "VaRestClientCredentials.h"

#include "VaRestBuiltinAuthConfigs.generated.h"

class UVaRestApi;

UCLASS(BlueprintType, EditInlineNew, DefaultToInstanced, meta = (DisplayName = "Bearer"))
class VARESTHTTP_API UVaRestBearerAuthConfig : public UVaRestAuthConfig
{
	GENERATED_BODY()
public:
	virtual TSharedPtr<IVaRestAuth, ESPMode::ThreadSafe> CreateAuth() const override;
	virtual void Validate(FVaRestApiValidationContext& Context) const override;
};

UCLASS(BlueprintType, EditInlineNew, DefaultToInstanced, meta = (DisplayName = "API Key"))
class VARESTHTTP_API UVaRestApiKeyAuthConfig : public UVaRestAuthConfig
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Auth")
	EVaRestApiKeyPlacement Placement = EVaRestApiKeyPlacement::Header;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Auth")
	FString Name = TEXT("X-Api-Key");
	/** Development value only. SetApiKey is the production credential entry point. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Auth", meta = (PasswordField = "true"))
	FString Key;

	virtual TSharedPtr<IVaRestAuth, ESPMode::ThreadSafe> CreateAuth() const override;
	virtual void Validate(FVaRestApiValidationContext& Context) const override;
};

UCLASS(BlueprintType, EditInlineNew, DefaultToInstanced, meta = (DisplayName = "Basic"))
class VARESTHTTP_API UVaRestBasicAuthConfig : public UVaRestAuthConfig
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Auth")
	FString User;
	/** Development value only. SetBasicCredentials is the production credential entry point. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Auth", meta = (PasswordField = "true"))
	FString Password;

	virtual TSharedPtr<IVaRestAuth, ESPMode::ThreadSafe> CreateAuth() const override;
	virtual void Validate(FVaRestApiValidationContext& Context) const override;
};

UCLASS(BlueprintType, EditInlineNew, DefaultToInstanced, meta = (DisplayName = "Bearer with Refresh"))
class VARESTHTTP_API UVaRestBearerRefreshAuthConfig : public UVaRestAuthConfig
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Auth")
	FVaRestBearerRefreshConfig Config;
	/** Optional POST endpoint name. Only its path and trace name are used. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Auth")
	FName RefreshEndpoint;

	/** Resolve on the per-wrapper duplicate before CreateAuth; the original asset is never mutated. */
	bool ResolveEndpoint(const UVaRestApi& Api, FString& OutError);
	virtual TSharedPtr<IVaRestAuth, ESPMode::ThreadSafe> CreateAuth() const override;
	virtual void Validate(FVaRestApiValidationContext& Context) const override;

private:
	FName ResolvedEndpoint;
};

UCLASS(BlueprintType, EditInlineNew, DefaultToInstanced, meta = (DisplayName = "OAuth Client Credentials"))
class VARESTHTTP_API UVaRestClientCredentialsAuthConfig : public UVaRestAuthConfig
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Auth")
	FVaRestClientCredentialsConfig Config;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Auth")
	FName TokenEndpoint;

	bool ResolveEndpoint(const UVaRestApi& Api, FString& OutError);
	virtual TSharedPtr<IVaRestAuth, ESPMode::ThreadSafe> CreateAuth() const override;
	virtual void Validate(FVaRestApiValidationContext& Context) const override;

private:
	FName ResolvedEndpoint;
};
