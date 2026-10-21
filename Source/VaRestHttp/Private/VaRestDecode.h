// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestResultHandler.h"

namespace VaRest::Http
{
	/** Spec 3.3: "RawBody | first 4 KiB of a non-JSON error body" */
	inline constexpr int32 RawBodyCap = 4096;

	/** The first RawBodyCap bytes as text, ill-formed UTF-8 as U+FFFD */
	FString RawBodyText(TConstArrayView<uint8> Bytes);

	/**
	 * A non-2xx body (spec 3.3): parsed into Error.Body when it is JSON (by type, or sniffed when there is none), else its
	 * first 4 KiB in Error.RawBody; the raw bytes stay in Response.Body for diagnostics (spec 3.2). Background thread.
	 */
	void ProcessErrorBody(FFinish& Finish);
}
