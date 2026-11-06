// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Tasks/Task.h"
#include "Templates/ValueOrError.h"

using FVaRestInspectorExportResult = TValueOrError<FString, FString>;
using FVaRestInspectorExportTask = UE::Tasks::TTask<FVaRestInspectorExportResult>;

struct FVaRestInspectorExportOptions
{
	TOptional<uint64> ScopeId;
	/** HTTP identities. Both identity arrays empty means every retained record in ScopeId. */
	TArray<uint64> RequestIds;
	/** Socket identities use a separate namespace, even when their numeric ids equal request ids. */
	TArray<uint64> SocketIds;
	/** Restrict to the union of both identity arrays, including an explicitly empty selection. */
	bool bRestrictRequestIds = false;
	bool bIncludeSockets = true;
	/** Logical DTO bytes, separate from the charged capture snapshot. Bounded to 1..64 MiB. */
	int32 MaxCopyMiB = 32;
};
