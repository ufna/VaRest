// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "VaRestHttpTypes.h"

#include "VaRestHttpSettings.generated.h"

/**
 * Project defaults for every FVaRestClient (spec 5.5), read when a client is made. M3 holds what M3 reads; the rest of
 * spec 5.5 (retry, naming, cache and queue directories) arrives with M5. v1's UVaRestSettings stays in module VaRest.
 */
UCLASS(config = Engine, defaultconfig, meta = (DisplayName = "VaRest HTTP"))
class VARESTHTTP_API UVaRestHttpSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UVaRestHttpSettings(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/** Default log level of every client (spec 4.5); the cvar VaRest.LogLevel overrides it at runtime */
	UPROPERTY(config, EditAnywhere, Category = "Logging")
	EVaRestLogLevel LogLevel = EVaRestLogLevel::Basic;

	/**
	 * Extra keys whose values are redacted in logs and traces, on top of the built-in rules (Inspector IN-13). Matched
	 * exactly after normalisation: lower case, '_', '-' and '.' removed (so player_pin also covers playerPin). Read when a
	 * client is made (FVaRestClient::Make): an edit in the editor reaches clients made afterwards (review M20).
	 */
	UPROPERTY(config, EditAnywhere, Category = "Logging")
	TArray<FString> RedactedKeys;

	/** Requests one client runs at once; the rest wait in a priority queue (spec 4.6) */
	UPROPERTY(config, EditAnywhere, Category = "Requests", meta = (ClampMin = "1"))
	int32 MaxConcurrent = 16;

	/** Seconds for a whole attempt; 0 = the engine's default ([HTTP] HttpTotalTimeout, 0 = none) */
	UPROPERTY(config, EditAnywhere, Category = "Requests", meta = (ClampMin = "0"))
	float Timeout = 0.f;

	/**
	 * Seconds without any transfer before an attempt fails. 0 = the total timeout when there is one (decision 30: the
	 * engine's own 30 s idle default would otherwise fire first, as a connection error), else the engine's default
	 * ([HTTP] HttpActivityTimeout, 30).
	 */
	UPROPERTY(config, EditAnywhere, Category = "Requests", meta = (ClampMin = "0"))
	float ActivityTimeout = 0.f;

	/**
	 * Largest response body parsed as JSON, in bytes; 0 = no limit. The arena DOM is about 4.2x its input (M2 final
	 * review), so this bounds a decode's memory: 32 MiB of JSON is about 135 MB of DOM. The HTTP transport stops a larger
	 * body as it arrives (Kind=Decode, nothing kept; an error response stays Kind=Http without its body); a transport that
	 * does not, fails it before anything is parsed (FVaRestJsonParseOptions::MaxInputBytes). Per client:
	 * FVaRestClient::WithMaxJsonBytes; per request: MaxJsonBytes().
	 */
	UPROPERTY(config, EditAnywhere, Category = "Requests", meta = (ClampMin = "0"))
	int64 MaxJsonBytes = 32 * 1024 * 1024;
};
