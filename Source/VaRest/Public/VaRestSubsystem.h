// Copyright 2014-2020 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Subsystems/GameInstanceSubsystem.h"
#include "Subsystems/SubsystemCollection.h"

#include "VaRestSubsystem.generated.h"

class UVaRestSettings;

UCLASS()
class VAREST_API UVaRestSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	UVaRestSubsystem();

	// Begin USubsystem
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	// End USubsystem

	/** Getter for internal settings object to support runtime configuration changes */
	UFUNCTION(BlueprintCallable, Category = "VaRest Subsystem")
	UVaRestSettings* GetSettings() const;

private:
	/** Plugin settings */
	UVaRestSettings* Settings;
};
