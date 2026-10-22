// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestBody.h"
#include "VaRestHttpTypes.h"
#include "VaRestJsonRedact.h"
#include "VaRestTrace.h"

/**
 * A client's known secret values (Inspector 2.2 layer 3, IN-13): the current value per slot (bearer and refresh tokens,
 * the API key, Basic credentials, session headers) and, after a rotation, the previous values for PreviousLifetime
 * seconds, at most MaxPrevious of them (Inspector D21). Game thread; the client copies GetActive() into each redactor it
 * builds, so a published redactor never changes (copy-on-write).
 */
class VARESTHTTP_API FVaRestSecretSet
{
public:
	/** Shorter values are not scrubbed: they would match ordinary text */
	static constexpr int32 MinBytes = 8;
	static constexpr int32 MaxPrevious = 4;
	static constexpr double PreviousLifetime = 600.0;

	/** Sets Slot's value; the value it replaces stays known until Now + PreviousLifetime. An empty Value only retires the old one. */
	void Set(FName Slot, const FString& Value, double Now);
	/** Logout: forgets every value, the previous ones included (Inspector 12: "cleared on logout") */
	void ClearAll();
	/** Forgets the current and the previous values of every slot Matches accepts (a logout forgets the session's, plan decision 17) */
	void ClearSlots(TFunctionRef<bool(FName Slot)> Matches);
	/** The values to scrub at Now: current ones and unexpired previous ones of at least MinBytes UTF-8 bytes, longest first */
	TArray<FString> GetActive(double Now) const;
	/**
	 * Expiries are doubles and clocks round (the virtual clock to whole nanoseconds), so a timer armed for ExpiresAt can
	 * fire a hair before it: a value within ExpirySlack of Now counts as expired in NextExpiry and Prune (reviews N-2,
	 * N-I3). The expiry timer also prunes at the deadline it was armed for (Task 14); this is the second guard.
	 */
	static constexpr double ExpirySlack = 1e-6;

	/**
	 * When the next previous value still pending at Now expires; negative when none will. Values already expired at Now
	 * do not count: a timer re-armed for one would be due at once, forever (review C1).
	 */
	double NextExpiry(double Now) const;
	/** Forgets the previous values expired at Now (within ExpirySlack) */
	void Prune(double Now);
	/** Moves every pending expiry by Delta seconds: the client's clock changed, and a new clock counts from its own zero */
	void Rebase(double Delta);

private:
	struct FPrevious
	{
		FString Value;
		double ExpiresAt = 0.0;
		/** The slot it was the value of: ClearSlots forgets it with its slot */
		FName Slot;
	};

	TMap<FName, FString> Current;
	TArray<FPrevious> Previous;
};

/**
 * The core redactor (Inspector 2.2, IN-13; WebSocket spec 19 item 3): URLs, headers and body excerpts, for logs and traces
 * alike. Immutable once built: a client publishes a new one when its rules or secrets change, and any thread may use a
 * published one. Bodies are never redacted on the game thread (Inspector 7): RedactBody and Describe* run on a background
 * task (VaRest::Http::LaunchRedaction). What stays uncovered, as documented there: a secret that is not a known value and
 * sits in a path segment, in a text body under no key, or in opt-in binary; and a known value shorter than
 * FVaRestSecretSet::MinBytes, which is never scrubbed (a 7-character key or PIN).
 */
class VARESTHTTP_API FVaRestRedactor
{
public:
	FVaRestRedactor() = default;
	FVaRestRedactor(FVaRestRedactRules InRules, TArray<FString> InSecrets);

	const FVaRestRedactRules& GetRules() const { return Rules; }
	/** The secrets as passed to the constructor (not the expanded wire forms) */
	const TArray<FString>& GetSecrets() const { return InputSecrets; }
	/** The UTF-8 size of the longest form scrubbed (0 without secrets): how far past a cap a scrub may need to read */
	int32 GetMaxNeedleBytes() const { return MaxNeedleBytes; }

	/** Userinfo becomes ***; values of secret query and fragment keys become *** (keys matched percent-decoded); then the scrub */
	FString RedactUrl(FStringView Url) const;

	/**
	 * *** for a secret header name; Location, Content-Location and Referer are redacted as URLs, so are the target of Refresh
	 * ("5; url=...") and every <target> of Link; every other value is scrubbed
	 */
	FString RedactHeaderValue(FStringView Name, FStringView Value) const;

	/** Every header through RedactHeaderValue; names and order kept */
	FVaRestHeaders RedactHeaders(const FVaRestHeaders& In) const;

	/** Every known secret value replaced by *** */
	FString ScrubText(FStringView Text) const;

	/**
	 * An excerpt built from at most Cap bytes (a negative Cap is 0) of a body whose first bytes are Prefix and whose size is TotalBytes. JSON (by type,
	 * or sniffed: any text type or none, and '{' or '[' first) through VaRest::Json::RedactPrefix; a form pair by pair,
	 * dropping a pair the cap may have cut; other text up to the cap, cut at a character boundary; anything else a
	 * descriptor (size, type, and the SHA-256 prefix when Prefix is the whole body). Then the value scrub, and, for a cut
	 * body, the tail that begins a known secret is dropped. The output is not bounded by Cap: a short value becomes ***,
	 * and invalid UTF-8 is repaired with U+FFFD (3 bytes each).
	 */
	FVaRestTraceExcerpt RedactBody(FStringView ContentType, TConstArrayView<uint8> Prefix, int64 TotalBytes, int32 Cap) const;

	/**
	 * A multipart request body (a negative Cap is 0): one line per part; a text part's value by its name's rules, then
	 * scrubbed; binary as a descriptor. A text part is read up to Cap + GetMaxNeedleBytes(); one cut there, or given with
	 * fewer Bytes than its Size (a caller's capped copy), loses the tail that begins a known secret, and the excerpt is
	 * truncated.
	 */
	FVaRestTraceExcerpt DescribeMultipart(TConstArrayView<FVaRestMultipartPart> Parts, int64 TotalBytes, int32 Cap) const;

	/** A streamed file upload: its path (relative to the project when inside it), size and type */
	FVaRestTraceExcerpt DescribeStream(FStringView FilePath, FStringView ContentType, int64 TotalBytes) const;

private:
	FString RedactUrlUncounted(FStringView Url) const;
	void AppendRedactedPairs(FStringView Pairs, EVaRestRedactKeyKind Kind, FString& Out) const;
	bool ScrubInPlace(FString& Text) const;
	bool ScrubUtf8(TArray<uint8>& Utf8) const;
	/** Drops the tail of a capped excerpt that begins a known secret; true when it dropped anything */
	bool TrimNeedleTail(TArray<uint8>& Utf8) const;
	/**
	 * The index of the "=" whose value is a secret, else INDEX_NONE: the first candidate key that Rules names, where a
	 * candidate starts at the front of Piece (judged as Kind) or after a '?', ';' or '#' (judged as a Query key, whatever Kind is)
	 */
	int32 SecretValueStart(FStringView Piece, EVaRestRedactKeyKind Kind) const;
	void AppendRedactedPath(FStringView Path, FString& Out) const;

	FVaRestRedactRules Rules;
	/** The secrets as passed in */
	TArray<FString> InputSecrets;
	/** What is scrubbed, longest first: each secret of at least MinBytes and its wire forms (percent, form, lower-case hex, JSON-escaped) */
	TArray<FString> Secrets;
	TArray<TArray<uint8>> SecretsUtf8;
	int32 MaxNeedleBytes = 0;
};
