// Current-v2 consumer proposal only; never copied into the original producer or serialized BP.
#pragma once
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "VaRestOriginalTagRequestConsumerTypes.generated.h"
class UVaRestRequestJSON;

UCLASS()
class UVaRestOriginalTagRequestObserver final : public UObject
{
 GENERATED_BODY()
public:
 TAtomic<UVaRestRequestJSON*> ExpectedRequest{nullptr};
 TAtomic<int32> CompleteCount{0};
 TAtomic<int32> FailureCount{0};
 TAtomic<bool> bOnGT{true};
 TAtomic<bool> bIdentity{true};
 UFUNCTION() void Complete(UVaRestRequestJSON* Request);
 UFUNCTION() void Fail(UVaRestRequestJSON* Request);
};
