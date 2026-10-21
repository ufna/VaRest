// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestResultHandler.h"

namespace VaRest::Http
{
	/** Spec 3.3: "RawBody | first 4 KiB of a non-JSON error body" */
	inline constexpr int32 RawBodyCap = 4096;

	/** The first RawBodyCap bytes as text, ill-formed UTF-8 as U+FFFD */
	FString RawBodyText(TConstArrayView<uint8> Bytes);

	/**
	 * A non-2xx body (spec 3.3): parsed into Error.Body when it is JSON (by type, or sniffed when there is none), else its
	 * first 4 KiB in Error.RawBody; the raw bytes stay in Response.Body for diagnostics (spec 3.2). Background thread.
	 */
	void ProcessErrorBody(FFinish& Finish);

	/**
	 * SaveTo's success (spec 3.2): the temp file renamed onto the destination -- rename() on POSIX, which replaces it
	 * atomically (UnixPlatformFile.cpp:1092); Windows' MoveFileW refuses an existing target (WindowsPlatformFile.cpp:1218),
	 * so there the old file is deleted first, as FFileManagerGeneric::Move does (FileManagerGeneric.cpp:303-323). An empty
	 * body never opened the sink, so it becomes an empty file. False, with a Decode error and the temp file deleted, when
	 * the move fails. IsCanceled is read right before the rename (review M-4): when it is true the temp file is deleted,
	 * the destination stays untouched, and false comes back with no error (Complete then delivers Canceled). A cancel
	 * after the rename is too late: the request delivers its success. Background thread.
	 */
	bool FinalizeSaveTo(FFinish& Finish, TFunctionRef<bool()> IsCanceled);

	/** The first RawBodyCap bytes of a file (an error body that went to SaveTo's temp file) */
	void LoadFilePrefix(const FString& Path, TArray<uint8>& Out);
}
