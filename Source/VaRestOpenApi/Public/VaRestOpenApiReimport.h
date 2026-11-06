// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "VaRestOpenApiMerge.h"
#include "VaRestOpenApiApply.h"

struct FVaRestOpenApiReimportPreview
{
	bool bValid=false;
	FVaRestJson Manifest;
	TArray<FVaRestOpenApiMergeItem> Base,Mine,Theirs;
	FVaRestOpenApiMergePlan Merge;
	/** Registry referencer closure for preview/checkout cost; live compiler side effects still require native validation. */
	TArray<FName> AffectedPackages;
	TArray<FVaRestOpenApiIssue> Issues;
};
struct FVaRestOpenApiReimportResult : FVaRestOpenApiApplyResult
{
	bool bNoChanges=false;
	TArray<FName> ChangedPackages;
};
struct VARESTOPENAPI_API FVaRestOpenApiReimporter final
{
	static FVaRestOpenApiReimportPreview Preview(UVaRestApi* Api,const FVaRestOpenApiPlan& Plan,const FVaRestOpenApiMergeOptions& Options={});
	/** Seed before planning, so operation and inline type identity survive upstream renames. */
	static bool SeedOptions(UVaRestApi* Api,FVaRestOpenApiPlanOptions& Options);
	/** Checks the preview snapshot again before any mutation. Cancellation ends when existing assets start changing. */
	static FVaRestOpenApiReimportResult Apply(UVaRestApi* Api,const FVaRestOpenApiPlan& Plan,FVaRestOpenApiReimportPreview& Preview,const FVaRestOpenApiApplyOptions& Options={});
	/** File source orchestration. Automated calls apply only non-conflicting/non-breaking changes by default. */
	static FVaRestOpenApiReimportResult FromSource(UVaRestApi* Api,bool bAutomated=false,bool bAllowBreaking=false,bool bCheckOnly=false);
};
