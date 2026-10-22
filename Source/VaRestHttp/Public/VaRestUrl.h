// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/ValueOrError.h"
#include "VaRestHttpTypes.h"

namespace VaRest::Http
{
	/** RFC 3986: every byte of Text's UTF-8 but the unreserved set (ALPHA DIGIT - . _ ~) becomes %XX, upper-case (spec 3.2) */
	VARESTHTTP_API FString PercentEncode(FStringView Text);

	/** application/x-www-form-urlencoded (WHATWG URL Standard): ALPHA DIGIT * - . _ kept, space as '+', the rest %XX */
	VARESTHTTP_API FString FormEncode(FStringView Text);

	/** %XX back to bytes, read as UTF-8 (invalid sequences become U+FFFD); an invalid escape stays as text */
	VARESTHTTP_API FString PercentDecode(FStringView Text, bool bPlusIsSpace = false);

	/** "http://" or "https://" (any case) followed by a host */
	VARESTHTTP_API bool IsAbsoluteHttpUrl(FStringView Url);

	/**
	 * BaseUrl + Path with exactly one '/' between them (OpenAPI N14: https://h/v1 + /pets is https://h/v1/pets, never
	 * RFC 3986 resolution -- though curl still removes dot-segments from a literal path, which is why ExpandPathTemplate
	 * refuses "." and ".."). An absolute Path replaces BaseUrl; a Path starting with '?' or '#' is appended as is. Both are
	 * trimmed first (v1's iOS fix, spec 3.2). The base's fragment is dropped. After the authority, every non-ASCII byte,
	 * CTL and space is percent-encoded (RFC 3987 3.1), other text -- '%' included -- is left alone.
	 */
	VARESTHTTP_API FString JoinUrl(FStringView BaseUrl, FStringView Path);

	/** True when the host of an "http(s)://host/..." URL holds a non-ASCII character: IDN needs punycode, which is out of scope, so the caller reports Kind=Config */
	VARESTHTTP_API bool HasNonAsciiHost(FStringView Url);

	/**
	 * True when the authority of an "http(s)://..." URL (after the scheme, to the first '/', '?' or '#') holds a CTL
	 * (U+0000-U+001F, U+007F) or a space. JoinUrl copies the authority as it is, so the caller reports Kind=Config.
	 */
	VARESTHTTP_API bool HasControlInAuthority(FStringView Url);

	/**
	 * The host of an "http(s)://..." URL as written: its authority (as HasControlInAuthority reads it) without the userinfo
	 * and the port; an IPv6 literal keeps its brackets. Empty for any other URL. Hosts compare case-insensitively (RFC 3986
	 * 3.2.2): the caller says ESearchCase::IgnoreCase.
	 */
	VARESTHTTP_API FString GetUrlHost(FStringView Url);

	/**
	 * "/users/{id}" with each {name} replaced by PercentEncode(value): one segment, so '/' in a value is %2F. The last
	 * value given for a name wins. Errors (decision 10): a placeholder without a value, an empty value, a value that is "." or "..", a value whose name
	 * the template lacks, '{' without '}', a stray '}', and "{}". An error's message never quotes the template, which may be
	 * an absolute URL with userinfo: a caller that shows it redacts it first (final review M8).
	 */
	VARESTHTTP_API TValueOrError<FString, FString> ExpandPathTemplate(FStringView Template, TConstArrayView<TPair<FString, FString>> Params);

	/** Appends ?k=v&... (or &k=v when Url has a query), each key and value PercentEncode'd, in order, repeats kept (OpenAPI N9), before any '#' */
	VARESTHTTP_API void AppendQuery(FString& Url, TConstArrayView<TPair<FString, FString>> Query);

	/**
	 * OpenAPI N9's array styles (decision 34): adds Values to Query as Style says -- Repeat one pair per value, Csv/Ssv/Pipes
	 * one pair, each value encoded on its own and joined by a literal ',', "%20" or '|' (RFC 6570 form
	 * expansion; a delimiter inside a value stays encoded). Every value after the first is a pair with a one-character
	 * marker key (a lone surrogate) that only AppendQuery turns into the literal; values are never inspected. No values: no pair.
	 */
	VARESTHTTP_API void AddQueryValues(TArray<TPair<FString, FString>>& Query, FStringView Name, TConstArrayView<FString> Values, EVaRestQueryStyle Style);

	/** RFC 9110 5.6.2 token: one or more of ALPHA DIGIT !#$%&'*+-.^_`|~ */
	VARESTHTTP_API bool IsValidHeaderName(FStringView Name);

	/** No CTL but HTAB (RFC 9110 5.5): CR or LF in a value could end its header and start another (header injection) */
	VARESTHTTP_API bool IsValidHeaderValue(FStringView Value);
}
