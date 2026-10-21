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
	 * RFC 3986 resolution). An absolute Path replaces BaseUrl; a Path starting with '?' or '#' is appended as is. Both are
	 * trimmed first (v1's iOS fix, spec 3.2).
	 */
	VARESTHTTP_API FString JoinUrl(FStringView BaseUrl, FStringView Path);

	/**
	 * "/users/{id}" with each {name} replaced by PercentEncode(value): one segment, so '/' in a value is %2F. The last
	 * value given for a name wins. Errors (decision 10): a placeholder without a value, an empty value, a value whose name
	 * the template lacks, '{' without '}', a stray '}', and "{}".
	 */
	VARESTHTTP_API TValueOrError<FString, FString> ExpandPathTemplate(FStringView Template, TConstArrayView<TPair<FString, FString>> Params);

	/** Appends ?k=v&... (or &k=v when Url has a query), each key and value PercentEncode'd, in order, repeats kept (OpenAPI N9), before any '#' */
	VARESTHTTP_API void AppendQuery(FString& Url, TConstArrayView<TPair<FString, FString>> Query);

	/**
	 * OpenAPI N9's array styles (decision 34): adds Values to Query as Style says -- Repeat one pair per value, Csv/Ssv/Pipes
	 * one pair with the values joined by ',', ' ' or '|' (encoded later, by AppendQuery). No values: no pair.
	 */
	VARESTHTTP_API void AddQueryValues(TArray<TPair<FString, FString>>& Query, FStringView Name, TConstArrayView<FString> Values, EVaRestQueryStyle Style);

	/** RFC 9110 5.6.2 token: one or more of ALPHA DIGIT !#$%&'*+-.^_`|~ */
	VARESTHTTP_API bool IsValidHeaderName(FStringView Name);

	/** No CR, LF or NUL: a value holding one could end its header and start another (header injection) */
	VARESTHTTP_API bool IsValidHeaderValue(FStringView Value);
}
