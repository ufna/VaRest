// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "VaRestOpenApiReimport.h"
#include "VaRestOpenApiApplyPrivate.h"
#include "VaRestOpenApiReimportValues.h"

namespace VaRest::OpenApi
{
class FReimport
{
public:
	FReimport(UVaRestApi& InApi,const FVaRestOpenApiPlan& InPlan,FVaRestOpenApiReimportPreview& InPreview,const FVaRestOpenApiApplyOptions& InOptions)
		:Api(InApi),FullPlan(InPlan),Preview(InPreview),Options(InOptions),Stage(NewPlan,Options),Root(&InApi) {}
	FVaRestOpenApiReimportResult Run();
private:
	bool Current() const;
	void Prepare();
	void ExistingTypes();
	FVaRestJson Suppressions() const;
	FVaRestJson Deferrals() const;
	void RecordActualBases(TArray<FVaRestOpenApiMergeItem>& Bases);
	void CleanupFailed();
	UVaRestApi& Api;
	const FVaRestOpenApiPlan& FullPlan;
	FVaRestOpenApiReimportPreview& Preview;
	FVaRestOpenApiApplyOptions Options;
	FVaRestOpenApiPlan NewPlan;
	FApply Stage;
	TStrongObjectPtr<UVaRestApi> Root;
	TArray<TStrongObjectPtr<UObject>> ExistingRoots;
	TArray<FVaRestOpenApiMergeItem*> All;
	TStringMap<FVaRestOpenApiMergeItem*> Items;
	TStringMap<TArray<FVaRestOpenApiMergeItem*>> Children;
	TStringMap<const FVaRestOpenApiMergeItem*> Live;
};
}
