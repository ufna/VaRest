// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "VaRestOpenApiPlan.h"
class UVaRestApi;

struct FVaRestOpenApiMergeItem
{
	FString Kind,Id,Owner,Target;
	FVaRestJson Properties;
	/** Live-only allocation context; never importer ownership or serialized base. */
	TArray<FString> ReservedTargets;
	bool bPresent=true;
	bool bRemovedUpstream=false;
};
enum class EVaRestOpenApiChange : uint8 { Added,Changed,Removed,Restore,Conflict,KeptUserEdit,Suppressed };
struct FVaRestOpenApiPropertyChange
{
	FString Kind,Id,Owner,Property;
	FVaRestJson Base,Mine,Theirs;
	EVaRestOpenApiChange Change=EVaRestOpenApiChange::Changed;
	bool bBreaking=false,bApply=false,bDeferred=false,bSuppress=false;
};
struct FVaRestOpenApiMergeOptions
{
	bool bAutomated=false,bAllowBreaking=false;
	TArray<FString> SuppressedIds;
};
struct FVaRestOpenApiMergePlan
{
	TArray<FVaRestOpenApiMergeItem> Items;
	TArray<FVaRestOpenApiPropertyChange> Changes;
	int32 Deferred=0,Conflicts=0;
	/** Kept user edits are informational; they do not justify dirtying a package or rewriting its manifest. */
	bool HasWrites() const;
	/** Call after changing preview checkboxes; recomputes output from the captured live state. */
	void ResolveChoices(TConstArrayView<FVaRestOpenApiMergeItem> Mine,TConstArrayView<FVaRestOpenApiMergeItem> Theirs);
	/** Advance declined/applied bases to upstream; keep deferred breaking property bases for the next review. */
	TArray<FVaRestOpenApiMergeItem> NextBase(TConstArrayView<FVaRestOpenApiMergeItem> Previous,TConstArrayView<FVaRestOpenApiMergeItem> Theirs) const;
};
struct VARESTOPENAPI_API FVaRestOpenApiMerge final
{
	/** Snapshots only items owned by the manifest; policies and unrelated user items are never scanned into ownership. */
	static TArray<FVaRestOpenApiMergeItem> Capture(UVaRestApi* Api,const FVaRestJson& Manifest);
	static TArray<FVaRestOpenApiMergeItem> Desired(const FVaRestOpenApiPlan& Plan,const FVaRestJson& Manifest,TConstArrayView<FVaRestOpenApiMergeItem> Live={});
	static TArray<FVaRestOpenApiMergeItem> Base(const FVaRestJson& Manifest);
	/** No live objects, mutation, IO or GUID allocation. Input arrays and output rows retain source order. */
	static FVaRestOpenApiMergePlan Compare(TConstArrayView<FVaRestOpenApiMergeItem> Base,TConstArrayView<FVaRestOpenApiMergeItem> Mine,TConstArrayView<FVaRestOpenApiMergeItem> Theirs,const FVaRestOpenApiMergeOptions& Options={});
	static FVaRestJson ItemsJson(TConstArrayView<FVaRestOpenApiMergeItem> Items);
};
