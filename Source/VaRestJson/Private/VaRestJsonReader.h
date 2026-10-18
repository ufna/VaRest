// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestJsonDocument.h"

namespace VaRest::Json
{
	/**
	 * Parses Doc.Input (NUL-terminated) in situ from StartOffset into Doc.Root.
	 *
	 * In situ because RapidJSON's SIMD whitespace and string scanners are specialised only for
	 * the null-terminated string streams (rapidjson/reader.h SkipWhitespace overloads), and because
	 * it lets every parsed string point into the buffer with no per-string copy.
	 *
	 * OriginalForDiagnostics: the unmodified input, used only to compute line/column of an error
	 * (in-situ unescaping rewrites the buffer). Empty -> Line/Column stay 0.
	 * OutConsumed: when non-null, stop after the first complete value and report how many bytes it took.
	 */
	bool ParseInSitu(FDocument& Doc, int64 StartOffset, const FVaRestJsonParseOptions& Options,
		FUtf8StringView OriginalForDiagnostics, FVaRestJsonError& OutError, int64* OutConsumed = nullptr);

	/** 3 when Bytes starts with the UTF-8 BOM, else 0 */
	int64 BomLength(const uint8* Bytes, int64 Num);
}
