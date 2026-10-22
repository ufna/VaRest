// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"

DECLARE_LOG_CATEGORY_EXTERN(LogVaRestHttp, Log, All);

namespace VaRest::Http
{
	/**
	 * Text for one log line (task 14 review, Critical 1). Every control character but TAB (C0, DEL, C1) and the line and
	 * paragraph separators U+2028/U+2029 become visible escapes (\r, \n, \xNN, \uNNNN), and a backslash becomes \\, so a
	 * value a server, a user or a stage wrote can neither end the line (UE writes a message as one block plus one
	 * terminator, OutputDeviceHelper.cpp:117-138) nor pass for an escape. Apply it after redaction: it never touches '*'.
	 */
	FString EscapeForLog(FStringView Text);

	/**
	 * EscapeForLog without the backslash rule (bead VaRest-3dj): for text that may already hold EscapeForLog's escapes, such
	 * as a Config error message (Resolve escapes the verb into it, because a game may log FVaRestError::Message as it is).
	 * A control character is still escaped, so the line cannot be broken; a literal backslash shows once.
	 */
	FString EscapeControlsForLog(FStringView Text);
}
