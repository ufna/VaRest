// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "VaRestRequestJSON.h"
#include "VaRestLegacyHttpTestTypes.generated.h"

/** Only weak request observations; the Hub/legacy subsystem own pending calls. */
UCLASS(HideDropdown,NotBlueprintable)
class UVaRestLegacyHttpTestProbe : public UObject
{
 GENERATED_BODY()
public:
 int32 Completes=0,Failures=0,CallCompletions=0,ResendAt=-1;
 int32 LatentContinuations=0,LastLatentLinkage=INDEX_NONE;
 bool bOnGT=true,bFanout=false;
 FString Root;
 TArray<TWeakObjectPtr<UVaRestRequestJSON>> CallRequests;
 UFUNCTION() void Completed(UVaRestRequestJSON* Request);
 UFUNCTION() void Failed(UVaRestRequestJSON* Request);
 UFUNCTION() void CallCompleted(UVaRestRequestJSON* Request);
 UFUNCTION() void LatentDone(int32 Linkage);
};
