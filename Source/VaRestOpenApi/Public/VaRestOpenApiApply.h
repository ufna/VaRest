// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "VaRestOpenApiPlan.h"

class UVaRestApi;

struct FVaRestOpenApiApplyOptions
{
	/** Factory-provided primary package; must not already contain an object with ApiName. */
	UObject* Parent=nullptr;
	FName ApiName;
	FString TypesFolder;
	EObjectFlags Flags=RF_Public|RF_Standalone|RF_Transactional;
	/** The factory registers the returned objects itself; direct scripting uses this default. */
	bool bRegisterAssets=true,bAddToSettings=true;
	/** Checked only before commit. No cancellation may interrupt publication. GT callback. */
	TFunction<bool()> IsCancelled;
	/** Source/options/report carried into the manifest; no source IO is performed by Apply. */
	FVaRestJson Source,Options;
};

struct FVaRestOpenApiApplyResult
{
	UVaRestApi* Api=nullptr;
	TArray<UObject*> AdditionalObjects;
	TArray<FVaRestOpenApiIssue> Issues;
	bool bCancelled=false,bUndoHistoryCleared=false;
};

/** GT only. New import application; existing assets go through the reimport merge, never replacement. */
struct VARESTOPENAPI_API FVaRestOpenApiApplier final
{
	static FVaRestOpenApiApplyResult Apply(const FVaRestOpenApiPlan& Plan,const FVaRestOpenApiApplyOptions& Options);
};
