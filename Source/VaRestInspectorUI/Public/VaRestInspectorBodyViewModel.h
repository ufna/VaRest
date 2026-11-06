// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Tasks/Task.h"
#include "VaRestJson.h"
#include "VaRestTrace.h"

struct FVaRestInspectorBodySource
{
	TArray<uint8> Bytes;
	EVaRestExcerptKind Kind = EVaRestExcerptKind::Text;
	int64 TotalBytes = -1;
	bool bTruncated = false, bRedacted = false, bAvailable = false;
	bool bBinaryCaptured = false;
	bool operator==(const FVaRestInspectorBodySource&) const = default;
};
struct FVaRestInspectorBodyTreeNode
{
	FText Label;
	FString Path;
	FVaRestJson Value;
	TArray<TSharedPtr<FVaRestInspectorBodyTreeNode>> Children;
	int32 Depth = 0;
	bool bLoaded = false;
};

/** GTK presentation owner; workers own only capped immutable captured bytes and native JSON/text results. */
class VARESTINSPECTORUI_API FVaRestInspectorBodyViewModel final : public TSharedFromThis<FVaRestInspectorBodyViewModel>
{
public:
	static TSharedPtr<FVaRestInspectorBodyViewModel> Create();
	static void ShutdownAll();
	~FVaRestInspectorBodyViewModel();
	void Shutdown();
	bool SetSource(FVaRestInspectorBodySource Source);
	bool Poll();
	bool IsPending() const { return Job.IsValid() || bNeedsJob; }
	bool IsPrettyJson() const { return bPrettyJson; }
	const FText& Raw() const { return RawText; }
	const FText& Pretty() const { return PrettyText; }
	const FText& Hex() const { return HexText; }
	const FText& Notice() const { return LimitText; }
	const TArray<TSharedPtr<FVaRestInspectorBodyTreeNode>>& Roots() const { return Tree; }
	void Children(TSharedPtr<FVaRestInspectorBodyTreeNode> Parent, TArray<TSharedPtr<FVaRestInspectorBodyTreeNode>>& Out);
private:
	struct FParsed { FString Pretty; FVaRestJson Root; };
	FVaRestInspectorBodyViewModel() = default;
	void Start();
	TSharedPtr<FVaRestInspectorBodyTreeNode> Node(FVaRestJson Value, FString Name, FString Path, int32 Depth);
	FVaRestInspectorBodySource Current;
	UE::Tasks::TTask<FParsed> Job;
	FText RawText, PrettyText, HexText, LimitText;
	TArray<TSharedPtr<FVaRestInspectorBodyTreeNode>> Tree;
	uint64 Generation = 0, JobGeneration = 0;
	int32 TreeNodes = 0;
	int64 TreeBytes = 0;
	bool bNeedsJob = false, bStopped = false;
	bool bPrettyJson = false;
};
