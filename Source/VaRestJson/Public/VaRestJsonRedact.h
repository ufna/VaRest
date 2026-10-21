// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestJson.h"

/** Where a key comes from: the exact-name rules differ per kind (Inspector spec 2.2, layer 1) */
enum class EVaRestRedactKeyKind : uint8
{
	Json,
	Form,
	Header,
	Query,
	/** A multipart part name */
	Part
};

/**
 * Which keys hold secrets (Inspector spec 2.2 layer 1 and IN-13; they replace core spec 4.5's lists). A key is normalised --
 * ASCII letters lower-cased, '_', '-' and '.' removed -- and is redacted when it equals an added key; otherwise, unless it
 * is exactly idempotencykey or tokentype, when it equals sig or key (and, for a query key, code), or contains password,
 * passwd, secret, token, apikey, authorization, credential, signature, session, cookie, privatekey or jwt. That covers
 * VaRest's own camelCase output (accessToken, clientSecret), presigned URLs (X-Amz-Signature, X-Goog-Signature, Azure sig)
 * and the usual headers (Authorization, Cookie, Set-Cookie, X-Api-Key, X-CSRF-Token). Build it, then share it: reading is
 * thread-safe, AddKey is not.
 */
class VARESTJSON_API FVaRestRedactRules
{
public:
	/** An exact key, normalised like every key: a client's auth token path or header name, a project addition */
	void AddKey(FStringView Key);

	bool ShouldRedact(FUtf8StringView Key, EVaRestRedactKeyKind Kind) const;
	bool ShouldRedact(FStringView Key, EVaRestRedactKeyKind Kind) const;

	/** ASCII letters lower-cased, '_', '-' and '.' dropped; every other byte kept */
	static void Normalize(FUtf8StringView Key, TArray<UTF8CHAR, TInlineAllocator<64>>& Out);

private:
	bool MatchesNormalized(FUtf8StringView Normalized, EVaRestRedactKeyKind Kind) const;

	TArray<TArray<UTF8CHAR>> AddedKeys;
};

enum class EVaRestRedactStop : uint8
{
	/** The whole input was read */
	Complete,
	/** The input ended mid-token: it is a prefix cut by a size cap */
	Cap,
	/** Invalid JSON (or nesting past VaRest::Json::MaxDepth): the output ends at the last complete token */
	ParseError
};

struct FVaRestRedactResult
{
	bool bRedacted = false;
	EVaRestRedactStop StopReason = EVaRestRedactStop::Complete;
	/** ParseError: what was wrong (RapidJSON's English message) */
	FString Error;
	/** ParseError: the byte offset into the input, else -1 */
	int64 ErrorOffset = -1;
};

namespace VaRest::Json
{
	/**
	 * Re-writes the JSON text Prefix into Out (appended) with every secret's value replaced by "***" (IN-13). Prefix is
	 * the first bytes of a body that may continue past it: a token the end cuts is never written, nor a number that ends
	 * exactly at the end (it may be the front of a longer one), and a secret's value becomes "***" whether it is complete,
	 * cut or nested (Inspector spec 2.2). bWholeInput says Prefix is the whole body, so a final number is complete.
	 * Numbers are copied as written. A parse error ends the output at the last complete token, with the error in the
	 * result. Any thread; the output is a JSON prefix, not always a document (containers the cut left open stay open).
	 */
	VARESTJSON_API FVaRestRedactResult RedactPrefix(FUtf8StringView Prefix, const FVaRestRedactRules& Rules, EVaRestJsonFormat Format,
		TArray<uint8>& Out, bool bWholeInput = false);
}
