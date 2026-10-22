// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Templates/Function.h"

namespace VaRest::Http
{
	/**
	 * Core's redaction pipe (Inspector 7): body excerpts for logs and traces are redacted there, in order, never on the game
	 * thread. Any thread (plan decision 16): false, and Body dropped, before StartRedactionPipe, after StopRedactionPipe, or
	 * while it stops. Callers never hold the pipe, so none can race its delete (review M-10).
	 */
	bool LaunchRedaction(const TCHAR* DebugName, TUniqueFunction<void()>&& Body);
	/** Game thread (StartupModule) */
	void StartRedactionPipe();
	/**
	 * Game thread (ShutdownModule): refuses new launches, waits for launches already inside LaunchRedaction, then for the
	 * queued work, then deletes the pipe -- FPipe's destructor checks it is idle (Pipe.h:40-43)
	 */
	void StopRedactionPipe();
	/** Tests, game thread (where StopRedactionPipe deletes it): waits until the pipe is idle; false when there is none */
	bool WaitForRedactionPipe();
}
