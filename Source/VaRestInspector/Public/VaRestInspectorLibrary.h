// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "VaRestInspectorLibrary.generated.h"

/** Declarations remain available in Shipping; capture/export implementations become inert. */
UCLASS()
class VARESTINSPECTOR_API UVaRestInspectorLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintPure, Category="VaRest|Inspector") static bool IsInspectorCompiledIn();
	UFUNCTION(BlueprintCallable, Category="VaRest|Inspector") static bool SetInspectorCapture(bool bEnabled);
	UFUNCTION(BlueprintPure, Category="VaRest|Inspector") static bool IsInspectorCapturing();
};
