// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestHttpStats.h"

#if ENABLE_LOW_LEVEL_MEM_TRACKER
#include "Async/InheritedContext.h"
#endif

#include <atomic>

namespace VaRest::Http
{
	/**
	 * One set of counters; any thread (relaxed atomics). The byte counts are approximate (Task 20 review M-2): they add up
	 * what each attempt's FVaRestTransportResult reports, no wire bytes.
	 */
	struct FHttpCounters
	{
		/** Logical requests that entered a pipeline */
		std::atomic<int64> Requests{ 0 };
		std::atomic<int64> InFlight{ 0 };
		/**
		 * Request bodies, per attempt. On the default path -- no progress delegate, no counting archive -- the engine
		 * transport reports the body's size for every attempt it handed to the engine, even one that failed before a byte
		 * went out, a refused connection say (BytesUp = UploadTotal, VaRestHttpTransport.cpp:165, :429); with one bound, what
		 * it saw sent; 0 for a request it refused before connecting (FailBeforeConnect, :116-125). No headers.
		 */
		std::atomic<int64> BytesUp{ 0 };
		/**
		 * Response bodies as the sink received them (VaRestHttpTransport.cpp:430): decompressed when the server compressed
		 * them (curl's CURLOPT_ACCEPT_ENCODING "", CurlHttp.cpp:1013-1020), no headers
		 */
		std::atomic<int64> BytesDown{ 0 };
		/** Re-issues by the retry stage (a session replay is not one, plan decision 2) */
		std::atomic<int64> Retries{ 0 };
	};

	/**
	 * Spec 4.8's stat VaRest (plan decision 21): VaRest's own counters, process-wide, mirrored to the engine's stats where
	 * STATS is on -- engine stats are compiled out in Shipping and a test cannot read them back. Each call also counts on
	 * the client's own set (FClientState::GetCounters), which a test compares exactly (concurrency review M-13).
	 */
	struct FHttpStats
	{
		static FHttpCounters& Global();

		static void RequestStarted(FHttpCounters& Client);
		static void RequestFinished(FHttpCounters& Client);
		/** One attempt's bytes */
		static void Transferred(FHttpCounters& Client, int64 Up, int64 Down);
		static void Retried(FHttpCounters& Client);
	};

	/**
	 * The LLM tags active where a request was sent, taken in Send() before its own VaRest scope; the request's callbacks
	 * run under them (VAREST_LLM_SCOPE_SENDER). A UE::Tasks task takes its launcher's tags (CaptureInheritedContext when it
	 * is launched, TaskPrivate.h:192; RestoreInheritedContext around its body, :521), so every delivery task VaRest launches
	 * from inside a VaRest scope -- the decode task's VaRest.Deliver, a deferred VaRest.Complete -- would otherwise charge
	 * the game's own callback allocations to VaRest. Holds nothing where LLM is compiled out.
	 */
	struct FLlmSenderTags
	{
#if ENABLE_LOW_LEVEL_MEM_TRACKER
		UE::FLLMActiveTagsCapture Tags = {};
#endif

		/** Any thread: the calling thread's active tags; null ones while LLM is off at runtime (InheritedContext.h:19-27) */
		void Capture()
		{
#if ENABLE_LOW_LEVEL_MEM_TRACKER
			Tags.CaptureActiveTagData();
#endif
		}
	};
}

#if ENABLE_LOW_LEVEL_MEM_TRACKER
/**
 * For the rest of the enclosing scope, the sender's tags are the active ones -- as a task's inherited context restores its
 * launcher's (FLLMActiveTagsScope, InheritedContext.h:31-49). A null tag is pushed as it is (LLM.cpp:6623-6626), and an
 * allocation under it takes its default tag (:5189-5194): the sender's own state. With LLM off at runtime each scope
 * returns at once (LLM.cpp:3782-3785).
 */
#define VAREST_LLM_SCOPE_SENDER(SenderTags) const UE::FLLMActiveTagsScope UE_JOIN(VaRestLlmSender_, __LINE__)((SenderTags).Tags)
#else
#define VAREST_LLM_SCOPE_SENDER(SenderTags)
#endif
