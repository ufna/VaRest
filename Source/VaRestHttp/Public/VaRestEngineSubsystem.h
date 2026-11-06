// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/EngineSubsystem.h"

#include "VaRestEngineSubsystem.generated.h"

class UVaRestApi;
class UVaRestHub;

/** Startup owner for configured API assets/rules and the independent engine fallback Hub. */
UCLASS()
class VARESTHTTP_API UVaRestEngineSubsystem : public UEngineSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	const TArray<TObjectPtr<UVaRestApi>>& GetLoadedApis() const { return LoadedApis; }
	UVaRestHub* GetHub() const { return Hub; }

private:
	UPROPERTY(Transient)
	TArray<TObjectPtr<UVaRestApi>> LoadedApis;
	UPROPERTY(Transient)
	TObjectPtr<UVaRestHub> Hub;
};
