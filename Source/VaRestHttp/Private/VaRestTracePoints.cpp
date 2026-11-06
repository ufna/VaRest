// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestTracePoints.h"
#include "VaRestNativeScope.h"

#include "Tasks/Task.h"
#include "UObject/Class.h"
#include "UObject/Script.h"
#include "UObject/Stack.h"
#include "VaRestClientState.h"
#include "VaRestHttpStats.h"
#include "VaRestTraceExcerpts.h"
#include "VaRestTransport.h"
#include "VaRestUrl.h"

namespace VaRest::Http::TracePointsPrivate
{
	/**
	 * IN-8's name, redacted by where it came from, not by its shape (task 15 review I1). A WithName name is the game's own
	 * text: the value scrub. An unnamed client is named by its current base URL, which Make does not validate, so it goes
	 * through RedactUrl whatever it looks like: userinfo and secret query values come out of any scheme and of a scheme-less
	 * "user:pw@host/x" too (any RFC 3986 scheme, VaRestRedactor.cpp:673-690; scheme-less userinfo, :719-735, review M3;
	 * the query and the fragment, :753-764; Inspector 2.2 layer 5, decision 23).
	 */
	FString DisplayNameOf(const FClientConfig& Config)
	{
		if (!Config.Redactor.IsValid())
		{
			return FString();
		}
		return Config.DisplayName.IsEmpty() ? Config.Redactor->RedactUrl(Config.BaseUrl) : Config.Redactor->ScrubText(Config.DisplayName);
	}

	/**
	 * What Started and a snapshot entry share. Inspector 2.2 layer 3 scrubs "every captured string", so the strings that
	 * are no URL and no header (the verb, the content type) are value-scrubbed too.
	 */
	FVaRestTraceStarted Describe(const FExchangeState& Ex)
	{
		const FClientConfig& Cfg = *Ex.Config;
		// Plan decision 17, review I-1: the Send() snapshot's secrets and the client's current ones
		const TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> Held = RedactorFor(Ex);
		const FVaRestRedactor& Redactor = *Held;
		FVaRestTraceStarted Out;
		Out.RequestId = Ex.RequestId;
		Out.QueueReplayOf = Ex.QueueReplayOf;
		Out.CachePolicy = Ex.CachePolicy.Mode == EVaRestCacheMode::Memory ? TEXT("Memory")
			: Ex.CachePolicy.Mode == EVaRestCacheMode::MemoryAndDisk ? TEXT("MemoryAndDisk") : TEXT("None");
		Out.CacheLookup = Ex.CacheLookup;
		Out.bCacheLookupPending = Ex.bCacheLookupPending;
		if (Ex.bQueueWhenOffline) Out.Flags |= EVaRestTraceRequestFlags::QueueWhenOffline;
		Out.ScopeId = Cfg.ScopeId;
		Out.ClientId = Ex.ClientId;
		Out.Verb = Redactor.ScrubText(Ex.Verb);
		// A request that failed before Resolve finished has no URL: the base and the raw path say what it was
		Out.Url = Redactor.RedactUrl(Ex.Url.IsEmpty() ? JoinUrl(Cfg.BaseUrl, Ex.RawPath) : Ex.Url);
		Out.PathTemplate = Redactor.RedactUrl(Ex.PathTemplate);
		Out.Tag = Ex.Tag;
		Out.Endpoint = Ex.Endpoint;
		Out.Priority = Ex.Priority;
		if (Ex.bIdempotent)
		{
			Out.Flags |= EVaRestTraceRequestFlags::Idempotent;
		}
		if (Ex.AuthMode == EVaRestAuthMode::None)
		{
			Out.Flags |= EVaRestTraceRequestFlags::NoAuth;
		}
		if (Ex.bIsAuthRefresh)
		{
			Out.Flags |= EVaRestTraceRequestFlags::AuthRefresh;
		}
		// Scrubbed once, when Started wrote it into the exchange
		Out.Initiator = Ex.Initiator;
		// The request's own Content-Type wins (spec 3.2); the body's bytes may already have moved into the transport (Task 8),
		// its size and kind stay
		const FString* Type = Ex.RequestHeaders.Find(TEXT("Content-Type"));
		Out.Body.ContentType = Redactor.ScrubText(Type ? *Type : Ex.Body.ContentType);
		Out.Body.Bytes = Ex.Body.ContentLength;
		Out.Body.Kind = Ex.Body.Kind;
		Out.StartUtc = Ex.StartUtc;
		Out.CurrentAttempt = Ex.Attempt;
		Out.StartedAt = Ex.StartedAt;
		return Out;
	}

#if DO_BLUEPRINT_GUARD
	/**
	 * IN-15: the top Blueprint frame when Send ran inside one. The tracker is per thread (a TThreadSingleton, Script.h:471;
	 * TryGet, :479); each FFrame pushes itself on construction and pops on destruction (Stack.h:321, :164-169), so the
	 * stack (GetCurrentScriptStack, Script.h:509-512) holds the frames being executed. Only for a send on the game
	 * thread, inside the call.
	 */
	void ReadScriptFrame(FVaRestTraceInitiator& Who, const FVaRestRedactor& Redactor)
	{
		const FBlueprintContextTracker* Tracker = FBlueprintContextTracker::TryGet();
		if (!Tracker)
		{
			return;
		}
		const TArrayView<const FFrame* const> Stack = Tracker->GetCurrentScriptStack();
		if (Stack.Num() == 0 || !Stack.Last())
		{
			return;
		}
		const FFrame& Top = *Stack.Last();
		if (Top.Object)
		{
			Who.ScriptObject = Redactor.ScrubText(Top.Object->GetPathName());
		}
		if (Top.Node)
		{
			Who.ScriptFunction = Redactor.ScrubText(Top.Node->GetName());
			const uint8* Begin = Top.Node->Script.GetData();
			if (Top.Code && Begin && Top.Code >= Begin && Top.Code <= Begin + Top.Node->Script.Num())
			{
				Who.ScriptCodeOffset = static_cast<int32>(Top.Code - Begin);
			}
		}
	}
#endif

	void EmitOnGameThread(EVaRestTraceKind Kind, double Time, FVaRestTracePayload&& Payload)
	{
		if (IsInGameThread())
		{
			FVaRestTrace::Emit(Kind, Time, MoveTemp(Payload));
			return;
		}
		// Inspector 9: every kind but Excerpt is emitted on the game thread
		UE::Tasks::Launch(TEXT("VaRest.TraceEmit"), [Kind, Time, Pending = MoveTemp(Payload)]() mutable
		{
			FVaRestTrace::Emit(Kind, Time, MoveTemp(Pending));
		}, UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
	}
}

void VaRest::Http::TracePoints::Started(FExchangeState& Ex)
{
	using namespace TracePointsPrivate;
	if (FVaRestTrace::Detail() == EVaRestTraceDetail::None)
	{
		return;
	}
	// Inspector IN-14: what a trace point builds -- its copies of the request -- is the trace's, not the request's
	LLM_SCOPE_BYTAG(VaRest_Trace);
	FVaRestTrace::FScopedCycles Cycles;
	const TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> Held = RedactorFor(Ex);
	const FVaRestRedactor& Redactor = *Held;
	// IN-10: clock reads and the initiator follow the mask
	Ex.StartUtc = FDateTime::UtcNow();
	FVaRestTraceInitiator& Who = Ex.Initiator;
	Who.SourceFile = Redactor.ScrubText(FString(Ex.SourceFile));
	Who.SourceLine = static_cast<int32>(Ex.SourceLine);
	if (Ex.bHasOwner)
	{
		if (const UObject* OwnerObject = Ex.Owner.Get())
		{
			Who.OwnerPath = Redactor.ScrubText(OwnerObject->GetPathName());
		}
	}
#if DO_BLUEPRINT_GUARD
	if (Ex.bSentOnGameThread)
	{
		ReadScriptFrame(Who, Redactor);
	}
#endif
	FVaRestTraceStarted Payload = Describe(Ex);
	if (FVaRestTrace::Wants(EVaRestTraceDetail::Headers))
	{
		Payload.Headers = Redactor.RedactHeaders(Ex.RequestHeaders);
	}
	// IN-14: the copy is made now, before the transport stage can move the bytes (Task 8); the redaction is launched after
	// Started is emitted, so the excerpt's Seq always follows the event that announces it (bExcerptPending). Launched first,
	// a pipe worker could emit the excerpt before this thread emits Started, and a sink would meet an excerpt of a request
	// it has not seen start.
	Excerpts::FJob Job = Excerpts::PrepareRequest(Ex);
	Payload.bExcerptPending = Job.IsSet();
	FVaRestTrace::Emit(EVaRestTraceKind::Started, Ex.StartedAt, FVaRestTracePayload(TInPlaceType<FVaRestTraceStarted>(), MoveTemp(Payload)));
	Job.Launch();
}

void VaRest::Http::TracePoints::AttemptDispatched(const FExchangeState& Ex, const FDispatchTag& Tag)
{
	if (FVaRestTrace::Detail() == EVaRestTraceDetail::None)
	{
		return;
	}
	LLM_SCOPE_BYTAG(VaRest_Trace);
	FVaRestTrace::FScopedCycles Cycles;
	FVaRestTraceAttemptDispatched Payload;
	Payload.RequestId = Ex.RequestId;
	Payload.DispatchId = static_cast<uint64>(Tag.Serial);
	Payload.Attempt = Ex.Attempt;
	Payload.bConditional = Ex.bCacheConditional;
	Payload.AttemptStart = Ex.AttemptStartedAt;
	Payload.LimiterWait = Ex.LimiterWait;
	Payload.GapKind = Ex.NextGap;
	Payload.GapSeconds = Ex.NextGapSeconds;
	// Inspector 3.3 (spec review I-1): a park before the attempt is its Auth phase; a replay's park is its Gap instead
	Payload.AuthWait = Ex.AuthWait;
	Payload.AuthRefreshId = Ex.AuthRefreshId;
	if (FVaRestTrace::Wants(EVaRestTraceDetail::Headers))
	{
		Payload.Headers = RedactorFor(Ex)->RedactHeaders(Ex.AttemptHeaders);
	}
	FVaRestTrace::Emit(EVaRestTraceKind::AttemptDispatched, Ex.Config->Clock->Now(), FVaRestTracePayload(TInPlaceType<FVaRestTraceAttemptDispatched>(), MoveTemp(Payload)));
}

void VaRest::Http::TracePoints::AttemptEnded(FExchangeState& Ex, const FVaRestTransportResult& Result, const FDispatchTag& Tag, bool bCurrent)
{
	if (bCurrent)
	{
		// Task 21 review M-1: an earlier attempt's decode excerpt that never reached the decode task (a custom stage replaced
		// its 2xx and the request was re-issued) resolves before this attempt's event; checked whatever the mask, so the
		// marker never outlives its attempt. M4a final review A1: only at the current dispatch's end -- a stale end
		// arrives while the marker belongs to a later dispatch, whose own end resolves it.
		Excerpts::ResolveDiscarded(Ex);
	}
	if (FVaRestTrace::Detail() == EVaRestTraceDetail::None)
	{
		return;
	}
	LLM_SCOPE_BYTAG(VaRest_Trace);
	FVaRestTrace::FScopedCycles Cycles;
	const TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> Held = RedactorFor(Ex);
	const FVaRestRedactor& Redactor = *Held;
	FVaRestTraceAttemptEnded Payload;
	Payload.RequestId = Ex.RequestId;
	Payload.DispatchId = static_cast<uint64>(Tag.Serial);
	// M4a final review A1 (ruling: relabel, not drop): the dispatch's own attempt, which a stale end no longer shares with
	// the exchange (Retry dispatched the next one), so its AttemptDispatched stays paired with it
	Payload.Attempt = Tag.Attempt;
	Payload.Status = Result.Status;
	if (FVaRestTrace::Wants(EVaRestTraceDetail::Headers))
	{
		Payload.Headers = Redactor.RedactHeaders(Result.Headers);
	}
	Payload.EffectiveUrl = Redactor.RedactUrl(Result.EffectiveUrl);
	// A server's header value: scrubbed as RedactHeaderValue scrubs it in Headers
	Payload.ContentType = Redactor.ScrubText(Result.ContentType);
	Payload.BytesUp = Result.BytesUp;
	Payload.BytesDown = Result.BytesDown;
	Payload.Failure = Result.Failure;
	Payload.FailureDetail = Result.FailureDetail;
	if (Tag.Probe.IsValid())
	{
		// Written on the HTTP thread by the delegates bound for this dispatch (IN-3); -1 when they were not bound. Its own
		// probe, not Ex.Probe, which a later dispatch replaced (A1).
		Payload.UploadDoneAt = Tag.Probe->UploadDoneAt.load();
		Payload.StatusAt = Tag.Probe->StatusAt.load();
	}
	Payload.TransportDoneAt = Result.TransportDoneAt;
	// IN-14: a 2xx a decoder parses is excerpted on the decode task from the body itself, before the in-situ parse (zero
	// copies); any other response has its prefix copied here and redacted on the pipe, launched after the event as in Started.
	// A1: a stale end's response reaches no decoder and is not the request's answer: no excerpt (bExcerptPending false), so
	// one dispatch announces at most one Response excerpt and a stale one never takes the live dispatch's.
	Excerpts::FJob Job;
	if (bCurrent && Excerpts::WillExcerptOnDecode(Ex, Result))
	{
		Ex.ExcerptOnDecodeAttempt = Tag.Attempt;
		Ex.ExcerptOnDecodeDispatchId = static_cast<uint64>(Tag.Serial);
		Ex.ExcerptOnDecodeBytes = Result.Body.Num();
		Payload.bExcerptPending = true;
	}
	else if (bCurrent)
	{
		Job = Excerpts::PrepareResponse(Ex, Result, Tag.Attempt, static_cast<uint64>(Tag.Serial));
		Payload.bExcerptPending = Job.IsSet();
	}
	FVaRestTrace::Emit(EVaRestTraceKind::AttemptEnded, Ex.Config->Clock->Now(), FVaRestTracePayload(TInPlaceType<FVaRestTraceAttemptEnded>(), MoveTemp(Payload)));
	Job.Launch();
}

void VaRest::Http::TracePoints::Completed(FExchangeState& Ex, const FFinish& Finish)
{
	Completed(Ex, Finish, nullptr); // ordinary handler is selected at its original point AFTER the excerpt callout
}

void VaRest::Http::TracePoints::Completed(FExchangeState& Ex, const FFinish& Finish, const IResultHandler* ActualHandler)
{
	const auto ManagedControl = Ex.Managed.Control;
	if (ManagedControl && ManagedControl->IsStopped())
		return;
	// Task 21 review M-1: before Completed, so the record has nothing pending when it completes (Inspector 2.3's sealing):
	// the last attempt's decode excerpt, announced and never handed to the decode task -- a custom Outer stage replaced the
	// 2xx, or a Cancel() (a sink's OnEvent at that AttemptEnded) finished the request first
	Excerpts::ResolveDiscarded(Ex);
	if (ManagedControl && ManagedControl->IsStopped())
		return; // nested excerpt clock or sink can reenter Stop before this normal Completed bridge
	if (FVaRestTrace::Detail() == EVaRestTraceDetail::None)
	{
		return;
	}
	LLM_SCOPE_BYTAG(VaRest_Trace);
	FVaRestTrace::FScopedCycles Cycles;
	const TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> Held = RedactorFor(Ex);
	const FVaRestRedactor& Redactor = *Held;
	FVaRestTraceCompleted Payload;
	Payload.RequestId = Ex.RequestId;
	Payload.Response.Status = Finish.Response.Status;
	Payload.Response.BodyBytes = Finish.TraceResponseBodyBytes;
	Payload.Response.Storage = Finish.TraceResponseStorage;
	Payload.Response.EffectiveUrl = Redactor.RedactUrl(Finish.Response.EffectiveUrl);
	Payload.Response.ContentType = Redactor.ScrubText(Finish.Response.ContentType);
	if (FVaRestTrace::Wants(EVaRestTraceDetail::Headers))
	{
		Payload.Response.Headers = Redactor.RedactHeaders(Finish.Response.Headers);
	}
	Payload.CacheLookup = Ex.CacheLookup;
	Payload.bFromCache = Finish.Response.bFromCache;
	if (Finish.bQueued)
    {
        Payload.Outcome = EVaRestTraceOutcome::Queued;
    }
    else if (Finish.Error.Kind == EVaRestErrorKind::Canceled)
	{
		Payload.Outcome = EVaRestTraceOutcome::Canceled;
		Payload.CancelReason = Ex.CancelReason == EVaRestCancelReason::None ? EVaRestCancelReason::User : Ex.CancelReason;
	}
	else if (Finish.Error.IsError())
	{
		Payload.Outcome = EVaRestTraceOutcome::Error;
	}
	if (Finish.Error.IsError())
	{
		// Inspector 2.2: "FVaRestError: Url redacted, Message value-scrubbed; Body/RawBody are not copied"
		Payload.Error.Kind = Finish.Error.Kind;
		Payload.Error.HttpStatus = Finish.Error.HttpStatus;
		Payload.Error.Code = Redactor.ScrubText(Finish.Error.Code);
		Payload.Error.Message = Redactor.ScrubText(Finish.Error.Message);
	}
	const IResultHandler* Handler = ActualHandler ? ActualHandler : Ex.Handler.Get();
	Payload.ResultType = Handler ? Handler->GetResultType() : NAME_None;
	if (ManagedControl && ManagedControl->IsStopped())
		return;
	Payload.DecodeStartAt = Finish.DecodeStartAt;
	Payload.DecodeEndAt = Finish.DecodeEndAt;
	const TArray<FVaRestJsonError>& Warnings = Finish.Response.DecodeWarnings;
	Payload.DecodeWarningCount = Warnings.Num();
	for (int32 Index = 0; Index < FMath::Min(Warnings.Num(), FVaRestTraceCompleted::MaxDecodeWarnings); ++Index)
	{
		FVaRestTraceDecodeWarning& Warning = Payload.DecodeWarnings.AddDefaulted_GetRef();
		// The path is made of the response's keys: scrubbed like every captured string (Inspector 2.2 layer 3)
		Warning.Path = Redactor.ScrubText(Warnings[Index].Path);
		Warning.Message = Redactor.ScrubText(Warnings[Index].Message);
	}
	// IN-5 Envelope: configured paths/expected values and server text are captured strings, so scrub both (Inspector 2.2 layer 3).
	Payload.Envelope = Finish.EnvelopeTrace;
	Payload.Envelope.DataPath = Redactor.ScrubText(Payload.Envelope.DataPath);
	Payload.Envelope.PredicatePath = Redactor.ScrubText(Payload.Envelope.PredicatePath);
	Payload.Envelope.Expected = Redactor.ScrubText(Payload.Envelope.Expected);
	Payload.Envelope.Actual = Redactor.ScrubText(Payload.Envelope.Actual);
	Payload.Envelope.Code = Redactor.ScrubText(Payload.Envelope.Code);
	Payload.Envelope.Message = Redactor.ScrubText(Payload.Envelope.Message);
	// M4a final review C-I-1: links a request a park ended unsent to refresh #R (Inspector 2.3's back-link table)
	Payload.AuthRefreshId = Ex.ParkRefreshId;
	const double At = Ex.Config->Clock->Now();
	if (ManagedControl && ManagedControl->IsStopped())
		return; // exact original clock has returned; never emit normal completion after it stopped this operation
	FVaRestTrace::Emit(EVaRestTraceKind::Completed, At, FVaRestTracePayload(TInPlaceType<FVaRestTraceCompleted>(), MoveTemp(Payload)));
}

void VaRest::Http::TracePoints::Client(uint64 ClientId, const FClientConfig& Config, bool bCreated)
{
	using namespace TracePointsPrivate;
	if (FVaRestTrace::Detail() == EVaRestTraceDetail::None)
	{
		return;
	}
	LLM_SCOPE_BYTAG(VaRest_Trace);
	FVaRestTrace::FScopedCycles Cycles;
	FVaRestTraceClient Payload;
	Payload.bCreated = bCreated;
	Payload.ClientId = ClientId;
	Payload.ScopeId = Config.ScopeId;
	// Built here, on the thread the state dies on: the task below carries plain data, no reference into the configuration
	Payload.DisplayName = DisplayNameOf(Config);
	const double Time = Config.Clock.IsValid() ? Config.Clock->Now() : 0.0;
	EmitOnGameThread(EVaRestTraceKind::Client, Time, FVaRestTracePayload(TInPlaceType<FVaRestTraceClient>(), MoveTemp(Payload)));
}

void VaRest::Http::TracePoints::ContributeSnapshot(FVaRestTraceSnapshot& Snapshot)
{
	using namespace TracePointsPrivate;
	// No mask check: a sink being added asked for this snapshot
	LLM_SCOPE_BYTAG(VaRest_Trace);
	// Managed snapshot pins live on AddSink's synchronous attachment stack through OnAttached/publication; ordinary
	// client pins keep their original later-GT release. A last managed owner never needs an uncounted cleanup task.
	FClientState::ForEachLive([&Snapshot](FClientState& State)
	{
		const TSharedRef<const FClientConfig, ESPMode::ThreadSafe> Current = State.GetConfig();
		if (Current->ScopeLifetime.IsValid() && !Current->ScopeLifetime->IsOpen())
			return; // retained closed-scope shells are not active clients in a late snapshot
		FVaRestTraceClient& Entry = Snapshot.Clients.AddDefaulted_GetRef();
		Entry.ClientId = State.GetClientId();
		Entry.ScopeId = State.GetScopeId();
		Entry.DisplayName = DisplayNameOf(*Current);
		State.ForEachInFlight([&Snapshot](FExchangeState& Ex)
		{
			if (Ex.Phase == EVaRestExchangePhase::Done || !Ex.Config.IsValid())
			{
				return;
			}
			FVaRestTraceStarted Partial = Describe(Ex);
			Partial.Flags |= EVaRestTraceRequestFlags::Partial;
			Snapshot.InFlight.Add(MoveTemp(Partial));
		});
	});
}

void VaRest::Http::TracePoints::Note(const FExchangeState& Ex, FVaRestTraceNote&& Payload)
{
	if (FVaRestTrace::Detail() == EVaRestTraceDetail::None)
	{
		return;
	}
	LLM_SCOPE_BYTAG(VaRest_Trace);
	FVaRestTrace::FScopedCycles Cycles;
	Payload.RequestId = Ex.RequestId;
	Payload.Attempt = Ex.Attempt;
	FVaRestTrace::Emit(EVaRestTraceKind::Note, Ex.Config->Clock->Now(), FVaRestTracePayload(TInPlaceType<FVaRestTraceNote>(), MoveTemp(Payload)));
}

void VaRest::Http::TracePoints::Marker(const FClientState& Client, EVaRestTraceMarkerKind Kind, uint64 CauseRequestId, FStringView Text)
{
	if (FVaRestTrace::Detail() == EVaRestTraceDetail::None)
	{
		return;
	}
	LLM_SCOPE_BYTAG(VaRest_Trace);
	FVaRestTrace::FScopedCycles Cycles;
	const TSharedRef<const FClientConfig, ESPMode::ThreadSafe> Current = Client.GetConfig();
	FVaRestTraceMarker Payload;
	Payload.Kind = Kind;
	Payload.ScopeId = Client.GetScopeId();
	Payload.ClientId = Client.GetClientId();
	Payload.CauseRequestId = CauseRequestId;
	Payload.Text = Client.GetRedactor()->ScrubText(Text);
	FVaRestTrace::Emit(EVaRestTraceKind::Marker, Current->Clock->Now(), FVaRestTracePayload(TInPlaceType<FVaRestTraceMarker>(), MoveTemp(Payload)));
}
