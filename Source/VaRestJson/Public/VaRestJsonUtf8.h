// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// Public on purpose: VaRest's other modules (WebSocket, Socket.IO) take byte strings too, and spec 2.2's rule for them
// must be this one validator, not a second copy.
namespace VaRest::Json
{
	/** Scratch for one API string converted or repaired on its way into a document */
	using FUtf8Scratch = TArray<UTF8CHAR, TInlineAllocator<256>>;

	/** True when Text is well-formed UTF-8 (U+0000 included) */
	VARESTJSON_API bool IsValidUtf8(FUtf8StringView Text);

	/**
	 * Appends Text with every ill-formed subsequence replaced by one U+FFFD each (The Unicode Standard, ch. 3,
	 * "U+FFFD Substitution of Maximal Subparts", the practice WHATWG decoders follow). Returns the replacements.
	 */
	VARESTJSON_API int32 AppendRepairedUtf8(FUtf8StringView Text, FUtf8Scratch& Out);

	/** UTF-8 length of UTF-16 Text, an unpaired surrogate counted as U+FFFD (3 bytes) */
	VARESTJSON_API int64 Utf8Length(FStringView Text);

	/** Writes exactly Utf8Length(Text) bytes to Dest; returns how many unpaired surrogates became U+FFFD */
	VARESTJSON_API int32 WriteUtf8(FStringView Text, UTF8CHAR* Dest);

	/**
	 * Text as UTF-8 for a document (spec 2.2): converted into Scratch, unpaired surrogates as U+FFFD, one warning when
	 * any was replaced and bWarn is set (lookups pass false: they store nothing). Empty, with an error, past 2 GiB.
	 */
	VARESTJSON_API FUtf8StringView ConvertForDocument(FStringView Text, FUtf8Scratch& Scratch, bool bWarn = true);

	/** Text itself when it is valid UTF-8, else a repaired copy in Scratch and one warning (spec 2.2) */
	VARESTJSON_API FUtf8StringView ValidateForDocument(FUtf8StringView Text, FUtf8Scratch& Scratch);
}
