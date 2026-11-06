// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestJsonDocument.h"

namespace VaRest::Json
{
/** Actual document ownership and builder scratch observation; excludes RapidJSON/allocator internals and RSS. */
struct FJsonParseUsage
{
	int64 Values = 0;
	int64 WorkUnits = 0;
	SIZE_T ArenaUsedBytes = 0;
	SIZE_T ArenaReservedBytes = 0;
	SIZE_T InputAllocatedBytes = 0;
	SIZE_T BuilderScratchPeakAllocatedBytes = 0;
};

/**
 * Parses Doc.Input (NUL-terminated) in situ from StartOffset into Doc.Root.
 *
 * In situ so that every parsed string points into the document-owned buffer: ZERO per-string copies.
 * This is not for SIMD: kParseValidateEncodingFlag disables RapidJSON's SIMD string scanner
 * (rapidjson/reader.h:1006), and a SIMD whitespace skipper exists for MemoryStream too (reader.h:514-515).
 * A raw NUL byte ends RapidJSON's iterative loop (reader.h:2199), so ParseInSitu rejects any NUL before
 * the end of the input.
 *
 * OriginalForDiagnostics: the unmodified input, used only to compute line/column of an error
 * (in-situ unescaping rewrites the buffer). Empty -> Line/Column stay 0.
 * OutConsumed: when non-null, stop after the first complete value and report how many bytes it took.
 */
bool ParseInSitu(FDocument& Doc, int64 StartOffset, const FVaRestJsonParseOptions& Options,
	FUtf8StringView OriginalForDiagnostics, FVaRestJsonError& OutError, int64* OutConsumed = nullptr,
	FJsonParseUsage* OutUsage = nullptr);

/** Fills Line/Column of Error.Offset from the unmodified input. */
void FillLineColumn(FVaRestJsonError& Error, FUtf8StringView Original);

/** 3 when Bytes starts with the UTF-8 BOM, else 0 */
int64 BomLength(const uint8* Bytes, int64 Num);
} // namespace VaRest::Json
