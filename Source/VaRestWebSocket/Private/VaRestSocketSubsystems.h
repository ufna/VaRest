// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/EngineSubsystem.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "VaRestSocketSubsystems.generated.h"

class UVaRestSocketHub;

UCLASS()
class UVaRestSocketGameInstanceSubsystem final : public UGameInstanceSubsystem
{
	GENERATED_BODY()
public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	UVaRestSocketHub* GetHub() const { return Hub; }
private:
	UPROPERTY(Transient) TObjectPtr<UVaRestSocketHub> Hub;
};

UCLASS()
class UVaRestSocketEngineSubsystem final : public UEngineSubsystem
{
	GENERATED_BODY()
public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	UVaRestSocketHub* GetHub() const { return Hub; }
private:
	UPROPERTY(Transient) TObjectPtr<UVaRestSocketHub> Hub;
};
