// Copyright 2016 Vladimir Alyamkin. All Rights Reserved.

#pragma once

/** Verb (GET, PUT, POST) used by the request */
UENUM(BlueprintType)
enum class ERequestVerb : uint8
{
	GET,
	POST,
	PUT,
	DEL UMETA(DisplayName = "DELETE"),
	/** Set CUSTOM verb by SetCustomVerb() function */
	CUSTOM
};

// clang-format off
/** Content type (json, urlencoded, etc.) used by the request */
UENUM(BlueprintType)
enum class ERequestContentType : uint8
{
	x_www_form_urlencoded_url	UMETA(DisplayName = "x-www-form-urlencoded (URL)"),
	x_www_form_urlencoded_body	UMETA(DisplayName = "x-www-form-urlencoded (Request Body)"),
	json,
	binary
};
// clang-format on

/** Enumerates the current state of an Http request */
UENUM(BlueprintType)
enum class ERequestStatus : uint8
{
	/** Has not been started via ProcessRequest() */
	NotStarted,
	/** Currently being ticked and processed */
	Processing,
	/** Finished but failed */
	Failed,
	/** Failed because it was unable to connect (safe to retry) */
	Failed_ConnectionError,
	/** Finished and was successful */
	Succeeded
};
