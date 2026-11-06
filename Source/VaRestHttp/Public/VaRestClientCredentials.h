// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaRestClientCredentials.generated.h"

/** RFC 6749 2.3.1 client authentication, independent of the refresh-token grant. */
UENUM(BlueprintType)
enum class EVaRestOAuthClientAuthMethod : uint8
{
	Basic,
	Form
};

/** A confidential client's client_credentials grant. No refresh token is required. */
USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestClientCredentialsConfig
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Auth")
	FString TokenPath = TEXT("/oauth/token");
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Auth")
	FString ClientId;
	/** Development only in assets. Supply production credentials at runtime. Never shipped as a protected secret. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Auth", meta = (PasswordField = "true"))
	FString ClientSecret;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Auth")
	FString Scope;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Auth")
	EVaRestOAuthClientAuthMethod ClientAuthMethod = EVaRestOAuthClientAuthMethod::Basic;
	/** Must include a protectable object key; numeric-only pointers cannot protect a token under a root array. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Auth")
	FString AccessTokenPath = TEXT("access_token");
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Auth")
	FString ExpiresInPath = TEXT("expires_in");
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Auth")
	TArray<int32> RefreshStatuses = {401};
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Auth", meta = (ClampMin = "0"))
	float RefreshLeadSeconds = 30.f;

	/** Metadata only. It never applies an endpoint's normal headers, auth, body or retry policy to a grant. */
	FName TraceEndpoint;

	/** No provider/log/request. Asset authoring may defer credentials; runtime providers always require them. */
	bool Validate(FString& OutError, bool bRequireCredentials = true) const;
};
