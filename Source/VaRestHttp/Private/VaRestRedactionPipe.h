// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Tasks/Pipe.h"

namespace VaRest::Http
{
	/**
	 * Core's redaction pipe (Inspector 7): body excerpts for logs (and M4's trace excerpts) are redacted there, in order,
	 * never on the game thread. Null before StartupModule and after ShutdownModule; callers skip body excerpts then.
	 * Game thread only (it checks): StopRedactionPipe deletes the pipe, so a launch from another thread could race the
	 * delete (review M-10). M4's decode-task excerpts must make it an atomic with a shutdown flag first.
	 */
	UE::Tasks::FPipe* GetRedactionPipe();
	void StartRedactionPipe();
	/** Waits for the queued work, then deletes the pipe: FPipe's destructor checks it is idle (Pipe.h:40-43) */
	void StopRedactionPipe();
}
