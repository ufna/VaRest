// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "VaRestGameInstanceSubsystem.generated.h"

class UVaRestHub;
UCLASS()
class VARESTHTTP_API UVaRestGameInstanceSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()
public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	UVaRestHub* GetHub() const { return Hub; }
private:
	UPROPERTY(Transient) TObjectPtr<UVaRestHub> Hub;
};
