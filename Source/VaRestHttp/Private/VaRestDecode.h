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
	 * A temp file name next to Destination, unique per call and per process (task 10 review I1): a re-issue under one
	 * attempt number, a second process downloading to the same destination, or a .part a crashed run left behind never
	 * shares it, so deleting a stale name can never hit a live download.
	 */
	FString MakeTempFileName(const FString& Destination, uint64 RequestId);

	/** The file operations MoveIntoPlace uses: the platform's (PlatformFileOps), or a test's */
	struct FFileOps
	{
		TFunction<bool(const FString& To, const FString& From)> Move;
		TFunction<bool(const FString& Path)> Delete;
		/** A regular file: a directory is never moved aside */
		TFunction<bool(const FString& Path)> IsFile;
	};

	/** IPlatformFile's MoveFile, DeleteFile and FileExists */
	FFileOps PlatformFileOps();

	/**
	 * Moves From onto To, replacing a file there; true when To now holds From's file. bMoveRefusesTarget false (every
	 * platform but Windows): MoveFile is rename(), which replaces atomically (UnixPlatformFile.cpp:1092,
	 * ApplePlatformFile.cpp:587, IOSPlatformFile.cpp:662, AndroidPlatformFile.cpp:1960), so the one move decides and a
	 * failure is real. True (Windows: MoveFileW without MOVEFILE_REPLACE_EXISTING refuses an existing target,
	 * WindowsPlatformFile.cpp:1218): the old file is moved aside to a unique .bak, From is moved in, and the .bak is deleted
	 * only then; when the second move fails the .bak is moved back (task 10 review I2: a delete first could lose the old
	 * file to a locked or delete-pending target and still fail). On false To holds what it held, unless even moving the
	 * .bak back failed: OutStranded then names where the old file is. A crash between the two Windows renames leaves the
	 * old file only as the .bak.
	 */
	bool MoveIntoPlace(const FString& To, const FString& From, bool bMoveRefusesTarget, const FFileOps& Ops, FString& OutStranded);

	/**
	 * SaveTo's success (spec 3.2): the temp file moved onto the destination by MoveIntoPlace (PLATFORM_WINDOWS picks the
	 * move-aside path). The transport's file is used only when it wrote one (bBodyInFile): an outcome a stage made (a cache
	 * hit, a fallback after an attempt) has its body in memory, and that goes to a fresh temp file, never an earlier
	 * attempt's (task 10 review M1). A transport file that is missing becomes an empty file only when no byte arrived
	 * (an empty body never opens the sink); after bytes arrived it is a failure, never an empty success (review I1). False,
	 * with a Decode error and the temp file deleted, when a write or the move fails. IsCanceled is read right before the
	 * move (review M-4): when it is true the temp file is deleted, the destination stays untouched, and false comes back
	 * with no error (Complete then delivers Canceled). A cancel after the move is too late: the request delivers its
	 * success. Background thread.
	 */
	bool FinalizeSaveTo(FFinish& Finish, TFunctionRef<bool()> IsCanceled);

	/** The first RawBodyCap bytes of a file (an error body that went to SaveTo's temp file) */
	void LoadFilePrefix(const FString& Path, TArray<uint8>& Out);
}
