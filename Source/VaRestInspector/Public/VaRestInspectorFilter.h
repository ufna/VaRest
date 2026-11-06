// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "VaRestInspectorRecords.h"

/** Borrowed only for the synchronous Matches call; UI supplies its cached presentation values. */
struct FVaRestInspectorFilterFields
{
	uint64 Id = 0, ScopeId = 0, Bytes = 0, Attempts = 0;
	int32 Status = 0;
	double Duration = -1;
	FStringView Method, Name, Url, Initiator, Scope, Client, Tag, Searchable;
	bool bContextKnown = false, bError = false, bRunning = false, bCached = false, bQueued = false, bAuth = false;
	bool bSocket = false, bSio = false;
};
/** One bounded grammar for the runtime console and UI: AND, negation, quoted terms and typed predicates. */
class VARESTINSPECTOR_API FVaRestInspectorFilter
{
public:
	static TArray<FString> Tokenize(FStringView Text);
	void SetText(FStringView Text);
	bool Matches(const FVaRestInspectorFilterFields&) const;
	bool Matches(const FVaRestInspectorRecordView&) const;
private:
	TArray<FString> Terms;
};
