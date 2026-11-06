// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestTraceExcerpts.h"

#include "VaRestClientState.h"
#include "VaRestHttpStats.h"
#include "VaRestHttpTesting.h"
#include "VaRestRedactionPipe.h"
#include "VaRestResultHandler.h"
#include "VaRestTransport.h"

#include <atomic>

namespace VaRest::Http::ExcerptsPrivate
{
	/** Trivially destructible: safe as a static (review I-4) */
	std::atomic<int64> Pending{ 0 };
	std::atomic<int32> PendingFrameJobs{ 0 };
	inline constexpr int32 MaxFrameJobs = 4096;

	bool Wanted()
	{
#if VAREST_WITH_TRACE_EXCERPTS
		return FVaRestTrace::Wants(EVaRestTraceDetail::Bodies);
#else
		return false;
#endif
	}

	void Emit(EVaRestTraceExcerptOwner Owner, uint64 RequestId, int32 Attempt, uint64 DispatchId, FVaRestTraceExcerpt&& Excerpt, double Time, uint64 SocketId = 0, uint64 FrameId = 0)
	{
		FVaRestTraceExcerptEvent Payload;
		Payload.Owner = Owner;
		Payload.RequestId = RequestId;
		Payload.Attempt = Attempt;
		Payload.DispatchId = DispatchId; Payload.SocketId = SocketId; Payload.FrameId = FrameId;
		Payload.Excerpt = MoveTemp(Excerpt);
		FVaRestTrace::Emit(EVaRestTraceKind::Excerpt, Time, FVaRestTracePayload(TInPlaceType<FVaRestTraceExcerptEvent>(), MoveTemp(Payload)), Owner == EVaRestTraceExcerptOwner::Frame ? EVaRestTraceSource::Socket : EVaRestTraceSource::Http);
	}

#if !UE_BUILD_SHIPPING
	/** What the trace copied for excerpts on the game thread, after its admission (tests: Task 21 review M-3) */
	void CountCopied(int64 Bytes)
	{
		Testing::Counters().TraceExcerptBytesCopied += Bytes;
	}
#else
	void CountCopied(int64) {}
#endif

	/** Admit before copying; binary bytes require their own explicit cap. Default oversized binary costs no body copy. */
	Excerpts::FJob WithCopy(Excerpts::FJob&& Job, TConstArrayView<uint8> Bytes, int32 Cap, FString ContentType, int32 BinaryCap = 0)
	{
		const auto Probe = Bytes.Left(FMath::Min(Bytes.Num(), FMath::Max(0, FMath::Max(Cap, BinaryCap))));
		const bool bBinary = FVaRestRedactor::IsBinaryBody(ContentType, Probe);
		const int32 Take = FMath::Min(Bytes.Num(), FMath::Max(0, bBinary && BinaryCap > 0 ? BinaryCap : Cap));
		if (Take < Bytes.Num() && bBinary && BinaryCap <= 0)
		{
			// M4a final review A7 (review C M-4): a binary body over the cap is described by its size and type alone -- the
			// SHA-256 prefix needs the whole body -- so nothing is copied on the game thread and nothing is admitted (an image
			// or an asset bundle with the Inspector capturing bodies). A body within the cap keeps its copy, which the SHA reads.
			Job.Redact = [Type = MoveTemp(ContentType), Total = Job.TotalBytes, Cap](const FVaRestRedactor& Redactor)
			{
				return Redactor.RedactBody(Type, TConstArrayView<uint8>(), Total, Cap);
			};
			return MoveTemp(Job);
		}
		// A longer binary probe can discover untyped JSON beyond the text cap's whitespace. Preserve that classification
		// without copying past the text cap merely to repeat the sniff on the worker.
		if (!bBinary && FVaRestRedactor::IsBinaryBody(ContentType, Bytes.Left(Take))) ContentType = TEXT("application/json");
		TOptional<Excerpts::FAdmission> Admission = Excerpts::TryAdmit(Take);
		if (!Admission.IsSet())
		{
			Job.bBacklog = true;
			return MoveTemp(Job);
		}
		Job.Admitted = MoveTemp(Admission.GetValue());
		CountCopied(Take);
		Job.Redact = [Prefix = TArray<uint8>(Bytes.GetData(), Take), Type = MoveTemp(ContentType), Total = Job.TotalBytes, Cap, BinaryCap](const FVaRestRedactor& Redactor)
		{
			return Redactor.RedactBody(Type, Prefix, Total, Cap, BinaryCap);
		};
		return MoveTemp(Job);
	}

	/**
	 * Game thread. Launcher is the core pipe's LaunchRedaction, or a test's own gate. A refused launch (the pipe is stopped
	 * or stopping: engine exit, a test's gate) drops the body, and the FAdmission it holds with it; the excerpt never comes,
	 * which the Inspector shows as "excerpt timed out" (its 2.3).
	 */
	void LaunchJob(Excerpts::FJob& Job, TFunctionRef<bool(const TCHAR*, TUniqueFunction<void()>&&)> Launcher)
	{
		if (!Job.IsSet())
		{
			return;
		}
		if (!Wanted())
		{
			// The sink that asked detached while the announcing event was emitted (its OnEvent removed it): nobody waits for
			// this excerpt. The copy goes, and its admission with it.
			Job.Admitted.Reset();
			Job.Redact = nullptr;
			return;
		}
		if (Job.bBacklog)
		{
			FVaRestTraceExcerpt Skipped;
			Skipped.Kind = EVaRestExcerptKind::Skipped;
			Skipped.SkipReason = EVaRestExcerptSkip::Backlog;
			Skipped.TotalBytes = Job.TotalBytes;
			Skipped.bTruncated = Job.TotalBytes > 0;
			Emit(Job.Owner, Job.RequestId, Job.Attempt, Job.DispatchId, MoveTemp(Skipped), Job.Time, Job.SocketId, Job.FrameId);
			return;
		}
		Launcher(TEXT("VaRest.TraceExcerpt"), [ForOwner = Job.Owner, Id = Job.RequestId, ForAttempt = Job.Attempt, ForDispatch = Job.DispatchId, Held = MoveTemp(Job.Admitted), At = Job.Time,
			Redacting = Job.Redactor, Work = MoveTemp(Job.Redact), SocketId = Job.SocketId, FrameId = Job.FrameId, FrameSlot = MoveTemp(Job.FrameAdmission)]() mutable
		{
			LLM_SCOPE_BYTAG(VaRest_Trace);
			if (!Wanted() || !Redacting.IsValid())
			{
				// Every sink that wanted bodies detached while this waited on the pipe: no redaction for nobody
				Held.Reset();
				return;
			}
			FVaRestTraceExcerpt Excerpt = Work(*Redacting);
			// Counted out before the emit, here rather than when the task body dies, so a test that waits for the pipe sees it
			Held.Reset(); FrameSlot.Reset();
			Emit(ForOwner, Id, ForAttempt, ForDispatch, MoveTemp(Excerpt), At, SocketId, FrameId);
		});
	}
}

namespace VaRest::Http::Excerpts
{
	struct FSocketFrameAdmission
	{
		~FSocketFrameAdmission() { ExcerptsPrivate::PendingFrameJobs.fetch_sub(1, std::memory_order_acq_rel); }
		static TSharedPtr<FSocketFrameAdmission, ESPMode::ThreadSafe> TryAcquire()
		{
			auto& Counter = ExcerptsPrivate::PendingFrameJobs;
			int32 Previous = Counter.load(std::memory_order_acquire);
			while (Previous < ExcerptsPrivate::MaxFrameJobs)
				if (Counter.compare_exchange_weak(Previous, Previous + 1, std::memory_order_acq_rel))
					return MakeShared<FSocketFrameAdmission, ESPMode::ThreadSafe>();
			return {};
		}
	};
}

namespace VaRest::Http
{
	struct FSocketFrameCaptureData
	{
		double Time = 0.0;
		FVaRestTraceSocketFrame Frame;
		Excerpts::FJob Job;
		TOptional<FVaRestTraceExcerpt> Immediate;
	};
}
FVaRestSocketFrameCapture::FVaRestSocketFrameCapture() = default;
FVaRestSocketFrameCapture::~FVaRestSocketFrameCapture() = default;
FVaRestSocketFrameCapture::FVaRestSocketFrameCapture(FVaRestSocketFrameCapture&& Other) = default;
FVaRestSocketFrameCapture& FVaRestSocketFrameCapture::operator=(FVaRestSocketFrameCapture&& Other) = default;
void FVaRestSocketFrameCapture::Publish()
{
	check(IsInGameThread());
	auto Owned = MoveTemp(Data); if (!Owned || FVaRestTrace::Detail() == EVaRestTraceDetail::None) return;
	const uint64 SocketId = Owned->Frame.SocketId, FrameId = Owned->Frame.FrameId;
	FVaRestTrace::Emit(EVaRestTraceKind::SocketFrame, Owned->Time, FVaRestTracePayload(TInPlaceType<FVaRestTraceSocketFrame>(), MoveTemp(Owned->Frame)), EVaRestTraceSource::Socket);
	if (Owned->Immediate.IsSet() && VaRest::Http::ExcerptsPrivate::Wanted())
		VaRest::Http::ExcerptsPrivate::Emit(EVaRestTraceExcerptOwner::Frame, 0, 0, 0, MoveTemp(Owned->Immediate.GetValue()), Owned->Time, SocketId, FrameId);
	Owned->Job.Launch();
}
void FVaRestTrace::EmitSocketFrame(double Time, FVaRestTraceSocketFrame&& Frame, TConstArrayView<uint8> Bytes,
	TSharedPtr<const FVaRestRedactor, ESPMode::ThreadSafe> Redactor)
{
	auto Capture = PrepareSocketFrame(Time, MoveTemp(Frame), Bytes, MoveTemp(Redactor)); Capture.Publish();
}

FVaRestSocketFrameCapture FVaRestTrace::PrepareSocketFrame(double Time, FVaRestTraceSocketFrame&& InFrame, TConstArrayView<uint8> Bytes,
	TSharedPtr<const FVaRestRedactor, ESPMode::ThreadSafe> Redactor, int32 JsonOffset, bool bRedactAllJsonValues)
{
	check(IsInGameThread() && InFrame.Dir == EVaRestTraceSocketDirection::Out);
	if (Detail() == EVaRestTraceDetail::None) return {};
	FVaRestSocketFrameCapture Capture; Capture.Data = MakeUnique<VaRest::Http::FSocketFrameCaptureData>();
	Capture.Data->Frame = MoveTemp(InFrame); Capture.Data->Time = Time;
	auto& Frame = Capture.Data->Frame;
	FScopedCycles Cycles;
	if (!Redactor) Redactor = MakeShared<const FVaRestRedactor, ESPMode::ThreadSafe>();
	if (bRedactAllJsonValues)
	{
		auto Rules = Redactor->GetRules(); Rules.RedactAllJsonValues();
		Redactor = MakeShared<const FVaRestRedactor, ESPMode::ThreadSafe>(MoveTemp(Rules), Redactor->GetSecrets());
	}
	Frame.Bytes = Bytes.Num(); Frame.AttachmentSizes.SetNum(FMath::Min(10, Frame.AttachmentSizes.Num()));
	const bool bJsonSuffix = JsonOffset >= 0 && Frame.Kind == EVaRestTraceSocketFrameKind::Text;
	if (bJsonSuffix) Bytes = Bytes.RightChop(FMath::Min(JsonOffset, Bytes.Num()));
	Frame.Channel = Redactor->ScrubText(Frame.Channel); Frame.Route = Redactor->ScrubText(Frame.Route); Frame.MsgId = Redactor->ScrubText(Frame.MsgId);
	const uint64 SocketId = Frame.SocketId, FrameId = Frame.FrameId;
	auto& Job = Capture.Data->Job;
	auto& Immediate = Capture.Data->Immediate;
#if VAREST_WITH_TRACE_EXCERPTS
	if (VaRest::Http::ExcerptsPrivate::Wanted())
	{
		const auto Settings = GetDetailSettings();
		const int32 Cap = FMath::Max(0, Settings.FrameCap);
		if (Cap > 0)
		{
			const bool bBinary = Frame.Kind == EVaRestTraceSocketFrameKind::Binary;
			const int32 EffectiveCap = bBinary ? FMath::Min(Cap, FMath::Max(0, Settings.BinaryCap)) : Cap;
			if (Bytes.IsEmpty() || (bBinary && EffectiveCap == 0))
			{
				// The descriptor needs no source bytes, redaction work or task slot. In particular a zero-byte frame cannot
				// accumulate an unlimited queue of zero-charge redaction jobs.
				FVaRestTraceExcerpt Description; Description.TotalBytes = Bytes.Num();
				Description.Kind = bBinary ? EVaRestExcerptKind::Binary : EVaRestExcerptKind::Text;
				Description.bBinaryCaptured = bBinary && EffectiveCap > 0;
				if (bBinary && !Description.bBinaryCaptured)
				{
					const FString Label = FString::Printf(TEXT("<binary: %lld bytes, application/octet-stream>"), static_cast<long long>(Bytes.Num()));
					const FTCHARToUTF8 Utf8(*Label, Label.Len());
					Description.Bytes = MakeShared<const TArray<uint8>, ESPMode::ThreadSafe>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
				}
				Immediate = MoveTemp(Description);
			}
			else
			{
				Job.Owner = EVaRestTraceExcerptOwner::Frame; Job.SocketId = SocketId; Job.FrameId = FrameId;
				Job.Time = Time; Job.TotalBytes = Bytes.Num(); Job.Redactor = Redactor;
				Job.FrameAdmission = VaRest::Http::Excerpts::FSocketFrameAdmission::TryAcquire();
				if (!Job.FrameAdmission) Job.bBacklog = true;
				else Job = VaRest::Http::ExcerptsPrivate::WithCopy(MoveTemp(Job), Bytes, EffectiveCap, bBinary ? FString(TEXT("application/octet-stream")) : (bJsonSuffix ? FString(TEXT("application/json")) : FString(TEXT("text/plain"))), bBinary ? EffectiveCap : 0);
			}
		}
	}
#endif
	Frame.bExcerptPending = Job.IsSet() || Immediate.IsSet();
	return Capture;
}

int64 VaRest::Http::Excerpts::AdmissionCap()
{
	// Merge already mapped each sink's 0 to the default (task 5 review minor 2); 0 here means no sink at all
	const int64 Asked = FVaRestTrace::GetDetailSettings().MaxPendingExcerptBytes;
	return Asked > 0 ? Asked : FVaRestTraceDetailSettings::DefaultMaxPendingExcerptBytes;
}

int64 VaRest::Http::Excerpts::PendingBytes()
{
	return ExcerptsPrivate::Pending.load();
}

TOptional<VaRest::Http::Excerpts::FAdmission> VaRest::Http::Excerpts::TryAdmit(int64 InBytes)
{
	using namespace ExcerptsPrivate;
	const int64 Cap = AdmissionCap();
	int64 Seen = Pending.load();
	do
	{
		if (Seen + InBytes > Cap)
		{
			return TOptional<FAdmission>();
		}
	}
	while (!Pending.compare_exchange_weak(Seen, Seen + InBytes));
	return TOptional<FAdmission>(FAdmission(InBytes));
}

void VaRest::Http::Excerpts::FAdmission::Reset()
{
	if (Bytes > 0)
	{
		ExcerptsPrivate::Pending.fetch_sub(Bytes);
		Bytes = 0;
	}
}

#if WITH_DEV_AUTOMATION_TESTS
VaRest::Http::Excerpts::FScopedResponseGate* VaRest::Http::Excerpts::FScopedResponseGate::Current = nullptr;

VaRest::Http::Excerpts::FScopedResponseGate::FScopedResponseGate(uint64 Request, uint64 Dispatch, FRedactionGate& InGate)
	: Previous(Current), RequestId(Request), DispatchId(Dispatch), Gate(&InGate)
{
	check(IsInGameThread() && RequestId != 0 && DispatchId != 0);
	Current = this;
}

VaRest::Http::Excerpts::FScopedResponseGate::~FScopedResponseGate()
{
	check(IsInGameThread() && Current == this);
	Current = Previous;
}
#endif

void VaRest::Http::Excerpts::FJob::Launch()
{
#if WITH_DEV_AUTOMATION_TESTS
	check(IsInGameThread());
	const FScopedResponseGate* Route = FScopedResponseGate::Current;
	if (Route && Owner == EVaRestTraceExcerptOwner::Response
		&& RequestId == Route->RequestId && DispatchId == Route->DispatchId)
	{
		LaunchOn(*Route->Gate);
		return;
	}
#endif
	ExcerptsPrivate::LaunchJob(*this, [](const TCHAR* DebugName, TUniqueFunction<void()>&& Body)
	{
		return LaunchRedaction(DebugName, MoveTemp(Body));
	});
}

#if WITH_DEV_AUTOMATION_TESTS
void VaRest::Http::Excerpts::FJob::LaunchOn(FRedactionGate& Gate)
{
	ExcerptsPrivate::LaunchJob(*this, [&Gate](const TCHAR* DebugName, TUniqueFunction<void()>&& Body)
	{
		return Gate.Launch(DebugName, MoveTemp(Body));
	});
}
#endif

VaRest::Http::Excerpts::FJob VaRest::Http::Excerpts::PrepareRequest(const FExchangeState& Ex)
{
	using namespace ExcerptsPrivate;
	FJob Job;
    if (!Ex.QueueReplayOf.QueueId.IsEmpty()) return Job;
	if (!Wanted() || Ex.Body.IsEmpty())
	{
		return Job;
	}
	LLM_SCOPE_BYTAG(VaRest_Trace);
	const FVaRestTraceDetailSettings Detail = FVaRestTrace::GetDetailSettings();
	Job.Owner = EVaRestTraceExcerptOwner::Request;
	Job.RequestId = Ex.RequestId;
	Job.TotalBytes = Ex.Body.ContentLength;
	Job.Time = Ex.Config->Clock->Now();
	// Plan decision 17, review I-1: the Send() snapshot's secrets and the client's current ones (a game-thread ref)
	Job.Redactor = RedactorFor(Ex);
	const FVaRestRequestBody& Body = Ex.Body;
	if (!Body.FilePath.IsEmpty())
	{
		Job.Redact = [Path = Body.FilePath, Type = Body.ContentType, Total = Body.ContentLength](const FVaRestRedactor& Redactor)
		{
			return Redactor.DescribeStream(Path, Type, Total);
		};
		return Job;
	}
	if (Body.Kind == EVaRestBodyKind::Multipart)
	{
		// Task 21 review M-3: sized, admitted, then copied -- a refused admission copies nothing, as WithCopy. The copy is the
		// log's (CopyPartsForLog, VaRestLogStage.cpp:83-88): DescribeMultipart's reach, a cut field keeping its Size.
		const int64 Budget = static_cast<int64>(Detail.BodyCap) + Job.Redactor->GetMaxNeedleBytes();
		const int64 Take = PartBytesWithin(Body.PartDescriptors, Budget);
		TOptional<FAdmission> Admission = TryAdmit(Take);
		if (!Admission.IsSet())
		{
			Job.bBacklog = true;
			return Job;
		}
		Job.Admitted = MoveTemp(Admission.GetValue());
		CountCopied(Take);
		Job.Redact = [Described = CopyPartsWithin(Body.PartDescriptors, Budget), Total = Body.ContentLength, Cap = Detail.BodyCap](const FVaRestRedactor& Redactor)
		{
			return Redactor.DescribeMultipart(Described, Total, Cap);
		};
		return Job;
	}
	// The request's own Content-Type wins (spec 3.2)
	const FString* Type = Ex.RequestHeaders.Find(TEXT("Content-Type"));
	return WithCopy(MoveTemp(Job), Body.Bytes, Detail.BodyCap, Type ? *Type : Body.ContentType, Detail.BinaryCap);
}

bool VaRest::Http::Excerpts::WillExcerptOnDecode(const FExchangeState& Ex, const FVaRestTransportResult& Result)
{
	return ExcerptsPrivate::Wanted() && Result.Failure == EVaRestTransportFailure::None && IsSuccessStatus(Result.Status) && !Result.bBodyInFile
		&& Result.Body.Num() > 0 && Ex.Handler.IsValid() && Ex.Handler->DecodesBody();
}

VaRest::Http::Excerpts::FJob VaRest::Http::Excerpts::PrepareResponse(const FExchangeState& Ex, const FVaRestTransportResult& Result, int32 Attempt, uint64 DispatchId)
{
	using namespace ExcerptsPrivate;
	FJob Job;
	if (!Wanted() || (!Result.bBodyInFile && Result.Body.Num() == 0))
	{
		return Job;
	}
	LLM_SCOPE_BYTAG(VaRest_Trace);
	Job.Owner = EVaRestTraceExcerptOwner::Response;
	Job.RequestId = Ex.RequestId;
	// The dispatch's own attempt (M4a final review A1), as its AttemptEnded carries it
	Job.Attempt = Attempt;
	Job.DispatchId = DispatchId;
	Job.TotalBytes = Result.bBodyInFile ? Result.BytesDown : Result.Body.Num();
	Job.Time = Ex.Config->Clock->Now();
	// Read as the result is back, like the log stage's (VaRestLogStage.cpp:130): with what AuthApply joined in for this
	// attempt (Task 12 review M-7) and a refresh request's token-path keys, which the client's redactor holds (Task 18)
	Job.Redactor = RedactorFor(Ex);
	if (Result.bBodyInFile)
	{
		// Inspector 2.2: a streamed body is a descriptor; its path is the destination the game named
		Job.Redact = [Path = Ex.SaveTo, Type = Result.ContentType, Total = Job.TotalBytes](const FVaRestRedactor& Redactor)
		{
			return Redactor.DescribeStream(Path, Type, Total);
		};
		return Job;
	}
	const auto Detail = FVaRestTrace::GetDetailSettings();
	return WithCopy(MoveTemp(Job), Result.Body, Detail.BodyCap, Result.ContentType, Detail.BinaryCap);
}

void VaRest::Http::Excerpts::EmitOnDecode(const FFinish& Finish)
{
	// Task 21 review M-2: Wanted() re-checked here, as the pipe's task does (LaunchJob): every sink that wanted bodies may
	// have detached since AttemptEnded announced it, and then nobody waits for this excerpt
	if (Finish.ExcerptAttempt <= 0 || !Finish.ExcerptRedactor.IsValid() || Finish.Body.Num() == 0 || !ExcerptsPrivate::Wanted())
	{
		return;
	}
	LLM_SCOPE_BYTAG(VaRest_Trace);
	// The body itself, before ParseOwned unescapes it in place (VaRestDecode.cpp:115) and the envelope reads the result:
	// IN-14's zero copies. The redactor's output is the excerpt's own.
	const int32 Take = FMath::Min(Finish.Body.Num(), FMath::Max(0, FMath::Max(Finish.ExcerptCap, Finish.ExcerptBinaryCap)));
	FVaRestTraceExcerpt Excerpt = Finish.ExcerptRedactor->RedactBody(Finish.Response.ContentType, TConstArrayView<uint8>(Finish.Body.GetData(), Take), Finish.Body.Num(),
		Finish.ExcerptCap, Finish.ExcerptBinaryCap);
	ExcerptsPrivate::Emit(EVaRestTraceExcerptOwner::Response, Finish.Response.RequestId, Finish.ExcerptAttempt, Finish.ExcerptDispatchId, MoveTemp(Excerpt),
		Finish.Clock.IsValid() ? Finish.Clock->Now() : 0.0);
}

void VaRest::Http::Excerpts::ResolveDiscarded(FExchangeState& Ex)
{
	const auto ManagedControl = Ex.Managed.Control;
	if (ManagedControl && ManagedControl->IsStopped())
		return;
	const int32 Attempt = Ex.ExcerptOnDecodeAttempt;
	const uint64 DispatchId = Ex.ExcerptOnDecodeDispatchId;
	if (Attempt <= 0)
	{
		return;
	}
	// Once: whatever follows, this attempt's excerpt is resolved
	Ex.ExcerptOnDecodeAttempt = 0;
	Ex.ExcerptOnDecodeDispatchId = 0;
	if (!ExcerptsPrivate::Wanted())
	{
		return;
	}
	LLM_SCOPE_BYTAG(VaRest_Trace);
	FVaRestTraceExcerpt Skipped;
	Skipped.Kind = EVaRestExcerptKind::Skipped;
	Skipped.SkipReason = EVaRestExcerptSkip::Discarded;
	Skipped.TotalBytes = Ex.ExcerptOnDecodeBytes;
	Skipped.bTruncated = Skipped.TotalBytes > 0;
	const double At = Ex.Config->Clock->Now();
	if (ManagedControl && ManagedControl->IsStopped())
		return;
	ExcerptsPrivate::Emit(EVaRestTraceExcerptOwner::Response, Ex.RequestId, Attempt, DispatchId, MoveTemp(Skipped), At);
}
