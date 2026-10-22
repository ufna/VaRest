// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestHttpTypes.h"
#include "VaRestJson.h"

#include "VaRestEnvelope.generated.h"

/** What FVaRestEnvelope::Evaluate found (spec 3.4; WebSocket spec 19 item 6) */
struct VARESTHTTP_API FVaRestEnvelopeResult
{
	/** The predicate held, or there is none */
	bool bSuccess = true;
	/** DataPath's value (the whole body when DataPath is empty); an invalid handle when it is missing or the predicate failed */
	FVaRestJson Data;
	/** False when DataPath names nothing in the body */
	bool bDataFound = true;
	/** A failure's ErrorCodePath and ErrorMessagePath values, as text; empty when absent */
	FString Code;
	FString Message;
	/** A failed predicate: its path, the values that pass joined with '|', and the value found ("(missing)" for none; at most 256 characters) */
	FString FailedPath;
	FString Expected;
	FString Actual;

	/** Kind=Server, not retryable: Code, Message (else a fallback naming the predicate), HttpStatus, and Body as the error body */
	FVaRestError ToError(const FVaRestJson& Body, int32 HttpStatus) const;
};

/**
 * Spec 3.4: where a response's data and its success flag are. Per client (WithEnvelope), replaced per request (Envelope(),
 * NoEnvelope()); M5's asset holds one per API and per endpoint, so it is BlueprintType and its field names freeze then
 * (plan decision 27). Applied to FVaRestJson, struct and void results (plan decision 9); raw results get the body as it came.
 * Paths are FVaRestJson::Path's: "data", "result.items[0]", or a JSON pointer ("/a.b/c"). SuccessValues compare as text,
 * case-sensitive (ruling Q3; MatchText): a string by its content; true and false as "true" and "false"; an integral number
 * as its decimal with no fraction and no exponent (200, 200.0 and 2e2 are all "200"); any other number as its shortest
 * round-trip text ("1.5"); null, an object or an array never match. So {"200"} accepts 200 and "200", and {"true"}
 * accepts true and "true": one envelope works for servers that send the same flag as a string or as a number.
 */
USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestEnvelope
{
	GENERATED_BODY()

	/** Where the result is; empty = the whole body */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	FString DataPath;

	/** A value that says success ("ok", "Code"); empty = no predicate: every 2xx is a success */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	FString SuccessPath;

	/** SuccessPath's value must be one of these, as MatchText renders it ("true", "200", "OK"); a 2xx whose value is not is Kind=Server */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	TArray<FString> SuccessValues;

	/** FVaRestError::Message of a Kind=Server failure, and of a 4xx/5xx with bParseErrorBodies */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	FString ErrorMessagePath;

	/** FVaRestError::Code, the same way */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	FString ErrorCodePath;

	/** Read ErrorMessagePath and ErrorCodePath from 4xx and 5xx JSON bodies too (spec 3.4) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	bool bParseErrorBodies = false;

	static FVaRestEnvelope DataAt(FStringView Path);
	FVaRestEnvelope SuccessWhen(FStringView Path, TArray<FString> Values) const;
	FVaRestEnvelope ErrorMessageAt(FStringView Path) const;
	FVaRestEnvelope ErrorCodeAt(FStringView Path) const;
	FVaRestEnvelope ParseErrorBodies(bool bParse = true) const;

	bool HasPredicate() const { return !SuccessPath.IsEmpty(); }

	/** WebSocket spec 19 item 6: Body through this envelope. Any thread: it only reads Body (spec 2.2's thread contract). */
	FVaRestEnvelopeResult Evaluate(const FVaRestJson& Body) const;

	/** ErrorCodePath's and ErrorMessagePath's values in Body, as text; empty when absent */
	void ReadErrorFields(const FVaRestJson& Body, FString& OutCode, FString& OutMessage) const;

	/**
	 * Ruling Q3: the text SuccessValues are compared with, case-sensitive. Unset never matches: an invalid handle, null, an
	 * object, an array.
	 */
	static TOptional<FString> MatchText(const FVaRestJson& Value);

	/** A value as a message or the trace shows it (IN-5 Actual): a string as itself, anything else as condensed JSON; "" for an invalid handle */
	static FString ValueText(const FVaRestJson& Value);
};
