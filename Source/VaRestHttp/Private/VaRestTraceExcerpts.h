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

#if WITH_DEV_AUTOMATION_TESTS
		/** GT-only scoped route for one real response job; Gate outlives the scope and its launched tasks. */
		class FScopedResponseGate final
		{
		public:
			FScopedResponseGate(uint64 Request, uint64 Dispatch, FRedactionGate& InGate);
			~FScopedResponseGate();
			FScopedResponseGate(const FScopedResponseGate&) = delete;
			FScopedResponseGate& operator=(const FScopedResponseGate&) = delete;
		private:
			friend struct FJob;
			static FScopedResponseGate* Current;
			FScopedResponseGate* Previous = nullptr;
			uint64 RequestId = 0;
			uint64 DispatchId = 0;
			FRedactionGate* Gate = nullptr;
		};
#endif

		struct FSocketFrameAdmission;
		/** An excerpt prepared on the game thread and launched right after the event that announces it (bExcerptPending) */
		struct FJob
		{
			EVaRestTraceExcerptOwner Owner = EVaRestTraceExcerptOwner::Request;
			uint64 RequestId = 0;
			int32 Attempt = 0;
			uint64 DispatchId = 0;
			uint64 SocketId = 0, FrameId = 0;
			/** Socket excerpts also bound task metadata, including small/empty payloads. Pure native lease. */
			TSharedPtr<FSocketFrameAdmission, ESPMode::ThreadSafe> FrameAdmission;
			int64 TotalBytes = 0;
			/** The admission counter's share of this job's copy (concurrency review M-12: released whatever path drops the job) */
			FAdmission Admitted;
			/** The copy would pass the admission cap: Skipped, Backlog, emitted at once */
			bool bBacklog = false;
			/**
			 * IVaRestClock seconds of the hop: the excerpt's Time, read when the job is prepared so the job holds no clock.
			 * Not a threading rule: Now() is callable from any thread (VaRestClock.h:38), and EmitOnDecode reads
			 * Finish.Clock->Now() on the decode task.
			 */
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
		/** At AttemptEnded, every other response of the current dispatch (of Attempt): its prefix copied here, redacted on the pipe */
		FJob PrepareResponse(const FExchangeState& Ex, const FVaRestTransportResult& Result, int32 Attempt, uint64 DispatchId);
		/**
		 * On the decode task, before the parse rewrites the body in place: no copy (IN-14). Nothing when no sink wants
		 * Bodies any more, as the pipe's path (Task 21 review M-2).
		 */
		void EmitOnDecode(const FFinish& Finish);
		/**
		 * Game thread, at AttemptEnded and at Completed, before their event (Task 21 review M-1): an excerpt AttemptEnded
		 * announced for the decode task that OnChainDone never handed over (Ex.ExcerptOnDecodeAttempt still set) resolves
		 * as Skipped: Discarded, once -- so the record can seal (Inspector 2.3). The marker is cleared either way; nothing
		 * is emitted when no sink wants Bodies any more, as at a launch (LaunchJob).
		 */
		void ResolveDiscarded(FExchangeState& Ex);
	}
}
