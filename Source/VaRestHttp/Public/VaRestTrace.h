// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "Misc/TVariant.h"
#include "UObject/WeakObjectPtr.h"
#include "VaRestBody.h"
#include "VaRestHttpTypes.h"

#include <atomic>

class FVaRestRedactor;
namespace VaRest::Http { struct FSocketFrameCaptureData; }

/**
 * Inspector 1.2 / IN-14: body excerpts in the trace. Default on in every build but Shipping; a target may set it in
 * GlobalDefinitions (a Shipping test build that wants capture sets 1). With 0 no body byte is copied for a sink, and
 * Started/AttemptEnded never say an excerpt is pending.
 */
#ifndef VAREST_WITH_TRACE_EXCERPTS
#define VAREST_WITH_TRACE_EXCERPTS !UE_BUILD_SHIPPING
#endif

/**
 * Inspector spec 9: thirteen kinds in all, in its order. M3 emits Started, AttemptDispatched, AttemptEnded, Completed and
 * Client, and provides scope 0 through the snapshot; Note, Excerpt and Marker arrive with M4, the socket kinds with W5.
 */
enum class EVaRestTraceKind : uint8
{
	Started,
	AttemptDispatched,
	AttemptEnded,
	Note,
	Completed,
	Excerpt,
	Scope,
	Client,
	Marker,
	Socket,
	SocketAttempt,
	SocketFrame,
	SocketNote
};

/** WebSocket spec 19 item 4: who emitted an event. Sinks ignore payload types they do not know. */
enum class EVaRestTraceSource : uint8
{
	Http,
	Socket
};

/** IN-11: what a sink wants. 0 = nothing: every trace point returns after one relaxed load. */
enum class EVaRestTraceDetail : uint32
{
	None = 0,
	Metadata = 1 << 0,
	Headers = 1 << 1,
	Bodies = 1 << 2
};
ENUM_CLASS_FLAGS(EVaRestTraceDetail);

struct FVaRestTraceDetailSettings
{
	EVaRestTraceDetail Flags = EVaRestTraceDetail::None;
	/** Text and JSON excerpt cap (Inspector 3.4 MaxBodyBytes) */
	int32 BodyCap = 0;
	/** Binary excerpt cap; 0 = a descriptor only (Inspector 3.4) */
	int32 BinaryCap = 0;
	/** Socket frame excerpt cap */
	int32 FrameCap = 0;
	/**
	 * IN-14: bytes of raw body copies that may wait for redaction at once; 0 = DefaultMaxPendingExcerptBytes. Past it an
	 * excerpt is Skipped: Backlog. GetDetailSettings() holds the largest any Bodies sink asked for, each 0 counted as the default.
	 */
	int64 MaxPendingExcerptBytes = 0;

	static constexpr int64 DefaultMaxPendingExcerptBytes = 16 * 1024 * 1024;
};

/** IN-6 */
enum class EVaRestExcerptKind : uint8
{
	Json,
	Text,
	Form,
	Multipart,
	Binary,
	Stream,
	Skipped
};

enum class EVaRestExcerptSkip : uint8
{
	None,
	Backlog,
	/**
	 * Reserved (W5): core never emits it. An announced excerpt that no sink wants any more lapses without an event
	 * (Inspector 2.3, Task 22 review; M4a final review C-M-7).
	 */
	Disabled,
	/**
	 * Task 21 review M-1: AttemptEnded announced the excerpt for the decode task, and that attempt's response never reached
	 * it (a custom stage replaced the outcome, or a cancel finished the request first). Emitted on the game thread, once,
	 * right before the request's next AttemptEnded or its Completed: every announced excerpt resolves.
	 */
	Discarded
};

/** IN-6: a redacted body excerpt. FVaRestRedactor builds it (logs use it in M3); M4's Excerpt event carries it. */
struct VARESTHTTP_API FVaRestTraceExcerpt
{
	/** Redacted UTF-8 text or, only with bBinaryCaptured, scrubbed binary bytes. Shared and never changed. */
	TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> Bytes;
	EVaRestExcerptKind Kind = EVaRestExcerptKind::Text;
	/** Explicit binary opt-in; Binary without this marker holds a text descriptor, never captured body bytes. */
	bool bBinaryCaptured = false;
	/** The whole body's size */
	int64 TotalBytes = 0;
	bool bTruncated = false;
	bool bRedacted = false;
	/** "" when complete, "cap" when the cap cut it, else the JSON parse error */
	FString StopReason;
	/** Binary: the first 8 hex digits of the body's SHA-256, when the whole body was seen */
	FString Sha256Prefix;
	EVaRestExcerptSkip SkipReason = EVaRestExcerptSkip::None;

	/** Captured binary projects to hex; other kinds retain their redacted UTF-8 text/descriptor projection. */
	FString ToText() const;
};

/** IN-15 */
struct FVaRestTraceInitiator
{
	FString OwnerPath;
	FString ScriptObject;
	FString ScriptFunction;
	/** Code - Script of the top Blueprint frame; -1 without one */
	int32 ScriptCodeOffset = INDEX_NONE;
	FString SourceFile;
	int32 SourceLine = 0;
};

enum class EVaRestTraceRequestFlags : uint8
{
	None = 0,
	Idempotent = 1 << 0,
	QueueWhenOffline = 1 << 1,
	NoAuth = 1 << 2,
	AuthRefresh = 1 << 3,
	/** A snapshot entry for a request that started before the sink attached */
	Partial = 1 << 4
};
ENUM_CLASS_FLAGS(EVaRestTraceRequestFlags);

enum class EVaRestTraceCacheLookup : uint8
{
	None,
	Miss,
	HitMemory,
	HitDisk,
	Stale,
	Revalidate
};

struct FVaRestTraceBody
{
	FString ContentType;
	int64 Bytes = 0;
	EVaRestBodyKind Kind = EVaRestBodyKind::None;
};

struct FVaRestTraceQueueReplay
{
	FString QueueId;
	uint64 OriginalRequestId = 0;
};

/** IN-1. Fields marked * in the spec arrive redacted: Url, Headers. */
struct FVaRestTraceStarted
{
	uint64 RequestId = 0;
	uint64 ScopeId = 0;
	uint64 ClientId = 0;
	FString Verb;
	FString Url;
	/** The path before {param} substitution, without the query */
	FString PathTemplate;
	FName Endpoint;
	FName Tag;
	EVaRestPriority Priority = EVaRestPriority::Normal;
	EVaRestTraceRequestFlags Flags = EVaRestTraceRequestFlags::None;
	FVaRestTraceQueueReplay QueueReplayOf;
	FVaRestTraceInitiator Initiator;
	/** M4 */
	FName CachePolicy;
	/** Initial state in Started; current state in an attach snapshot. */
	EVaRestTraceCacheLookup CacheLookup = EVaRestTraceCacheLookup::None;
	bool bCacheLookupPending = false;
	/** As built, redacted; empty unless a sink wants Headers */
	FVaRestHeaders Headers;
	FVaRestTraceBody Body;
	bool bExcerptPending = false;
	FDateTime StartUtc;
	/** Snapshot entries (Partial): the attempt in progress */
	int32 CurrentAttempt = 0;
	/** Snapshot entries: when the request entered the pipeline (IVaRestClock seconds) */
	double StartedAt = 0.0;
};

enum class EVaRestTraceGap : uint8
{
	None,
	Backoff,
	SessionRefresh
};

/** IN-2: on the game thread, immediately before the transport is started */
struct FVaRestTraceAttemptDispatched
{
	uint64 RequestId = 0;
	/** Captured physical dispatch serial (1..MAX_uint32), independent of Attempt. */
	uint64 DispatchId = 0;
	int32 Attempt = 0;
	double AttemptStart = 0.0;
	/** M4 */
	EVaRestTraceGap GapKind = EVaRestTraceGap::None;
	double GapSeconds = 0.0;
	/** M4 */
	double AuthWait = 0.0;
	uint64 AuthRefreshId = 0;
	/** Seconds this attempt waited in the limiter queue */
	double LimiterWait = 0.0;
	/** Final (after auth and custom per-attempt stages), redacted; empty unless a sink wants Headers */
	FVaRestHeaders Headers;
	/** M4 (revalidation) */
	bool bConditional = false;
};

/** IN-3: on the game thread, when the outcome re-enters the stage chain. The times were written on the HTTP thread. */
struct FVaRestTraceAttemptEnded
{
	uint64 RequestId = 0;
	/** Captured physical dispatch serial (1..MAX_uint32), independent of Attempt. */
	uint64 DispatchId = 0;
	int32 Attempt = 0;
	/** 0 = no response */
	int32 Status = 0;
	/** Redacted; empty unless a sink wants Headers */
	FVaRestHeaders Headers;
	/** Redacted */
	FString EffectiveUrl;
	FString ContentType;
	int64 BytesUp = 0;
	int64 BytesDown = 0;
	EVaRestTransportFailure Failure = EVaRestTransportFailure::None;
	FName FailureDetail;
	/**
	 * IVaRestClock seconds; -1 when unknown. UploadDoneAt is when the transport took the last body byte (curl's read
	 * callback, NSURLSession's didSendBodyData), which for a body smaller than the socket's send buffer is before it left
	 * the machine: the upload phase reads short and the wait phase long (decision 29, review N-8).
	 */
	double UploadDoneAt = -1.0;
	double StatusAt = -1.0;
	double TransportDoneAt = -1.0;
	bool bExcerptPending = false;
};

enum class EVaRestTraceOutcome : uint8
{
	Success,
	Error,
	Queued,
	Canceled
};

struct FVaRestTraceError
{
	EVaRestErrorKind Kind = EVaRestErrorKind::None;
	int32 HttpStatus = 0;
	FString Code;
	/** Value-scrubbed */
	FString Message;
};

struct FVaRestTraceDecodeWarning
{
	FString Path;
	FString Message;
};

/**
 * IN-5: what the response envelope did (spec 3.4). bApplied: its success half judged a 2xx body; DataPath is set when that
 * included the data unwrap (FVaRestJson and struct results, not void ones: plan decision 9), and the predicate's fields
 * only when it failed. Code and Message without bApplied: bParseErrorBodies read them from a 4xx/5xx body, which it does
 * for every result type. Actual, Code and Message are value-scrubbed.
 */
struct FVaRestTraceEnvelope
{
	bool bApplied = false;
	FString DataPath;
	FString PredicatePath;
	FString Expected;
	FString Actual;
	FString Code;
	FString Message;
};

/** Delivered body storage, independently of request-body construction kind and wire byte counters. */
enum class EVaRestTraceResponseStorage : uint8 { Unknown, Memory, Stream };

/** Final delivered metadata; strings are redacted and headers follow the Headers detail mask. */
struct FVaRestTraceResponse
{
	int32 Status = 0;
	FVaRestHeaders Headers;
	FString EffectiveUrl;
	FString ContentType;
	/** Captured before decode consumes the entity; -1 when unobserved or streamed. */
	int64 BodyBytes = -1;
	EVaRestTraceResponseStorage Storage = EVaRestTraceResponseStorage::Unknown;
};

/** IN-5: on the game thread, before any callback (OnHttpThread ones included) */
struct FVaRestTraceCompleted
{
	static constexpr int32 MaxDecodeWarnings = 32;

	uint64 RequestId = 0;
	FVaRestTraceResponse Response;
	EVaRestTraceOutcome Outcome = EVaRestTraceOutcome::Success;
	/** Final lookup history and actual delivered response provenance are independent. */
	EVaRestTraceCacheLookup CacheLookup = EVaRestTraceCacheLookup::None;
	bool bFromCache = false;
	EVaRestCancelReason CancelReason = EVaRestCancelReason::None;
	FVaRestTraceError Error;
	FName ResultType;
	/** IVaRestClock seconds; -1 when there was no decode step */
	double DecodeStartAt = -1.0;
	double DecodeEndAt = -1.0;
	/** The first MaxDecodeWarnings, Message scrubbed */
	TArray<FVaRestTraceDecodeWarning> DecodeWarnings;
	int32 DecodeWarningCount = 0;
	FVaRestTraceEnvelope Envelope;
	/**
	 * M4a final review C-I-1: the refresh request (its RequestId) this request's last park behind a session refresh waited
	 * on; 0 = it never parked. Set also when a park ended it before any dispatch (a failed or rejected refresh), which no
	 * AttemptDispatched.AuthRefreshId reports.
	 */
	uint64 AuthRefreshId = 0;
};

/** IN-7. M3 has only scope 0, "Process"; M5's hubs add one per game instance. */
struct FVaRestTraceScope
{
	bool bOpened = true;
	uint64 ScopeId = 0;
	int32 PIEInstance = INDEX_NONE;
	bool bEditorScope = false;
	FString Label;
	/** Game thread use only */
	TWeakObjectPtr<UObject> GameInstance;
};

/** IN-8 */
struct FVaRestTraceClient
{
	bool bCreated = true;
	uint64 ClientId = 0;
	uint64 ScopeId = 0;
	/** The asset path (M5), WithName (value-scrubbed), or the current base URL (URL-redacted) */
	FString DisplayName;
};

/** IN-4 */
enum class EVaRestTraceNoteKind : uint8
{
	CacheRevalidated,
	CacheStored,
	CacheServedStale,
	RetryScheduled,
	Queued,
	QueueExpired
};

/** IN-4: something a stage did to a request, on the game thread. The fields a kind does not use keep their defaults. */
struct FVaRestTraceNote
{
	uint64 RequestId = 0;
	/**
	 * The attempt the note is about; for RetryScheduled the one that ended, which may be one a park behind a refresh or
	 * the limiter ended before any dispatch. Pre-attempt CacheServedStale, Queued and QueueExpired notes use 0;
	 * no attempt was entered. RequestId is always the actual nonzero logical request identity.
	 */
	int32 Attempt = 0;
	EVaRestTraceNoteKind Kind = EVaRestTraceNoteKind::RetryScheduled;
	/** RetryScheduled: why ("Status503", "Connection", "Timeout"); Queued (M4b): why it was queued */
	FName Reason;
	/** RetryScheduled: seconds until the next attempt starts */
	double Delay = 0.0;
	/** RetryScheduled: the seconds a Retry-After header asked for; -1 without one */
	double RetryAfter = -1.0;
	/** CacheStored (M4b) */
	int64 Bytes = 0;
	/** Queued, QueueExpired (M4b) */
	FString QueueId;
};

/** IN-6 Owner */
enum class EVaRestTraceExcerptOwner : uint8
{
	Request,
	Response,
	/** A socket frame (W5): SocketId and FrameId are set */
	Frame
};

/** IN-6: a redacted excerpt, from a decode task or the redaction pipe (never the HTTP thread) */
struct FVaRestTraceExcerptEvent
{
	EVaRestTraceExcerptOwner Owner = EVaRestTraceExcerptOwner::Request;
	uint64 RequestId = 0;
	/** Captured physical dispatch serial (1..MAX_uint32); 0 for request/frame excerpts. */
	uint64 DispatchId = 0;
	/** The attempt whose response it is; 0 for a request body, taken before the first attempt */
	int32 Attempt = 0;
	/** W5 */
	uint64 SocketId = 0;
	/** W5 */
	uint64 FrameId = 0;
	FVaRestTraceExcerpt Excerpt;
};

/** IN-9 */
enum class EVaRestTraceMarkerKind : uint8
{
	Online,
	Offline,
	SessionRefreshed,
	SessionReplaced,
	SessionCleared,
	SessionExpired,
	/** M5 */
	EnvironmentChanged
};

/** IN-9: a client-level event, on the game thread */
struct FVaRestTraceMarker
{
	EVaRestTraceMarkerKind Kind = EVaRestTraceMarkerKind::SessionRefreshed;
	uint64 ScopeId = 0;
	uint64 ClientId = 0;
	/** The request that caused it (the 401 that started a refresh); 0 when none */
	uint64 CauseRequestId = 0;
	/** Value-scrubbed */
	FString Text;
};

/** IW-1..IW-6 live in the core; protocol modules map their own enums without a reverse dependency. */
enum class EVaRestTraceSocketLifecycle : uint8 { Created, Released };
enum class EVaRestTraceSocketAttemptPhase : uint8 { Connecting, Open, Closed };
enum class EVaRestTraceSocketGap : uint8 { None, Backoff, Offline, Background, Auth };
enum class EVaRestTraceSocketCloseBy : uint8 { Client, Server, Transport, Heartbeat, Timeout };
enum class EVaRestTraceSocketDirection : uint8 { In, Out };
enum class EVaRestTraceSocketFrameKind : uint8 { Text, Binary };
enum class EVaRestTraceSocketMessageKind : uint8 { Message, Request, Reply, ErrorReply, Control };
enum class EVaRestTraceSocketLane : uint8 { Bypass, Ready, Backlog };
enum class EVaRestTraceSocketNoteKind : uint8 { SendDropped, RequestEnded, Flow, Backpressure, Channel, Ping };
enum class EVaRestTraceSocketDropReason : uint8 { Volatile, Overflow, Rejected, StalePin, SendLimit };
enum class EVaRestTraceSocketRequestOutcome : uint8 { Replied, ErrorReply, Timeout, Connection, Canceled };
enum class EVaRestTraceSocketChannelState : uint8 { Up, Down, Refused };

struct FVaRestTraceSocketClose
{
	int32 Code = 0;
	/** Value-scrubbed by the socket's held connection redactor. */
	FString Reason;
	bool bClean = false;
	EVaRestTraceSocketCloseBy By = EVaRestTraceSocketCloseBy::Transport;
	bool bWillReconnect = false;
	double Delay = 0.0;
	bool bWhileReadPaused = false;
	FName Failure;
	int32 HttpStatus = 0;
	bool bGaveUp = false;
	EVaRestErrorKind ErrorKind = EVaRestErrorKind::None;
};

struct FVaRestTraceSocket
{
	EVaRestTraceSocketLifecycle Phase = EVaRestTraceSocketLifecycle::Created;
	uint64 SocketId = 0, ScopeId = 0;
	FName Name, Protocol, Entry;
	/** Redacted URL, including configured auth-placement keys. */
	FString Url;
	FVaRestTraceInitiator Initiator;
	/** Snapshot-only live state; no socket objects or owner pointers enter the trace. */
	FName State;
	bool bReady = false, bPartial = false;
	uint64 CurrentAttemptId = 0;
};

struct FVaRestTraceSocketAttempt
{
	uint64 SocketId = 0, AttemptId = 0;
	EVaRestTraceSocketAttemptPhase Phase = EVaRestTraceSocketAttemptPhase::Connecting;
	EVaRestTraceSocketGap GapBefore = EVaRestTraceSocketGap::None;
	double GapSeconds = 0.0;
	FString Url;
	FVaRestHeaders HandshakeHeaders;
	TArray<FString> Protocols;
	double ConnectMs = 0.0;
	FString Protocol;
	FVaRestTraceSocketClose Close;
	bool bPartial = false;
};

struct FVaRestTraceSocketFrame
{
	uint64 SocketId = 0, AttemptId = 0, FrameId = 0;
	EVaRestTraceSocketDirection Dir = EVaRestTraceSocketDirection::In;
	EVaRestTraceSocketFrameKind Kind = EVaRestTraceSocketFrameKind::Text;
	int64 Bytes = 0;
	FString Channel, Route, MsgId;
	EVaRestTraceSocketMessageKind MsgKind = EVaRestTraceSocketMessageKind::Message;
	FName ProtocolTag;
	/** At most ten sizes; never an attachment body. */
	TArray<int64> AttachmentSizes;
	bool bAutoReplied = false;
	double ReceivedAt = 0.0, CalledAt = 0.0;
	EVaRestTraceSocketLane Lane = EVaRestTraceSocketLane::Backlog;
	/** Inbound only: prepared on the native socket pipe. */
	FVaRestTraceExcerpt Excerpt;
	/** Outbound only: a later Excerpt{Frame} resolves this by SocketId and FrameId. */
	bool bExcerptPending = false;
};

struct FVaRestTraceSocketNote
{
	uint64 SocketId = 0, AttemptId = 0;
	EVaRestTraceSocketNoteKind Kind = EVaRestTraceSocketNoteKind::SendDropped;
	int64 Count = 0;
	EVaRestTraceSocketDropReason DropReason = EVaRestTraceSocketDropReason::Rejected;
	FString MsgId, Channel, Route;
	EVaRestTraceSocketRequestOutcome Outcome = EVaRestTraceSocketRequestOutcome::Canceled;
	EVaRestErrorKind ErrorKind = EVaRestErrorKind::None;
	double Latency = 0.0;
	bool bPaused = false, bHigh = false;
	int64 PendingBytes = 0;
	EVaRestTraceSocketChannelState ChannelState = EVaRestTraceSocketChannelState::Down;
	FString PublicId;
	bool bRecovered = false;
	FName Reason;
	/** Value-scrubbed, including protocol session ids. */
	FString Message;
	double Rtt = 0.0;
};

using FVaRestTracePayload = TVariant<FVaRestTraceStarted, FVaRestTraceAttemptDispatched, FVaRestTraceAttemptEnded, FVaRestTraceNote, FVaRestTraceCompleted,
	FVaRestTraceExcerptEvent, FVaRestTraceScope, FVaRestTraceClient, FVaRestTraceMarker,
	FVaRestTraceSocket, FVaRestTraceSocketAttempt, FVaRestTraceSocketFrame, FVaRestTraceSocketNote>;

struct FVaRestTraceEvent
{
	EVaRestTraceKind Kind = EVaRestTraceKind::Started;
	EVaRestTraceSource Source = EVaRestTraceSource::Http;
	/** Taken only when the mask is non-zero (Inspector 2.3) */
	uint64 Seq = 0;
	/** IVaRestClock seconds of the emitting client */
	double Time = 0.0;
	FVaRestTracePayload Payload;
};

/** IN-12: what a sink learns when it attaches */
struct FVaRestTraceSnapshot
{
	TArray<FVaRestTraceScope> Scopes;
	TArray<FVaRestTraceClient> Clients;
	/** Every in-flight request as Started{Partial} */
	TArray<FVaRestTraceStarted> InFlight;
	TArray<FVaRestTraceSocket> Sockets;
	TArray<FVaRestTraceSocketAttempt> SocketAttempts;
	/** Current protocol channel metadata, produced on GTK without callback/owner objects. */
	TArray<FVaRestTraceSocketNote> SocketNotes;
};

/**
 * Remove a sink (RemoveSink) before the module that owns it shuts down: one still registered at static destruction is
 * called from there, for the Client{Destroyed} of a global FVaRestClient dying late (task 15 review M9).
 */
class IVaRestTraceSink
{
public:
	virtual ~IVaRestTraceSink() = default;

	/** Game thread; read by AddSink and RefreshDetail */
	virtual FVaRestTraceDetailSettings GetDetail() const = 0;

	/** Game thread, from AddSink, before the sink receives any event */
	virtual void OnAttached(const FVaRestTraceSnapshot& Snapshot) = 0;

	/**
	 * The emitting thread: the game thread for every kind but Excerpt (a background task); never the HTTP thread
	 * (Inspector 9). Return quickly. Emit iterates the list it took when it started, so a sink can still be called once
	 * after RemoveSink returns (IN-12): from a background emitter, and on the game thread for the event in progress when
	 * another sink's OnEvent removed it. A sink added meanwhile gets the next event, not the one in progress.
	 * Switch on Event.Kind and ignore kinds you do not know: W5 adds the socket kinds and a later version may add more
	 * (WebSocket spec 19 item 4). An Excerpt event arrives on a worker, maybe after RemoveSink returned, and the worker's
	 * list may hold the sink's last reference, so the sink can be destroyed there. Never wait for the game thread in
	 * OnEvent: the game thread may itself be waiting for the redaction pipe that emits it (StopRedactionPipe at module shutdown).
	 */
	virtual void OnEvent(const FVaRestTraceEvent& Event) = 0;
};

/** IN-11 dev-test counters: the zero-cost proof counts instead of timing (Inspector 3.1 item 6) */
struct FVaRestTraceCounters
{
	std::atomic<uint64> EventsBuilt{ 0 };
	std::atomic<uint64> RedactorCalls{ 0 };
	/** Transport delegate binds made for tracing or progress (OnStatusCodeReceived, OnRequestProgress64) */
	std::atomic<uint64> DelegateBinds{ 0 };
	/** Dev-only cycle accumulator of the trace points (Inspector 8) */
	std::atomic<uint64> TraceCycles{ 0 };

	void Reset()
	{
		EventsBuilt = 0;
		RedactorCalls = 0;
		DelegateBinds = 0;
		TraceCycles = 0;
	}
};

#if WITH_DEV_AUTOMATION_TESTS
/**
 * Test-only (IN-12): while one lives, only the sinks added inside it are active. Game thread; strictly LIFO (checked).
 * RemoveSink also strips the sink from the saved outer lists, so a Pause during an isolation is not undone at restore.
 * An AddSink inside an isolation is discarded at restore: the sink belongs to the isolated list.
 */
class VARESTHTTP_API FVaRestScopedSinkIsolation
{
public:
	FVaRestScopedSinkIsolation();
	~FVaRestScopedSinkIsolation();
	FVaRestScopedSinkIsolation(const FVaRestScopedSinkIsolation&) = delete;
	FVaRestScopedSinkIsolation& operator=(const FVaRestScopedSinkIsolation&) = delete;

private:
	/** Saved-stack depth before this one pushed; the destructor checks nothing nested outlives it */
	int32 Depth = 0;
};
#endif

/** Prepared native frame metadata/excerpt. Unpublished captures may be discarded on any thread.
 * Publish on GTK only after the transport accepted the frame; no source bytes are borrowed after preparation. */
class VARESTHTTP_API FVaRestSocketFrameCapture
{
public:
	FVaRestSocketFrameCapture();
	~FVaRestSocketFrameCapture();
	FVaRestSocketFrameCapture(FVaRestSocketFrameCapture&& Other);
	FVaRestSocketFrameCapture& operator=(FVaRestSocketFrameCapture&& Other);
	FVaRestSocketFrameCapture(const FVaRestSocketFrameCapture&) = delete;
	FVaRestSocketFrameCapture& operator=(const FVaRestSocketFrameCapture&) = delete;
	void Publish();
private:
	friend class FVaRestTrace;
	TUniquePtr<VaRest::Http::FSocketFrameCaptureData> Data;
};

/** The sink registry and the trace points' gate (Inspector 3.1, IN-10 to IN-12) */
class VARESTHTTP_API FVaRestTrace
{
public:
	using FSinkRef = TSharedRef<IVaRestTraceSink, ESPMode::ThreadSafe>;
	using FSinkList = TArray<FSinkRef>;
#if WITH_DEV_AUTOMATION_TESTS
	using FScopedSinkIsolation = FVaRestScopedSinkIsolation;
#endif

	/** IN-7: before M5, everything is in this scope */
	static constexpr uint64 ProcessScopeId = 0;

	/** IN-11: one relaxed load. Every trace point starts with `if (FVaRestTrace::Detail() == EVaRestTraceDetail::None) { return; }`. */
	static EVaRestTraceDetail Detail()
	{
		return static_cast<EVaRestTraceDetail>(DetailMask.load(std::memory_order_relaxed));
	}

	static bool Wants(EVaRestTraceDetail Flag)
	{
		return EnumHasAnyFlags(Detail(), Flag);
	}

	/** The largest caps any sink asked for; read after the mask check */
	static FVaRestTraceDetailSettings GetDetailSettings();

	/** Game thread. Calls Sink->OnAttached(BuildSnapshot()) first (IN-12). */
	static void AddSink(const FSinkRef& Sink);
	/** Game thread */
	static void RemoveSink(const FSinkRef& Sink);
	/** Game thread: a sink's GetDetail() changed */
	static void RefreshDetail();
	/** Any thread: the current immutable list (null when there is none) */
	static TSharedPtr<const FSinkList, ESPMode::ThreadSafe> GetSinks();

	/** Stamps Seq and hands the event to every sink. Game thread for every kind but Excerpt. Callers checked Detail() first. */
	static void Emit(EVaRestTraceKind Kind, double Time, FVaRestTracePayload&& Payload, EVaRestTraceSource Source = EVaRestTraceSource::Http);
	/** GT, at outgoing hand-off: bounded copy, metadata announcement, then redaction on the core pipe.
	 * The worker owns only native bytes, immutable redactor and native admission leases. */
	static void EmitSocketFrame(double Time, FVaRestTraceSocketFrame&& Frame, TConstArrayView<uint8> Bytes,
		TSharedPtr<const FVaRestRedactor, ESPMode::ThreadSafe> Redactor);
	static FVaRestSocketFrameCapture PrepareSocketFrame(double Time, FVaRestTraceSocketFrame&& Frame, TConstArrayView<uint8> Bytes,
		TSharedPtr<const FVaRestRedactor, ESPMode::ThreadSafe> Redactor, int32 JsonOffset = -1, bool bRedactAllJsonValues = false);
	/** GTK attachment stack only: terminal events raised by OnAttached reach the new sink after publication.
	 * Only plain event payloads are deferred; no callbacks, socket owners or client shells are retained. */
	static bool IsAttachingOnGT();
	static void EmitAfterAttachmentOnGT(EVaRestTraceKind Kind, double Time, FVaRestTracePayload&& Payload, EVaRestTraceSource Source);

	/** Snapshot contributors (core: clients and in-flight requests; W5: sockets). Game thread. */
	static TMulticastDelegate<void(FVaRestTraceSnapshot&)>& OnBuildSnapshot();
	/** Scope 0, then every contributor. Game thread. */
	static FVaRestTraceSnapshot BuildSnapshot();

	/** IN-10: assigned at creation whatever the mask; any thread; never 0 */
	static uint64 NewRequestId();
	static uint64 NewClientId();
	/** Mask-independent scope IDs; 0 means exhaustion, never wraps into a previously issued identity. */
	static uint64 NewScopeId();
	/** Mask-independent socket/frame identity. Zero means exhaustion; ids never wrap or get reused. */
	static uint64 NewSocketId();
	static uint64 NewSocketFrameId();

	static FVaRestTraceCounters& Counters();

	/**
	 * Adds the cycles it lived to Counters().TraceCycles. Build one inside a trace point, after the mask check. Dev-only:
	 * a no-op under Shipping. Not copyable, and [[nodiscard]] so an unnamed temporary (which would measure nothing) warns.
	 */
	struct FScopedCycles
	{
		[[nodiscard]] FScopedCycles() = default;
#if !UE_BUILD_SHIPPING
		~FScopedCycles() { Counters().TraceCycles += FPlatformTime::Cycles64() - Start; }
		uint64 Start = FPlatformTime::Cycles64();
#endif
		FScopedCycles(const FScopedCycles&) = delete;
		FScopedCycles& operator=(const FScopedCycles&) = delete;
	};

private:
#if WITH_DEV_AUTOMATION_TESTS
	friend class FVaRestScopedSinkIsolation;
#endif

	/**
	 * Game thread. Publishes Next with its mask and caps. The caps are merged before the lock is taken and the old list is
	 * released after it is dropped (review M-8): a sink's GetDetail may call GetDetailSettings or GetSinks, and a sink's
	 * destructor may drop a client whose Client{Destroyed} calls GetSinks -- FRWLock is not recursive.
	 */
	static void Swap(TSharedPtr<const FSinkList, ESPMode::ThreadSafe> Next);

	static std::atomic<uint32> DetailMask;
};
