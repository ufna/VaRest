// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "VaRestOpenApiCommandlet.generated.h"

UCLASS()
class VARESTOPENAPI_API UVaRestOpenApiCommandlet : public UCommandlet
{
	GENERATED_BODY()
public:
	UVaRestOpenApiCommandlet();
	virtual int32 Main(const FString& Params) override;
};
