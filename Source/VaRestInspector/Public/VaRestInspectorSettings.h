// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "VaRestInspectorSettings.generated.h"

USTRUCT()
struct VARESTINSPECTOR_API FVaRestInspectorLimits
{
	GENERATED_BODY()

	UPROPERTY(config, EditAnywhere, Category = "Limits")
	int32 BudgetMiB = 64;

	UPROPERTY(config, EditAnywhere, Category = "Limits")
	int32 MaxRecords = 10000;

	UPROPERTY(config, EditAnywhere, Category = "Limits")
	int32 MaxBodyBytes = 65536;

	UPROPERTY(config, EditAnywhere, Category = "Limits")
	int32 MaxBinaryExcerptBytes = 0;

	/** Used by I3/I4; inert in I1. */
	UPROPERTY(config, EditAnywhere, Category = "Limits")
	int32 MaxFrameExcerptBytes = 4096;

	/** Used by I3/I4; inert in I1. */
	UPROPERTY(config, EditAnywhere, Category = "Limits")
	int32 MaxFramesPerSocket = 5000;

	UPROPERTY(config, EditAnywhere, Category = "Limits")
	int32 MaxPendingEvents = 65536;

	UPROPERTY(config, EditAnywhere, Category = "Limits")
	int32 MaxPendingMiB = 16;
};

UCLASS(config = Game, defaultconfig, meta = (DisplayName = "VaRest Inspector"))
class VARESTINSPECTOR_API UVaRestInspectorSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UVaRestInspectorSettings(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	UPROPERTY(config, EditAnywhere, Category = "Capture")
	bool bCaptureInEditor = true;

	UPROPERTY(config, EditAnywhere, Category = "Capture")
	bool bCaptureInPackaged = false;

	UPROPERTY(config, EditAnywhere, Category = "Capture")
	bool bCaptureHeaders = true;

	UPROPERTY(config, EditAnywhere, Category = "Capture")
	bool bCaptureBodies = true;

	UPROPERTY(config, EditAnywhere, Category = "Limits")
	FVaRestInspectorLimits Editor;

	UPROPERTY(config, EditAnywhere, Category = "Limits")
	FVaRestInspectorLimits Packaged;

	/** Used by I3/I4; inert in I1. */
	UPROPERTY(config, EditAnywhere, Category = "Frames")
	bool bHideEngineIoFrames = true;
};
