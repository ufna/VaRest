// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestTracePoints.h"

#include "Tasks/Task.h"
#include "UObject/Class.h"
#include "UObject/Script.h"
#include "UObject/Stack.h"
#include "VaRestClientState.h"
#include "VaRestTransport.h"
#include "VaRestUrl.h"

namespace VaRest::Http::TracePointsPrivate
{
	/**
	 * IN-8's name, redacted by where it came from, not by its shape (task 15 review I1). A WithName name is the game's own
	 * text: the value scrub. An unnamed client is named by its current base URL, which Make does not validate, so it goes
	 * through RedactUrl whatever it looks like: userinfo and secret query values come out of any scheme and of a scheme-less
	 * "user:pw@host/x" too (any RFC 3986 scheme, VaRestRedactor.cpp:525-539; scheme-less userinfo, :566-585, review M3;
	 * the query, :602-607; Inspector 2.2 layer 5, decision 23).
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
		// Plan decision 17: the redactor of now, not the Send() snapshot's
		const TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> Held = Ex.State->GetRedactor();
		const FVaRestRedactor& Redactor = *Held;
		FVaRestTraceStarted Out;
		Out.RequestId = Ex.RequestId;
		Out.ScopeId = FVaRestTrace::ProcessScopeId;
		Out.ClientId = Ex.ClientId;
		Out.Verb = Redactor.ScrubText(Ex.Verb);
		// A request that failed before Resolve finished has no URL: the base and the raw path say what it was
		Out.Url = Redactor.RedactUrl(Ex.Url.IsEmpty() ? JoinUrl(Cfg.BaseUrl, Ex.RawPath) : Ex.Url);
		Out.PathTemplate = Redactor.RedactUrl(Ex.PathTemplate);
		Out.Tag = Ex.Tag;
		Out.Priority = Ex.Priority;
		if (Ex.bIdempotent)
		{
			Out.Flags |= EVaRestTraceRequestFlags::Idempotent;
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
	FVaRestTrace::FScopedCycles Cycles;
	const TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> Held = Ex.State->GetRedactor();
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
	FVaRestTrace::Emit(EVaRestTraceKind::Started, Ex.StartedAt, FVaRestTracePayload(TInPlaceType<FVaRestTraceStarted>(), MoveTemp(Payload)));
}

void VaRest::Http::TracePoints::AttemptDispatched(const FExchangeState& Ex)
{
	if (FVaRestTrace::Detail() == EVaRestTraceDetail::None)
	{
		return;
	}
	FVaRestTrace::FScopedCycles Cycles;
	FVaRestTraceAttemptDispatched Payload;
	Payload.RequestId = Ex.RequestId;
	Payload.Attempt = Ex.Attempt;
	Payload.AttemptStart = Ex.AttemptStartedAt;
	Payload.LimiterWait = Ex.LimiterWait;
	if (FVaRestTrace::Wants(EVaRestTraceDetail::Headers))
	{
		Payload.Headers = Ex.State->GetRedactor()->RedactHeaders(Ex.AttemptHeaders);
	}
	FVaRestTrace::Emit(EVaRestTraceKind::AttemptDispatched, Ex.Config->Clock->Now(), FVaRestTracePayload(TInPlaceType<FVaRestTraceAttemptDispatched>(), MoveTemp(Payload)));
}

void VaRest::Http::TracePoints::AttemptEnded(const FExchangeState& Ex, const FVaRestTransportResult& Result)
{
	if (FVaRestTrace::Detail() == EVaRestTraceDetail::None)
	{
		return;
	}
	FVaRestTrace::FScopedCycles Cycles;
	const TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> Held = Ex.State->GetRedactor();
	const FVaRestRedactor& Redactor = *Held;
	FVaRestTraceAttemptEnded Payload;
	Payload.RequestId = Ex.RequestId;
	Payload.Attempt = Ex.Attempt;
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
	if (Ex.Probe.IsValid())
	{
		// Written on the HTTP thread by the delegates bound for this attempt (IN-3); -1 when they were not bound
		Payload.UploadDoneAt = Ex.Probe->UploadDoneAt.load();
		Payload.StatusAt = Ex.Probe->StatusAt.load();
	}
	Payload.TransportDoneAt = Result.TransportDoneAt;
	FVaRestTrace::Emit(EVaRestTraceKind::AttemptEnded, Ex.Config->Clock->Now(), FVaRestTracePayload(TInPlaceType<FVaRestTraceAttemptEnded>(), MoveTemp(Payload)));
}

void VaRest::Http::TracePoints::Completed(const FExchangeState& Ex, const FFinish& Finish)
{
	if (FVaRestTrace::Detail() == EVaRestTraceDetail::None)
	{
		return;
	}
	FVaRestTrace::FScopedCycles Cycles;
	const TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> Held = Ex.State->GetRedactor();
	const FVaRestRedactor& Redactor = *Held;
	FVaRestTraceCompleted Payload;
	Payload.RequestId = Ex.RequestId;
	if (Finish.Error.Kind == EVaRestErrorKind::Canceled)
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
	Payload.ResultType = Ex.Handler.IsValid() ? Ex.Handler->GetResultType() : NAME_None;
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
	FVaRestTrace::Emit(EVaRestTraceKind::Completed, Ex.Config->Clock->Now(), FVaRestTracePayload(TInPlaceType<FVaRestTraceCompleted>(), MoveTemp(Payload)));
}

void VaRest::Http::TracePoints::Client(uint64 ClientId, const FClientConfig& Config, bool bCreated)
{
	using namespace TracePointsPrivate;
	if (FVaRestTrace::Detail() == EVaRestTraceDetail::None)
	{
		return;
	}
	FVaRestTrace::FScopedCycles Cycles;
	FVaRestTraceClient Payload;
	Payload.bCreated = bCreated;
	Payload.ClientId = ClientId;
	Payload.ScopeId = FVaRestTrace::ProcessScopeId;
	// Built here, on the thread the state dies on: the task below carries plain data, no reference into the configuration
	Payload.DisplayName = DisplayNameOf(Config);
	const double Time = Config.Clock.IsValid() ? Config.Clock->Now() : 0.0;
	EmitOnGameThread(EVaRestTraceKind::Client, Time, FVaRestTracePayload(TInPlaceType<FVaRestTraceClient>(), MoveTemp(Payload)));
}

void VaRest::Http::TracePoints::ContributeSnapshot(FVaRestTraceSnapshot& Snapshot)
{
	using namespace TracePointsPrivate;
	// Review M-9: ForEachLive releases its pins from a later game-thread task, so a client listed here that loses its last
	// other reference meanwhile dies after AddSink added the sink, and its Client{Destroyed} reaches it
	FClientState::ForEachLive([&Snapshot](FClientState& State)
	{
		const TSharedRef<const FClientConfig, ESPMode::ThreadSafe> Current = State.GetConfig();
		FVaRestTraceClient& Entry = Snapshot.Clients.AddDefaulted_GetRef();
		Entry.ClientId = State.GetClientId();
		Entry.ScopeId = FVaRestTrace::ProcessScopeId;
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
