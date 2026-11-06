// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "VaRestApiTypes.h"

#include "VaRestApiSocketConfig.generated.h"

/** Protocol-independent asset entry. Concrete WebSocket/Socket.IO modules own transport and message behavior. */
UCLASS(Abstract, BlueprintType, EditInlineNew, DefaultToInstanced)
class VARESTHTTP_API UVaRestApiSocketConfig : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	FName Name;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	FString Description;

	virtual FName GetProtocolTag() const PURE_VIRTUAL(UVaRestApiSocketConfig::GetProtocolTag, return NAME_None;);
	virtual void Validate(FVaRestApiValidationContext& Context) const PURE_VIRTUAL(UVaRestApiSocketConfig::Validate, );
};
