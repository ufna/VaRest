// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestTrace.h"

struct FVaRestTransportResult;
class FVaRestRedactor;

namespace VaRest::Http
{
	struct FExchangeState;
	struct FFinish;
	class FRedactionGate;

	/**
	 * Inspector IN-14 (plan decision 20). Nothing here copies or builds anything unless a sink wants Bodies and
	 * VAREST_WITH_TRACE_EXCERPTS is on. LLM tag VaRest_Trace.
	 */
	namespace Excerpts
	{
		/** The sinks' MaxPendingExcerptBytes, else 16 MiB (FVaRestTraceDetailSettings::DefaultMaxPendingExcerptBytes) */
		int64 AdmissionCap();
		/** Bytes of raw copies waiting for redaction now (tests) */
		int64 PendingBytes();

		/**
		 * Concurrency review M-12: bytes counted in by the admission counter, counted out exactly once when this goes --
		 * after the redaction ran on the pipe, when the pipe task finds no sink wanting it, at a refused launch (the pipe is
		 * stopped: the dropped body takes it along), or for a job prepared and never launched -- so no early return can leak
		 * the count. A moved-from one holds nothing and counts nothing out. Move-only; any thread (the counter is atomic).
		 */
		class FAdmission
		{
		public:
			FAdmission() = default;
			FAdmission(FAdmission&& Other)
				: Bytes(Other.Bytes)
			{
				Other.Bytes = 0;
			}
			FAdmission& operator=(FAdmission&& Other)
			{
				if (this != &Other)
				{
					Reset();
					Bytes = Other.Bytes;
					Other.Bytes = 0;
				}
				return *this;
			}
			FAdmission(const FAdmission&) = delete;
			FAdmission& operator=(const FAdmission&) = delete;
			~FAdmission() { Reset(); }

			int64 Num() const { return Bytes; }
			/** Counts the bytes out now (once); a second call, or one on a moved-from object, does nothing */
			void Reset();

		private:
			friend TOptional<FAdmission> TryAdmit(int64 InBytes);
			explicit FAdmission(int64 InBytes)
				: Bytes(InBytes)
			{
			}

			int64 Bytes = 0;
		};

		/** Counts Bytes in when they fit under AdmissionCap(); unset (nothing counted) when they do not */
		TOptional<FAdmission> TryAdmit(int64 Bytes);

		/** An excerpt prepared on the game thread and launched right after the event that announces it (bExcerptPending) */
		struct FJob
		{
			EVaRestTraceExcerptOwner Owner = EVaRestTraceExcerptOwner::Request;
			uint64 RequestId = 0;
			int32 Attempt = 0;
			int64 TotalBytes = 0;
			/** The admission counter's share of this job's copy (concurrency review M-12: released whatever path drops the job) */
			FAdmission Admitted;
			/** The copy would pass the admission cap: Skipped, Backlog, emitted at once */
			bool bBacklog = false;
			/** IVaRestClock seconds of the hop: the excerpt's Time (the clock itself stays on the game thread, review I-1) */
			double Time = 0.0;
			/** RedactorFor(Ex), taken on the game thread: the pipe holds a ref to an immutable redactor, nothing of the exchange */
			TSharedPtr<const FVaRestRedactor, ESPMode::ThreadSafe> Redactor;
			/** Runs on the redaction pipe */
			TUniqueFunction<FVaRestTraceExcerpt(const FVaRestRedactor&)> Redact;

			bool IsSet() const { return bBacklog || static_cast<bool>(Redact); }
			/**
			 * Game thread: the skip at once, or the redaction on the core pipe; nothing for an unset job, nor when no sink
			 * wants Bodies any more (the sink that asked detached while the announcing event was emitted)
			 */
			void Launch();
#if WITH_DEV_AUTOMATION_TESTS
			/** Tests: Launch on a gate the test owns, so a test can stop it without touching the module's (review I-2) */
			void LaunchOn(FRedactionGate& Gate);
#endif
		};

		/** At Started: IN-14's "copied at Send" (after Resolve, before the transport stage can move the bytes) */
		FJob PrepareRequest(const FExchangeState& Ex);
		/** At AttemptEnded: the decode task will excerpt this response from the body itself (a 2xx a decoder parses) */
		bool WillExcerptOnDecode(const FExchangeState& Ex, const FVaRestTransportResult& Result);
		/** At AttemptEnded, every other response: its prefix copied here, redacted on the pipe */
		FJob PrepareResponse(const FExchangeState& Ex, const FVaRestTransportResult& Result);
		/** On the decode task, before the parse rewrites the body in place: no copy (IN-14) */
		void EmitOnDecode(const FFinish& Finish);
	}
}
