// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSocket.h"
#include "VaRestSocketState.h"
#include "VaRestSocketInbound.h"
#include "VaRestSocketOutbox.h"
#include "VaRestSocketText.h"
#include "VaRestSocketReply.h"
#include "VaRestSocketRequestState.h"
#include "VaRestSocketSettings.h"
#include "VaRestSocketStatsInternal.h"
#include "VaRestSocketTraceState.h"
#include "VaRestSocketLog.h"
#include "VaRestTrace.h"
#include "VaRestJsonUtf8.h"

#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Misc/CoreDelegates.h"
#include "Misc/ScopeExit.h"
#include "HAL/IConsoleManager.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "VaRestAuth.h"
#include "VaRestConnectivity.h"
#include "VaRestHub.h"
#include "VaRestUrl.h"
#include "VaRestWebSocketLog.h"

namespace VaRest::WebSocket
{
namespace
{
	TArray<TWeakObjectPtr<UVaRestSocketState>> States;
	TSharedPtr<FSocketModuleGate, ESPMode::ThreadSafe> ModuleGate;
	FTSTicker::FDelegateHandle TickHandle;
	FDelegateHandle BackgroundHandle, ForegroundHandle, SnapshotHandle;
#if !UE_BUILD_SHIPPING
	IConsoleObject* ListCommand = nullptr;
	IConsoleObject* DropCommand = nullptr;
#endif

	struct FStructInputOwner
	{
		TStrongObjectPtr<UVaRestSocketStructPayload> Box;
		FStructInputOwner() : Box(NewObject<UVaRestSocketStructPayload>(GetTransientPackage())) { check(IsInGameThread()); }
		~FStructInputOwner() { check(IsInGameThread()); if (Box.Get()) Box->Value.Reset(); }
	};

	template <typename Signature> struct TSocketCallback
	{
		TWeakObjectPtr<const UObject> Owner;
		bool bOwned = false;
		bool bRequestOnly = false;
		uint64 Order = 0;
		TFunction<Signature> Function;
		bool IsAlive() const { return !bOwned || Owner.IsValid(); }
	};
	template <typename Signature> using TCallbacks = TArray<TSharedPtr<TSocketCallback<Signature>>>;
	template <typename Signature> void AddCallback(TCallbacks<Signature>& To, const UObject* Owner, TFunction<Signature> Function, bool bRequestOnly = false, uint64 Order = 0)
	{
		if (!Function) return;
		auto Entry = MakeShared<TSocketCallback<Signature>>();
		Entry->Owner = Owner; Entry->bOwned = Owner != nullptr; Entry->Function = MoveTemp(Function);
		Entry->bRequestOnly = bRequestOnly;
		Entry->Order = Order;
		To.Add(MoveTemp(Entry));
	}
	template <typename Signature> void RemoveCallbacks(TCallbacks<Signature>& From, const UObject* Owner)
	{
		From.RemoveAll([Owner](const auto& Entry) { return Entry->Owner.Get() == Owner && Entry->bOwned == (Owner != nullptr); });
	}

	enum class EOperation : uint8 { Connect, Close, ReconnectNow, Stop, Restart, RefreshResult, Background, Online, Dispose, StageReady, StageClose, StageRefresh, ChannelUp, ChannelDown };
	struct FOperation
	{
		FOperation(EOperation InType) : Type(InType) {}
		EOperation Type;
		FString Reason;
		int32 Code = 1000;
		EVaRestSocketState OldState = EVaRestSocketState::Disconnected;
		uint64 Generation = 0;
		EVaRestAuthRefreshOutcome Refresh = EVaRestAuthRefreshOutcome::Unavailable;
		EVaRestErrorKind Kind = EVaRestErrorKind::None;
		bool bFlag = false;
		TOptional<bool> Reconnect;
		uint64 Holder = 0;
		FString Channel;
		FVaRestSocketOpen ChannelOpen;
		FVaRestSocketClose ChannelClose;
		EVaRestChannelEnd ChannelEnd = EVaRestChannelEnd::WillRetry;
	};
	struct FDetached
	{
		TSharedPtr<IVaRestSocketConnection, ESPMode::ThreadSafe> Connection;
		double Deadline = 0.0;
	};
	struct FRetryDecision
	{
		bool bRetry = true, bRefresh = false;
		double Floor = 0.0;
		EVaRestErrorKind Kind = EVaRestErrorKind::Connection;
	};
	struct FStageTimer
	{
		uint64 Id = 0, Generation = 0;
		double Due = 0.0;
		TUniqueFunction<void()> Callback;
	};
	struct FHeldChannelDelivery
	{
		FSocketDelivery Delivery;
		TSharedPtr<FSocketInbound, ESPMode::ThreadSafe> Origin;
		uint64 ChannelEpoch = 0;
	};
	struct FChannelData
	{
		FString Name;
		uint64 Epoch = 1, BacklogFence = 0;
		bool bUp = false, bReady = false, bHoldInbound = false, bReleaseInbound = false, bNeedsDisconnected = false, bEverUp = false;
		bool bWaitingReady = false;
		double ReadySince = 0.0, LastReadyTick = 0.0;
		bool bReadyHitchProbeUsed = false;
		FString ReadyRoute;
		bool bReadyRouteSeen = true;
		TSet<uint64> ReadyHolds;
		FVaRestSocketOpen Open;
		TArray<FHeldChannelDelivery> Held;
		TCallbacks<void(const FVaRestSocketOpen&)> Connected, Ready;
		TCallbacks<void(const FVaRestSocketClose&)> Disconnected;
		TCallbacks<void(const FVaRestSocketMessage&)> Message, Unrouted;
		TCallbacks<void(const FVaRestError&)> Error;
		TMap<FSocketTextKey, TCallbacks<void(const FVaRestSocketMessage&)>> Routes;
	};
	FVaRestSocketClose ToClose(const FVaRestSocketEnd& End, int32 Attempt)
	{
		FVaRestSocketClose Out;
		Out.Code = End.Code; Out.Reason = End.Reason; Out.bWasClean = End.bWasClean; Out.bWhileReadPaused = End.bWhileReadPaused;
		Out.By = End.By; Out.Failure = End.Failure; Out.HttpStatus = End.HttpStatus; Out.Attempt = Attempt;
		return Out;
	}
	FString WireReason(FStringView Reason)
	{
		bool bTruncated = false;
		const TArray<uint8> Bytes = CloseReasonBytes(Reason, bTruncated);
		if (bTruncated) UE_LOG(LogVaRestSocket, Warning, TEXT("VaRest socket: close reason truncated to 123 UTF-8 bytes"));
		if (Bytes.IsEmpty()) return {};
		const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		return FString(Text.Length(), Text.Get());
	}
}

struct FSocketData
{
	UVaRestSocketState* const Self;
	TWeakObjectPtr<UVaRestHub> Hub;
	TWeakPtr<FSocketHandle, ESPMode::ThreadSafe> Handle;
	TSharedPtr<FSocketModuleGate, ESPMode::ThreadSafe> Gate;
	TSharedPtr<IVaRestSocketTransport, ESPMode::ThreadSafe> Transport;
	TSharedPtr<IVaRestClock, ESPMode::ThreadSafe> Clock;
	TSharedPtr<FSocketTraceClock, ESPMode::ThreadSafe> TraceClock;
	TSharedPtr<FVaRestConnectivity, ESPMode::ThreadSafe> Connectivity;
	TSharedPtr<IVaRestSocketConnection, ESPMode::ThreadSafe> Connection;
	TSharedPtr<FSocketInbound, ESPMode::ThreadSafe> Inbound;
	TSharedPtr<FSocketOutbox, ESPMode::ThreadSafe> Outbox;
	TSharedPtr<FSocketFrameLog, ESPMode::ThreadSafe> FrameLog;
	EVaRestLogLevel LogLevel = EVaRestLogLevel::Basic;
	FVaRestSocketLimits Limits;
	FVaRestHeartbeat Heartbeat;
	FVaRestProtocolPing ProtocolPing;
	TArray<FVaRestSocketAutoReply> AutoReplies;
	FVaRestSocketAuthPlacement AuthPlacement;
	TArray<FVaRestSocketFrame> AuthFrames;
	bool bAllowSelfSignedTls = false;
	TSharedPtr<IVaRestSocketCodec, ESPMode::ThreadSafe> Codec;
	FVaRestSocketCodecContext CodecContext;
	TArray<FDetached> Detached;
	FVaRestClientWeak Client;
	FDelegateHandle SessionHandle, ConnectivityHandle;
	FDelegateHandle RulesHandle;
	TOptional<EVaRestJsonNaming> NamingOverride;
	TOptional<bool> StrictOverride;
	TSharedPtr<const FVaRestRedactor, ESPMode::ThreadSafe> Redactor;
	/** One attempt's applied policy, independent of later HTTP session rotations. Reset at the next attempt. */
	TSharedPtr<const FVaRestRedactor, ESPMode::ThreadSafe> AppliedRedactor;
	TArray<FString> ProtocolRedactedKeys, ProtocolSecrets;
	int64 ProtocolSecretBytes = 0;
	FName SocketName;
	FString Url;
	FString ReadyRoute;
	FVaRestSocketOpen LastOpen;
	TArray<FString> Protocols;
	FVaRestHeaders Headers;
	TArray<TPair<FString, FString>> Query;
	TOptional<FString> HandshakeOrigin;
	TFunction<FString()> Bearer;
	TFunction<TOptional<bool>(const FVaRestSocketClose&)> ReconnectPredicate;
	FVaRestReconnectPolicy Policy;
	EVaRestSocketBackground BackgroundPolicy =
#if PLATFORM_ANDROID || PLATFORM_IOS
		EVaRestSocketBackground::Disconnect;
#else
		EVaRestSocketBackground::Keep;
#endif
	TWeakObjectPtr<const UObject> Owner;
	bool bOwned = false, bDisposed = false, bDisposing = false, bOperating = false;
	bool bHasClient = false, bEverOpened = false, bStable = false, bBackground = false;
	bool bApplyClientAuth = true;
	bool bStopRequested = false, bRefreshSpent = false, bSkipPreemptiveRefresh = false, bAuthRetry = false;
	bool bInReadyHandler = false;
	bool bAwaitingHeartbeat = false, bHeartbeatHitchProbeUsed = false;
	int32 DispatchDepth = 0, CountedAttempts = 0, ProtocolFailures = 0;
	uint64 Registration = 0, Generation = 1;
	double NextAttemptAt = 0.0, OpenedAt = 0.0, ConnectTimeout = 10.0, CloseTimeout = 5.0;
	double LastSendErrorAt = -1e30;
	double LastHeartbeatTick = 0.0, HeartbeatDeadline = 0.0, NextHeartbeatAt = 0.0;
	double Latency = 0.0, LatencyAverage = 0.0;
	bool bHasLatency = false;
	FVaRestSocketStatus Status;
	FVaRestSocketStats Counters;
	TSharedRef<FVaRestSocketServiceStats, ESPMode::ThreadSafe> ServiceStats = MakeShared<FVaRestSocketServiceStats, ESPMode::ThreadSafe>();
	uint64 SocketId = 0, TraceAttemptId = 0;
	FName TraceEntry;
	bool bTraceCreated = false, bTraceReleased = false;
	bool bTraceDropCapture = false, bTraceBackpressure = false;
	FVaRestSocketHandshake LastHandshake;
	EVaRestTraceSocketAttemptPhase TracePhase = EVaRestTraceSocketAttemptPhase::Connecting;
	FVaRestSocketClose TraceLastClose;
	EVaRestErrorKind TraceLastError = EVaRestErrorKind::None;
	bool bTraceGaveUp = false;
	FVaRestSocketClose AuthClose;
	FRandomStream Random;
	TArray<FOperation> Operations;
	TCallbacks<void(FVaRestSocketHandshake&)> Handshake;
	TCallbacks<void(const FVaRestSocketOpen&)> Connected, Ready;
	TCallbacks<void(const FVaRestSocketClose&)> Disconnected;
	TCallbacks<void(const FVaRestSocketStatus&, const FVaRestSocketClose&)> Retry;
	TCallbacks<void(const FVaRestError&, const FVaRestSocketClose&)> GaveUp;
	TCallbacks<void(EVaRestSocketState, EVaRestSocketState)> StateChanged;
	TCallbacks<void(const FVaRestError&)> Error;
	TCallbacks<void()> Drained;
	TCallbacks<void(const FVaRestSocketMessage&)> Message;
	TCallbacks<void(const FVaRestSocketMessage&)> Unrouted;
	TMap<FSocketTextKey, TCallbacks<void(const FVaRestSocketMessage&)>> Routes;
	TMap<FSocketTextKey, TSharedPtr<FSocketJsonPending>> Pending;
	int64 NextRequestId = 0;
	TArray<TSharedRef<IVaRestSocketStage>> Stages;
	TWeakObjectPtr<UObject> ProtocolState;
	TSet<uint64> ReadyHolds;
	TArray<FStageTimer> StageTimers;
	TArray<FVaRestSocketFrame> ClosingFrames;
	int64 ClosingBytes = 0;
	bool bReadyRouteSeen = true, bClosingHook = false;
	TMap<FSocketTextKey, TSharedPtr<FChannelData>> Channels;
	FString ReadyLaneChannel;
	uint64 NextRouteOrder = 1;
	int64 HeldChannelBytes = 0;
	int32 HeldChannelEvents = 0;
	bool bReleasingHeld = false;
	bool bChannelIdentityFrozen = false;
	TCallbacks<void(TConstArrayView<uint8>, const FVaRestSocketMessage&)> Binary;

	explicit FSocketData(UVaRestSocketState* InSelf) : Self(InSelf) {}
	bool IsWaiting() const { return Status.State == EVaRestSocketState::Connecting || Status.State == EVaRestSocketState::Reconnecting; }
	double Now() const { return Clock ? Clock->Now() : FPlatformTime::Seconds(); }
	bool IsOnline() const { return !Connectivity || Connectivity->IsOnline(); }
	FVaRestSocketStatus GetStatusForCallback() const
	{
		auto Out = Status;
		Out.NextAttemptIn = IsWaiting() && !Out.bAttemptInFlight && Out.WaitReason != EVaRestSocketWait::Auth
			&& Out.WaitReason != EVaRestSocketWait::Background ? FMath::Max(0.0, NextAttemptAt - Now()) : 0.0;
		return Out;
	}
	void BumpGeneration() { check(Generation != MAX_uint64); ++Generation; if (Outbox) Outbox->SetConnectionGeneration(Generation); }

	template <typename Signature, typename... Args> void Notify(const TCallbacks<Signature>& Handlers, Args&&... Arguments)
	{
		check(IsInGameThread());
		if (bDisposed) return;
		const auto Snapshot = Handlers;
		++DispatchDepth;
		ON_SCOPE_EXIT { --DispatchDepth; };
		for (const auto& Entry : Snapshot)
		{
			if (bDisposed) break;
			if (Entry->IsAlive()) Entry->Function(Arguments...);
		}
	}
	void ChangeState(EVaRestSocketState To)
	{
		const auto Old = Status.State;
		Status.State = To;
		if (Old != To) Notify(StateChanged, Old, To);
	}
	void Queue(FOperation Operation)
	{
		check(IsInGameThread());
		Operations.Add(MoveTemp(Operation));
		DrainOperations();
	}
	void DrainOperations();
	void Run(FOperation Operation);
	void Tick();
	void BeginAttempt();
	void Open(const FVaRestSocketEvent& Event);
	void BecomeReady();
	EVaRestSendResult SendUnit(FSocketSendUnit Unit, uint64 Epoch);
	EVaRestSendResult Flush(uint64 TargetSequence = 0);
	void ReportSendErrors();
	void ReportTraceNotes(bool bTerminal = false);
	FVaRestSocketStats GetStats() const;
	TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> TraceRedactor() const;
	void HoldAppliedSecrets(const FVaRestSocketHandshake& Attempt);
	FVaRestTraceSocket TraceRecord(bool bPartial) const;
	FVaRestTraceSocketAttempt TraceAttemptRecord(bool bPartial) const;
	void EnsureTraceCreated();
	void TraceReleased();
	void TraceAttempt(EVaRestTraceSocketAttemptPhase Phase, const FVaRestSocketClose& Close = {}, EVaRestErrorKind FailureKind = EVaRestErrorKind::None, bool bGaveUp = false);
	void TraceNote(FVaRestTraceSocketNote Note, bool bCurrentAttempt = true);
	FVaRestSocketFrameCapture PrepareFrameTrace(const FSocketSendUnit& Unit, int32 Index);
	FSocketFrameLogCapture PrepareFrameLog(const FSocketSendUnit& Unit, int32 Index);
	void LogAttempt(EVaRestTraceSocketAttemptPhase Phase, const FVaRestSocketClose& Close, bool bGaveUp);
	void LogRetry(const FVaRestSocketClose& Close) const;
	template <typename T> void EmitTrace(EVaRestTraceKind Kind, T Payload)
	{
		++DispatchDepth;
		FVaRestTrace::EmitAfterAttachmentOnGT(Kind, Now(), FVaRestTracePayload(TInPlaceType<T>(), MoveTemp(Payload)), EVaRestTraceSource::Socket);
		--DispatchDepth;
	}
	void ResetHeartbeat();
	void SendHeartbeat(bool bReply);
	void TickHeartbeat();
	void DeliverMessage(const FSocketDelivery& Delivery);
	bool RouteDelivery(const FSocketDelivery& Delivery, const TSharedPtr<FSocketInbound, ESPMode::ThreadSafe>& Origin);
	void Lose(const FVaRestSocketEnd& End, FVaRestJson ReasonJson = {}, EVaRestErrorKind KindOverride = EVaRestErrorKind::None, TOptional<bool> StageDecision = {});
	void Schedule(FVaRestSocketClose Close, EVaRestErrorKind Kind, double Floor = 0.0);
	void GiveUp(const FVaRestSocketClose& Close, EVaRestErrorKind Kind, FStringView Detail = {});
	void Refresh(const FVaRestSocketClose& Close, bool bReactive);
	void RefreshResult(const FOperation& Operation);
	void CloseNow(FOperation Operation);
	void RequestClose(int32 Code, FStringView Reason);
	void Detach(int32 Code, FStringView Reason);
	bool AbortTransport(FStringView Reason, EVaRestSocketCloseBy By, EVaRestSocketFailure Failure);
	void StopInbound();
	void BindClient(FVaRestClient InClient, bool bApplyCredentials = true);
	void UnbindClient();
	void SessionChanged(EVaRestSessionChange Change);
	void RefreshConversionOptions();
	bool PlaceAuth(FVaRestSocketHandshake& Attempt, FString& Failure);
	void Dispose();
	void PruneHandlers();
	FVaRestSocketRequestHandle StartRequest(FSocketJsonRequestOptions Options);
	void FinishRequest(const FSocketTextKey& Key, FVaRestJson Value, FVaRestError Failure, const FVaRestSocketMessage* ReplyMessage = nullptr);
	void FailRequests(EVaRestErrorKind Kind, bool bSentOnly, FStringView Reason);
	void TickRequests();
	bool DeliverReply(const FVaRestSocketMessage& Incoming);
	void SentRequest(FStringView Key);
	void FailedRequest(FStringView Key, EVaRestErrorKind Kind, FStringView Reason);
	void TickStageTimers();
	void DrainTickOperations();
	void InvokeClosing(int32 Code, FStringView Reason, bool bReplacement = false);
	bool StageInbound(const FVaRestSocketMessage& Incoming);
	bool ProvidesHeartbeat() const;
	EVaRestSendResult WriteUnit(FSocketSendUnit& Unit);
	TSharedPtr<FChannelData> FindChannel(FStringView Name, bool bCreate = false);
	FString ResolveChannel(FStringView Name) const { return Name.IsEmpty() ? CodecContext.DefaultChannel : FString(Name); }
	void PublishChannel(const FChannelData& Channel);
	void UpChannel(FStringView Name, FVaRestSocketOpen Info, uint64 Holder = 0);
	void DownChannel(FStringView Name, FVaRestSocketClose Close, EVaRestChannelEnd End);
	void ReadyChannel(const TSharedPtr<FChannelData>& Channel);
	void ReleaseHeldChannels();
	void DropHeldChannel(FChannelData& Channel);
	void MarkChannelsDown();
	void NotifyChannelsDown(const FVaRestSocketClose& Close);
	void FailChannelRequests(FStringView Name, EVaRestErrorKind Kind, bool bSentOnly, FStringView Reason);
	bool ControlsChannels() const;
	void TickChannelDeadlines();
	template <typename F> void EachStage(F&& Function, bool bClosing = false)
	{
		const auto Snapshot = Stages;
		const uint64 CapturedGeneration = Generation;
		++DispatchDepth;
		for (const auto& Stage : Snapshot)
		{
			if (bDisposed || Generation != CapturedGeneration) break;
			FVaRestSocketStageContext Context(Self, CapturedGeneration, bClosing, reinterpret_cast<UPTRINT>(&Stage.Get()));
			Function(Stage.Get(), Context);
		}
		--DispatchDepth;
	}
	FVaRestError MakeError(EVaRestErrorKind Kind, const FVaRestSocketClose& Close, FStringView Detail = {}) const;
	FRetryDecision Classify(const FVaRestSocketClose& Close);
	double Delay(double Floor);
};

TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> FSocketData::TraceRedactor() const
{
	FVaRestRedactRules Rules = Redactor ? Redactor->GetRules() : FVaRestRedactRules();
	if (AppliedRedactor) Rules.Append(AppliedRedactor->GetRules());
	if (Codec) Codec->ExtendRedactionRules(Rules);
	for (const auto& Key : ProtocolRedactedKeys) Rules.AddKey(Key);
	if (AuthPlacement.Mode != EVaRestSocketAuthPlacement::Header && !AuthPlacement.Name.IsEmpty() && FVaRestRedactRules::IsAcceptableKey(AuthPlacement.Name)) Rules.AddKey(AuthPlacement.Name);
	auto Secrets = Redactor ? Redactor->GetSecrets() : TArray<FString>();
	if (AppliedRedactor) for (const auto& Secret : AppliedRedactor->GetSecrets())
		if (!Secrets.ContainsByPredicate([&](const auto& Existing) { return SameText(Existing, Secret); })) Secrets.Add(Secret);
	for (const auto& Secret : ProtocolSecrets) if (!Secrets.ContainsByPredicate([&](const auto& Existing) { return SameText(Existing, Secret); })) Secrets.Add(Secret);
	return MakeShared<const FVaRestRedactor, ESPMode::ThreadSafe>(MoveTemp(Rules), MoveTemp(Secrets));
}
void FSocketData::HoldAppliedSecrets(const FVaRestSocketHandshake& Attempt)
{
	auto Applied = TraceRedactor(); auto Secrets = Applied->GetSecrets();
	const auto& Rules = Applied->GetRules();
	for (const auto& Header : Attempt.Headers.GetPairs())
	{
		if (Rules.ShouldRedact(Header.Key, EVaRestRedactKeyKind::Header) && !Header.Value.IsEmpty()) Secrets.Add(Header.Value);
		if (Header.Key.Equals(TEXT("Authorization"), ESearchCase::IgnoreCase) && Header.Value.StartsWith(TEXT("Bearer "), ESearchCase::IgnoreCase)) Secrets.Add(Header.Value.Mid(7));
	}
	for (const auto& Parameter : Attempt.Query)
		if (Rules.ShouldRedact(Parameter.Key, EVaRestRedactKeyKind::Query) && !Parameter.Value.IsEmpty()) Secrets.Add(Parameter.Value);
	int32 Question = INDEX_NONE;
	if (Attempt.Url.FindChar(TEXT('?'), Question))
	{
		FString Tail = Attempt.Url.Mid(Question + 1); int32 Fragment = INDEX_NONE;
		if (Tail.FindChar(TEXT('#'), Fragment)) Tail.LeftInline(Fragment);
		TArray<FString> Fields; Tail.ParseIntoArray(Fields, TEXT("&"));
		for (const auto& Field : Fields)
		{
			int32 Equal = INDEX_NONE; if (!Field.FindChar(TEXT('='), Equal)) continue;
			const auto Key = VaRest::Http::PercentDecode(Field.Left(Equal));
			if (Rules.ShouldRedact(Key, EVaRestRedactKeyKind::Query)) Secrets.Add(VaRest::Http::PercentDecode(Field.Mid(Equal + 1)));
		}
	}
	Redactor = MakeShared<const FVaRestRedactor, ESPMode::ThreadSafe>(Rules, MoveTemp(Secrets));
}
FVaRestTraceSocket FSocketData::TraceRecord(bool bPartial) const
{
	FVaRestTraceSocket Record; Record.SocketId = SocketId; Record.ScopeId = Hub.IsValid() ? Hub->GetScopeId() : 0;
	Record.Name = SocketName; Record.Entry = TraceEntry;
	const FName Tag = Codec ? Codec->GetProtocolTag() : NAME_None;
	Record.Protocol = Tag == TEXT("WebSocket") ? FName(TEXT("ws")) : Tag;
	const auto Applied = TraceRedactor();
	Record.Url = Applied->RedactUrl(LastHandshake.Url.IsEmpty() ? Url : LastHandshake.Url);
	static const FName StatesByIndex[] = {FName(TEXT("Disconnected")), FName(TEXT("Connecting")), FName(TEXT("Connected")), FName(TEXT("Reconnecting"))};
	Record.State = StatesByIndex[static_cast<uint8>(Status.State)];
	Record.bReady = Status.bReady; Record.bPartial = bPartial; Record.CurrentAttemptId = TraceAttemptId;
	if (Owner.IsValid()) Record.Initiator.OwnerPath = Owner->GetPathName();
	return Record;
}
FVaRestTraceSocketAttempt FSocketData::TraceAttemptRecord(bool bPartial) const
{
	FVaRestTraceSocketAttempt Record; Record.SocketId = SocketId; Record.AttemptId = TraceAttemptId; Record.Phase = TracePhase; Record.bPartial = bPartial;
	const auto Applied = TraceRedactor(); Record.Url = Applied->RedactUrl(LastHandshake.Url.IsEmpty() ? Url : LastHandshake.Url);
	Record.Protocols = LastHandshake.Protocols;
	if (bPartial || FVaRestTrace::Wants(EVaRestTraceDetail::Headers)) Record.HandshakeHeaders = Applied->RedactHeaders(LastHandshake.Headers);
	Record.Protocol = LastOpen.Protocol; Record.ConnectMs = LastOpen.ConnectSeconds * 1000.0;
	Record.Close.Code = TraceLastClose.Code; Record.Close.Reason = Applied->ScrubText(TraceLastClose.Reason); Record.Close.bClean = TraceLastClose.bWasClean;
	switch (TraceLastClose.By)
	{
	case EVaRestSocketCloseBy::Client: Record.Close.By = EVaRestTraceSocketCloseBy::Client; break;
	case EVaRestSocketCloseBy::Server: Record.Close.By = EVaRestTraceSocketCloseBy::Server; break;
	case EVaRestSocketCloseBy::Heartbeat: Record.Close.By = EVaRestTraceSocketCloseBy::Heartbeat; break;
	case EVaRestSocketCloseBy::Timeout: Record.Close.By = EVaRestTraceSocketCloseBy::Timeout; break;
	default: Record.Close.By = EVaRestTraceSocketCloseBy::Transport; break;
	}
	Record.Close.bWillReconnect = TraceLastClose.bWillReconnect; Record.Close.Delay = TraceLastClose.ReconnectDelay;
	Record.Close.Failure = FName(LexToString(TraceLastClose.Failure)); Record.Close.HttpStatus = TraceLastClose.HttpStatus;
	Record.Close.bWhileReadPaused = TraceLastClose.bWhileReadPaused;
	Record.Close.bGaveUp = bTraceGaveUp; Record.Close.ErrorKind = TraceLastError; return Record;
}
void FSocketData::EnsureTraceCreated()
{
	if (bTraceCreated) return;
	bTraceCreated = true;
	if (FVaRestTrace::Detail() != EVaRestTraceDetail::None || FVaRestTrace::IsAttachingOnGT()) EmitTrace(EVaRestTraceKind::Socket, TraceRecord(false));
}
void FSocketData::TraceReleased()
{
	if (bTraceReleased) return;
	EnsureTraceCreated(); bTraceReleased = true;
	if (FVaRestTrace::Detail() == EVaRestTraceDetail::None && !FVaRestTrace::IsAttachingOnGT()) return;
	auto Record = TraceRecord(false); Record.Phase = EVaRestTraceSocketLifecycle::Released; EmitTrace(EVaRestTraceKind::Socket, MoveTemp(Record));
}
void FSocketData::TraceAttempt(EVaRestTraceSocketAttemptPhase Phase, const FVaRestSocketClose& Close, EVaRestErrorKind FailureKind, bool bGaveUp)
{
	EnsureTraceCreated(); if (Phase == EVaRestTraceSocketAttemptPhase::Connecting || !TraceAttemptId) TraceAttemptId = Generation;
	LogAttempt(Phase, Close, bGaveUp);
	TracePhase = Phase; TraceLastClose = Close; TraceLastError = FailureKind; bTraceGaveUp = bGaveUp;
	if (FVaRestTrace::Detail() != EVaRestTraceDetail::None || FVaRestTrace::IsAttachingOnGT()) EmitTrace(EVaRestTraceKind::SocketAttempt, TraceAttemptRecord(false));
}
void FSocketData::TraceNote(FVaRestTraceSocketNote Note, bool bCurrentAttempt)
{
	if (FVaRestTrace::Detail() == EVaRestTraceDetail::None) return;
	Note.SocketId = SocketId; if (bCurrentAttempt) Note.AttemptId = TraceAttemptId;
	const auto Applied = TraceRedactor(); Note.Message = Applied->ScrubText(Note.Message); Note.Channel = Applied->ScrubText(Note.Channel);
	Note.Route = Applied->ScrubText(Note.Route); Note.MsgId = Applied->ScrubText(Note.MsgId); Note.PublicId = Applied->ScrubText(Note.PublicId);
	EmitTrace(EVaRestTraceKind::SocketNote, MoveTemp(Note));
}
FVaRestSocketFrameCapture FSocketData::PrepareFrameTrace(const FSocketSendUnit& Unit, int32 Index)
{
	if (FVaRestTrace::Detail() == EVaRestTraceDetail::None || !Unit.TraceStamps.IsValidIndex(Index)) return {};
	const auto& NativeFrame = Unit.Frames[Index]; FVaRestTraceSocketFrame Record;
	Record.SocketId = SocketId; Record.AttemptId = TraceAttemptId; Record.FrameId = Unit.TraceStamps[Index].Id;
	Record.CalledAt = Unit.TraceStamps[Index].CalledAt; Record.Dir = EVaRestTraceSocketDirection::Out;
	Record.Kind = NativeFrame.Kind == EVaRestSocketFrameKind::Binary ? EVaRestTraceSocketFrameKind::Binary : EVaRestTraceSocketFrameKind::Text;
	Record.Channel = Unit.Channel; Record.Lane = Unit.Flags == EVaRestSend::Handshake ? EVaRestTraceSocketLane::Bypass : (Unit.bReadyLane ? EVaRestTraceSocketLane::Ready : EVaRestTraceSocketLane::Backlog);
	Record.ProtocolTag = Codec ? Codec->GetProtocolTag() : NAME_None;
	if (NativeFrame.Metadata)
	{
		Record.Route = NativeFrame.Metadata->Route; Record.MsgId = NativeFrame.Metadata->Id; Record.ProtocolTag = NativeFrame.Metadata->ProtocolTag;
		Record.MsgKind = static_cast<EVaRestTraceSocketMessageKind>(NativeFrame.Metadata->Kind); Record.AttachmentSizes = NativeFrame.Metadata->AttachmentSizes;
		Record.bAutoReplied = NativeFrame.Metadata->bAutoReplied;
	}
	const int32 JsonOffset = NativeFrame.Metadata && NativeFrame.Metadata->ExcerptJsonOffset >= 0 ? NativeFrame.Metadata->ExcerptJsonOffset
		: (Codec ? Codec->GetExcerptJsonOffset(NativeFrame.GetPayload()) : -1);
	const bool bAllValues = (NativeFrame.Metadata && NativeFrame.Metadata->bRedactAllExcerptValues) || (Codec && Codec->RedactAllOutgoingExcerptValues(NativeFrame.GetPayload()));
	return FVaRestTrace::PrepareSocketFrame(Now(), MoveTemp(Record), NativeFrame.GetPayload(), TraceRedactor(), JsonOffset, bAllValues);
}
FSocketFrameLogCapture FSocketData::PrepareFrameLog(const FSocketSendUnit& Unit, int32 Index)
{
	if (!FrameLog || SocketLogLevel(LogLevel) != EVaRestLogLevel::Body) return {};
	const auto& Frame = Unit.Frames[Index];
	const int32 JsonOffset = Frame.Metadata && Frame.Metadata->ExcerptJsonOffset >= 0 ? Frame.Metadata->ExcerptJsonOffset
		: (Codec ? Codec->GetExcerptJsonOffset(Frame.GetPayload()) : -1);
	const bool bAllValues = (Frame.Metadata && Frame.Metadata->bRedactAllExcerptValues) || (Codec && Codec->RedactAllOutgoingExcerptValues(Frame.GetPayload()));
	auto Capture = FrameLog->Prepare(SocketId, TraceAttemptId, true, Frame.Kind, Frame.GetPayload(), TraceRedactor(), JsonOffset, bAllValues);
	Capture.SetMetadata(Unit.Channel, Frame.Metadata ? FStringView(Frame.Metadata->Route) : FStringView(), Frame.Metadata ? Frame.Metadata->ProtocolTag : (Codec ? Codec->GetProtocolTag() : NAME_None));
	return Capture;
}
void FSocketData::LogAttempt(EVaRestTraceSocketAttemptPhase Phase, const FVaRestSocketClose& Close, bool bGaveUp)
{
	const auto Effective = SocketLogLevel(LogLevel);
	if (Effective == EVaRestLogLevel::None || !UE_LOG_ACTIVE(LogVaRestSocket, Log)) return;
	const auto Applied = TraceRedactor();
	const auto Id = static_cast<unsigned long long>(SocketId);
	if (Phase == EVaRestTraceSocketAttemptPhase::Connecting)
	{
		const auto SafeUrl = SocketLogText(*Applied, Applied->RedactUrl(LastHandshake.Url));
		UE_LOG(LogVaRestSocket, Log, TEXT("Socket %llu connecting attempt %d %s"), Id, Status.Attempt, *SafeUrl);
		if (Effective >= EVaRestLogLevel::Headers)
		{
			const auto SafeHeaders = Applied->RedactHeaders(LastHandshake.Headers);
			for (const auto& Header : SafeHeaders.GetPairs())
				UE_LOG(LogVaRestSocket, Log, TEXT("Socket %llu > %s: %s"), Id, *SocketLogText(*Applied, Header.Key), *SocketLogText(*Applied, Header.Value));
			if (LastHandshake.Origin.IsSet())
				UE_LOG(LogVaRestSocket, Log, TEXT("Socket %llu > Origin: %s"), Id, *SocketLogText(*Applied, Applied->RedactHeaderValue(TEXTVIEW("Origin"), LastHandshake.Origin.GetValue())));
			for (const auto& Protocol : LastHandshake.Protocols)
				UE_LOG(LogVaRestSocket, Log, TEXT("Socket %llu > subprotocol: %s"), Id, *SocketLogText(*Applied, Protocol));
		}
	}
	else if (Phase == EVaRestTraceSocketAttemptPhase::Open)
	{
		UE_LOG(LogVaRestSocket, Log, TEXT("Socket %llu open %.2f ms protocol=%s"), Id, LastOpen.ConnectSeconds * 1000.0, *SocketLogText(*Applied, LastOpen.Protocol));
	}
	else
	{
		UE_LOG(LogVaRestSocket, Log, TEXT("Socket %llu closed code=%d reason=%s clean=%d by=%d read-paused=%d"), Id, Close.Code,
			*SocketLogText(*Applied, Close.Reason), Close.bWasClean, static_cast<int32>(Close.By), Close.bWhileReadPaused);
		if (bGaveUp) UE_LOG(LogVaRestSocket, Log, TEXT("Socket %llu gave up after attempt %d"), Id, Status.Attempt);
	}
}
void FSocketData::LogRetry(const FVaRestSocketClose& Close) const
{
	if (SocketLogLevel(LogLevel) != EVaRestLogLevel::None)
		UE_LOG(LogVaRestSocket, Log, TEXT("Socket %llu retry scheduled after attempt %d in %.3f seconds code=%d"),
			static_cast<unsigned long long>(SocketId), Status.Attempt, Close.ReconnectDelay, Close.Code);
}

void FSocketData::DrainOperations()
{
	if (bOperating || DispatchDepth != 0) return;
	const TStrongObjectPtr<UVaRestSocketState> Pin(Self);
	TGuardValue<bool> Guard(bOperating, true);
	// FIFO: a callback's Close then Connect is applied in that order, without entering the transport recursively.
	while (!Operations.IsEmpty())
	{
		FOperation Operation = MoveTemp(Operations[0]);
		Operations.RemoveAt(0);
		if (!bDisposed || Operation.Type == EOperation::Dispose) Run(MoveTemp(Operation));
	}
}

void FSocketData::RequestClose(int32 Code, FStringView Reason)
{
	check(IsInGameThread());
	if (bDisposed) return;
	if (Status.State == EVaRestSocketState::Disconnected)
	{
		if (Outbox) Outbox->Clear();
		if (!Pending.IsEmpty()) { FOperation Cancel{EOperation::Close}; Queue(MoveTemp(Cancel)); }
		return;
	}
	bool bReplaced = false;
	const int32 WireCode = SanitizeCloseCode(Code, bReplaced);
	if (bReplaced) UE_LOG(LogVaRestSocket, Warning, TEXT("VaRest socket: invalid close code replaced with 1000"));
	FOperation Operation{EOperation::Close};
	Operation.Code = WireCode; Operation.Reason = WireReason(Reason); Operation.OldState = Status.State;
	BumpGeneration(); // at the CALL, including inside a message handler
	if (Outbox) Outbox->Clear();
	bStopRequested = true;
	Status.State = EVaRestSocketState::Disconnected;
	Status.bReady = false; Status.bAttemptInFlight = false; Status.WaitReason = EVaRestSocketWait::None;
	Status.NextAttemptIn = 0.0;
	Queue(MoveTemp(Operation));
}

void FSocketData::StopInbound()
{
	if (!Inbound) return;
	Inbound->StopAndWaitOnGT(); const auto Completed = Inbound->GetStatsOnGT();
	Counters.FramesIn += Completed.FramesIn; Counters.BytesIn += Completed.BytesIn; Counters.DecodeErrors += Completed.DecodeErrors;
	Inbound.Reset();
}

void FSocketData::Detach(int32 Code, FStringView Reason)
{
	AuthFrames.Reset();
	MarkChannelsDown(); // release every held transport charge before its pipeline resets the connection
	StageTimers.Reset(); ReadyHolds.Reset();
	StopInbound();
	if (!Connection) return;
	Connection->Close(Code, Reason);
	Detached.Add({MoveTemp(Connection), Now() + FMath::Min(CloseTimeout, 5.0)});
}
bool FSocketData::AbortTransport(FStringView Reason, EVaRestSocketCloseBy By, EVaRestSocketFailure Failure)
{
	if (!Connection || !Connection->Abort(Reason, By, Failure)) return false;
	AuthFrames.Reset(); MarkChannelsDown(); StageTimers.Reset(); ReadyHolds.Reset(); StopInbound();
	Detached.Add({MoveTemp(Connection), Now() + FMath::Min(CloseTimeout, 5.0)});
	return true;
}

void FSocketData::CloseNow(FOperation Operation)
{
	if (Operation.OldState == EVaRestSocketState::Connected) InvokeClosing(Operation.Code, Operation.Reason);
	Detach(Operation.Code, Operation.Reason);
	bSkipPreemptiveRefresh = false;
	FVaRestSocketClose Close;
	Close.Code = Operation.OldState == EVaRestSocketState::Connected ? Operation.Code : 0;
	Close.Reason = MoveTemp(Operation.Reason); Close.By = EVaRestSocketCloseBy::Client; Close.Attempt = Status.Attempt;
	Close.Failure = Operation.OldState == EVaRestSocketState::Connected ? EVaRestSocketFailure::None : EVaRestSocketFailure::Canceled;
	if (Operation.OldState != EVaRestSocketState::Disconnected) TraceAttempt(EVaRestTraceSocketAttemptPhase::Closed, Close, EVaRestErrorKind::Canceled);
	FailRequests(EVaRestErrorKind::Canceled, false, TEXTVIEW("WebSocket request canceled by Close"));
	if (Operation.OldState != EVaRestSocketState::Disconnected)
	{
		EachStage([&](auto& Stage, auto& Context) { Stage.OnConnectionClosed(Close, Context); });
		Notify(StateChanged, Operation.OldState, EVaRestSocketState::Disconnected);
		Notify(Disconnected, Close);
		NotifyChannelsDown(Close);
	}
}

void FSocketData::Run(FOperation Operation)
{
	switch (Operation.Type)
	{
	case EOperation::Connect:
		if (Status.State != EVaRestSocketState::Disconnected) break;
		bStopRequested = false; bEverOpened = false; bStable = false; bRefreshSpent = false;
		CountedAttempts = 0; ProtocolFailures = 0; Status.Attempt = 0;
		Status.bReady = false; Status.bAttemptInFlight = false;
		Status.WaitReason = bBackground ? EVaRestSocketWait::Background : EVaRestSocketWait::None;
		NextAttemptAt = Now(); ChangeState(EVaRestSocketState::Connecting);
		break;
	case EOperation::Close: CloseNow(MoveTemp(Operation)); break;
	case EOperation::Stop:
		if (IsWaiting()) RequestClose(1000, TEXTVIEW("canceled"));
		break;
	case EOperation::ReconnectNow:
		if (Status.State == EVaRestSocketState::Connected)
		{
			Operation.Type = EOperation::Restart; Operation.Reason = TEXT("reconnect"); Run(MoveTemp(Operation));
		}
		else if (IsWaiting() && !Status.bAttemptInFlight && Status.WaitReason != EVaRestSocketWait::Auth)
		{
			NextAttemptAt = Now(); Status.WaitReason = bBackground ? EVaRestSocketWait::Background : EVaRestSocketWait::None;
		}
		break;
	case EOperation::Restart:
		if (Status.State == EVaRestSocketState::Disconnected) break;
		{
			const bool bWasOpen = Status.State == EVaRestSocketState::Connected;
			const uint64 BeforeClosing = Generation;
			if (bWasOpen && Operation.bFlag) InvokeClosing(1001, Operation.Reason, true);
			if (bDisposed || Generation != BeforeClosing || Status.State == EVaRestSocketState::Disconnected) break;
			BumpGeneration(); Detach(1001, Operation.Reason);
			const uint64 RestartGeneration = Generation;
			if (Operation.Holder) { bAuthRetry = true; bSkipPreemptiveRefresh = true; }
			FailRequests(EVaRestErrorKind::Connection, true, TEXTVIEW("WebSocket connection restarted"));
			if (bDisposed || Generation != RestartGeneration) break;
			Status.bReady = false; Status.bAttemptInFlight = false;
			Status.WaitReason = bBackground ? EVaRestSocketWait::Background : EVaRestSocketWait::None;
			NextAttemptAt = Now(); ChangeState(bEverOpened ? EVaRestSocketState::Reconnecting : EVaRestSocketState::Connecting);
			if (bWasOpen)
			{
				FVaRestSocketClose Close; Close.Code = 1001; Close.Reason = MoveTemp(Operation.Reason);
				Close.By = EVaRestSocketCloseBy::Client; Close.bWillReconnect = true; Close.Attempt = Status.Attempt;
				EachStage([&](auto& Stage, auto& Context) { Stage.OnConnectionClosed(Close, Context); });
				if (bDisposed || Generation != RestartGeneration) break;
				Notify(Disconnected, Close);
				NotifyChannelsDown(Close);
			}
		}
		break;
	case EOperation::RefreshResult: RefreshResult(Operation); break;
	case EOperation::Background:
		if (BackgroundPolicy != EVaRestSocketBackground::Disconnect)
		{
			if (!Operation.bFlag && Status.State == EVaRestSocketState::Connected && Heartbeat.Mode != EVaRestHeartbeatMode::None)
			{ ResetHeartbeat(); SendHeartbeat(Heartbeat.Mode == EVaRestHeartbeatMode::ServerPing); }
			break;
		}
		if (bBackground == Operation.bFlag) break;
		bBackground = Operation.bFlag;
		if (Status.State == EVaRestSocketState::Disconnected) break;
		if (bBackground)
		{
			FOperation Restart{EOperation::Restart}; Restart.Reason = TEXT("background"); Run(MoveTemp(Restart));
			Status.WaitReason = EVaRestSocketWait::Background;
		}
		else { NextAttemptAt = Now(); Status.WaitReason = EVaRestSocketWait::None; }
		break;
	case EOperation::Online:
		if (IsWaiting() && !Status.bAttemptInFlight && Status.WaitReason != EVaRestSocketWait::Auth && !bBackground)
		{
			NextAttemptAt = Now() + (Operation.bFlag ? Random.FRand() : Policy.MaxDelay);
			Status.WaitReason = Operation.bFlag ? EVaRestSocketWait::Backoff : EVaRestSocketWait::Offline;
		}
		break;
	case EOperation::Dispose: Dispose(); break;
	case EOperation::StageReady:
		if (Operation.Generation == Generation)
		{
			if (auto Channel = FindChannel(Operation.Channel))
			{
				if (SameText(Channel->Name, CodecContext.DefaultChannel)) ReadyHolds.Remove(Operation.Holder);
				else Channel->ReadyHolds.Remove(Operation.Holder);
				ReadyChannel(Channel);
			}
		}
		break;
	case EOperation::ChannelUp:
		if (Operation.Generation == Generation) UpChannel(Operation.Channel, MoveTemp(Operation.ChannelOpen), Operation.Holder);
		break;
	case EOperation::ChannelDown:
		if (Operation.Generation == Generation) DownChannel(Operation.Channel, MoveTemp(Operation.ChannelClose), Operation.ChannelEnd);
		break;
	case EOperation::StageClose:
	case EOperation::StageRefresh:
		if (Operation.Generation == Generation && Status.State != EVaRestSocketState::Disconnected)
		{
			const bool bRefresh = Operation.Type == EOperation::StageRefresh;
			const bool bPaused = Connection && Connection->IsReadPaused();
			const bool bAborted = !bRefresh && Operation.bFlag && AbortTransport(Operation.Reason, EVaRestSocketCloseBy::Heartbeat, EVaRestSocketFailure::HeartbeatTimeout);
			if (!bAborted) Detach(bRefresh ? 1000 : (Operation.bFlag ? 1001 : Operation.Code), Operation.Reason);
			FVaRestSocketEnd End; End.Code = bRefresh ? 4401 : (Operation.bFlag ? (bAborted ? 1006 : 1001) : Operation.Code); End.Reason = MoveTemp(Operation.Reason);
			End.bWhileReadPaused = bPaused; if (Operation.bFlag) End.Failure = EVaRestSocketFailure::HeartbeatTimeout;
			End.By = bRefresh ? EVaRestSocketCloseBy::Server : (Operation.bFlag ? EVaRestSocketCloseBy::Heartbeat : EVaRestSocketCloseBy::Transport);
			End.Detail = End.Reason;
			Lose(End, {}, bRefresh ? EVaRestErrorKind::Auth : Operation.Kind, Operation.Reconnect);
		}
		break;
	}
}

void FSocketData::Tick()
{
	check(IsInGameThread());
	if (bDisposed) return;
	if (!Handle.IsValid() || !Hub.IsValid() || (bOwned && !Owner.IsValid()))
	{
		Self->DisposeOnGT(!Handle.IsValid() ? TEXTVIEW("released") : TEXTVIEW("owner destroyed"));
		return;
	}
	EnsureTraceCreated(); DrainOperations();
	if (bDisposed) return;
	{
		TGuardValue<bool> Guard(bOperating, true);
		PruneHandlers();
		const bool bCapture = FVaRestTrace::Detail() != EVaRestTraceDetail::None;
		if (bCapture != bTraceDropCapture) { bTraceDropCapture = bCapture; Outbox->CaptureDropNotes(bCapture); }
		const double Time = Now();
		for (int32 Index = Detached.Num() - 1; Index >= 0; --Index)
		{
			TArray<FVaRestSocketEvent> Events;
			Detached[Index].Connection->PopEvents(Events);
			for (const auto& Event : Events)
				if (Event.Type == EVaRestSocketEventType::Message) Detached[Index].Connection->ConsumedMessages(Event.Bytes.Num(), 1);
			if (Detached[Index].Connection->HasEnded() || Time >= Detached[Index].Deadline) Detached.RemoveAt(Index);
		}
		if (Status.State == EVaRestSocketState::Connected && !bStable && Time - OpenedAt >= Policy.StableAfter)
		{
			bStable = true; CountedAttempts = 0; ProtocolFailures = 0; bRefreshSpent = false;
		}
		// Attempt admission is exclusive to this tick. No lifecycle callback can start a physical connection.
		if (IsWaiting() && !Status.bAttemptInFlight && !bStopRequested && !bBackground
			&& Status.WaitReason != EVaRestSocketWait::Auth && Time >= NextAttemptAt) BeginAttempt();
		const auto Current = Connection;
		const auto Pipeline = Inbound;
		const uint64 CurrentGeneration = Generation;
		if (Current && Pipeline && !bDisposed)
		{
			FSocketDelivery Item;
			const int32 Count = Pipeline->GetDeliveryCountOnGT();
			for (int32 Index = 0; Index < Count && Pipeline->PopOnGT(Item); ++Index)
			{
				bool bConsumed = false;
				ON_SCOPE_EXIT { if (!bConsumed) Pipeline->ConsumedOnGT(Item); };
				if (bDisposed || CurrentGeneration != Generation || Item.Generation != Generation || Current != Connection) continue;
				for (const auto& FrameTrace : Item.TraceFrames)
				{
					if (bDisposed || Generation != CurrentGeneration) break;
					if (FVaRestTrace::Detail() != EVaRestTraceDetail::None) EmitTrace(EVaRestTraceKind::SocketFrame, FrameTrace);
				}
				if (bDisposed || Generation != CurrentGeneration) continue;
				auto& Event = Item.Event;
				if (Event.Type == EVaRestSocketEventType::Open) Open(Event);
				else if (Event.Type == EVaRestSocketEventType::Message)
					bConsumed = RouteDelivery(Item, Pipeline); // true transfers the charge into the channel's held queue
				else if (Event.IsTerminal())
				{
					Pipeline->ConsumedOnGT(Item); bConsumed = true;
					if (Item.bOverflow) Current->Close(1009, TEXTVIEW("delivery backlog exceeded"));
					if (Item.bProtocolError) Current->Close(Event.End.Code == 1009 ? 1009 : 1002, TEXTVIEW("protocol error"));
					Lose(Event.End, MoveTemp(Item.ReasonJson), Item.bProtocolError ? EVaRestErrorKind::Decode : EVaRestErrorKind::None);
				}
				else if (Event.Type == EVaRestSocketEventType::Drained) Notify(Drained);
				else if (Event.Type == EVaRestSocketEventType::Pong)
				{
					Latency = FMath::Max(0.0, Event.Seconds);
					LatencyAverage = bHasLatency ? 0.2 * Latency + 0.8 * LatencyAverage : Latency;
					bHasLatency = true;
					FVaRestTraceSocketNote Note; Note.Kind = EVaRestTraceSocketNoteKind::Ping; Note.Rtt = Latency; TraceNote(MoveTemp(Note));
				}
				else if (Event.Type == EVaRestSocketEventType::Flow)
				{
					FVaRestTraceSocketNote Note; Note.Kind = EVaRestTraceSocketNoteKind::Flow; Note.bPaused = Event.bReadPaused; Note.PendingBytes = Event.PendingBytes; TraceNote(MoveTemp(Note));
				}
				if (!bConsumed) { Pipeline->ConsumedOnGT(Item); bConsumed = true; }
				DrainTickOperations(); // after this event's stable dispatch and charge release, before deadlines/next event
			}
		}
		TickStageTimers();
		DrainTickOperations();
		TickRequests();
		DrainTickOperations();
		TickChannelDeadlines();
		DrainTickOperations();
		TickHeartbeat(); // deadlines follow delivery, so a queued pong wins over its deadline
		Flush();
		ReleaseHeldChannels();
		ReportTraceNotes();
		ReportSendErrors();
	}
	DrainOperations();
}

bool FSocketData::PlaceAuth(FVaRestSocketHandshake& Attempt, FString& Failure)
{
	AuthFrames.Reset();
	if (AuthPlacement.Mode == EVaRestSocketAuthPlacement::Header) return true;
	bool bControlInKey = false;
	for (const TCHAR Character : AuthPlacement.Name) bControlInKey |= Character < 0x20 || Character == 0x7f;
	if (AuthPlacement.Name.IsEmpty() || !FVaRestRedactRules::IsAcceptableKey(AuthPlacement.Name) || bControlInKey)
	{ Failure = TEXT("The socket authentication field must be a nonempty redaction-compatible key"); return false; }
	const FString Authorization = Attempt.Headers.Get(TEXTVIEW("Authorization"));
	if (Authorization.IsEmpty()) return true; // Cookie/session headers need no Bearer placement.
	if (!Authorization.StartsWith(TEXT("Bearer "), ESearchCase::IgnoreCase))
	{ Failure = TEXT("Query and FirstMessage placement require Bearer authentication"); return false; }
	const FString Credential = Authorization.Mid(7);
	Attempt.Headers.Remove(TEXTVIEW("Authorization"));
	FVaRestRedactRules Rules = Redactor ? Redactor->GetRules() : FVaRestRedactRules();
	TArray<FString> Secrets = Redactor ? Redactor->GetSecrets() : TArray<FString>();
	Rules.AddKey(AuthPlacement.Name); if (!Credential.IsEmpty()) Secrets.Add(Credential);
	Redactor = MakeShared<const FVaRestRedactor, ESPMode::ThreadSafe>(MoveTemp(Rules), MoveTemp(Secrets));
	if (AuthPlacement.Mode == EVaRestSocketAuthPlacement::Query)
	{
		Attempt.Query.RemoveAll([this](const auto& Pair) { return SameText(Pair.Key, AuthPlacement.Name); });
		Attempt.Query.Emplace(AuthPlacement.Name, Credential); return true;
	}
	const auto& Routing = CodecContext.Routing;
	if (Routing.Envelope.DataPath.IsEmpty() && (SameText(AuthPlacement.Name, Routing.RouteField)
		|| SameText(AuthPlacement.Name, Routing.IdField) || (Routing.bJsonRpc && SameText(AuthPlacement.Name, TEXTVIEW("jsonrpc")))))
	{ Failure = TEXT("The authentication field collides with a socket envelope identity field"); return false; }
	const auto Payload = FVaRestJson::Object().Set(AuthPlacement.Name, Credential);
	auto Write = [&Payload](VaRest::Json::FCondensedWriter& Writer, bool bMembers, TConstArrayView<FString> Skip, FVaRestJsonError& Error)
	{ return WriteJsonPayload(Writer, Payload, bMembers, Skip, Error); };
	const FVaRestSocketPayloadWriter View(Write, true);
	auto Encoded = Codec->Encode(CodecContext.DefaultChannel, AuthPlacement.Route, View, {}, CodecContext);
	if (!Encoded.HasValue()) { Failure = TEXT("The codec refused the first authentication message"); return false; }
	AuthFrames = Encoded.StealValue();
	if (AuthFrames.IsEmpty()) { Failure = TEXT("The codec produced no authentication frame"); return false; }
	return true;
}

void FSocketData::BeginAttempt()
{
	check(IsInGameThread() && bOperating && DispatchDepth == 0);
	bChannelIdentityFrozen = true;
	BumpGeneration();
	Outbox->SetSendLimit(MAX_int64);
	const uint64 StartingGeneration = Generation;
	ProtocolSecrets.Reset(); ProtocolSecretBytes = 0; AppliedRedactor.Reset(); if (!bHasClient) Redactor.Reset();
	if (bHasClient)
	{
		auto Pinned = Client.PinOnGT();
		if (!Pinned.IsValid()) { GiveUp({}, EVaRestErrorKind::Canceled, TEXTVIEW("HTTP client retired")); return; }
		Redactor = Pinned.GetRedactor();
		if (bApplyClientAuth && !bSkipPreemptiveRefresh && Pinned.NeedsAuthRefresh()) { Refresh({}, false); return; }
	}
	bSkipPreemptiveRefresh = false;
	if (!bAuthRetry && IsOnline() && CountedAttempts < MAX_int32) ++CountedAttempts;
	bAuthRetry = false;
	Status.Attempt = FMath::Max(1, CountedAttempts);
	Status.bAttemptInFlight = true; Status.WaitReason = EVaRestSocketWait::None;
	FVaRestSocketHandshake Attempt;
	Attempt.Url = Url; Attempt.Headers = Headers; Attempt.Query = Query; Attempt.Protocols = Protocols; Attempt.Origin = HandshakeOrigin;
	Attempt.Attempt = Status.Attempt; Attempt.bReconnect = bEverOpened;
	if (bHasClient && bApplyClientAuth)
	{
		auto Pinned = Client.PinOnGT();
		if (!Pinned.IsValid()) { GiveUp({}, EVaRestErrorKind::Canceled, TEXTVIEW("HTTP client retired")); return; }
		FString ConfigError;
		const bool bApplied = Pinned.ApplyAuth(Attempt.Headers, Attempt.Query, &ConfigError);
		Redactor = Pinned.GetRedactor();
		if (!bApplied) { GiveUp({}, ConfigError.IsEmpty() ? EVaRestErrorKind::Auth : EVaRestErrorKind::Config, ConfigError); return; }
	}
	else if (Bearer)
	{
		const auto Provider = Bearer;
		++DispatchDepth;
		FString Token = Provider();
		--DispatchDepth;
		if (bDisposed || Generation != StartingGeneration || bStopRequested) return;
		Attempt.Headers.Set(TEXTVIEW("Authorization"), FString(TEXT("Bearer ")) + Token);
		TArray<FString> Secrets; if (!Token.IsEmpty()) Secrets.Add(MoveTemp(Token));
		Redactor = MakeShared<const FVaRestRedactor, ESPMode::ThreadSafe>(FVaRestRedactRules(), MoveTemp(Secrets));
	}
	FString PlacementFailure;
	if (!PlaceAuth(Attempt, PlacementFailure)) { GiveUp({}, EVaRestErrorKind::Config, PlacementFailure); return; }
	EachStage([&Attempt](auto& Stage, auto& Context) { Stage.OnHandshake(Attempt, Context); });
	if (bDisposed || Generation != StartingGeneration || bStopRequested) return;
	Notify(Handshake, Attempt);
	if (bDisposed || Generation != StartingGeneration || bStopRequested) return;
	if (Attempt.Url.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase)) Attempt.Url = TEXT("wss://") + Attempt.Url.Mid(8);
	else if (Attempt.Url.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase)) Attempt.Url = TEXT("ws://") + Attempt.Url.Mid(7);
	VaRest::Http::AppendQuery(Attempt.Url, Attempt.Query);
	HoldAppliedSecrets(Attempt); AppliedRedactor = Redactor; LastHandshake = Attempt;
	TraceAttempt(EVaRestTraceSocketAttemptPhase::Connecting);
	if (bDisposed || Generation != StartingGeneration || bStopRequested) return;
	FVaRestSocketConnectRequest Request;
	Request.Url = MoveTemp(Attempt.Url); Request.Headers = Attempt.Headers.GetPairs();
	Request.Protocols = MoveTemp(Attempt.Protocols); Request.Origin = MoveTemp(Attempt.Origin);
	Request.ConnectTimeout = ConnectTimeout; Request.CloseTimeout = CloseTimeout;
	Request.bAllowSelfSignedTls = bAllowSelfSignedTls;
	Request.MaxMessageBytes = Limits.MaxMessageBytes; Request.SendHighWater = Limits.SendHighWater;
	Request.MaxPendingInboundBytes = Limits.MaxPendingInboundBytes; Request.bExplicitMessageConsumption = true;
	Request.PingInterval = ProtocolPing.Interval; Request.PingTimeout = ProtocolPing.Timeout; Request.AutoReplies = AutoReplies;
	Request.ServiceStats = ServiceStats;
	Request.LogName = SocketName.IsNone() ? FString() : SocketName.ToString();
	if (!Transport) Transport = GetDefaultTransport();
	// Flow pauses after the crossing message. Keep one assembled-message reserve in the pipeline's hard admission bound.
	const int64 InboundHardLimit = FMath::Min<int64>(Limits.MaxPendingInboundBytes, MAX_int64 - Limits.MaxMessageBytes) + Limits.MaxMessageBytes;
	Inbound = MakeShared<FSocketInbound, ESPMode::ThreadSafe>(Generation, InboundHardLimit, Limits.MaxMessageBytes);
	Inbound->SetHeartbeatOnGT(Heartbeat);
	Inbound->ConfigureOnGT(Codec.ToSharedRef(), CodecContext);
	Inbound->SetTraceOnGT(SocketId, TraceAttemptId, Redactor, TraceClock);
	Inbound->SetFrameLogOnGT(FrameLog.ToSharedRef());
	const TWeakPtr<FSocketInbound, ESPMode::ThreadSafe> WeakInbound(Inbound);
	++Counters.Attempts; Stats::Attempt();
	Connection = Transport->Connect(MoveTemp(Request), [WeakInbound]
	{
		if (auto Native = WeakInbound.Pin()) Native->SchedulePump();
	});
	Inbound->AttachOnGT(Connection.ToSharedRef());
}

void FSocketData::Open(const FVaRestSocketEvent& Event)
{
	if (!IsWaiting() || bStopRequested) return;
	const uint64 OpenGeneration = Generation;
	FVaRestSocketOpen Info; Info.Attempt = Status.Attempt; Info.bReconnect = bEverOpened;
	Info.ConnectSeconds = Event.Seconds; Info.Protocol = Event.Protocol;
	if (Info.bReconnect) ++Counters.Reconnects;
	Stats::Open(Info.bReconnect);
	bEverOpened = true; bStable = false; OpenedAt = Now();
	ResetHeartbeat();
	Status.bAttemptInFlight = false; Status.WaitReason = EVaRestSocketWait::None;
	ReadyHolds.Reset(); bReadyRouteSeen = ReadyRoute.IsEmpty();
	for (auto& Pair : Channels)
	{
		auto& Channel = *Pair.Value;
		Channel.bUp = false; Channel.bReady = false; Channel.bWaitingReady = false; Channel.ReadySince = Now();
		Channel.LastReadyTick = Channel.ReadySince; Channel.bReadyHitchProbeUsed = false;
		Channel.ReadyHolds.Reset(); Channel.bReadyRouteSeen = Channel.ReadyRoute.IsEmpty(); PublishChannel(Channel);
	}
	Status.bReady = false; LastOpen = Info;
	TraceAttempt(EVaRestTraceSocketAttemptPhase::Open);
	if (bDisposed || Generation != OpenGeneration) return;
	ChangeState(EVaRestSocketState::Connected);
	if (bDisposed || Generation != OpenGeneration) return;
	EachStage([&Info](auto& Stage, auto& Context) { Stage.OnOpen(Info, Context); });
	if (bDisposed || Generation != OpenGeneration) return;
	if (!AuthFrames.IsEmpty())
	{
		FSocketSendUnit Unit; Unit.Flags = EVaRestSend::Handshake; Unit.Frames = MoveTemp(AuthFrames);
		Unit.Channel = CodecContext.DefaultChannel;
		FVaRestSocketBatchPin Batch; Batch.ConnectionGeneration = Generation; Batch.Channel = Unit.Channel;
		if (auto Channel = FindChannel({})) Batch.ChannelEpoch = Channel->Epoch;
		Unit.Pin = MoveTemp(Batch);
		for (const auto& Frame : Unit.Frames) Unit.Bytes += Frame.Length;
		const auto Admission = SendUnit(MoveTemp(Unit), Outbox->GetEncoder().Epoch);
		if (Admission == EVaRestSendResult::Rejected || Admission == EVaRestSendResult::Dropped)
		{
			Detach(1001, TEXTVIEW("auth message admission failed"));
			GiveUp({}, EVaRestErrorKind::Config, TEXTVIEW("First authentication message was not admitted")); return;
		}
	}
	if (Connectivity) Connectivity->ReportOnline(); // WS never reports offline
	if (bDisposed || Generation != OpenGeneration) return;
	Notify(Connected, Info);
	if (!bDisposed && Generation == OpenGeneration && !ControlsChannels())
	{
		TArray<TSharedPtr<FChannelData>> Snapshot; Channels.GenerateValueArray(Snapshot);
		for (const auto& Channel : Snapshot) { if (bDisposed || Generation != OpenGeneration) break; UpChannel(Channel->Name, Info); }
	}
}

void FSocketData::BecomeReady()
{
	if (bDisposed || Status.State != EVaRestSocketState::Connected || Status.bReady || !ReadyHolds.IsEmpty() || !bReadyRouteSeen) return;
	if (DispatchDepth)
	{
		FOperation ReadyOperation{EOperation::StageReady}; ReadyOperation.Generation = Generation; ReadyOperation.Channel = CodecContext.DefaultChannel; Queue(MoveTemp(ReadyOperation)); return;
	}
	const auto Channel = FindChannel({});
	if (!Channel || !Channel->bUp) return;
	Flush(); // the bypass lane precedes OnReady's subscriptions
	Status.bReady = true;
	Channel->bReady = true; Channel->bWaitingReady = false; PublishChannel(*Channel);
	{
		TGuardValue<bool> ReadyLane(bInReadyHandler, true);
		TGuardValue<FString> Lane(ReadyLaneChannel, Channel->Name);
		auto Snapshot = Ready; Snapshot.Append(Channel->Ready); Snapshot.StableSort([](const auto& A, const auto& B) { return A->Order < B->Order; });
		const uint64 ReadyGeneration = Generation, ReadyEpoch = Channel->Epoch;
		++DispatchDepth;
		for (const auto& Entry : Snapshot)
		{
			if (bDisposed || Generation != ReadyGeneration || Channel->Epoch != ReadyEpoch) break;
			if (Entry->IsAlive()) Entry->Function(Channel->Open);
		}
		--DispatchDepth;
	}
	Channel->BacklogFence = Outbox->GetLastSequence(); Channel->bReleaseInbound = true;
	Flush();
	ReleaseHeldChannels();
}

EVaRestSendResult FSocketData::SendUnit(FSocketSendUnit Unit, uint64 Epoch)
{
	check(IsInGameThread());
	if (bDisposed || !Outbox) return EVaRestSendResult::Rejected;
	Outbox->Stamp(Unit);
	Unit.Channel = ResolveChannel(Unit.Channel);
	if (!Unit.QueueEpoch) Unit.QueueEpoch = Outbox->GetEncoder(Unit.Channel).QueueEpoch;
	Unit.bReadyLane = bInReadyHandler && SameText(Unit.Channel, ReadyLaneChannel);
	EVaRestSendResult Result = EVaRestSendResult::Queued;
	const bool bOpen = Status.State == EVaRestSocketState::Connected && Connection;
	if (DispatchDepth == 0 && !bOperating && Outbox->TryImmediate(Unit, Epoch, bOpen, Status.bReady,
		Connection ? Connection->GetBufferedAmount() : 0, Result))
	{
		if (Result == EVaRestSendResult::Sent)
			Result = WriteUnit(Unit);
		DrainOperations();
		return Result;
	}
	uint64 Sequence = 0;
	Result = Outbox->Post(MoveTemp(Unit), Epoch, Sequence);
	if (Result == EVaRestSendResult::Queued && DispatchDepth == 0 && !bOperating) Result = Flush(Sequence);
	if (DispatchDepth == 0 && !bOperating) DrainOperations();
	return Result;
}

EVaRestSendResult FSocketData::Flush(uint64 TargetSequence)
{
	if (bDisposed || !Outbox || DispatchDepth != 0) return EVaRestSendResult::Queued;
	EVaRestSendResult TargetResult = EVaRestSendResult::Queued;
	const uint64 FlushGeneration = Generation;
	// Bounded work per tick, including while worker producers keep filling the staging queue.
	for (int32 Work = 0; Work < 256; ++Work)
	{
		FSocketSendUnit Unit;
		EVaRestSendResult Result;
		const bool bOpen = Status.State == EVaRestSocketState::Connected && Connection;
		if (!Outbox->PopNext(bOpen, Status.bReady, Connection ? Connection->GetBufferedAmount() : 0, Unit, Result)) break;
		if (Result == EVaRestSendResult::Sent)
			Result = WriteUnit(Unit);
		if (Unit.Sequence == TargetSequence) TargetResult = Result;
		if (bDisposed || Generation != FlushGeneration) break;
	}
	return TargetResult;
}

FVaRestSocketStats FSocketData::GetStats() const
{
	FVaRestSocketStats Out = Counters;
	Out.AutoRepliesOut = ServiceStats->AutoRepliesSent.load(std::memory_order_relaxed);
	Out.LongestAutoReplyDelay = ServiceStats->LongestAutoReplyDelay.load(std::memory_order_relaxed);
	Out.bOpen = Status.State == EVaRestSocketState::Connected;
	if (Inbound)
	{
		const auto Current = Inbound->GetStatsOnGT();
		Out.FramesIn += Current.FramesIn; Out.BytesIn += Current.BytesIn; Out.DecodeErrors += Current.DecodeErrors;
		Out.PendingInboundBytes = Current.PendingInboundBytes;
	}
	if (Outbox)
	{
		Out.Queued = Outbox->Count(); Out.QueuedBytes = Outbox->GetQueuedBytes();
		const auto Errors = Outbox->GetTotalErrors(); Out.SendRefusals = Errors.Config + Errors.Offline + Errors.Canceled;
	}
	Out.BufferedBytes = Connection ? Connection->GetBufferedAmount() : 0; Out.PendingRequests = Pending.Num();
	Out.bHasLatency = bHasLatency; Out.Latency = Latency; Out.LatencyAverage = LatencyAverage; return Out;
}

void FSocketData::ReportTraceNotes(bool bTerminal)
{
	if ((!bTerminal && bDisposed) || FVaRestTrace::Detail() == EVaRestTraceDetail::None) return;
	const uint64 Before = Generation; const auto Drops = Outbox->TakeDropNotes();
	for (uint8 Index = 0; Index < 5; ++Index) if (Drops.Counts[Index])
	{
		FVaRestTraceSocketNote Note; Note.Kind = EVaRestTraceSocketNoteKind::SendDropped;
		Note.Count = Drops.Counts[Index]; Note.DropReason = static_cast<EVaRestTraceSocketDropReason>(Index); TraceNote(MoveTemp(Note));
		if ((!bTerminal && bDisposed) || Generation != Before) return;
	}
	const bool bHigh = Connection && Status.State == EVaRestSocketState::Connected && Connection->GetBufferedAmount() >= Limits.SendHighWater;
	if (bHigh != bTraceBackpressure)
	{
		bTraceBackpressure = bHigh; FVaRestTraceSocketNote Note;
		Note.Kind = EVaRestTraceSocketNoteKind::Backpressure; Note.bHigh = bHigh; TraceNote(MoveTemp(Note));
	}
}

void FSocketData::ReportSendErrors()
{
	if (!Outbox || bDisposed || Now() - LastSendErrorAt < 1.0) return;
	const auto Counts = Outbox->TakeErrors();
	if (!Counts.Config && !Counts.Offline && !Counts.Canceled) return;
	LastSendErrorAt = Now();
	FVaRestError Failure;
	Failure.Kind = Counts.Config ? EVaRestErrorKind::Config : (Counts.Offline ? EVaRestErrorKind::Offline : EVaRestErrorKind::Canceled);
	Failure.Message = FString::Printf(TEXT("WebSocket send refusals: config=%lld, offline=%lld, canceled=%lld"),
		static_cast<long long>(Counts.Config), static_cast<long long>(Counts.Offline), static_cast<long long>(Counts.Canceled));
	Notify(Error, Failure);
}

void FSocketData::DeliverMessage(const FSocketDelivery& Delivery)
{
	if (DeliverReply(Delivery.Message)) return;
	if (Delivery.Message.Kind == EVaRestSocketMessageKind::Control) return; // stages consume protocol control before app dispatch
	const uint64 MessageGeneration = Generation;
	const auto Channel = FindChannel(Delivery.Message.Channel);
	const uint64 MessageEpoch = Channel ? Channel->Epoch : 0;
	FVaRestSocketMessage AppMessage = Delivery.Message;
	if (AppMessage.Kind == EVaRestSocketMessageKind::Request && AppMessage.bHasId)
	{
		auto Token = MakeShared<FSocketReplyToken, ESPMode::ThreadSafe>();
		Token->Socket = FVaRestSocket(Handle.Pin()).AsWeak(); Token->Codec = Delivery.MessageCodec; Token->Context = Delivery.MessageContext;
		Token->Id = AppMessage.bNumericId ? FVaRestSocketId::Numeric(AppMessage.NumericId) : FVaRestSocketId::String(AppMessage.Id);
		Token->Pin.ConnectionGeneration = Generation; Token->Pin.Channel = AppMessage.Channel;
		Token->Pin.ChannelEpoch = MessageEpoch;
		AppMessage.ReplyToken = MoveTemp(Token);
	}
	auto Handlers = Message;
	if (Channel) Handlers.Append(Channel->Message);
	Handlers.StableSort([](const auto& A, const auto& B) { return A->Order < B->Order; });
	const auto BinarySnapshot = Binary;
	TCallbacks<void(const FVaRestSocketMessage&)> UnroutedSnapshot;
	if (SameText(AppMessage.Channel, CodecContext.DefaultChannel)) UnroutedSnapshot = Unrouted;
	if (Channel) UnroutedSnapshot.Append(Channel->Unrouted);
	UnroutedSnapshot.StableSort([](const auto& A, const auto& B) { return A->Order < B->Order; });
	TCallbacks<void(const FVaRestSocketMessage&)> RouteSnapshot;
	if (SameText(AppMessage.Channel, CodecContext.DefaultChannel))
		if (const auto* Found = Routes.Find(FSocketTextKey(AppMessage.Route))) RouteSnapshot = *Found;
	if (Channel)
		if (const auto* Found = Channel->Routes.Find(FSocketTextKey(AppMessage.Route))) RouteSnapshot.Append(*Found);
	RouteSnapshot.StableSort([](const auto& A, const auto& B) { return A->Order < B->Order; });
	++DispatchDepth;
	for (const auto& Handler : Handlers)
	{
		if (bDisposed || Generation != MessageGeneration || (Channel && Channel->Epoch != MessageEpoch)) break;
		if (Handler->IsAlive()) Handler->Function(AppMessage);
	}
	if (!bDisposed && Generation == MessageGeneration && (!Channel || Channel->Epoch == MessageEpoch) && Delivery.Message.IsBinary())
	{
		for (const auto& Handler : BinarySnapshot)
		{
			if (bDisposed || Generation != MessageGeneration || (Channel && Channel->Epoch != MessageEpoch)) break;
			if (Handler->IsAlive()) Handler->Function(AppMessage.Bytes(), AppMessage);
		}
	}
	--DispatchDepth;
	if (bDisposed || Generation != MessageGeneration || (Channel && Channel->Epoch != MessageEpoch)) return;
	bool bMatched = false;
	if (!RouteSnapshot.IsEmpty())
	{
		++DispatchDepth;
		for (const auto& Handler : RouteSnapshot)
		{
			if (bDisposed || Generation != MessageGeneration || (Channel && Channel->Epoch != MessageEpoch)) break;
			if (Handler->IsAlive() && (!Handler->bRequestOnly || AppMessage.Kind == EVaRestSocketMessageKind::Request))
			{ bMatched = true; Handler->Function(AppMessage); }
		}
		--DispatchDepth;
	}
	if (bDisposed || Generation != MessageGeneration || (Channel && Channel->Epoch != MessageEpoch)) return;
	if (!bMatched)
	{
		++DispatchDepth;
		for (const auto& Handler : UnroutedSnapshot)
		{
			if (bDisposed || Generation != MessageGeneration || (Channel && Channel->Epoch != MessageEpoch)) break;
			if (Handler->IsAlive()) Handler->Function(AppMessage);
		}
		--DispatchDepth;
	}
	if (bDisposed || Generation != MessageGeneration || (Channel && Channel->Epoch != MessageEpoch)) return;
	EachStage([&](auto& Stage, auto& Context) { Stage.OnDispatched(AppMessage, Context); });
}

void FSocketData::ResetHeartbeat()
{
	bAwaitingHeartbeat = false; bHeartbeatHitchProbeUsed = false;
	LastHeartbeatTick = Now(); NextHeartbeatAt = LastHeartbeatTick + Heartbeat.IntervalSeconds;
	HeartbeatDeadline = NextHeartbeatAt + Heartbeat.TimeoutSeconds;
}

void FSocketData::SendHeartbeat(bool bReply)
{
	if (bDisposed || Status.State != EVaRestSocketState::Connected || !Outbox) return;
	const FString& Text = bReply ? Heartbeat.Pong : Heartbeat.Ping;
	FVaRestSocketFrame Frame;
	if (Heartbeat.bJson)
	{
		Frame.Storage.SetNumUninitialized(FrameHeadroom);
		const auto Json = FVaRestJson::Object().Set(CodecContext.Routing.RouteField, Text);
		if (Json.TryToUtf8(Frame.Storage).HasValue()) Frame.Length = Frame.Storage.Num() - Frame.Offset;
		else Frame.Storage.Reset();
	}
	else Frame = FVaRestSocketFrame::Text(Text);
	FSocketSendUnit Unit; Unit.Flags = EVaRestSend::Handshake; Unit.Bytes = Frame.Length; Unit.Frames.Add(MoveTemp(Frame));
	SendUnit(MoveTemp(Unit), Outbox->GetEpoch());
}

void FSocketData::TickHeartbeat()
{
	if (bDisposed || Status.State != EVaRestSocketState::Connected || Heartbeat.Mode == EVaRestHeartbeatMode::None
		|| Heartbeat.IntervalSeconds <= 0.0 || Heartbeat.TimeoutSeconds <= 0.0 || ProvidesHeartbeat()) return;
	const double Time = Now();
	const bool bDeadline = Heartbeat.Mode == EVaRestHeartbeatMode::ServerPing || bAwaitingHeartbeat;
	if (bDeadline && Time >= HeartbeatDeadline)
	{
		if (!bHeartbeatHitchProbeUsed && Time - LastHeartbeatTick > Heartbeat.TimeoutSeconds)
		{
			// One grace probe for an observed GT hitch; no repeated rearming without new inbound evidence.
			bHeartbeatHitchProbeUsed = true;
			SendHeartbeat(Heartbeat.Mode == EVaRestHeartbeatMode::ServerPing);
			bAwaitingHeartbeat = Heartbeat.Mode == EVaRestHeartbeatMode::ClientPing;
			HeartbeatDeadline = Time + Heartbeat.TimeoutSeconds;
			NextHeartbeatAt = Time + Heartbeat.IntervalSeconds;
		}
		else
		{
			const bool bPaused = Connection && Connection->IsReadPaused();
			const bool bAborted = AbortTransport(TEXTVIEW("ping timeout"), EVaRestSocketCloseBy::Heartbeat, EVaRestSocketFailure::HeartbeatTimeout);
			if (!bAborted) Detach(1001, TEXTVIEW("ping timeout"));
			FVaRestSocketEnd End; End.Code = bAborted ? 1006 : 1001; End.By = EVaRestSocketCloseBy::Heartbeat; End.bWhileReadPaused = bPaused;
			End.Failure = EVaRestSocketFailure::HeartbeatTimeout; End.Detail = TEXT("WebSocket application heartbeat timed out");
			Lose(End);
			return;
		}
	}
	else if (Heartbeat.Mode == EVaRestHeartbeatMode::ClientPing && !bAwaitingHeartbeat && Time >= NextHeartbeatAt)
	{
		SendHeartbeat(false); bAwaitingHeartbeat = true;
		HeartbeatDeadline = Time + Heartbeat.TimeoutSeconds; NextHeartbeatAt = Time + Heartbeat.IntervalSeconds;
	}
	LastHeartbeatTick = Time;
}

FRetryDecision FSocketData::Classify(const FVaRestSocketClose& Close)
{
	FRetryDecision Result;
	const auto Failure = Close.Failure;
	if (Close.By != EVaRestSocketCloseBy::Server || Close.Code != 1002) ProtocolFailures = 0;
	if (Failure == EVaRestSocketFailure::BadUrl || Failure == EVaRestSocketFailure::NotAllowed
		|| Failure == EVaRestSocketFailure::BadHandshake || Failure == EVaRestSocketFailure::ProtocolMismatch)
		{ Result.bRetry = false; Result.Kind = EVaRestErrorKind::Config; }
	else if (Failure == EVaRestSocketFailure::TlsVerification) Result.bRetry = false;
	else if (Failure == EVaRestSocketFailure::Canceled || Failure == EVaRestSocketFailure::Shutdown)
		{ Result.bRetry = false; Result.Kind = EVaRestErrorKind::Canceled; }
	else if (Failure == EVaRestSocketFailure::Redirect)
		{ Result.bRetry = false; Result.Kind = EVaRestErrorKind::Http; }
	else if (Failure == EVaRestSocketFailure::UpgradeRefused)
	{
		Result.Kind = EVaRestErrorKind::Http;
		if (Close.HttpStatus == 401) { Result.bRefresh = true; Result.Kind = EVaRestErrorKind::Auth; }
		else Result.bRetry = Close.HttpStatus == 408 || Close.HttpStatus == 429 || (Close.HttpStatus >= 500 && Close.HttpStatus <= 599);
	}
	else if (Failure == EVaRestSocketFailure::ConnectTimeout || Failure == EVaRestSocketFailure::HeartbeatTimeout
		|| Failure == EVaRestSocketFailure::IdleTimeout) Result.Kind = EVaRestErrorKind::Timeout;
	else if (Failure == EVaRestSocketFailure::MessageTooBig || Failure == EVaRestSocketFailure::InvalidUtf8) Result.Kind = EVaRestErrorKind::Decode;
	else if (Close.By == EVaRestSocketCloseBy::Server)
	{
		Result.Kind = EVaRestErrorKind::Server;
		if (Close.Code == 1002) { ++ProtocolFailures; Result.bRetry = ProtocolFailures < 3; }
		else ProtocolFailures = 0;
		if (Close.Code == 4401) { Result.bRefresh = true; Result.Kind = EVaRestErrorKind::Auth; }
		else if (Close.Code == 4403) { Result.bRetry = false; Result.Kind = EVaRestErrorKind::Auth; }
		else if (Close.Code == 1003 || Close.Code == 1007 || Close.Code == 1008 || Close.Code == 1010 || Close.Code == 4400 || Close.Code == 4404) Result.bRetry = false;
		else if (Close.Code == 1000 && Close.bWasClean && Policy.bStopOnNormalClose) Result.bRetry = false;
		else if (Close.Code == 1013 || Close.Code == 4429) Result.Floor = 5.0;
	}
	return Result;
}

FVaRestError FSocketData::MakeError(EVaRestErrorKind Kind, const FVaRestSocketClose& Close, FStringView Detail) const
{
	FVaRestError Out;
	Out.Kind = Kind; Out.HttpStatus = Close.HttpStatus; Out.Code = Close.Code ? FString::FromInt(Close.Code) : FString(); Out.Attempts = Close.Attempt;
	Out.Message = Detail.IsEmpty() ? FString::Printf(TEXT("WebSocket ended (%s, code %d, HTTP %d)"), LexToString(Close.Failure), Close.Code, Close.HttpStatus) : FString(Detail);
	Out.Message = TraceRedactor()->ScrubText(Out.Message);
	return Out;
}

double FSocketData::Delay(double Floor)
{
	const double Exponent = FMath::Min(1024.0, static_cast<double>(FMath::Max(0, CountedAttempts - 1)));
	const double Raw = Policy.BaseDelay == 0.0 ? 0.0 : FMath::Min(Policy.MaxDelay, Policy.BaseDelay * FMath::Pow(Policy.Multiplier, Exponent));
	const double Jittered = Policy.Jitter == EVaRestSocketJitter::Full ? Raw * Random.FRand()
		: Raw * (1.0 + Policy.JitterRatio * (2.0 * Random.FRand() - 1.0));
	return FMath::Max(Floor, FMath::Min(Policy.MaxDelay, Jittered));
}

void FSocketData::Lose(const FVaRestSocketEnd& End, FVaRestJson ReasonJson, EVaRestErrorKind KindOverride, TOptional<bool> StageDecision)
{
	const bool bWasOpen = Status.State == EVaRestSocketState::Connected;
	const uint64 LostGeneration = Generation;
	FVaRestSocketClose Close = ToClose(End, Status.Attempt);
	Close.ReasonJson = MoveTemp(ReasonJson);
	MarkChannelsDown();
	StopInbound();
	Connection.Reset(); Status.bAttemptInFlight = false; Status.bReady = false;
	StageTimers.Reset(); ReadyHolds.Reset();
	FailRequests(EVaRestErrorKind::Connection, true, TEXTVIEW("WebSocket connection was lost before the reply"));
	auto Decision = Classify(Close);
	if (KindOverride != EVaRestErrorKind::None) Decision.Kind = KindOverride;
	FString ProtocolDetail = End.Detail;
	FVaRestSocketStageCloseDecision ProtocolDecision;
	EachStage([&](auto& Stage, auto& Context) { Stage.ClassifyClose(Close, ProtocolDecision, Context); });
	if (bDisposed || Generation != LostGeneration) return;
	if (ProtocolDecision.bRetry.IsSet()) StageDecision = ProtocolDecision.bRetry;
	if (ProtocolDecision.Kind.IsSet()) Decision.Kind = ProtocolDecision.Kind.GetValue();
	if (ProtocolDecision.bRefresh.IsSet()) Decision.bRefresh = ProtocolDecision.bRefresh.GetValue();
	if (!ProtocolDecision.Detail.IsEmpty()) ProtocolDetail = MoveTemp(ProtocolDecision.Detail);
	TOptional<bool> UserDecision;
	if (!StageDecision.IsSet())
	{
		const auto Snapshot = Stages;
		++DispatchDepth;
		for (const auto& Stage : Snapshot)
		{
			if (bDisposed || Generation != LostGeneration) break;
			StageDecision = Stage->ShouldReconnect(Close);
			if (StageDecision.IsSet()) break;
		}
		--DispatchDepth;
	}
	if (!StageDecision.IsSet() && ReconnectPredicate)
	{
		const auto Predicate = ReconnectPredicate;
		++DispatchDepth;
		UserDecision = Predicate(Close);
		--DispatchDepth;
	}
	if (bDisposed || Generation != LostGeneration) return;
	if (StageDecision.IsSet())
	{
		Decision.bRetry = StageDecision.GetValue();
		if (!Decision.bRetry) Decision.bRefresh = false;
	}
	else if (UserDecision.IsSet())
	{
		Decision.bRetry = UserDecision.GetValue();
		if (!Decision.bRetry) Decision.bRefresh = false;
	}
	else if (Policy.TerminalCloseCodes.Contains(Close.Code)) { Decision.bRetry = false; Decision.bRefresh = false; }
	else if (Policy.ReconnectCloseCodes.Contains(Close.Code)) Decision.bRetry = true;
	Decision.bRetry = Decision.bRetry && Policy.bEnabled && (Decision.bRefresh || !IsOnline() || Policy.MaxAttempts == 0 || CountedAttempts < Policy.MaxAttempts);
	Close.bWillReconnect = Decision.bRetry;
	Close.ReconnectDelay = Decision.bRefresh ? 0.0 : (IsOnline() ? Delay(Decision.Floor) : FMath::Max(Decision.Floor, Policy.MaxDelay));
	TraceAttempt(EVaRestTraceSocketAttemptPhase::Closed, Close, Decision.Kind, !Decision.bRetry);
	if (bDisposed || Generation != LostGeneration) return;
	ChangeState(Decision.bRetry ? (bEverOpened ? EVaRestSocketState::Reconnecting : EVaRestSocketState::Connecting) : EVaRestSocketState::Disconnected);
	if (bDisposed || Generation != LostGeneration) return;
	EachStage([&](auto& Stage, auto& Context) { Stage.OnConnectionClosed(Close, Context); });
	if (bDisposed || Generation != LostGeneration) return;
	if (bWasOpen) Notify(Disconnected, Close);
	NotifyChannelsDown(Close);
	if (bDisposed || Generation != LostGeneration || bStopRequested) return;
	if (End.Failure != EVaRestSocketFailure::None || !End.bWasClean || End.Code == 1009)
	{
		const auto Failure = MakeError(Decision.Kind, Close, ProtocolDetail); Notify(Error, Failure);
	}
	if (bDisposed || Generation != LostGeneration || bStopRequested) return;
	if (!Decision.bRetry) { GiveUp(Close, Decision.Kind, ProtocolDetail); return; }
	if (Decision.bRefresh) { Refresh(Close, true); return; }
	NextAttemptAt = Now() + Close.ReconnectDelay;
	Status.WaitReason = IsOnline() ? EVaRestSocketWait::Backoff : EVaRestSocketWait::Offline;
	const auto Snapshot = GetStatusForCallback();
	LogRetry(Close);
	Notify(Retry, Snapshot, Close);
}

void FSocketData::Schedule(FVaRestSocketClose Close, EVaRestErrorKind Kind, double Floor)
{
	if (bDisposed || bStopRequested) return;
	if (!Policy.bEnabled || (IsOnline() && Policy.MaxAttempts > 0 && CountedAttempts >= Policy.MaxAttempts)) { GiveUp(Close, Kind); return; }
	Status.bAttemptInFlight = false;
	Close.bWillReconnect = true; Close.ReconnectDelay = IsOnline() ? Delay(Floor) : FMath::Max(Floor, Policy.MaxDelay);
	NextAttemptAt = Now() + Close.ReconnectDelay;
	Status.WaitReason = bBackground ? EVaRestSocketWait::Background : (IsOnline() ? EVaRestSocketWait::Backoff : EVaRestSocketWait::Offline);
	const auto Snapshot = GetStatusForCallback();
	LogRetry(Close);
	Notify(Retry, Snapshot, Close);
}

void FSocketData::GiveUp(const FVaRestSocketClose& Close, EVaRestErrorKind Kind, FStringView Detail)
{
	Status.bAttemptInFlight = false; Status.bReady = false; Status.WaitReason = EVaRestSocketWait::None;
	FailRequests(Kind, false, TEXTVIEW("WebSocket stopped before the request completed"));
	ChangeState(EVaRestSocketState::Disconnected);
	if (bDisposed || bStopRequested) return;
	EachStage([&](auto& Stage, auto& Context) { Stage.OnGiveUp(Context); });
	if (bDisposed || bStopRequested) return;
	auto Terminal = Close; Terminal.bWillReconnect = false; Terminal.ReconnectDelay = 0.0;
	const auto Failure = MakeError(Kind, Terminal, Detail);
	TraceAttempt(EVaRestTraceSocketAttemptPhase::Closed, Terminal, Kind, true);
	if (!bDisposed && !bStopRequested) Notify(GaveUp, Failure, Terminal);
}

void FSocketData::Refresh(const FVaRestSocketClose& Close, bool bReactive)
{
	AuthClose = Close;
	auto Pinned = Client.PinOnGT();
	if (!bHasClient || !bApplyClientAuth || !Pinned.IsValid()) { GiveUp(Close, EVaRestErrorKind::Auth); return; }
	if (bReactive && bRefreshSpent)
	{
		const uint64 BeforeNotify = Generation;
		Pinned.NotifySessionExpired();
		if (!bDisposed && Generation == BeforeNotify) GiveUp(Close, EVaRestErrorKind::Auth);
		return;
	}
	if (bReactive) bRefreshSpent = true;
	Status.bAttemptInFlight = false; Status.WaitReason = EVaRestSocketWait::Auth;
	const uint64 RefreshGeneration = Generation;
	const TWeakObjectPtr<UVaRestSocketState> Weak(Self);
	Pinned.RefreshAuth([Weak, RefreshGeneration](EVaRestAuthRefreshOutcome Outcome, EVaRestErrorKind FailedKind)
	{
		check(IsInGameThread());
		if (auto* State = Weak.Get(); State && !State->Data->bDisposed)
		{
			FOperation Result{EOperation::RefreshResult}; Result.Generation = RefreshGeneration; Result.Refresh = Outcome; Result.Kind = FailedKind;
			State->Data->Queue(MoveTemp(Result));
		}
	});
}

void FSocketData::RefreshResult(const FOperation& Operation)
{
	if (Operation.Generation != Generation || !IsWaiting() || Status.WaitReason != EVaRestSocketWait::Auth) return;
	if (Operation.Refresh == EVaRestAuthRefreshOutcome::Refreshed)
	{
		bSkipPreemptiveRefresh = true; bAuthRetry = true; NextAttemptAt = Now();
		Status.WaitReason = bBackground ? EVaRestSocketWait::Background : EVaRestSocketWait::None;
	}
	else if (Operation.Refresh == EVaRestAuthRefreshOutcome::Failed
		&& (Operation.Kind == EVaRestErrorKind::Connection || Operation.Kind == EVaRestErrorKind::Timeout))
	{ bRefreshSpent = false; Schedule(AuthClose, Operation.Kind); }
	else GiveUp(AuthClose, Operation.Refresh == EVaRestAuthRefreshOutcome::Failed ? EVaRestErrorKind::Config : EVaRestErrorKind::Auth);
}

void FSocketData::UnbindClient()
{
	if (auto Pinned = Client.PinOnGT(); Pinned.IsValid()) Pinned.OnSessionChanged().Remove(SessionHandle);
	SessionHandle.Reset(); Client = {}; bHasClient = false;
	RefreshConversionOptions();
}

void FSocketData::RefreshConversionOptions()
{
	if (bDisposed || !Outbox) return;
	FVaRestJsonConvertOptions Options;
	if (auto Pinned = Client.PinOnGT(); Pinned.IsValid()) Options = Pinned.GetJsonOptionsOnGT();
	if (!Options.Rules) Options.Rules = FVaRestJsonRules::DefaultSnapshot();
	if (NamingOverride.IsSet()) Options.Naming = NamingOverride.GetValue();
	if (StrictOverride.IsSet()) Options.bStrict = StrictOverride.GetValue();
	Outbox->SetConversionOptions(Options);
}

void FSocketData::BindClient(FVaRestClient InClient, bool bApplyCredentials)
{
	if (bDisposed) return;
	const bool bReplacement = [&]()
	{
		const auto Previous = Client.PinOnGT();
		const bool bHadCredentials = (bHasClient && bApplyClientAuth) || Bearer;
		return (bHadCredentials || (bApplyCredentials && (bHasClient || bEverOpened)))
			&& (!InClient.IsValid() || !Previous.IsValid() || Previous.GetId() != InClient.GetId() || bApplyClientAuth != bApplyCredentials);
	}();
	UnbindClient(); Bearer = nullptr;
	bApplyClientAuth = bApplyCredentials;
	if (!InClient.IsValid()) { if (bReplacement) SessionChanged(EVaRestSessionChange::Replaced); return; }
	Client = InClient.AsWeak(); bHasClient = true; Redactor = InClient.GetRedactor();
	if (Inbound) Inbound->SetTraceOnGT(SocketId, TraceAttemptId, TraceRedactor(), TraceClock);
	RefreshConversionOptions();
	const TWeakObjectPtr<UVaRestSocketState> Weak(Self);
	SessionHandle = InClient.OnSessionChanged().AddWeakLambda(Self, [Weak](EVaRestSessionChange Change)
	{
		if (auto* State = Weak.Get(); State && !State->Data->bDisposed)
		{
			const TStrongObjectPtr<UVaRestSocketState> Pin(State);
			{
				const auto Client = State->Data->Client.PinOnGT();
				if (Client.IsValid()) State->Data->Redactor = Client.GetRedactor();
			} // No strong HTTP shell is retained across the protocol/user session notification below.
			if (State->Data->Inbound) State->Data->Inbound->SetTraceOnGT(State->Data->SocketId, State->Data->TraceAttemptId, State->Data->TraceRedactor(), State->Data->TraceClock);
			if (State->Data->bApplyClientAuth) State->Data->SessionChanged(Change);
		}
	});
	InClient = {}; // Binding retains only the weak client before a replacement hook can reenter HTTP shutdown.
	if (bReplacement) SessionChanged(EVaRestSessionChange::Replaced);
}
void FSocketData::SessionChanged(EVaRestSessionChange Change)
{
	if (bDisposed) return; const uint64 Before = Generation;
	EachStage([&](auto& Stage, auto& Context) { Stage.OnSessionChanged(Change, Context); });
	if (bDisposed || Generation != Before) return;
	if (Change == EVaRestSessionChange::Cleared) RequestClose(1000, TEXTVIEW("logout"));
	else if (Change == EVaRestSessionChange::Replaced)
	{
		FOperation Operation{EOperation::Restart}; Operation.Reason = TEXT("session replaced"); Operation.bFlag = true; Queue(MoveTemp(Operation));
	}
}

void FSocketData::Dispose()
{
	check(IsInGameThread());
	if (bDisposing) return;
	bDisposing = true;
	FVaRestJsonRules::OnRulesChanged().Remove(RulesHandle);
	FailRequests(EVaRestErrorKind::Canceled, false, TEXTVIEW("WebSocket request canceled by shutdown"));
	if (Outbox) Outbox->Clear(true);
	Detach(1001, TEXTVIEW("shutdown")); Detached.Reset();
	if (FrameLog) { FrameLog->StopAndWaitOnGT(); FrameLog.Reset(); }
	UnbindClient();
	if (Connectivity) Connectivity->OnChanged().Remove(ConnectivityHandle);
	ReportTraceNotes(true);
	TraceReleased();
	if (TraceClock) TraceClock->RetireOnGT();
	Connectivity.Reset(); Transport.Reset(); Clock.Reset(); TraceClock.Reset(); Redactor.Reset(); AppliedRedactor.Reset();
	Codec.Reset();
	Bearer = nullptr; ReconnectPredicate = nullptr;
	{
		auto RetiringStages = MoveTemp(Stages);
		for (const auto& Stage : RetiringStages) Stage->OnRetire();
	}
	StageTimers.Reset(); ReadyHolds.Reset(); ClosingFrames.Reset();
	ProtocolState.Reset();
	Channels.Reset();
	Handshake.Reset(); Connected.Reset(); Ready.Reset(); Disconnected.Reset(); Retry.Reset(); GaveUp.Reset();
	StateChanged.Reset(); Error.Reset(); Drained.Reset(); Message.Reset(); Binary.Reset(); Unrouted.Reset(); Routes.Reset();
	Headers = {}; LastHandshake = {}; Query.Reset(); HandshakeOrigin.Reset(); Url.Reset(); Protocols.Reset();
	AutoReplies.Reset();
	ProtocolRedactedKeys.Reset(); ProtocolSecrets.Reset(); ProtocolSecretBytes = 0;
	Operations.Reset();
	const uint64 OldRegistration = Registration; Registration = 0;
	if (auto* Core = Hub.Get(); Core && OldRegistration) Core->ReleaseInFlight(OldRegistration);
	Hub.Reset();
}

void FSocketData::PruneHandlers()
{
	auto Prune = [](auto& List) { List.RemoveAll([](const auto& Entry) { return !Entry->IsAlive(); }); };
	Prune(Handshake); Prune(Connected); Prune(Ready); Prune(Disconnected); Prune(Retry); Prune(GaveUp);
	Prune(StateChanged); Prune(Error); Prune(Drained); Prune(Message); Prune(Binary); Prune(Unrouted);
	for (auto It = Routes.CreateIterator(); It; ++It) { Prune(It.Value()); if (It.Value().IsEmpty()) It.RemoveCurrent(); }
	for (auto& Pair : Channels)
	{
		auto& Channel = *Pair.Value;
		Prune(Channel.Connected); Prune(Channel.Ready); Prune(Channel.Disconnected); Prune(Channel.Message); Prune(Channel.Unrouted); Prune(Channel.Error);
		for (auto It = Channel.Routes.CreateIterator(); It; ++It) { Prune(It.Value()); if (It.Value().IsEmpty()) It.RemoveCurrent(); }
	}
}

FSocketHandle::~FSocketHandle()
{
	if (bDisposed.load(std::memory_order_acquire) || !Gate || Gate->bStopped.load(std::memory_order_acquire)) return;
	const auto Weak = State;
	const auto NativeGate = Gate;
	AsyncTask(ENamedThreads::GameThread, [Weak, NativeGate]
	{
		if (NativeGate->bStopped.load(std::memory_order_acquire)) return;
		if (auto* Object = Weak.Get()) Object->DisposeOnGT(TEXTVIEW("released"));
	});
}

UVaRestSocketState* FSocketAccess::StateOnGT(const FVaRestSocket& Socket)
{
	check(IsInGameThread());
	return Socket.IsValid() ? Socket.Handle->State.Get() : nullptr;
}

void FSocketAccess::SetEntryOnGT(const FVaRestSocket& Socket, FName Entry)
{
	check(IsInGameThread()); if (auto* Object = StateOnGT(Socket)) Object->Data->TraceEntry = Entry;
}
void FSocketAccess::DisposeOnGT(const FVaRestSocket& Socket, FStringView Reason)
{
	if (auto* Object = StateOnGT(Socket)) Object->DisposeOnGT(Reason);
}

FVaRestSocket FSocketAccess::CreateOnGT(UVaRestHub* CoreHub, FName Name, FString Url,
	TSharedPtr<IVaRestSocketTransport, ESPMode::ThreadSafe> Transport, TSharedPtr<IVaRestClock, ESPMode::ThreadSafe> Clock, int32 Seed)
{
	check(IsInGameThread());
	if (!CoreHub || !ModuleGate || ModuleGate->bStopped.load(std::memory_order_acquire)) return {};
	const uint64 NewSocketId = FVaRestTrace::NewSocketId(); if (!NewSocketId) return {};
	const TStrongObjectPtr<UVaRestHub> CorePin(CoreHub);
	const TStrongObjectPtr<UVaRestSocketState> State(NewObject<UVaRestSocketState>(CoreHub));
	auto Token = MakeShared<FSocketHandle, ESPMode::ThreadSafe>();
	Token->State = State.Get(); Token->Gate = ModuleGate;
	Token->Outbox = MakeShared<FSocketOutbox, ESPMode::ThreadSafe>();
	auto& Data = *State->Data;
	Data.Hub = CoreHub; Data.SocketId = NewSocketId; Data.SocketName = Name; Data.Url = MoveTemp(Url); Data.Handle = Token; Data.Gate = ModuleGate;
	Data.Outbox = Token->Outbox;
	Data.FrameLog = MakeShared<FSocketFrameLog, ESPMode::ThreadSafe>();
	Data.Codec = MakeJsonCodec();
	Data.CodecContext.ReplyIdPrefix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8) + TEXT("-");
	Data.Outbox->SetEncoder(Data.Codec.ToSharedRef(), Data.CodecContext);
	Data.FindChannel({}, true);
	Data.Transport = MoveTemp(Transport); Data.Clock = Clock ? MoveTemp(Clock) : FVaRestRealClock::Get();
	Data.TraceClock = MakeShared<FSocketTraceClock, ESPMode::ThreadSafe>(); Data.TraceClock->InitializeOnGT(Data.Clock.Get());
	Data.Outbox->SetTraceClockOnGT(Data.TraceClock.ToSharedRef());
	Data.Random.Initialize(Seed ? Seed : static_cast<int32>(FPlatformTime::Cycles()));
	Data.Connectivity = CoreHub->GetConnectivity();
	const TWeakObjectPtr<UVaRestSocketState> Weak(State.Get());
	Data.RefreshConversionOptions();
	Data.RulesHandle = FVaRestJsonRules::OnRulesChanged().AddWeakLambda(State.Get(), [Weak]
	{
		if (auto* Object = Weak.Get(); Object && !Object->Data->bDisposed) Object->Data->RefreshConversionOptions();
	});
	Data.Registration = CoreHub->RetainInFlight(State.Get(), [Weak]
	{
		if (auto* Object = Weak.Get()) Object->DisposeOnGT(TEXTVIEW("shutdown"));
	});
	if (!Data.Registration) { State->DisposeOnGT(TEXTVIEW("admission refused")); return {}; }
	if (Data.Connectivity)
		Data.ConnectivityHandle = Data.Connectivity->OnChanged().AddWeakLambda(State.Get(), [Weak](bool bOnline)
		{
			if (auto* Object = Weak.Get(); Object && !Object->Data->bDisposed)
			{
				FOperation Operation{EOperation::Online}; Operation.bFlag = bOnline; Object->Data->Queue(MoveTemp(Operation));
			}
		});
	States.Add(State.Get());
	FVaRestSocket Socket(MoveTemp(Token));
	const auto* Settings = GetDefault<UVaRestSocketSettings>();
	Data.bAllowSelfSignedTls = Settings->bAllowSelfSignedTls;
	Socket.WithLogLevel(Settings->LogLevel);
	Socket.WithProtocolPing(Settings->ProtocolPing);
	Socket.WithTimeouts(Settings->ConnectTimeout, Settings->CloseTimeout).WithReconnect(Settings->Reconnect)
		.WithLimits(Settings->Limits).WithQueue(Settings->Queue).WithHeartbeat(Settings->Heartbeat).WithBackground(Settings->Background);
	Data.bTraceDropCapture = FVaRestTrace::Detail() != EVaRestTraceDetail::None; Data.Outbox->CaptureDropNotes(Data.bTraceDropCapture);
	return Socket;
}

void StartSocketsOnGT()
{
	check(IsInGameThread());
	ModuleGate = MakeShared<FSocketModuleGate, ESPMode::ThreadSafe>();
	SnapshotHandle = FVaRestTrace::OnBuildSnapshot().AddLambda([](FVaRestTraceSnapshot& Result)
	{
		check(IsInGameThread()); const auto Snapshot = States;
		for (const auto& Weak : Snapshot) if (auto* Object = Weak.Get())
		{ const TStrongObjectPtr<UVaRestSocketState> Pin(Object); Object->ContributeSnapshotOnGT(Result); }
	});
#if !UE_BUILD_SHIPPING
	ListCommand = IConsoleManager::Get().RegisterConsoleCommand(TEXT("VaRest.Socket.List"), TEXT("List live VaRest sockets with redacted URLs and queue gauges."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			check(IsInGameThread()); const auto Snapshot = States;
			for (const auto& Weak : Snapshot) if (auto* Object = Weak.Get(); Object && !Object->IsDisposedOnGT())
				UE_LOG(LogVaRestSocket, Display, TEXT("%s"), *Object->DescribeOnGT());
		}), ECVF_Default);
	DropCommand = IConsoleManager::Get().RegisterConsoleCommand(TEXT("VaRest.Socket.Drop"), TEXT("VaRest.Socket.Drop <socket id or exact name>: abort a live native connection."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Arguments)
		{
			check(IsInGameThread());
			if (Arguments.Num() != 1) { UE_LOG(LogVaRestSocket, Warning, TEXT("Usage: VaRest.Socket.Drop <socket id or exact name>")); return; }
			const auto Snapshot = States; TArray<TStrongObjectPtr<UVaRestSocketState>> Matches;
			for (const auto& Weak : Snapshot) if (auto* Object = Weak.Get(); Object && !Object->IsDisposedOnGT())
			{
				if (Object->MatchesConsoleKeyOnGT(Arguments[0])) Matches.Emplace(Object);
			}
			if (Matches.Num() != 1) { UE_LOG(LogVaRestSocket, Warning, TEXT("Socket drop needs one unambiguous live socket; use its numeric id.")); return; }
			if (!Matches[0]->AbortOnGT(TEXTVIEW("debug drop"))) UE_LOG(LogVaRestSocket, Warning, TEXT("This connection did not admit a native abort."));
		}), ECVF_Default);
#endif
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
	{
		const auto Snapshot = States;
		for (const auto& Weak : Snapshot)
			if (auto* Object = Weak.Get()) { const TStrongObjectPtr<UVaRestSocketState> Pin(Object); Object->TickOnGT(); }
		States.RemoveAll([](const auto& Weak) { return !Weak.IsValid() || Weak->IsDisposedOnGT(); });
#if STATS
		int64 OpenCount = 0, BufferedCount = 0, QueuedCount = 0, Samples = 0; double Rtt = 0.0;
		for (const auto& Weak : States) if (auto* Object = Weak.Get())
		{
			const auto Sample = Object->GetStatsOnGT(); OpenCount += Sample.bOpen; BufferedCount += Sample.BufferedBytes; QueuedCount += Sample.Queued;
			if (Sample.bHasLatency) { Rtt += Sample.LatencyAverage; ++Samples; }
		}
		Stats::Gauges(OpenCount, BufferedCount, QueuedCount, Samples ? Rtt / Samples : 0.0);
#endif
		return true;
	}));
	auto Background = [](bool bValue)
	{
		const auto Gate = ModuleGate;
		AsyncTask(ENamedThreads::GameThread, [Gate, bValue]
		{
			if (!Gate || Gate->bStopped.load(std::memory_order_acquire)) return;
			const auto Snapshot = States;
			for (const auto& Weak : Snapshot)
				if (auto* Object = Weak.Get()) { const TStrongObjectPtr<UVaRestSocketState> Pin(Object); Object->BackgroundOnGT(bValue); }
		});
	};
	BackgroundHandle = FCoreDelegates::ApplicationWillEnterBackgroundDelegate.AddLambda([Background] { Background(true); });
	ForegroundHandle = FCoreDelegates::ApplicationHasEnteredForegroundDelegate.AddLambda([Background] { Background(false); });
}

void StopSocketsOnGT()
{
	check(IsInGameThread());
	if (!ModuleGate || ModuleGate->bStopped.exchange(true, std::memory_order_acq_rel)) return;
	FCoreDelegates::ApplicationWillEnterBackgroundDelegate.Remove(BackgroundHandle);
	FCoreDelegates::ApplicationHasEnteredForegroundDelegate.Remove(ForegroundHandle);
	FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
#if !UE_BUILD_SHIPPING
	if (ListCommand) { IConsoleManager::Get().UnregisterConsoleObject(ListCommand, false); ListCommand = nullptr; }
	if (DropCommand) { IConsoleManager::Get().UnregisterConsoleObject(DropCommand, false); DropCommand = nullptr; }
#endif
	Stats::Gauges(0, 0, 0, 0.0);
	const auto Snapshot = States;
	for (const auto& Weak : Snapshot)
		if (auto* Object = Weak.Get()) { const TStrongObjectPtr<UVaRestSocketState> Pin(Object); Object->DisposeOnGT(TEXTVIEW("shutdown")); }
	States.Reset(); FVaRestTrace::OnBuildSnapshot().Remove(SnapshotHandle); SnapshotHandle.Reset();
}
}

void VaRest::WebSocket::FSocketDataDeleter::operator()(VaRest::WebSocket::FSocketData* Pointer) const
{
	delete Pointer;
}

UVaRestSocketState::UVaRestSocketState() : Data(new VaRest::WebSocket::FSocketData(this)) {}
UVaRestSocketState::~UVaRestSocketState() = default;
void UVaRestSocketState::ContributeSnapshotOnGT(FVaRestTraceSnapshot& Snapshot)
{
	check(IsInGameThread()); if (Data->bDisposed || Data->bTraceReleased) return;
	Data->bTraceCreated = true; Data->bTraceDropCapture = true; Data->Outbox->CaptureDropNotes(true);
	Snapshot.Sockets.Add(Data->TraceRecord(true));
	if (Data->TraceAttemptId) Snapshot.SocketAttempts.Add(Data->TraceAttemptRecord(true));
	const TStrongObjectPtr<UVaRestSocketState> Self(this);
	const auto Stages = Data->Stages; const auto Redactor = Data->TraceRedactor();
	const uint64 SocketId = Data->SocketId, AttemptId = Data->TraceAttemptId;
	for (const auto& Stage : Stages)
	{
		if (Data->bDisposed || Data->bTraceReleased || Data->TraceAttemptId != AttemptId) break;
		Stage->AppendTraceSnapshot(Snapshot, SocketId, AttemptId, *Redactor);
	}
}
FVaRestSocketStats UVaRestSocketState::GetStatsOnGT() const { check(IsInGameThread()); return Data->GetStats(); }
bool UVaRestSocketState::MatchesConsoleKeyOnGT(FStringView Key) const
{
	check(IsInGameThread());
	return Data && !Data->bDisposed && (VaRest::WebSocket::SameText(Key, LexToString(Data->SocketId))
		|| (!Data->SocketName.IsNone() && VaRest::WebSocket::SameText(Key, Data->SocketName.ToString())));
}
bool UVaRestSocketState::AbortOnGT(FStringView Reason)
{
	check(IsInGameThread()); const TStrongObjectPtr<UVaRestSocketState> Pin(this);
	return Data && !Data->bDisposed && Data->Connection && Data->Connection->Abort(Reason);
}
FString UVaRestSocketState::DescribeOnGT() const
{
	check(IsInGameThread()); const auto Sample = Data->GetStats();
	const FString Redacted = Data->Redactor ? Data->Redactor->RedactUrl(Data->Url) : FVaRestRedactor().RedactUrl(Data->Url);
	return FString::Printf(TEXT("%s state=%d attempt=%d ready=%d queued=%d buffered=%lld %s"), *Data->SocketName.ToString(),
		static_cast<int32>(Data->Status.State), Data->Status.Attempt, Data->Status.bReady, Sample.Queued, static_cast<long long>(Sample.BufferedBytes), *Redacted);
}
bool UVaRestSocketState::IsDisposedOnGT() const { check(IsInGameThread()); return Data->bDisposed; }
void UVaRestSocketState::TickOnGT() { const TStrongObjectPtr<UVaRestSocketState> Pin(this); Data->Tick(); }
void UVaRestSocketState::BackgroundOnGT(bool bBackground)
{
	using namespace VaRest::WebSocket;
	FOperation Operation{EOperation::Background}; Operation.bFlag = bBackground; Data->Queue(MoveTemp(Operation));
}
void UVaRestSocketState::DisposeOnGT(FStringView Reason)
{
	check(IsInGameThread());
	const TStrongObjectPtr<UVaRestSocketState> Pin(this);
	if (Data->bDisposed) return;
	// Refuse resurrection through an existing handle before the closing notification calls user code.
	if (auto Token = Data->Handle.Pin()) Token->bDisposed.store(true, std::memory_order_release);
	Data->RequestClose(1001, Reason);
	Data->bDisposed = true; Data->BumpGeneration();
	VaRest::WebSocket::FOperation Operation{VaRest::WebSocket::EOperation::Dispose}; Data->Queue(MoveTemp(Operation));
}
void UVaRestSocketState::BeginDestroy()
{
	check(IsInGameThread());
	Data->bDisposed = true;
	if (auto Token = Data->Handle.Pin()) Token->bDisposed.store(true, std::memory_order_release);
	Data->Dispose();
	Super::BeginDestroy();
}

using namespace VaRest::WebSocket;

bool FVaRestSocket::IsValid() const
{
	return Handle && !Handle->bDisposed.load(std::memory_order_acquire) && Handle->Gate
		&& !Handle->Gate->bStopped.load(std::memory_order_acquire);
}
FName FVaRestSocket::GetProtocolTag() const
{
	if (!IsValid() || !Handle->Outbox) return NAME_None;
	const auto Snapshot = Handle->Outbox->GetEncoder();
	return Snapshot.Codec ? Snapshot.Codec->GetProtocolTag() : NAME_None;
}

FVaRestSocketWeak FVaRestSocket::AsWeak() const { FVaRestSocketWeak Out; Out.Handle = Handle; return Out; }
FVaRestSocket FVaRestSocketWeak::Pin() const
{
	FVaRestSocket Out(Handle.Pin());
	return Out.IsValid() ? Out : FVaRestSocket();
}

// A local strong UObject pin protects against a callback deinitializing the Hub and running GC synchronously.
#define VAREST_SOCKET_EDIT(Body) \
	check(IsInGameThread()); \
	if (auto* Object = FSocketAccess::StateOnGT(*this)) \
	{ \
		const TStrongObjectPtr<UVaRestSocketState> Pin(Object); \
		auto& Data = *Object->Data; \
		Body; \
		Data.DrainOperations(); \
	} \
	return *this

UObject* FVaRestSocket::GetProtocolStateOnGT() const
{
	check(IsInGameThread()); if (auto* Object = FSocketAccess::StateOnGT(*this)) return Object->Data->ProtocolState.Get(); return nullptr;
}
bool FVaRestSocket::SetProtocolStateOnGT(UObject* State)
{
	check(IsInGameThread());
	if (auto* Object = FSocketAccess::StateOnGT(*this); Object && Object->Data->Status.State == EVaRestSocketState::Disconnected)
	{ Object->Data->ProtocolState = State; return true; } return false;
}
FVaRestSocketBatchPin FVaRestSocket::GetProtocolDispatchPinOnGT(FStringView Channel) const { return CaptureDispatchPinOnGT(Channel); }
bool FVaRestSocket::IsProtocolDispatchCurrentOnGT(const FVaRestSocketBatchPin& DispatchPin) const
{
	check(IsInGameThread());
	if (auto* Object = FSocketAccess::StateOnGT(*this); Object && Object->Data->Generation == DispatchPin.ConnectionGeneration)
	{ const auto Channel = Object->Data->FindChannel(DispatchPin.Channel); return Channel && Channel->Epoch == DispatchPin.ChannelEpoch; } return false;
}
void FVaRestSocket::ReportProtocolErrorOnGT(FVaRestError Error, FStringView Channel) const
{
	check(IsInGameThread());
	if (auto* Object = FSocketAccess::StateOnGT(*this))
	{ FVaRestSocketStageContext Context(Object, Object->Data->Generation); Context.ReportError(Channel, MoveTemp(Error)); }
}

FVaRestSocket FVaRestSocket::WithUrl(FString Url) { VAREST_SOCKET_EDIT(Data.Url = MoveTemp(Url)); }
FVaRestSocket FVaRestSocket::WithProtocols(TArray<FString> Protocols) { VAREST_SOCKET_EDIT(Data.Protocols = MoveTemp(Protocols)); }
FVaRestSocket FVaRestSocket::WithHeader(FString Name, FString Value) { VAREST_SOCKET_EDIT(Data.Headers.Set(Name, Value)); }
FVaRestSocket FVaRestSocket::RemoveHeader(FStringView Name) { VAREST_SOCKET_EDIT(Data.Headers.Remove(Name)); }
FVaRestSocket FVaRestSocket::RemoveQuery(FStringView Name) { VAREST_SOCKET_EDIT(Data.Query.RemoveAll([Name](const auto& Pair) { return SameText(Pair.Key, Name); })); }
FVaRestSocket FVaRestSocket::WithAuthPlacement(FVaRestSocketAuthPlacement Placement) { VAREST_SOCKET_EDIT(Data.AuthPlacement = MoveTemp(Placement)); }
void FVaRestSocket::ResetSession()
{
	check(IsInGameThread());
	if (auto* Object = FSocketAccess::StateOnGT(*this))
	{
		const TStrongObjectPtr<UVaRestSocketState> ObjectPin(Object);
		Object->Data->SessionChanged(EVaRestSessionChange::Replaced);
	}
}
FVaRestSocket FVaRestSocket::WithQuery(FString Name, FString Value)
{
	VAREST_SOCKET_EDIT(
		Data.Query.RemoveAll([&Name](const auto& Pair) { return SameText(Pair.Key, Name); });
		Data.Query.Emplace(MoveTemp(Name), MoveTemp(Value))
	);
}
FVaRestSocket FVaRestSocket::WithOrigin(TOptional<FString> Origin) { VAREST_SOCKET_EDIT(Data.HandshakeOrigin = MoveTemp(Origin)); }
FVaRestSocket FVaRestSocket::ReadyWhen(FString Route)
{
	VAREST_SOCKET_EDIT(
		Data.ReadyRoute = MoveTemp(Route); Data.bReadyRouteSeen = Data.ReadyRoute.IsEmpty();
		if (!Data.bReadyRouteSeen) Data.Status.bReady = false;
		if (auto Channel = Data.FindChannel({}))
		{
			Channel->bReady = Data.Status.bReady;
			if (!Channel->bReady && Channel->bUp && !Channel->bWaitingReady) { Channel->bWaitingReady = true; Channel->ReadySince = Data.Now(); }
			Data.PublishChannel(*Channel);
		}
		Data.BecomeReady()
	);
}
FVaRestSocket FVaRestSocket::WithLimits(FVaRestSocketLimits Limits)
{
	VAREST_SOCKET_EDIT(
		Limits.MaxMessageBytes = FMath::Clamp<int64>(Limits.MaxMessageBytes, 0, MAX_int32 - 1);
		Limits.MaxPendingInboundBytes = FMath::Max<int64>(0, Limits.MaxPendingInboundBytes);
		Limits.MaxSendBytes = FMath::Clamp<int64>(Limits.MaxSendBytes, 0, MAX_int32 - VaRest::WebSocket::FrameHeadroom);
		Limits.MaxBufferedBytes = FMath::Max<int64>(0, Limits.MaxBufferedBytes);
		Limits.SendHighWater = FMath::Max<int64>(0, Limits.SendHighWater);
		Limits.MaxPendingRequests = FMath::Max(0, Limits.MaxPendingRequests);
		Limits.MaxChannels = FMath::Clamp(Limits.MaxChannels, 1, 4096);
		Limits.ReadyTimeout = FMath::IsFinite(Limits.ReadyTimeout) ? FMath::Clamp(Limits.ReadyTimeout, 0.0, 1e9) : 0.0;
		Data.Limits = Limits; Data.Outbox->SetLimits(MoveTemp(Limits));
		if (Data.Inbound) Data.Inbound->SetLimitsOnGT(FMath::Min<int64>(Data.Limits.MaxPendingInboundBytes, MAX_int64 - Data.Limits.MaxMessageBytes) + Data.Limits.MaxMessageBytes, Data.Limits.MaxMessageBytes)
	);
}
FVaRestSocket FVaRestSocket::WithQueue(FVaRestSocketQueuePolicy Policy)
{
	VAREST_SOCKET_EDIT(
		Policy.MaxQueued = FMath::Max(0, Policy.MaxQueued); Policy.MaxQueuedBytes = FMath::Max<int64>(0, Policy.MaxQueuedBytes);
		Data.Outbox->SetPolicy(MoveTemp(Policy))
	);
}
FVaRestSocket FVaRestSocket::WithHeartbeat(FVaRestHeartbeat Heartbeat)
{
	VAREST_SOCKET_EDIT(
		Heartbeat.IntervalSeconds = FMath::IsFinite(Heartbeat.IntervalSeconds) ? FMath::Clamp(Heartbeat.IntervalSeconds, 0.0, 1e9) : 20.0;
		Heartbeat.TimeoutSeconds = FMath::IsFinite(Heartbeat.TimeoutSeconds) ? FMath::Clamp(Heartbeat.TimeoutSeconds, 0.0, 1e9) : 10.0;
		Data.Heartbeat = MoveTemp(Heartbeat); Data.ResetHeartbeat();
		if (Data.Inbound) Data.Inbound->SetHeartbeatOnGT(Data.Heartbeat)
	);
}
FVaRestSocket FVaRestSocket::WithProtocolPing(FVaRestProtocolPing Ping)
{
	VAREST_SOCKET_EDIT(
		Ping.Interval = FMath::IsFinite(Ping.Interval) ? FMath::Clamp(Ping.Interval, 0.0, 1e9) : 20.0;
		Ping.Timeout = FMath::IsFinite(Ping.Timeout) ? FMath::Clamp(Ping.Timeout, 0.05, 1e9) : 10.0;
		Data.ProtocolPing = Ping
	);
}
FVaRestSocket FVaRestSocket::WithAutoReplies(TArray<FVaRestSocketAutoReply> Rules)
{
	VAREST_SOCKET_EDIT(Data.AutoReplies = MoveTemp(Rules));
}
FVaRestSocket FVaRestSocket::WithRouting(FVaRestSocketRouting Routing)
{
	VAREST_SOCKET_EDIT(
		Data.CodecContext.Routing = MoveTemp(Routing);
		Data.Outbox->SetEncoder(Data.Codec.ToSharedRef(), Data.CodecContext);
		if (Data.Inbound) Data.Inbound->ConfigureOnGT(Data.Codec.ToSharedRef(), Data.CodecContext)
	);
}
FVaRestSocket FVaRestSocket::WithCodec(TSharedRef<IVaRestSocketCodec, ESPMode::ThreadSafe> Codec)
{
	VAREST_SOCKET_EDIT(
		Data.Codec = MoveTemp(Codec); Data.Outbox->SetEncoder(Data.Codec.ToSharedRef(), Data.CodecContext);
		if (Data.Inbound) Data.Inbound->ConfigureOnGT(Data.Codec.ToSharedRef(), Data.CodecContext)
	);
}
FVaRestSocket FVaRestSocket::WithNaming(EVaRestJsonNaming Naming) { VAREST_SOCKET_EDIT(Data.NamingOverride = Naming; Data.RefreshConversionOptions()); }
FVaRestSocket FVaRestSocket::WithStrictDecode(bool bStrict) { VAREST_SOCKET_EDIT(Data.StrictOverride = bStrict; Data.RefreshConversionOptions()); }
FVaRestSocket FVaRestSocket::Use(TSharedRef<IVaRestSocketStage> Stage)
{
	VAREST_SOCKET_EDIT(
		Data.Stages.Add(Stage);
		FVaRestSocketStageContext Context(Object, Data.Generation, false, reinterpret_cast<UPTRINT>(&Stage.Get()));
		++Data.DispatchDepth; Stage->OnAttach(Context); --Data.DispatchDepth
	);
}
FVaRestSocket FVaRestSocket::WithClient(FVaRestClient Client) { VAREST_SOCKET_EDIT(Data.BindClient(MoveTemp(Client))); }
FVaRestSocket FVaRestSocket::WithClient(FVaRestClient Client, bool bApplyCredentials) { VAREST_SOCKET_EDIT(Data.BindClient(MoveTemp(Client), bApplyCredentials)); }
FVaRestSocket FVaRestSocket::WithApi(UVaRestApi* Api)
{
	VAREST_SOCKET_EDIT(if (auto* Hub = Data.Hub.Get()) Data.BindClient(Hub->Api(Api)));
}
FVaRestSocket FVaRestSocket::WithBearer(TFunction<FString()> Provider)
{
	VAREST_SOCKET_EDIT(Data.UnbindClient(); Data.Bearer = MoveTemp(Provider));
}
FVaRestSocket FVaRestSocket::WithReconnect(FVaRestReconnectPolicy Policy)
{
	VAREST_SOCKET_EDIT(
		Policy.MaxAttempts = FMath::Max(0, Policy.MaxAttempts);
		Policy.BaseDelay = FMath::IsFinite(Policy.BaseDelay) ? FMath::Clamp(Policy.BaseDelay, 0.0, 1e9) : 0.5;
		Policy.Multiplier = FMath::IsFinite(Policy.Multiplier) ? FMath::Clamp(Policy.Multiplier, 1.0, 1e6) : 2.0;
		Policy.MaxDelay = FMath::IsFinite(Policy.MaxDelay) ? FMath::Clamp(Policy.MaxDelay, 0.0, 1e9) : 30.0;
		Policy.JitterRatio = FMath::IsFinite(Policy.JitterRatio) ? FMath::Clamp(Policy.JitterRatio, 0.0, 1.0) : 0.2;
		Policy.StableAfter = FMath::IsFinite(Policy.StableAfter) ? FMath::Clamp(Policy.StableAfter, 0.0, 1e9) : 5.0;
		Data.Policy = MoveTemp(Policy)
	);
}
FVaRestSocket FVaRestSocket::NoReconnect() { VAREST_SOCKET_EDIT(Data.Policy.bEnabled = false); }
FVaRestSocket FVaRestSocket::WithBackground(EVaRestSocketBackground Policy) { VAREST_SOCKET_EDIT(Data.BackgroundPolicy = Policy); }
FVaRestSocket FVaRestSocket::WithLogLevel(EVaRestLogLevel Level)
{
	VAREST_SOCKET_EDIT(
		Data.LogLevel = static_cast<EVaRestLogLevel>(FMath::Clamp(static_cast<int32>(Level), 0, 3)); Data.FrameLog->SetLevelOnGT(Data.LogLevel);
		Data.CodecContext.LogLevel = Data.LogLevel; Data.Outbox->SetEncoder(Data.Codec.ToSharedRef(), Data.CodecContext);
		if (Data.Inbound) Data.Inbound->ConfigureOnGT(Data.Codec.ToSharedRef(), Data.CodecContext)
	);
}
FVaRestSocket FVaRestSocket::WithTimeouts(double ConnectSeconds, double CloseSeconds)
{
	VAREST_SOCKET_EDIT(
		Data.ConnectTimeout = FMath::IsFinite(ConnectSeconds) ? FMath::Clamp(ConnectSeconds, 0.0, MaxConnectTimeoutSeconds) : 10.0;
		Data.CloseTimeout = FMath::IsFinite(CloseSeconds) ? FMath::Clamp(CloseSeconds, 0.0, 5.0) : 5.0
	);
}
FVaRestSocket FVaRestSocket::Owner(const UObject* Owner)
{
	VAREST_SOCKET_EDIT(
		if (ensureMsgf(Data.SocketName.IsNone(), TEXT("Owner is only available on unnamed sockets")))
		{ Data.Owner = Owner; Data.bOwned = Owner != nullptr; }
	);
}
FVaRestSocket FVaRestSocket::ReconnectIf(TFunction<TOptional<bool>(const FVaRestSocketClose&)> Predicate)
{
	VAREST_SOCKET_EDIT(Data.ReconnectPredicate = MoveTemp(Predicate));
}

#define VAREST_SOCKET_HANDLER(Method, List, Signature) \
	FVaRestSocket FVaRestSocket::Method(const UObject* Owner, TFunction<Signature> Handler) \
	{ VAREST_SOCKET_EDIT(AddCallback(Data.List, Owner, MoveTemp(Handler), false, Data.NextRouteOrder++)); }
VAREST_SOCKET_HANDLER(OnHandshake, Handshake, void(FVaRestSocketHandshake&))
VAREST_SOCKET_HANDLER(OnConnected, Connected, void(const FVaRestSocketOpen&))
VAREST_SOCKET_HANDLER(OnReady, Ready, void(const FVaRestSocketOpen&))
VAREST_SOCKET_HANDLER(OnDisconnected, Disconnected, void(const FVaRestSocketClose&))
VAREST_SOCKET_HANDLER(OnRetryScheduled, Retry, void(const FVaRestSocketStatus&, const FVaRestSocketClose&))
VAREST_SOCKET_HANDLER(OnGaveUp, GaveUp, void(const FVaRestError&, const FVaRestSocketClose&))
VAREST_SOCKET_HANDLER(OnStateChanged, StateChanged, void(EVaRestSocketState, EVaRestSocketState))
VAREST_SOCKET_HANDLER(OnError, Error, void(const FVaRestError&))
VAREST_SOCKET_HANDLER(OnDrained, Drained, void())
VAREST_SOCKET_HANDLER(OnMessage, Message, void(const FVaRestSocketMessage&))
VAREST_SOCKET_HANDLER(OnUnrouted, Unrouted, void(const FVaRestSocketMessage&))
VAREST_SOCKET_HANDLER(OnBinary, Binary, void(TConstArrayView<uint8>, const FVaRestSocketMessage&))
#undef VAREST_SOCKET_HANDLER

FVaRestSocket FVaRestSocket::On(FStringView Route, const UObject* Owner, TFunction<void(const FVaRestSocketMessage&)> Handler)
{
	VAREST_SOCKET_EDIT(AddCallback(Data.Routes.FindOrAdd(FSocketTextKey(Route)), Owner, MoveTemp(Handler), false, Data.NextRouteOrder++));
}
FVaRestSocket FVaRestSocket::On(FStringView Route, const UObject* Owner, TFunction<void(const FVaRestJson&)> Handler)
{
	check(IsInGameThread());
	if (!Handler) return *this;
	TFunction<void(const FVaRestSocketMessage&)> Adapted = [Handler = MoveTemp(Handler)](const FVaRestSocketMessage& Message) { Handler(Message.Data()); };
	return On(Route, Owner, MoveTemp(Adapted));
}
FVaRestSocket FVaRestSocket::OnRequest(FStringView Route, const UObject* Owner, TFunction<void(const FVaRestSocketMessage&)> Handler)
{
	VAREST_SOCKET_EDIT(AddCallback(Data.Routes.FindOrAdd(FSocketTextKey(Route)), Owner, MoveTemp(Handler), true, Data.NextRouteOrder++));
}
void FVaRestSocket::Off(FStringView Route, const UObject* Owner)
{
	check(IsInGameThread());
	if (auto* Object = FSocketAccess::StateOnGT(*this))
	{
		const TStrongObjectPtr<UVaRestSocketState> Pin(Object);
		auto& Routes = Object->Data->Routes;
		const FSocketTextKey Key(Route);
		if (auto* Handlers = Routes.Find(Key))
		{
			RemoveCallbacks(*Handlers, Owner);
			if (Handlers->IsEmpty()) Routes.Remove(Key);
		}
		if (auto Channel = Object->Data->FindChannel({}))
			if (auto* Handlers = Channel->Routes.Find(Key)) { RemoveCallbacks(*Handlers, Owner); if (Handlers->IsEmpty()) Channel->Routes.Remove(Key); }
	}
}

void FVaRestSocket::OffAll(const UObject* Owner)
{
	check(IsInGameThread());
	if (auto* Object = FSocketAccess::StateOnGT(*this))
	{
		const TStrongObjectPtr<UVaRestSocketState> Pin(Object);
		auto& Data = *Object->Data;
		RemoveCallbacks(Data.Handshake, Owner); RemoveCallbacks(Data.Connected, Owner); RemoveCallbacks(Data.Ready, Owner);
		RemoveCallbacks(Data.Disconnected, Owner); RemoveCallbacks(Data.Retry, Owner); RemoveCallbacks(Data.GaveUp, Owner);
		RemoveCallbacks(Data.StateChanged, Owner); RemoveCallbacks(Data.Error, Owner); RemoveCallbacks(Data.Drained, Owner);
		RemoveCallbacks(Data.Message, Owner); RemoveCallbacks(Data.Binary, Owner);
		RemoveCallbacks(Data.Unrouted, Owner);
		for (auto It = Data.Routes.CreateIterator(); It; ++It)
		{
			RemoveCallbacks(It.Value(), Owner);
			if (It.Value().IsEmpty()) It.RemoveCurrent();
		}
		for (auto& Pair : Data.Channels)
		{
			auto& Channel = *Pair.Value;
			RemoveCallbacks(Channel.Connected, Owner); RemoveCallbacks(Channel.Ready, Owner); RemoveCallbacks(Channel.Disconnected, Owner);
			RemoveCallbacks(Channel.Message, Owner); RemoveCallbacks(Channel.Unrouted, Owner); RemoveCallbacks(Channel.Error, Owner);
			for (auto It = Channel.Routes.CreateIterator(); It; ++It) { RemoveCallbacks(It.Value(), Owner); if (It.Value().IsEmpty()) It.RemoveCurrent(); }
		}
	}
}
FVaRestSocket FVaRestSocket::Connect() { VAREST_SOCKET_EDIT(Data.Queue({EOperation::Connect})); }
void FVaRestSocket::Close(int32 Code, FStringView Reason)
{
	check(IsInGameThread());
	if (auto* Object = FSocketAccess::StateOnGT(*this))
	{
		const TStrongObjectPtr<UVaRestSocketState> Pin(Object);
		Object->Data->RequestClose(Code, Reason);
	}
}
void FVaRestSocket::ReconnectNow()
{
	check(IsInGameThread());
	if (auto* Object = FSocketAccess::StateOnGT(*this)) Object->Data->Queue({EOperation::ReconnectNow});
}
void FVaRestSocket::StopReconnecting()
{
	check(IsInGameThread());
	if (auto* Object = FSocketAccess::StateOnGT(*this))
	{
		if (Object->Data->IsWaiting()) Object->Data->bStopRequested = true;
		Object->Data->Queue({EOperation::Stop});
	}
}
void FVaRestSocket::Restart(FStringView Reason)
{
	check(IsInGameThread());
	if (auto* Object = FSocketAccess::StateOnGT(*this))
	{
		FOperation Operation{EOperation::Restart}; Operation.Reason = FString(Reason); Object->Data->Queue(MoveTemp(Operation));
	}
}
#undef VAREST_SOCKET_EDIT

EVaRestSocketState FVaRestSocket::GetState() const { return GetStatus().State; }
FVaRestSocketStatus FVaRestSocket::GetStatus() const
{
	if (auto* Object = FSocketAccess::StateOnGT(*this)) return Object->Data->GetStatusForCallback();
	return {};
}
FVaRestSocketBatchPin FVaRestSocket::CaptureDispatchPinOnGT(FStringView Channel) const
{
	check(IsInGameThread()); FVaRestSocketBatchPin Result;
	if (auto* Object = FSocketAccess::StateOnGT(*this))
	{
		auto& Data = *Object->Data; Result.ConnectionGeneration = Data.Generation;
		if (auto Found = Data.FindChannel(Channel)) { Result.Channel = Found->Name; Result.ChannelEpoch = Found->Epoch; }
	}
	return Result;
}
bool FVaRestSocket::IsDispatchPinCurrentOnGT(const FVaRestSocketBatchPin& DispatchPin) const
{
	check(IsInGameThread());
	if (auto* Object = FSocketAccess::StateOnGT(*this))
	{
		auto& Data = *Object->Data;
		if (Data.bDisposed || Data.Status.State != EVaRestSocketState::Connected || Data.Generation != DispatchPin.ConnectionGeneration) return false;
		const auto Channel = Data.FindChannel(DispatchPin.Channel);
		return Channel && Channel->bUp && Channel->Epoch == DispatchPin.ChannelEpoch;
	}
	return false;
}
FVaRestJsonConvertOptions FVaRestSocket::GetConversionOptionsOnGT() const
{
	check(IsInGameThread());
	if (auto* Object = FSocketAccess::StateOnGT(*this)) return Object->Data->Outbox->GetEncoder().ConvertOptions;
	return {};
}
FVaRestError FVaRestSocket::MakeDecodeFailureOnGT(const FVaRestJsonDecodeReport& Report) const
{
	check(IsInGameThread()); FVaRestError Failure; Failure.Kind = EVaRestErrorKind::Decode;
	Failure.Message = Report.Errors.IsEmpty() ? TEXT("Socket message data does not match its registered native type") : Report.Errors[0].ToString();
	if (auto* Object = FSocketAccess::StateOnGT(*this); Object && Object->Data->Redactor) Failure.Message = Object->Data->Redactor->ScrubText(Failure.Message);
	return Failure;
}
void FVaRestSocket::ReportDecodeFailureOnGT(const FVaRestJsonDecodeReport& Report)
{
	check(IsInGameThread());
	if (auto* Object = FSocketAccess::StateOnGT(*this))
	{
		const TStrongObjectPtr<UVaRestSocketState> ObjectPin(Object);
		const auto Failure = MakeDecodeFailureOnGT(Report);
		Object->Data->Notify(Object->Data->Error, Failure); Object->Data->DrainOperations();
	}
}
FVaRestSocketJsonRequest FVaRestSocket::RequestStructSnapshotOnGT(FStringView Route, const UScriptStruct* Struct, const void* Value)
{
	check(IsInGameThread()); FVaRestSocketJsonRequest Builder(*this, Route, {});
	if (!IsValid() || !Struct || !Value || !Struct->GetCppStructOps())
	{
		FVaRestError Failure; Failure.Kind = EVaRestErrorKind::Config; Failure.Message = TEXT("A typed socket payload requires a live socket and a native struct");
		Builder.Options->PayloadFailure = MoveTemp(Failure); return Builder;
	}
	const TStrongObjectPtr<const UScriptStruct> TypePin(Struct);
	auto Payload = MakeUnique<FStructInputOwner>();
	Payload->Box->Value.InitializeAs(Struct, static_cast<const uint8*>(Value));
	Builder.Options->PayloadStruct = Payload->Box->Value.GetScriptStruct();
	Builder.Options->PayloadData = Payload->Box->Value.GetMemory();
	Builder.Options->PayloadWriter = [Payload = MoveTemp(Payload)](VaRest::Json::FCondensedWriter& Writer, bool bMembers,
		TConstArrayView<FString> Skip, const FVaRestJsonConvertOptions& Options, FVaRestJsonError& Error)
	{
		check(IsInGameThread());
		return WriteStructPayload(Writer, Payload->Box->Value.GetScriptStruct(), Payload->Box->Value.GetMemory(), Options, bMembers, Skip, Error);
	};
	return Builder;
}

FVaRestSocketJsonRequest FVaRestSocket::PreparePropertyRequestOnGT(FStringView Route, FStringView Channel, const FProperty* Property, const void* Value)
{
	check(IsInGameThread()); FVaRestSocketJsonRequest Builder(*this, Route, {});
	const auto Fail = [&](const TCHAR* Message)
	{ FVaRestError Error; Error.Kind = EVaRestErrorKind::Config; Error.Message = Message; Builder.Options->PayloadFailure = MoveTemp(Error); };
	auto* Object = FSocketAccess::StateOnGT(*this);
	if (!Object || Object->Data->bDisposed || !Property || !Value)
	{ Fail(TEXT("A prepared socket request requires a live socket and payload")); return Builder; }
	const TStrongObjectPtr<UVaRestSocketState> StatePin(Object);
	const TStrongObjectPtr<UStruct> PropertyOwner(Property->GetOwnerStruct());
	auto& Data = *Object->Data; const FString RequestChannel = Data.ResolveChannel(Channel);
	const auto Encoder = Data.Outbox->GetEncoder(RequestChannel);
	if (!Encoder.Codec || Encoder.Context.Routing.IdField.IsEmpty() || !Data.FindChannel(RequestChannel)
		|| Data.NextRequestId == MAX_int64 || (Encoder.Codec->GetProtocolTag() == TEXT("SocketIO") && Data.NextRequestId >= 9007199254740991LL))
	{ Fail(TEXT("The request needs an admitted channel, correlation field and available id")); return Builder; }
	const int64 Number = ++Data.NextRequestId;
	const auto Id = Encoder.Context.Routing.IdFormat == EVaRestSocketIdFormat::Number ? FVaRestSocketId::Numeric(Number)
		: FVaRestSocketId::String(Encoder.Context.ReplyIdPrefix + LexToString(Number));
	const auto* StructProperty = CastField<FStructProperty>(Property);
	const bool bJson = StructProperty && StructProperty->Struct == FVaRestJson::StaticStruct();
	const auto* Json = bJson ? static_cast<const FVaRestJson*>(Value) : nullptr;
	auto Write = [&](VaRest::Json::FCondensedWriter& Writer, bool bMembers, TConstArrayView<FString> Skip, FVaRestJsonError& Error)
	{
		if (Json) return WriteJsonPayload(Writer, *Json, bMembers, Skip, Error);
		if (StructProperty) return WriteStructPayload(Writer, StructProperty->Struct, Value, Encoder.ConvertOptions, bMembers, Skip, Error);
		return !bMembers && VaRest::Json::WritePropertyInternal(Writer, Property, Value, Encoder.ConvertOptions, 1, &Error);
	};
	FVaRestSocketPayloadWriter View(Write, Json ? Json->IsObject() : StructProperty != nullptr);
	View.NativeStruct = StructProperty && !bJson ? StructProperty->Struct.Get() : nullptr; View.NativeValue = Value;
	View.Json = Json; View.ConvertOptions = &Encoder.ConvertOptions;
	auto Encoded = Encoder.Codec->Encode(RequestChannel, Route, View, Id, Encoder.Context);
	if (!Encoded.HasValue())
	{
		auto Error = Encoded.StealError(); if (!Error.IsError()) Error.Kind = EVaRestErrorKind::Config;
		Builder.Options->PayloadFailure = MoveTemp(Error); return Builder;
	}
	Builder.Options->Channel = RequestChannel; Builder.Options->PreparedId = Id;
	Builder.Options->PreparedEpoch = Encoder.Epoch; Builder.Options->PreparedEncoderRevision = Encoder.EncoderRevision;
	Builder.Options->PreparedQueueEpoch = Encoder.QueueEpoch;
	Builder.Options->FrameFactory = [Frames = Encoded.StealValue()](const FVaRestSocketId&, const IVaRestSocketCodec&,
		const FVaRestSocketCodecContext&, const FVaRestJsonConvertOptions&) mutable -> TValueOrError<TArray<FVaRestSocketFrame>, FVaRestError>
	{ return MakeValue(MoveTemp(Frames)); };
	// No descriptor, address, owned reflected copy or user conversion callback survives this synchronous borrow.
	return Builder;
}

FVaRestSocketStats FVaRestSocket::GetStats() const
{
	if (auto* Object = FSocketAccess::StateOnGT(*this)) return Object->Data->GetStats();
	return {};
}
FVaRestSocketOpen FVaRestSocket::GetLastOpen() const
{
	if (auto* Object = FSocketAccess::StateOnGT(*this)) return Object->Data->LastOpen;
	return {};
}
FName FVaRestSocket::GetName() const
{
	if (auto* Object = FSocketAccess::StateOnGT(*this)) return Object->Data->SocketName;
	return NAME_None;
}
FString FVaRestSocket::GetRedactedUrl() const
{
	if (auto* Object = FSocketAccess::StateOnGT(*this))
	{
		const auto& Data = *Object->Data;
		return Data.TraceRedactor()->RedactUrl(Data.Url);
	}
	return {};
}
FString FVaRestSocket::ScrubTextOnGT(FStringView Text) const
{
	check(IsInGameThread());
	if (auto* Object = FSocketAccess::StateOnGT(*this)) return SocketLogText(*Object->Data->TraceRedactor(), Text);
	return {};
}
int64 FVaRestSocket::GetBufferedAmount() const
{
	if (auto* Object = FSocketAccess::StateOnGT(*this)) return Object->Data->Connection ? Object->Data->Connection->GetBufferedAmount() : 0;
	return 0;
}

EVaRestSendResult FVaRestSocket::SendEncoded(TArray<FVaRestSocketFrame> Frames, EVaRestSend Flags, uint64 Epoch, TOptional<FVaRestSocketBatchPin> Pin, FString Channel, uint64 QueueEpoch)
{
	if (!IsValid() || !Handle->Outbox) return EVaRestSendResult::Rejected;
	FSocketSendUnit Unit; Unit.Frames = MoveTemp(Frames); Unit.Flags = Flags; Unit.Pin = MoveTemp(Pin);
	const auto Snapshot = Handle->Outbox->GetEncoder(Channel);
	Unit.Channel = Snapshot.Channel; Unit.QueueEpoch = QueueEpoch ? QueueEpoch : Snapshot.QueueEpoch;
	for (const auto& Frame : Unit.Frames) Unit.Bytes += FMath::Max(0, Frame.Length);
	if (IsInGameThread())
	{
		if (auto* Object = FSocketAccess::StateOnGT(*this))
		{
			const TStrongObjectPtr<UVaRestSocketState> ObjectPin(Object);
			return Object->Data->SendUnit(MoveTemp(Unit), Epoch);
		}
		return EVaRestSendResult::Rejected;
	}
	uint64 Sequence = 0;
	return Handle->Outbox->Post(MoveTemp(Unit), Epoch, Sequence);
}
EVaRestSendResult FVaRestSocket::Send(FStringView Text, EVaRestSend Flags)
{
	if (!IsValid()) return EVaRestSendResult::Rejected;
	const auto Snapshot = Handle->Outbox->GetEncoder();
	TArray<FVaRestSocketFrame> Frames; Frames.Add(FVaRestSocketFrame::Text(Text));
	return SendEncoded(MoveTemp(Frames), Flags, Snapshot.Epoch, {}, Snapshot.Channel, Snapshot.QueueEpoch);
}
EVaRestSendResult FVaRestSocket::Send(FUtf8StringView Text, EVaRestSend Flags)
{
	if (!IsValid()) return EVaRestSendResult::Rejected;
	const auto Snapshot = Handle->Outbox->GetEncoder();
	TArray<FVaRestSocketFrame> Frames; Frames.Add(FVaRestSocketFrame::Text(Text));
	return SendEncoded(MoveTemp(Frames), Flags, Snapshot.Epoch, {}, Snapshot.Channel, Snapshot.QueueEpoch);
}
EVaRestSendResult FVaRestSocket::Send(const FVaRestJson& Json, EVaRestSend Flags)
{
	if (!IsValid()) return EVaRestSendResult::Rejected;
	const auto Snapshot = Handle->Outbox->GetEncoder();
	FVaRestSocketFrame Frame;
	if (Json.IsValid())
	{
		Frame.Storage.SetNumUninitialized(VaRest::WebSocket::FrameHeadroom);
		if (Json.TryToUtf8(Frame.Storage).HasValue()) Frame.Length = Frame.Storage.Num() - Frame.Offset;
		else Frame.Storage.Reset();
	}
	TArray<FVaRestSocketFrame> Frames; Frames.Add(MoveTemp(Frame));
	return SendEncoded(MoveTemp(Frames), Flags, Snapshot.Epoch, {}, Snapshot.Channel, Snapshot.QueueEpoch);
}
EVaRestSendResult FVaRestSocket::SendBinary(TArray<uint8>&& Bytes, EVaRestSend Flags)
{
	if (!IsValid()) return EVaRestSendResult::Rejected;
	const auto Snapshot = Handle->Outbox->GetEncoder();
	FVaRestSocketFrame Frame; Frame.Kind = EVaRestSocketFrameKind::Binary;
	if (Bytes.Num() <= MAX_int32 - VaRest::WebSocket::FrameHeadroom)
	{
		Frame.Length = Bytes.Num(); Frame.Storage = MoveTemp(Bytes); Frame.Storage.InsertUninitialized(0, VaRest::WebSocket::FrameHeadroom);
	}
	TArray<FVaRestSocketFrame> Frames; Frames.Add(MoveTemp(Frame));
	return SendEncoded(MoveTemp(Frames), Flags, Snapshot.Epoch, {}, Snapshot.Channel, Snapshot.QueueEpoch);
}
EVaRestSendResult FVaRestSocket::SendBatch(TArray<FVaRestSocketFrame> Frames, EVaRestSend Flags, TOptional<FVaRestSocketBatchPin> Pin)
{
	if (!IsValid()) return EVaRestSendResult::Rejected;
	const auto Snapshot = Handle->Outbox->GetEncoder(Pin.IsSet() ? FStringView(Pin->Channel) : FStringView());
	if (Pin.IsSet() && Pin->Channel.IsEmpty()) Pin->Channel = Snapshot.Channel;
	return SendEncoded(MoveTemp(Frames), Flags, Snapshot.Epoch, MoveTemp(Pin), Snapshot.Channel, Snapshot.QueueEpoch);
}
EVaRestSendResult FVaRestSocket::SendFrame(FVaRestSocketFrame Frame)
{
	check(IsInGameThread());
	TArray<FVaRestSocketFrame> Frames; Frames.Add(MoveTemp(Frame)); return SendBatch(MoveTemp(Frames));
}
EVaRestSendResult FVaRestSocket::EncodeAndSend(FStringView Channel, FEncodeCall Encode, EVaRestSend Flags)
{
	if (!IsValid() || !Handle->Outbox) return EVaRestSendResult::Rejected;
	const auto Snapshot = Handle->Outbox->GetEncoder(Channel);
	if (!Snapshot.Codec) return EVaRestSendResult::Rejected;
	auto Frames = Encode(*Snapshot.Codec, Snapshot.Context, Snapshot.ConvertOptions, Snapshot.Channel);
	if (!Frames.HasValue()) { Handle->Outbox->EncodingFailed(); return EVaRestSendResult::Rejected; }
	return SendEncoded(Frames.StealValue(), Flags, Snapshot.Epoch, {}, Snapshot.Channel, Snapshot.QueueEpoch);
}
TValueOrError<TArray<FVaRestSocketFrame>, FVaRestError> FVaRestSocket::EncodeOnly(FStringView Channel, FEncodeCall Encode) const
{
	if (IsValid() && Handle->Outbox)
	{
		const auto Snapshot = Handle->Outbox->GetEncoder(Channel);
		if (Snapshot.Codec) return Encode(*Snapshot.Codec, Snapshot.Context, Snapshot.ConvertOptions, Snapshot.Channel);
	}
	FVaRestError Error; Error.Kind = EVaRestErrorKind::Canceled; Error.Message = TEXT("Socket encoder is unavailable"); return MakeError(MoveTemp(Error));
}
EVaRestSendResult FVaRestSocket::Emit(FStringView Route, const FVaRestJson& Payload, EVaRestSend Flags)
{
	if (!IsValid() || !Handle->Outbox) return EVaRestSendResult::Rejected;
	const auto Snapshot = Handle->Outbox->GetEncoder();
	if (!Snapshot.Codec) return EVaRestSendResult::Rejected;
	auto Write = [&Payload](VaRest::Json::FCondensedWriter& Writer, bool bMembers, TConstArrayView<FString> SkipKeys, FVaRestJsonError& Error)
	{ return WriteJsonPayload(Writer, Payload, bMembers, SkipKeys, Error); };
	FVaRestSocketPayloadWriter View(Write, Payload.IsObject()); View.Json = &Payload;
	auto Encoded = Snapshot.Codec->Encode(Snapshot.Channel, Route, View, {}, Snapshot.Context);
	if (!Encoded.HasValue()) { Handle->Outbox->EncodingFailed(); return EVaRestSendResult::Rejected; }
	return SendEncoded(Encoded.StealValue(), Flags, Snapshot.Epoch, {}, Snapshot.Channel, Snapshot.QueueEpoch);
}
EVaRestSendResult FVaRestSocket::SendStruct(const UScriptStruct* Struct, const void* Value, EVaRestSend Flags)
{
	if (!IsValid()) return EVaRestSendResult::Rejected;
	const auto Snapshot = Handle->Outbox->GetEncoder();
	FVaRestSocketFrame Frame; Frame.Storage.SetNumUninitialized(FrameHeadroom);
	VaRest::Json::FUtf8Sink Sink{Frame.Storage}; VaRest::Json::FCondensedWriter Writer(Sink);
	FVaRestJsonError Error;
	if (!VaRest::Json::WriteStructInternal(Writer, Struct, Value, Snapshot.ConvertOptions, 0, &Error))
	{ Handle->Outbox->EncodingFailed(); return EVaRestSendResult::Rejected; }
	Frame.Length = Frame.Storage.Num() - Frame.Offset;
	TArray<FVaRestSocketFrame> Frames; Frames.Add(MoveTemp(Frame));
	return SendEncoded(MoveTemp(Frames), Flags, Snapshot.Epoch, {}, Snapshot.Channel, Snapshot.QueueEpoch);
}
EVaRestSendResult FVaRestSocket::EmitStruct(FStringView Channel, FStringView Route, const UScriptStruct* Struct, const void* Value, EVaRestSend Flags)
{
	if (!IsValid()) return EVaRestSendResult::Rejected;
	const auto Snapshot = Handle->Outbox->GetEncoder(Channel); if (!Snapshot.Codec) return EVaRestSendResult::Rejected;
	auto Write = [&](VaRest::Json::FCondensedWriter& Writer, bool bMembers, TConstArrayView<FString> Skip, FVaRestJsonError& Error)
	{ return WriteStructPayload(Writer, Struct, Value, Snapshot.ConvertOptions, bMembers, Skip, Error); };
	FVaRestSocketPayloadWriter View(Write, true); View.NativeStruct = Struct; View.NativeValue = Value; View.ConvertOptions = &Snapshot.ConvertOptions;
	auto Encoded = Snapshot.Codec->Encode(Snapshot.Channel, Route, View, {}, Snapshot.Context);
	if (!Encoded.HasValue()) { Handle->Outbox->EncodingFailed(); return EVaRestSendResult::Rejected; }
	return SendEncoded(Encoded.StealValue(), Flags, Snapshot.Epoch, {}, Snapshot.Channel, Snapshot.QueueEpoch);
}
EVaRestSendResult FVaRestSocket::SendPropertyOnGT(const FProperty* Property, const void* Value, EVaRestSend Flags)
{
	check(IsInGameThread()); if (!Property || !Value || !IsValid()) return EVaRestSendResult::Rejected;
	const TStrongObjectPtr<UStruct> TypePin(Property->GetOwnerStruct());
	const auto Snapshot = Handle->Outbox->GetEncoder();
	FVaRestSocketFrame Frame; Frame.Storage.SetNumUninitialized(FrameHeadroom);
	VaRest::Json::FUtf8Sink Sink{Frame.Storage}; VaRest::Json::FCondensedWriter Writer(Sink); FVaRestJsonError Error;
	if (!VaRest::Json::WritePropertyInternal(Writer, Property, Value, Snapshot.ConvertOptions, 0, &Error))
	{ Handle->Outbox->EncodingFailed(); return EVaRestSendResult::Rejected; }
	Frame.Length = Frame.Storage.Num() - Frame.Offset;
	TArray<FVaRestSocketFrame> Frames; Frames.Add(MoveTemp(Frame));
	return SendEncoded(MoveTemp(Frames), Flags, Snapshot.Epoch, {}, Snapshot.Channel, Snapshot.QueueEpoch);
}
EVaRestSendResult FVaRestSocket::EmitPropertyOnGT(FStringView Route, const FProperty* Property, const void* Value, EVaRestSend Flags)
{
	check(IsInGameThread()); if (!Property || !Value || !IsValid()) return EVaRestSendResult::Rejected;
	const TStrongObjectPtr<UStruct> TypePin(Property->GetOwnerStruct());
	if (const auto* StructProperty = CastField<FStructProperty>(Property); StructProperty && Property->ArrayDim == 1)
	{
		if (StructProperty->Struct == FVaRestJson::StaticStruct()) return Emit(Route, *static_cast<const FVaRestJson*>(Value), Flags);
		return EmitStruct({}, Route, StructProperty->Struct, Value, Flags);
	}
	const auto Snapshot = Handle->Outbox->GetEncoder(); if (!Snapshot.Codec) return EVaRestSendResult::Rejected;
	auto Write = [&](VaRest::Json::FCondensedWriter& Writer, bool bMembers, TConstArrayView<FString>, FVaRestJsonError& Error)
	{ return !bMembers && VaRest::Json::WritePropertyInternal(Writer, Property, Value, Snapshot.ConvertOptions, 1, &Error); };
	FVaRestSocketPayloadWriter View(Write, false); View.ConvertOptions = &Snapshot.ConvertOptions;
	auto Encoded = Snapshot.Codec->Encode(Snapshot.Channel, Route, View, {}, Snapshot.Context);
	if (!Encoded.HasValue()) { Handle->Outbox->EncodingFailed(); return EVaRestSendResult::Rejected; }
	return SendEncoded(Encoded.StealValue(), Flags, Snapshot.Epoch, {}, Snapshot.Channel, Snapshot.QueueEpoch);
}
int32 FVaRestSocket::GetQueuedCount() const { check(IsInGameThread()); return IsValid() ? Handle->Outbox->CountChannel(Handle->Outbox->GetEncoder().Channel) : 0; }
void FVaRestSocket::ClearQueue() { check(IsInGameThread()); if (IsValid()) Handle->Outbox->ClearChannel(Handle->Outbox->GetEncoder().Channel); }

double FVaRestSocket::GetLatency() const
{
	if (auto* Object = FSocketAccess::StateOnGT(*this)) return Object->Data->Latency;
	return 0.0;
}
double FVaRestSocket::GetLatencyAverage() const
{
	if (auto* Object = FSocketAccess::StateOnGT(*this)) return Object->Data->LatencyAverage;
	return 0.0;
}

FVaRestHeartbeat FVaRestHeartbeat::Text(FString Ping, FString Pong)
{
	FVaRestHeartbeat Out; Out.Mode = EVaRestHeartbeatMode::ClientPing; Out.Ping = MoveTemp(Ping); Out.Pong = MoveTemp(Pong); return Out;
}
FVaRestHeartbeat FVaRestHeartbeat::Json(FString PingRoute, FString PongRoute)
{
	auto Out = Text(MoveTemp(PingRoute), MoveTemp(PongRoute)); Out.bJson = true; return Out;
}
FVaRestHeartbeat FVaRestHeartbeat::ServerPing(FString Ping, FString Pong, bool bJson)
{
	auto Out = Text(MoveTemp(Ping), MoveTemp(Pong)); Out.Mode = EVaRestHeartbeatMode::ServerPing; Out.bJson = bJson; return Out;
}

EVaRestSendResult FVaRestSocketMessage::Reply(const FVaRestJson& InPayload) const
{
	return ReplyToken ? ReplyToken->Send(InPayload, false) : EVaRestSendResult::Rejected;
}
EVaRestSendResult FVaRestSocketMessage::ReplyArguments(FVaRestSocketArgumentsWriter Arguments) const
{
	return ReplyToken ? ReplyToken->SendArguments(Arguments) : EVaRestSendResult::Rejected;
}
bool FVaRestSocketMessage::CanReplyOnGT() const
{
	check(IsInGameThread());
	if (!ReplyToken || !ReplyToken->Codec || ReplyToken->bUsed.load(std::memory_order_acquire)) return false;
	const auto Socket = ReplyToken->Socket.Pin(); return Socket.IsValid() && Socket.IsProtocolDispatchCurrentOnGT(ReplyToken->Pin);
}
EVaRestSendResult FVaRestSocketMessage::ReplyStruct(const UScriptStruct* Struct, const void* Value) const
{
	return ReplyToken ? ReplyToken->SendStruct(Struct, Value) : EVaRestSendResult::Rejected;
}
EVaRestSendResult FVaRestSocketMessage::ReplyPropertyOnGT(const FProperty* Property, const void* Value) const
{
	check(IsInGameThread()); return ReplyToken ? ReplyToken->SendPropertyOnGT(Property, Value) : EVaRestSendResult::Rejected;
}
EVaRestSendResult FVaRestSocketMessage::ReplyError(FString Code, FString Message) const
{
	const auto Error = FVaRestJson::Object().Set(TEXT("code"), MoveTemp(Code)).Set(TEXT("message"), MoveTemp(Message));
	return ReplyToken ? ReplyToken->Send(Error, true) : EVaRestSendResult::Rejected;
}
EVaRestSendResult FVaRestSocketMessage::ReplyError(int64 Code, FString Message) const
{
	const auto Error = FVaRestJson::Object().Set(TEXT("code"), Code).Set(TEXT("message"), MoveTemp(Message));
	return ReplyToken ? ReplyToken->Send(Error, true) : EVaRestSendResult::Rejected;
}
EVaRestSendResult VaRest::WebSocket::FSocketReplyToken::Send(const FVaRestJson& Payload, bool bError)
{
	auto Write = [&Payload](VaRest::Json::FCondensedWriter& Writer, bool bMembers, TConstArrayView<FString> SkipKeys, FVaRestJsonError& Error)
	{ return WriteJsonPayload(Writer, Payload, bMembers, SkipKeys, Error); };
	FVaRestSocketPayloadWriter View(Write, Payload.IsObject()); View.Json = &Payload;
	return SendWriter(View, bError);
}
EVaRestSendResult VaRest::WebSocket::FSocketReplyToken::SendStruct(const UScriptStruct* Struct, const void* Value)
{
	return SendWith([&](const FVaRestJsonConvertOptions& Options)
	{
		auto Write = [&](VaRest::Json::FCondensedWriter& Writer, bool bMembers, TConstArrayView<FString> Skip, FVaRestJsonError& Error)
		{ return WriteStructPayload(Writer, Struct, Value, Options, bMembers, Skip, Error); };
		FVaRestSocketPayloadWriter View(Write, true); View.NativeStruct = Struct; View.NativeValue = Value; View.ConvertOptions = &Options;
		return Codec->EncodeReply(Pin.Channel, Id, View, false, Context);
	});
}
EVaRestSendResult VaRest::WebSocket::FSocketReplyToken::SendWriter(const FVaRestSocketPayloadWriter& Payload, bool bError)
{
	return SendWith([&](const FVaRestJsonConvertOptions& Options)
	{
		auto View = Payload; if (!View.ConvertOptions) View.ConvertOptions = &Options;
		return Codec->EncodeReply(Pin.Channel, Id, View, bError, Context);
	});
}
EVaRestSendResult VaRest::WebSocket::FSocketReplyToken::SendArguments(FVaRestSocketArgumentsWriter Arguments)
{
	return SendWith([&](const FVaRestJsonConvertOptions& Options)
	{
		auto Unused = [](auto&, bool, TConstArrayView<FString>, FVaRestJsonError&) { return false; };
		FVaRestSocketPayloadWriter View(Unused, false); View.Arguments.Emplace(Arguments); View.ConvertOptions = &Options;
		return Codec->EncodeReply(Pin.Channel, Id, View, false, Context);
	}, true);
}
EVaRestSendResult VaRest::WebSocket::FSocketReplyToken::SendWith(FEncodeReply Encode, bool bRejectDuplicate)
{
	auto Pinned = Socket.Pin();
	if (!Pinned.IsValid() || !Codec) return EVaRestSendResult::Rejected;
	if (bUsed.exchange(true, std::memory_order_acq_rel))
	{
		UE_LOG(LogVaRestSocket, Warning, TEXT("VaRest socket: a request message was replied to more than once"));
		return bRejectDuplicate ? EVaRestSendResult::Rejected : EVaRestSendResult::Dropped;
	}
	const auto Snapshot = Pinned.Handle->Outbox->GetEncoder(Pin.Channel);
	auto Encoded = Encode(Snapshot.ConvertOptions);
	if (!Encoded.HasValue()) { Pinned.Handle->Outbox->EncodingFailed(); return EVaRestSendResult::Rejected; }
	return Pinned.SendEncoded(Encoded.StealValue(), EVaRestSend::Handshake, Snapshot.Epoch, Pin, Snapshot.Channel, Snapshot.QueueEpoch);
}
EVaRestSendResult VaRest::WebSocket::FSocketReplyToken::SendPropertyOnGT(const FProperty* Property, const void* Value)
{
	check(IsInGameThread()); if (!Property || !Value) return EVaRestSendResult::Rejected;
	const TStrongObjectPtr<UStruct> TypePin(Property->GetOwnerStruct());
	if (const auto* StructProperty = CastField<FStructProperty>(Property); StructProperty && Property->ArrayDim == 1)
	{
		if (StructProperty->Struct == FVaRestJson::StaticStruct()) return Send(*static_cast<const FVaRestJson*>(Value), false);
		return SendStruct(StructProperty->Struct, Value);
	}
	return SendWith([&](const FVaRestJsonConvertOptions& Options)
	{
		auto Write = [&](VaRest::Json::FCondensedWriter& Writer, bool bMembers, TConstArrayView<FString>, FVaRestJsonError& Error)
		{ return !bMembers && VaRest::Json::WritePropertyInternal(Writer, Property, Value, Options, 1, &Error); };
		FVaRestSocketPayloadWriter View(Write, false); View.ConvertOptions = &Options;
		return Codec->EncodeReply(Pin.Channel, Id, View, false, Context);
	});
}

namespace VaRest::WebSocket
{
namespace
{
	FVaRestError RequestFailure(EVaRestErrorKind Kind, FStringView Message)
	{
		FVaRestError Out; Out.Kind = Kind; Out.Message = FString(Message);
		Out.bRetryable = Kind == EVaRestErrorKind::Connection || Kind == EVaRestErrorKind::Timeout || Kind == EVaRestErrorKind::Offline;
		return Out;
	}
}

FVaRestSocketRequestHandle FSocketData::StartRequest(FSocketJsonRequestOptions Options)
{
	check(IsInGameThread());
	auto Refuse = [&](EVaRestErrorKind Kind, FStringView Reason)
	{
		const auto Failure = RequestFailure(Kind, Reason);
		if (Options.Error && (!Options.bOwned || Options.Owner.IsValid()))
		{
			++DispatchDepth; Options.Error(Failure); --DispatchDepth;
		}
		return FVaRestSocketRequestHandle();
	};
	if (bDisposed) return Refuse(EVaRestErrorKind::Canceled, TEXTVIEW("WebSocket scope has stopped"));
	if (Options.bOwned && !Options.Owner.IsValid()) return {};
	if (Options.PayloadFailure.IsSet()) return Refuse(Options.PayloadFailure->Kind, Options.PayloadFailure->Message);
	if (CodecContext.Routing.IdField.IsEmpty()) return Refuse(EVaRestErrorKind::Config, TEXTVIEW("Socket Request requires a routing IdField"));
	if (!FMath::IsFinite(Options.Timeout) || Options.Timeout < 0.0) return Refuse(EVaRestErrorKind::Config, TEXTVIEW("Socket request timeout must be finite and nonnegative"));
	if (Pending.Num() >= Limits.MaxPendingRequests) return Refuse(EVaRestErrorKind::Offline, TEXTVIEW("Socket pending request limit reached"));
	if (!Options.PreparedId.IsSet() && NextRequestId == MAX_int64) return Refuse(EVaRestErrorKind::Config, TEXTVIEW("Socket request id space exhausted"));
	if (!Options.PreparedId.IsSet() && Codec && Codec->GetProtocolTag() == TEXT("SocketIO") && NextRequestId >= 9007199254740991LL) return Refuse(EVaRestErrorKind::Config, TEXTVIEW("Socket.IO ack id space exhausted"));
	const FString RequestChannel = ResolveChannel(Options.Channel);
	if (!FindChannel(RequestChannel)) return Refuse(EVaRestErrorKind::Config, TEXTVIEW("Socket request channel is not registered"));
	const auto Encoder = Outbox->GetEncoder(RequestChannel);
	if (!Encoder.Codec) return Refuse(EVaRestErrorKind::Config, TEXTVIEW("Socket codec is unavailable"));
	if (Options.PreparedId.IsSet() && (Options.PreparedEpoch != Encoder.Epoch || Options.PreparedEncoderRevision != Encoder.EncoderRevision
		|| Options.PreparedQueueEpoch != Encoder.QueueEpoch))
		return Refuse(EVaRestErrorKind::Canceled, TEXTVIEW("The request encoder or queue changed after payload preparation"));
	auto Row = MakeShared<FSocketJsonPending>();
	if (Options.PreparedId.IsSet()) Row->Id = Options.PreparedId.GetValue();
	else
	{
		const int64 Number = ++NextRequestId;
		Row->Id = CodecContext.Routing.IdFormat == EVaRestSocketIdFormat::Number ? FVaRestSocketId::Numeric(Number)
			: FVaRestSocketId::String(CodecContext.ReplyIdPrefix + LexToString(Number));
	}
	Row->Options = MoveTemp(Options); Row->Channel = RequestChannel;
	Row->StartedAt = Now();
	Row->Deadline = Row->Options.Timeout > 0.0 ? Row->StartedAt + FMath::Min(1e9, Row->Options.Timeout) : 0.0;
	Row->Control = MakeShared<FSocketRequestControl, ESPMode::ThreadSafe>();
	const FString Key = Row->Id.Key();
	Pending.Add(FSocketTextKey(Key), Row);
	FVaRestSocketRequestHandle RequestHandle; RequestHandle.Control = Row->Control; RequestHandle.Id = Row->Id.bNumber ? LexToString(Row->Id.Number) : Row->Id.Text;
	auto Write = [&Row, &Encoder](VaRest::Json::FCondensedWriter& Writer, bool bMembers, TConstArrayView<FString> SkipKeys, FVaRestJsonError& Error)
	{
		return Row->Options.PayloadWriter ? Row->Options.PayloadWriter(Writer, bMembers, SkipKeys, Encoder.ConvertOptions, Error)
			: WriteJsonPayload(Writer, Row->Options.Payload, bMembers, SkipKeys, Error);
	};
	FVaRestSocketPayloadWriter View(Write, Row->Options.PayloadWriter ? true : Row->Options.Payload.IsObject());
	View.NativeStruct = Row->Options.PayloadStruct; View.NativeValue = Row->Options.PayloadData;
	View.ConvertOptions = &Encoder.ConvertOptions; View.Json = View.NativeStruct ? nullptr : &Row->Options.Payload;
	auto Encoded = Row->Options.FrameFactory ? Row->Options.FrameFactory(Row->Id, *Encoder.Codec, Encoder.Context, Encoder.ConvertOptions)
		: Encoder.Codec->Encode(Row->Channel, Row->Options.Route, View, Row->Id, Encoder.Context);
	Row->Options.PayloadStruct = nullptr; Row->Options.PayloadData = nullptr;
	Row->Options.PayloadWriter = nullptr; Row->Options.Payload = {}; // every affine input retires GTK before native enqueue
	Row->Options.FrameFactory = nullptr;
	if (!Row->Control->bActive.load(std::memory_order_acquire)) return RequestHandle; // an explicit custom encoder may have stopped the scope
	if (!Encoded.HasValue())
	{
		Row->Failure = Encoded.StealError();
		if (Row->Failure->Kind == EVaRestErrorKind::None) Row->Failure->Kind = EVaRestErrorKind::Config;
		if (Row->Failure->Message.IsEmpty()) Row->Failure->Message = TEXT("Socket request payload could not be encoded");
		return RequestHandle;
	}
	FSocketSendUnit Unit; Unit.Frames = Encoded.StealValue(); Unit.RequestKey = Key; Unit.RequestControl = Row->Control;
	Unit.Channel = Row->Channel; Unit.QueueEpoch = Encoder.QueueEpoch;
	bool bValid = !Unit.Frames.IsEmpty();
	for (const auto& Frame : Unit.Frames)
	{
		bValid = bValid && Frame.IsValid() && Frame.Length <= Limits.MaxSendBytes;
		Unit.Bytes += FMath::Max(0, Frame.Length);
	}
	const auto Result = SendUnit(MoveTemp(Unit), Encoder.Epoch);
	if ((Result == EVaRestSendResult::Dropped || Result == EVaRestSendResult::Rejected) && !Row->Failure.IsSet())
		Row->Failure = RequestFailure(bValid ? EVaRestErrorKind::Offline : EVaRestErrorKind::Config, TEXTVIEW("Socket request was refused by the send queue"));
	return RequestHandle;
}

void FSocketData::FinishRequest(const FSocketTextKey& Key, FVaRestJson Value, FVaRestError Failure, const FVaRestSocketMessage* ReplyMessage)
{
	check(IsInGameThread());
	TSharedPtr<FSocketJsonPending> Row;
	if (!Pending.RemoveAndCopyValue(Key, Row)) return;
	Row->Control->bActive.store(false, std::memory_order_release);
	Outbox->RemoveRequest(Key.Text);
	if (Failure.Kind == EVaRestErrorKind::None && ReplyMessage && Row->Options.ReplyTransform)
	{
		const uint64 BeforeTransform = Generation;
		++DispatchDepth; Failure = Row->Options.ReplyTransform(*ReplyMessage, Value); --DispatchDepth;
		if (bDisposed || Generation != BeforeTransform) return;
	}
	const uint64 BeforeTrace = Generation;
	if (FVaRestTrace::Detail() != EVaRestTraceDetail::None)
	{
		FVaRestTraceSocketNote Note; Note.Kind = EVaRestTraceSocketNoteKind::RequestEnded; Note.AttemptId = Row->SentAttemptId;
		Note.MsgId = Row->Id.bNumber ? LexToString(Row->Id.Number) : Row->Id.Text; Note.Channel = Row->Channel; Note.Route = Row->Options.Route;
		Note.Latency = FMath::Max(0.0, Now() - Row->StartedAt); Note.ErrorKind = Failure.Kind;
		switch (Failure.Kind)
		{
		case EVaRestErrorKind::None: Note.Outcome = EVaRestTraceSocketRequestOutcome::Replied; break;
		case EVaRestErrorKind::Server: case EVaRestErrorKind::Decode: Note.Outcome = EVaRestTraceSocketRequestOutcome::ErrorReply; break;
		case EVaRestErrorKind::Timeout: Note.Outcome = EVaRestTraceSocketRequestOutcome::Timeout; break;
		case EVaRestErrorKind::Connection: Note.Outcome = EVaRestTraceSocketRequestOutcome::Connection; break;
		default: Note.Outcome = EVaRestTraceSocketRequestOutcome::Canceled; break;
		}
		TraceNote(MoveTemp(Note), false);
	}
	if (bDisposed || Generation != BeforeTrace || (Row->Options.bOwned && !Row->Options.Owner.IsValid())) return;
	Failure.Message = TraceRedactor()->ScrubText(Failure.Message);
	++DispatchDepth;
	if (Failure.Kind == EVaRestErrorKind::None)
	{
		if (ReplyMessage && Row->Options.ProtocolSuccess) Row->Options.ProtocolSuccess(*ReplyMessage, Value);
		else if (ReplyMessage && Row->Options.MessageSuccess) Row->Options.MessageSuccess(*ReplyMessage);
		else if (Row->Options.Success) Row->Options.Success(Value);
	}
	else if (Row->Options.Error) Row->Options.Error(Failure);
	--DispatchDepth;
}

void FSocketData::FailRequests(EVaRestErrorKind Kind, bool bSentOnly, FStringView Reason)
{
	TArray<FSocketTextKey> Keys;
	for (const auto& Pair : Pending) if (!bSentOnly || Pair.Value->bSent) Keys.Add(Pair.Key);
	const auto Failure = RequestFailure(Kind, Reason);
	for (const auto& Key : Keys) FinishRequest(Key, {}, Failure);
}
void FSocketData::TickRequests()
{
	for (const auto& Drop : Outbox->TakeDroppedRequests()) FailedRequest(Drop.Key, Drop.Kind, TEXTVIEW("Queued socket request was removed before sending"));
	TArray<FSocketTextKey> Keys; Pending.GetKeys(Keys);
	const double Time = Now();
	for (const auto& Key : Keys)
	{
		const auto* Found = Pending.Find(Key); if (!Found) continue;
		const auto Row = *Found;
		if (Row->Control->bCancelRequested.load(std::memory_order_acquire) || (Row->Options.bOwned && !Row->Options.Owner.IsValid()))
			FinishRequest(Key, {}, RequestFailure(EVaRestErrorKind::Canceled, TEXTVIEW("Socket request canceled")));
		else if (Row->Failure.IsSet()) FinishRequest(Key, {}, Row->Failure.GetValue());
		else if (Row->Deadline > 0.0 && Time >= Row->Deadline) FinishRequest(Key, {}, RequestFailure(EVaRestErrorKind::Timeout, TEXTVIEW("Socket request reply deadline passed")));
	}
}
bool FSocketData::DeliverReply(const FVaRestSocketMessage& Incoming)
{
	if (Incoming.Kind != EVaRestSocketMessageKind::Reply && Incoming.Kind != EVaRestSocketMessageKind::ErrorReply) return false;
	if (!Incoming.bHasId) return true;
	const auto Id = Incoming.bNumericId ? FVaRestSocketId::Numeric(Incoming.NumericId) : FVaRestSocketId::String(Incoming.Id);
	const FSocketTextKey Key(Id.Key());
	const auto* Found = Pending.Find(Key);
	if (!Found || !SameText((*Found)->Channel, Incoming.Channel)) return true; // late or foreign-channel reply
	const auto Row = *Found;
	if (Row->Control->bCancelRequested.load(std::memory_order_acquire))
		FinishRequest(Key, {}, RequestFailure(EVaRestErrorKind::Canceled, TEXTVIEW("Socket request canceled")));
	else if (Row->bSent)
	{
		if (Incoming.Kind == EVaRestSocketMessageKind::ErrorReply)
		{
			auto Failure = Incoming.GetReplyError();
			if (Failure.Kind == EVaRestErrorKind::None) Failure = RequestFailure(EVaRestErrorKind::Server, TEXTVIEW("Socket peer returned an error reply"));
			FinishRequest(Key, {}, MoveTemp(Failure));
		}
		else if (!Incoming.Data().IsValid()) FinishRequest(Key, {}, RequestFailure(EVaRestErrorKind::Decode, TEXTVIEW("Socket reply has no configured data value")));
		else FinishRequest(Key, Incoming.Data(), {}, &Incoming);
	}
	return true;
}
void FSocketData::SentRequest(FStringView Key)
{
	if (const auto* Found = Pending.Find(FSocketTextKey(Key))) { (*Found)->bSent = true; (*Found)->SentAttemptId = TraceAttemptId; }
}
void FSocketData::FailedRequest(FStringView Key, EVaRestErrorKind Kind, FStringView Reason)
{
	if (const auto* Found = Pending.Find(FSocketTextKey(Key)); Found && !(*Found)->Failure.IsSet()) (*Found)->Failure = RequestFailure(Kind, Reason);
}

TSharedPtr<FChannelData> FSocketData::FindChannel(FStringView Name, bool bCreate)
{
	const FString Resolved = ResolveChannel(Name);
	const FSocketTextKey Key(Resolved);
	if (const auto* Found = Channels.Find(Key)) return *Found;
	if (!bCreate || bDisposed || Channels.Num() >= Limits.MaxChannels) return {};
	auto Channel = MakeShared<FChannelData>(); Channel->Name = Resolved;
	Channels.Add(Key, Channel); PublishChannel(*Channel);
	return Channel;
}
void FSocketData::PublishChannel(const FChannelData& Channel)
{
	Outbox->PublishChannel(Channel.Name, Channel.Epoch, Channel.bUp, Channel.bReady);
}

bool FSocketData::ControlsChannels() const
{
	for (const auto& Stage : Stages) if (Stage->ControlsChannels()) return true;
	return false;
}
void FSocketData::UpChannel(FStringView Name, FVaRestSocketOpen Info, uint64 Holder)
{
	const auto Channel = FindChannel(Name);
	if (!Channel || bDisposed || Status.State != EVaRestSocketState::Connected) return;
	const bool bWasUp = Channel->bUp;
	Info.bReconnect = Info.bReconnect || Channel->bEverUp;
	Channel->Open = MoveTemp(Info); Channel->bUp = true; Channel->bEverUp = true;
	if (!Channel->bWaitingReady) { Channel->bWaitingReady = true; Channel->ReadySince = Now(); }
	Channel->bNeedsDisconnected = false;
	if (SameText(Channel->Name, CodecContext.DefaultChannel)) ReadyHolds.Remove(Holder);
	else Channel->ReadyHolds.Remove(Holder);
	PublishChannel(*Channel);
	const uint64 UpGeneration = Generation, UpEpoch = Channel->Epoch;
	if (!bWasUp)
	{
		FVaRestTraceSocketNote Note; Note.Kind = EVaRestTraceSocketNoteKind::Channel; Note.Channel = Channel->Name;
		Note.ChannelState = EVaRestTraceSocketChannelState::Up; Note.bRecovered = Channel->Open.bRecovered; Note.PublicId = Channel->Open.PublicId; TraceNote(MoveTemp(Note));
		if (bDisposed || Generation != UpGeneration || Channel->Epoch != UpEpoch) return;
		Notify(Channel->Connected, Channel->Open);
	}
	if (!bDisposed && Generation == UpGeneration && Channel->Epoch == UpEpoch) ReadyChannel(Channel);
}
void FSocketData::ReadyChannel(const TSharedPtr<FChannelData>& Channel)
{
	if (!Channel || bDisposed || !Channel->bUp || Channel->bReady || Status.State != EVaRestSocketState::Connected) return;
	if (SameText(Channel->Name, CodecContext.DefaultChannel)) { BecomeReady(); return; }
	if (!Channel->ReadyHolds.IsEmpty() || !Channel->bReadyRouteSeen) return;
	if (DispatchDepth)
	{
		FOperation ReadyOperation{EOperation::StageReady}; ReadyOperation.Generation = Generation; ReadyOperation.Channel = Channel->Name; Queue(MoveTemp(ReadyOperation)); return;
	}
	Flush();
	Channel->bReady = true; Channel->bWaitingReady = false; PublishChannel(*Channel);
	{
		TGuardValue<bool> ReadyLane(bInReadyHandler, true); TGuardValue<FString> Lane(ReadyLaneChannel, Channel->Name);
		const auto Snapshot = Channel->Ready;
		const uint64 ReadyGeneration = Generation, ReadyEpoch = Channel->Epoch;
		++DispatchDepth;
		for (const auto& Entry : Snapshot)
		{
			if (bDisposed || Generation != ReadyGeneration || Channel->Epoch != ReadyEpoch) break;
			if (Entry->IsAlive()) Entry->Function(Channel->Open);
		}
		--DispatchDepth;
	}
	Channel->BacklogFence = Outbox->GetLastSequence(); Channel->bReleaseInbound = true;
	Flush(); ReleaseHeldChannels();
}
void FSocketData::DropHeldChannel(FChannelData& Channel)
{
	for (const auto& Held : Channel.Held)
	{
		HeldChannelBytes -= Held.Delivery.Charge; --HeldChannelEvents;
		Held.Origin->ConsumedOnGT(Held.Delivery);
	}
	Channel.Held.Reset(); Channel.bReleaseInbound = false;
}
void FSocketData::MarkChannelsDown()
{
	for (auto& Pair : Channels)
	{
		auto& Channel = *Pair.Value;
		DropHeldChannel(Channel);
		if (Channel.bUp || Channel.bReady)
		{
			check(Channel.Epoch != MAX_uint64); ++Channel.Epoch; Channel.bNeedsDisconnected = true;
		}
		Channel.bUp = false; Channel.bReady = false; Channel.bHoldInbound = false; Channel.bWaitingReady = false; Channel.ReadyHolds.Reset();
		Channel.bReadyRouteSeen = Channel.ReadyRoute.IsEmpty(); PublishChannel(Channel);
	}
}
void FSocketData::NotifyChannelsDown(const FVaRestSocketClose& Close)
{
	TArray<TSharedPtr<FChannelData>> Snapshot; Channels.GenerateValueArray(Snapshot);
	for (const auto& Channel : Snapshot)
	{
		if (!Channel->bNeedsDisconnected) continue;
		Channel->bNeedsDisconnected = false;
		const uint64 Before = Generation;
		FVaRestTraceSocketNote Note; Note.Kind = EVaRestTraceSocketNoteKind::Channel; Note.Channel = Channel->Name;
		Note.ChannelState = EVaRestTraceSocketChannelState::Down; Note.Reason = FName(LexToString(Close.Failure)); Note.Message = Close.Reason;
		TraceNote(MoveTemp(Note));
		if (bDisposed || Generation != Before) return;
		Notify(Channel->Disconnected, Close);
	}
}
void FSocketData::FailChannelRequests(FStringView Name, EVaRestErrorKind Kind, bool bSentOnly, FStringView Reason)
{
	TArray<FSocketTextKey> Keys;
	for (const auto& Pair : Pending) if (SameText(Pair.Value->Channel, Name) && (!bSentOnly || Pair.Value->bSent)) Keys.Add(Pair.Key);
	const auto Failure = RequestFailure(Kind, Reason);
	for (const auto& Key : Keys) FinishRequest(Key, {}, Failure);
}
void FSocketData::DownChannel(FStringView Name, FVaRestSocketClose Close, EVaRestChannelEnd End)
{
	const auto Channel = FindChannel(Name); if (!Channel || bDisposed) return;
	DropHeldChannel(*Channel);
	Channel->ReadyHolds.Reset(); Channel->bReadyRouteSeen = Channel->ReadyRoute.IsEmpty(); Channel->bWaitingReady = false;
	TArray<FSocketTextKey> Keys;
	for (const auto& Pair : Pending) if (SameText(Pair.Value->Channel, Channel->Name) && (Pair.Value->bSent || End == EVaRestChannelEnd::Ended)) Keys.Add(Pair.Key);
	if (End == EVaRestChannelEnd::Ended) Outbox->ClearChannel(Channel->Name);
	for (const auto& Key : Keys)
	{
		if (const auto* Found = Pending.Find(Key))
			FinishRequest(Key, {}, RequestFailure((*Found)->bSent ? EVaRestErrorKind::Connection : EVaRestErrorKind::Canceled, TEXTVIEW("Socket channel disconnected before the reply")));
	}
	Close.bWillReconnect = End == EVaRestChannelEnd::WillRetry;
	Channel->bNeedsDisconnected = false;
	const uint64 BeforeTrace = Generation;
	FVaRestTraceSocketNote Note; Note.Kind = EVaRestTraceSocketNoteKind::Channel; Note.Channel = Channel->Name;
	Note.ChannelState = EVaRestTraceSocketChannelState::Down; Note.Reason = FName(LexToString(Close.Failure)); Note.Message = Close.Reason;
	TraceNote(MoveTemp(Note));
	if (!bDisposed && Generation == BeforeTrace) Notify(Channel->Disconnected, Close);
}
bool FSocketData::RouteDelivery(const FSocketDelivery& Delivery, const TSharedPtr<FSocketInbound, ESPMode::ThreadSafe>& Origin)
{
	const bool bAppHeartbeat = !ProvidesHeartbeat() && Heartbeat.Mode != EVaRestHeartbeatMode::None;
	if (bAppHeartbeat && Heartbeat.bAnyInboundIsAlive)
	{
		bAwaitingHeartbeat = false; bHeartbeatHitchProbeUsed = false; HeartbeatDeadline = Now() + Heartbeat.IntervalSeconds + Heartbeat.TimeoutSeconds;
	}
	const uint64 BeforeStage = Generation;
	if (StageInbound(Delivery.Message) || bDisposed || Generation != BeforeStage) return false;
	if (bAppHeartbeat && (Delivery.bHeartbeatPing || Delivery.bHeartbeatPong))
	{
		const bool bAlive = Heartbeat.Mode == EVaRestHeartbeatMode::ClientPing ? Delivery.bHeartbeatPong : Delivery.bHeartbeatPing;
		if (bAlive) { bAwaitingHeartbeat = false; bHeartbeatHitchProbeUsed = false; HeartbeatDeadline = Now() + Heartbeat.IntervalSeconds + Heartbeat.TimeoutSeconds; }
		if (Delivery.bHeartbeatPing && Heartbeat.Mode == EVaRestHeartbeatMode::ServerPing) SendHeartbeat(true);
		return false; // heartbeat traffic bypasses application and held-channel delivery
	}
	if (Delivery.Message.Kind == EVaRestSocketMessageKind::Control) return false;
	const auto Channel = FindChannel(Delivery.Message.Channel);
	if (Channel)
	{
		bool bMatchedReady = false;
		if (SameText(Channel->Name, CodecContext.DefaultChannel))
		{
			if (!ReadyRoute.IsEmpty() && SameText(Delivery.Message.Route, ReadyRoute)) { bReadyRouteSeen = true; bMatchedReady = true; }
		}
		else if (!Channel->ReadyRoute.IsEmpty() && SameText(Delivery.Message.Route, Channel->ReadyRoute)) { Channel->bReadyRouteSeen = true; bMatchedReady = true; }
		if (bMatchedReady)
		{
			FOperation ReadyOperation{EOperation::StageReady}; ReadyOperation.Generation = Generation; ReadyOperation.Channel = Channel->Name; Operations.Add(MoveTemp(ReadyOperation));
		}
		if (Channel->bHoldInbound)
		{
			if (HeldChannelEvents >= 4096 || Delivery.Charge > Limits.MaxPendingInboundBytes - HeldChannelBytes)
			{
				bool bHandled = false;
				EachStage([&](auto& Stage, auto& Context) { if (!bHandled) bHandled = Stage.OnInboundOverflow(Channel->Name, Context); });
				if (!bHandled) { FVaRestSocketStageContext Context(Self, Generation); Context.Close(1009, TEXT("channel inbound hold exceeded its limit"), EVaRestReconnect::Default, EVaRestErrorKind::Offline); }
				return false;
			}
			FHeldChannelDelivery Held; Held.Delivery = Delivery; Held.Delivery.TraceFrames.Reset(); Held.Origin = Origin; Held.ChannelEpoch = Channel->Epoch;
			// GTK has accepted W3 delivery into this independently bounded W4 queue. Release only its batch admission;
			// payload/event charges stay held until application delivery or discard, including across later ready batches.
			Held.Delivery.BatchId = 0;
			Channel->Held.Add(MoveTemp(Held)); HeldChannelBytes += Delivery.Charge; ++HeldChannelEvents;
			Origin->CompleteBatchDeliveryOnGT(Delivery);
			return true; // the outer dispatch must not release this transport charge yet
		}
	}
	DeliverMessage(Delivery); return false;
}
void FSocketData::ReleaseHeldChannels()
{
	if (bReleasingHeld || bDisposed) return;
	TGuardValue<bool> Guard(bReleasingHeld, true);
	TArray<TSharedPtr<FChannelData>> Snapshot; Channels.GenerateValueArray(Snapshot);
	for (const auto& Channel : Snapshot)
	{
		if (!Channel->bReleaseInbound || !Channel->bUp || !Channel->bReady || Outbox->CountChannel(Channel->Name, Channel->BacklogFence)) continue;
		Channel->bReleaseInbound = false; Channel->bHoldInbound = false;
		const int32 Count = Channel->Held.Num();
		for (int32 Index = 0; Index < Count && !Channel->Held.IsEmpty(); ++Index)
		{
			auto Held = MoveTemp(Channel->Held[0]); Channel->Held.RemoveAt(0);
			HeldChannelBytes -= Held.Delivery.Charge; --HeldChannelEvents;
			if (!bDisposed && Held.Delivery.Generation == Generation && Held.ChannelEpoch == Channel->Epoch && Channel->bUp && Channel->bReady) DeliverMessage(Held.Delivery);
			Held.Origin->ConsumedOnGT(Held.Delivery); // release BEFORE a lifecycle barrier can stop its pipeline
			DrainTickOperations();
		}
	}
}
void FSocketData::TickChannelDeadlines()
{
	if (bDisposed || Status.State != EVaRestSocketState::Connected) return;
	const double Deadline = Limits.ReadyTimeout > 0.0 ? Limits.ReadyTimeout : ConnectTimeout;
	const double Time = Now();
	TArray<TSharedPtr<FChannelData>> Snapshot; Channels.GenerateValueArray(Snapshot);
	for (const auto& Channel : Snapshot)
	{
		if (!Channel->bWaitingReady || Channel->bReady) continue;
		const double LastTick = Channel->LastReadyTick; Channel->LastReadyTick = Time;
		if (Time - Channel->ReadySince < Deadline) continue;
		if (!Channel->bReadyHitchProbeUsed && Time - LastTick > Deadline)
		{ Channel->bReadyHitchProbeUsed = true; Channel->ReadySince = Time; continue; }
		Channel->bWaitingReady = false;
		bool bHandled = false;
		EachStage([&](auto& Stage, auto& Context) { if (!bHandled) bHandled = Stage.OnChannelReadyTimeout(Channel->Name, Context); });
		if (!bHandled)
		{
			FVaRestSocketStageContext Context(Self, Generation); Context.Close(1001, TEXT("channel readiness deadline passed"), EVaRestReconnect::Default, EVaRestErrorKind::Timeout);
			break;
		}
	}
}

void FSocketData::DrainTickOperations()
{
	check(DispatchDepth == 0);
	TGuardValue<bool> Barrier(bOperating, false);
	DrainOperations();
}
bool FSocketData::ProvidesHeartbeat() const
{
	for (const auto& Stage : Stages) if (Stage->ProvidesHeartbeat()) return true;
	return false;
}
bool FSocketData::StageInbound(const FVaRestSocketMessage& Incoming)
{
	bool bConsumed = false;
	EachStage([&](auto& Stage, auto& Context)
	{
		if (!bConsumed) bConsumed = Stage.OnInbound(Incoming, Context) == EVaRestSocketStageResult::Consumed;
	});
	return bConsumed;
}
void FSocketData::TickStageTimers()
{
	if (bDisposed) return;
	TArray<uint64> Due;
	const double Time = Now();
	for (const auto& Timer : StageTimers) if (Timer.Due <= Time || Timer.Generation != Generation) Due.Add(Timer.Id);
	for (uint64 Id : Due)
	{
		const int32 Index = StageTimers.IndexOfByPredicate([Id](const auto& Timer) { return Timer.Id == Id; });
		if (Index == INDEX_NONE) continue;
		auto Timer = MoveTemp(StageTimers[Index]); StageTimers.RemoveAt(Index);
		if (!bDisposed && Timer.Generation == Generation && Timer.Callback)
		{
			++DispatchDepth; Timer.Callback(); --DispatchDepth;
		}
	}
}
void FSocketData::InvokeClosing(int32 Code, FStringView Reason, bool bReplacement)
{
	if (!Connection || bClosingHook) return;
	const auto OldConnection = Connection;
	FVaRestSocketCloseRequest Close; Close.Code = Code; Close.Reason = FString(Reason); Close.bSessionReplacement = bReplacement;
	ClosingFrames.Reset(); ClosingBytes = 0;
	{
		TGuardValue<bool> Guard(bClosingHook, true);
		EachStage([&Close](auto& Stage, auto& Context) { Stage.OnClosing(Context, Close); }, true);
	}
	if (!bDisposed)
		for (auto& Frame : ClosingFrames)
		{
			if (bDisposed) break;
			FSocketSendUnit Check; Check.Flags = EVaRestSend::Handshake; Check.Bytes = Frame.Length;
			Check.Channel = CodecContext.DefaultChannel;
			Check.Frames.Add(MoveTemp(Frame));
			if (Outbox->CheckDetached(Check, true, false, OldConnection->GetBufferedAmount()) == EVaRestSendResult::Sent)
			{
				Outbox->Stamp(Check); auto Capture = PrepareFrameTrace(Check, 0); auto LogCapture = PrepareFrameLog(Check, 0);
				const int64 AcceptedBytes = Check.Frames[0].Length;
				if (!OldConnection->Send(MoveTemp(Check.Frames[0]))) Outbox->FailedSend();
				else
				{
					++Counters.FramesOut; Counters.BytesOut += AcceptedBytes; Stats::Sent(AcceptedBytes);
					LogCapture.Publish();
					++DispatchDepth; Capture.Publish(); --DispatchDepth;
				}
			}
		}
	ClosingFrames.Reset(); ClosingBytes = 0;
}
EVaRestSendResult FSocketData::WriteUnit(FSocketSendUnit& Unit)
{
	if (bDisposed || !Connection || Status.State != EVaRestSocketState::Connected) return EVaRestSendResult::Rejected;
	const auto Current = Connection;
	const uint64 WritingGeneration = Generation;
	const bool bReadyAtAdmission = Outbox->IsChannelReady(Unit.Channel, Status.bReady);
	TGuardValue<bool> Operating(bOperating, true);
	if (!Unit.bSkipStages)
	{
		const auto Snapshot = Stages;
		for (auto& Frame : Unit.Frames)
		{
			++DispatchDepth;
			bool bConsumed = false;
			for (int32 Index = Snapshot.Num() - 1; Index >= 0; --Index)
			{
				if (bDisposed || Generation != WritingGeneration) break;
				const auto& Stage = Snapshot[Index];
				FVaRestSocketStageContext Context(Self, WritingGeneration, false, reinterpret_cast<UPTRINT>(&Stage.Get()));
				if (Stage->OnOutbound(Frame, Context) == EVaRestSocketStageResult::Consumed) { bConsumed = true; break; }
			}
			--DispatchDepth;
			if (bDisposed || Generation != WritingGeneration) return EVaRestSendResult::Dropped;
			if (bConsumed) { SentRequest(Unit.RequestKey); return EVaRestSendResult::Sent; }
		}
	}
	Unit.Bytes = 0; for (const auto& Frame : Unit.Frames) Unit.Bytes += FMath::Max(0, Frame.Length);
	const auto Result = Outbox->CheckDetached(Unit, true, bReadyAtAdmission, Current->GetBufferedAmount(), false);
	if (Result != EVaRestSendResult::Sent)
	{
		FailedRequest(Unit.RequestKey, EVaRestErrorKind::Offline, TEXTVIEW("Socket request was refused after outbound stages"));
		return Result;
	}
	for (auto& Frame : Unit.Frames)
	{
		if (bDisposed || Generation != WritingGeneration)
		{
			FailedRequest(Unit.RequestKey, EVaRestErrorKind::Canceled, TEXTVIEW("Socket closed while sending the request"));
			return EVaRestSendResult::Dropped;
		}
		const int32 FrameIndex = static_cast<int32>(&Frame - Unit.Frames.GetData());
		auto Capture = PrepareFrameTrace(Unit, FrameIndex);
		auto LogCapture = PrepareFrameLog(Unit, FrameIndex);
		const int64 AcceptedBytes = Frame.Length;
		const auto AcceptedMetadata = Frame.Metadata; const auto AcceptedKind = Frame.Kind;
		if (!Current->Send(MoveTemp(Frame)))
		{
			Outbox->FailedSend(); FailedRequest(Unit.RequestKey, EVaRestErrorKind::Connection, TEXTVIEW("WebSocket refused the request frame"));
			return EVaRestSendResult::Rejected;
		}
		++Counters.FramesOut; Counters.BytesOut += AcceptedBytes; Stats::Sent(AcceptedBytes);
		if (AcceptedMetadata && !bDisposed && Generation == WritingGeneration)
			EachStage([&](auto& Stage, auto& Context) { Stage.OnSent(*AcceptedMetadata, AcceptedKind, AcceptedBytes, Context); });
		LogCapture.Publish();
		SentRequest(Unit.RequestKey);
		++DispatchDepth; Capture.Publish(); --DispatchDepth;
	}
	return EVaRestSendResult::Sent;
}
}

bool FVaRestSocketRequestHandle::IsActive() const
{
	const auto State = Control.Pin();
	return State && State->bActive.load(std::memory_order_acquire) && !State->bCancelRequested.load(std::memory_order_acquire);
}
void FVaRestSocketRequestHandle::Cancel() const { if (auto State = Control.Pin()) State->bCancelRequested.store(true, std::memory_order_release); }
FVaRestSocketJsonRequest FVaRestSocket::Request(FStringView Route, FVaRestJson Payload) { return FVaRestSocketJsonRequest(*this, Route, MoveTemp(Payload)); }
FVaRestSocketJsonRequest FVaRestSocket::RequestEncoded(FStringView Route, FRequestFrameFactory Frames)
{
	check(IsInGameThread()); FVaRestSocketJsonRequest Out(*this, Route, {}); Out.Options->FrameFactory = MoveTemp(Frames); return Out;
}
FVaRestSocketJsonRequest::FVaRestSocketJsonRequest(FVaRestSocket InSocket, FStringView Route, FVaRestJson Payload)
	: Socket(MoveTemp(InSocket)), Options(MakeUnique<FSocketJsonRequestOptions>())
{
	check(IsInGameThread()); Options->Route = FString(Route); Options->Payload = MoveTemp(Payload);
}
FVaRestSocketJsonRequest::FVaRestSocketJsonRequest(FVaRestSocketJsonRequest&& Other)
	: Socket(MoveTemp(Other.Socket)), Options(MoveTemp(Other.Options)) { check(IsInGameThread()); }
FVaRestSocketJsonRequest& FVaRestSocketJsonRequest::operator=(FVaRestSocketJsonRequest&& Other)
{
	check(IsInGameThread()); if (this != &Other) { Socket = MoveTemp(Other.Socket); Options = MoveTemp(Other.Options); } return *this;
}
FVaRestSocketJsonRequest::~FVaRestSocketJsonRequest() { check(IsInGameThread() || !Options); }
void FVaRestSocketJsonRequest::SetChannelOnGT(FString Channel) { check(IsInGameThread()); if (Options) Options->Channel = MoveTemp(Channel); }
void FVaRestSocketJsonRequest::SetMessageSuccessOnGT(TFunction<void(const FVaRestSocketMessage&)> Handler)
{ check(IsInGameThread()); if (Options) Options->MessageSuccess = MoveTemp(Handler); }
FVaRestSocketJsonRequest& FVaRestSocketJsonRequest::Timeout(double Seconds) { check(IsInGameThread()); if (Options) Options->Timeout = Seconds; return *this; }
FVaRestSocketJsonRequest& FVaRestSocketJsonRequest::Owner(const UObject* Owner) { check(IsInGameThread()); if (Options) { Options->Owner = Owner; Options->bOwned = Owner != nullptr; } return *this; }
FVaRestSocketJsonRequest& FVaRestSocketJsonRequest::OnSuccess(TFunction<void(const FVaRestJson&)> Handler) { check(IsInGameThread()); if (Options) Options->Success = MoveTemp(Handler); return *this; }
FVaRestSocketJsonRequest& FVaRestSocketJsonRequest::OnError(TFunction<void(const FVaRestError&)> Handler) { check(IsInGameThread()); if (Options) Options->Error = MoveTemp(Handler); return *this; }
FVaRestSocketJsonRequest& FVaRestSocketJsonRequest::InChannel(FString Channel) { SetChannelOnGT(MoveTemp(Channel)); return *this; }
FVaRestSocketJsonRequest& FVaRestSocketJsonRequest::TransformReply(TUniqueFunction<FVaRestError(const FVaRestSocketMessage&, FVaRestJson&)>&& Transform)
{ check(IsInGameThread()); if (Options) Options->ReplyTransform = MoveTemp(Transform); return *this; }
FVaRestSocketJsonRequest& FVaRestSocketJsonRequest::OnReply(TFunction<void(const FVaRestSocketMessage&, const FVaRestJson&)> Handler)
{ check(IsInGameThread()); if (Options) Options->ProtocolSuccess = MoveTemp(Handler); return *this; }
FVaRestSocketJsonRequest& FVaRestSocketJsonRequest::OnRetired(TUniqueFunction<void()>&& Cleanup)
{ check(IsInGameThread()); if (Options) Options->Retired = MoveTemp(Cleanup); return *this; }
FVaRestSocketRequestHandle FVaRestSocketJsonRequest::Send()
{
	check(IsInGameThread());
	if (!Options) return {};
	auto Owned = MoveTemp(Options);
	if (auto* Object = FSocketAccess::StateOnGT(Socket))
	{
		const TStrongObjectPtr<UVaRestSocketState> Pin(Object);
		auto Result = Object->Data->StartRequest(MoveTemp(*Owned)); Object->Data->DrainOperations(); return Result;
	}
	if (Owned->Error && (!Owned->bOwned || Owned->Owner.IsValid()))
	{
		FVaRestError Failure; Failure.Kind = EVaRestErrorKind::Config; Failure.Message = TEXT("Socket request requires a live scoped socket"); Owned->Error(Failure);
	}
	return {};
}

FVaRestSocketStageContext::FVaRestSocketStageContext(UVaRestSocketState* InState, uint64 InGeneration, bool bInClosing, uint64 InHolder)
	: State(InState), Generation(InGeneration), bClosing(bInClosing), Holder(InHolder) {}
UVaRestSocketState* FVaRestSocketStageContext::StateOnGT() const
{
	check(IsInGameThread());
	auto* Object = State.Get();
	return Object && !Object->Data->bDisposed && Object->Data->Generation == Generation ? Object : nullptr;
}
double FVaRestSocketStageContext::Now() const { if (const auto* Object = StateOnGT()) return Object->Data->Now(); return 0.0; }
bool FVaRestSocketStageContext::IsCurrent() const { return StateOnGT() != nullptr; }
bool FVaRestSocketStageContext::IsDispatching() const { if (const auto* Object = StateOnGT()) return Object->Data->DispatchDepth != 0; return false; }
double FVaRestSocketStageContext::GetLatency() const { if (const auto* Object = StateOnGT()) return Object->Data->Latency; return 0.0; }
FVaRestSessionData FVaRestSocketStageContext::Session() const
{
	if (auto* Object = StateOnGT(); Object && Object->Data->bApplyClientAuth) { auto Client = Object->Data->Client.PinOnGT(); if (Client.IsValid()) return Client.GetSession(); }
	return {};
}
bool FVaRestSocketStageContext::NeedsAuthRefresh() const
{
	if (auto* Object = StateOnGT(); Object && Object->Data->bApplyClientAuth) { auto Client = Object->Data->Client.PinOnGT(); return Client.IsValid() && Client.NeedsAuthRefresh(); }
	return false;
}
void FVaRestSocketStageContext::NotifySessionExpired() const
{
	if (auto* Object = StateOnGT(); Object && !bClosing && Object->Data->bApplyClientAuth)
	{
		const TStrongObjectPtr<UVaRestSocketState> Pin(Object);
		auto Client = Object->Data->Client.PinOnGT(); if (Client.IsValid()) Client.NotifySessionExpired();
	}
}
bool FVaRestSocketStageContext::SendBypass(FVaRestSocketFrame Frame) const
{
	auto* Object = StateOnGT(); if (!Object || !Frame.IsValid()) return false;
	const TStrongObjectPtr<UVaRestSocketState> Pin(Object);
	auto& Data = *Object->Data;
	if (bClosing)
	{
		if (!Data.bClosingHook || Data.ClosingFrames.Num() >= 256 || Frame.Length > 1024 * 1024 - Data.ClosingBytes) return false;
		Data.ClosingBytes += Frame.Length; Data.ClosingFrames.Add(MoveTemp(Frame)); return true;
	}
	FSocketSendUnit Unit; Unit.Flags = EVaRestSend::Handshake; Unit.bSkipStages = true;
	Unit.Bytes = Frame.Length; Unit.Frames.Add(MoveTemp(Frame));
	FVaRestSocketBatchPin Pinned; Pinned.ConnectionGeneration = Generation; Unit.Pin = MoveTemp(Pinned);
	const auto Result = Data.SendUnit(MoveTemp(Unit), Data.Outbox->GetEpoch());
	return Result == EVaRestSendResult::Queued || Result == EVaRestSendResult::Sent;
}
EVaRestLogLevel FVaRestSocketStageContext::GetLogLevel() const
{
	if (auto* Object = StateOnGT()) return SocketLogLevel(Object->Data->LogLevel); return EVaRestLogLevel::None;
}
FString FVaRestSocketStageContext::ScrubText(FStringView Text) const
{
	if (auto* Object = StateOnGT()) return SocketLogText(*Object->Data->TraceRedactor(), Text); return {};
}
FString FVaRestSocketStageContext::RedactUrl(FStringView Url) const
{
	if (auto* Object = StateOnGT()) return Object->Data->TraceRedactor()->RedactUrl(Url); return {};
}
void FVaRestSocketStageContext::ReportError(FStringView Name, FVaRestError Failure) const
{
	if (auto* Object = StateOnGT(); Object && !bClosing)
	{
		const TStrongObjectPtr<UVaRestSocketState> Pin(Object); auto& Data = *Object->Data;
		Failure.Message = Data.TraceRedactor()->ScrubText(Failure.Message);
		if (auto Channel = Data.FindChannel(Name)) Data.Notify(Channel->Error, Failure);
		if (StateOnGT() == Object) Data.Notify(Data.Error, Failure);
	}
}
void FVaRestSocketStageContext::EmitNote(FVaRestTraceSocketNote Note) const
{
	if (auto* Object = StateOnGT()) { const TStrongObjectPtr<UVaRestSocketState> Pin(Object); Object->Data->TraceNote(MoveTemp(Note)); }
}
bool FVaRestSocketStageContext::RefreshAuth(TUniqueFunction<void(EVaRestAuthRefreshOutcome, EVaRestErrorKind)>&& Callback) const
{
	auto* Object = StateOnGT(); if (!Object || bClosing || !Callback) return false;
	const TStrongObjectPtr<UVaRestSocketState> Pin(Object);
	auto Client = Object->Data->Client.PinOnGT();
	if (!Object->Data->bApplyClientAuth || !Client.IsValid())
	{
		return Timer(0.0, [Context = *this, Done = MoveTemp(Callback)]() mutable
		{ if (Context.IsCurrent()) Done(EVaRestAuthRefreshOutcome::Unavailable, EVaRestErrorKind::None); }).Id != 0;
	}
	struct FContinuation
	{
		TUniqueFunction<void(EVaRestAuthRefreshOutcome, EVaRestErrorKind)> Done;
		EVaRestAuthRefreshOutcome Outcome{};
		EVaRestErrorKind Kind = EVaRestErrorKind::None;
		bool bReady = false;
	};
	// Core keeps external captures in its GT-owned delivery slots. Neither reference crosses to a worker.
	const auto Continuation = MakeShared<FContinuation>(); Continuation->Done = MoveTemp(Callback);
	const auto Reserved = Timer(1e9, [Context = *this, Continuation]() mutable
	{
		if (Context.StateOnGT() && Continuation->bReady && Continuation->Done)
			Continuation->Done(Continuation->Outcome, Continuation->Kind);
	});
	if (!Reserved.Id) return false;
	Client.RefreshAuth([Context = *this, Reserved, Continuation](EVaRestAuthRefreshOutcome Outcome, EVaRestErrorKind Kind)
	{
		check(IsInGameThread());
		if (auto* Current = Context.StateOnGT())
		{
			auto* Slot = Current->Data->StageTimers.FindByPredicate([&](const auto& Row) { return Row.Id == Reserved.Id; });
			if (!Slot) return;
			Continuation->Outcome = Outcome; Continuation->Kind = Kind; Continuation->bReady = true;
			Slot->Due = Current->Data->Now();
		}
	});
	return true;
}
bool FVaRestSocketStageContext::SetIdleDeadline(double Seconds) const
{
	if (!FMath::IsFinite(Seconds) || Seconds < 0.0 || bClosing) return false;
	if (auto* Object = StateOnGT(); Object && Object->Data->Connection) return Object->Data->Connection->SetIdleDeadline(Seconds);
	return false;
}
bool FVaRestSocketStageContext::AddRedactedKey(FString Key) const
{
	if (!FVaRestRedactRules::IsAcceptableKey(Key) || bClosing) return false;
	if (auto* Object = StateOnGT())
	{
		auto& Data = *Object->Data;
		if (Data.ProtocolRedactedKeys.ContainsByPredicate([&](const auto& Existing) { return Existing.Equals(Key, ESearchCase::IgnoreCase); })) return true;
		if (Data.ProtocolRedactedKeys.Num() >= 64) return false;
		Data.ProtocolRedactedKeys.Add(MoveTemp(Key));
		if (Data.Inbound) Data.Inbound->SetTraceOnGT(Data.SocketId, Data.TraceAttemptId, Data.TraceRedactor(), Data.TraceClock);
		return true;
	}
	return false;
}
bool FVaRestSocketStageContext::RememberSecret(FString Value) const
{
	if (bClosing) return false;
	if (auto* Object = StateOnGT())
	{
		auto& Data = *Object->Data;
		if (Value.IsEmpty() || Data.ProtocolSecrets.ContainsByPredicate([&](const auto& Existing) { return SameText(Existing, Value); })) return true;
		const int64 Bytes = VaRest::Json::Utf8Length(Value);
		if (Data.ProtocolSecrets.Num() >= 1024 || Bytes > 256 * 1024 - Data.ProtocolSecretBytes) return false;
		Data.ProtocolSecretBytes += Bytes; Data.ProtocolSecrets.Add(MoveTemp(Value));
		if (Data.Inbound) Data.Inbound->SetTraceOnGT(Data.SocketId, Data.TraceAttemptId, Data.TraceRedactor(), Data.TraceClock);
		return true;
	}
	return false;
}
void FVaRestSocketStageContext::HoldReady(FStringView Name) const
{
	if (auto* Object = StateOnGT(); Object && !bClosing)
	{
		auto& Data = *Object->Data;
		if (auto Channel = Data.FindChannel(Name))
		{
			if (SameText(Channel->Name, Data.CodecContext.DefaultChannel)) { Data.ReadyHolds.Add(Holder); Data.Status.bReady = false; }
			else Channel->ReadyHolds.Add(Holder);
			if (!Channel->bWaitingReady) { Channel->bWaitingReady = true; Channel->ReadySince = Data.Now(); Channel->LastReadyTick = Channel->ReadySince; Channel->bReadyHitchProbeUsed = false; }
			Channel->bReady = false; Channel->bReleaseInbound = false; Data.PublishChannel(*Channel);
		}
	}
}
void FVaRestSocketStageContext::ResetReadyTimeout(FStringView Name) const
{
	if (auto* Object = StateOnGT(); Object && !bClosing)
		if (auto Channel = Object->Data->FindChannel(Name))
		{ Channel->bWaitingReady = true; Channel->ReadySince = Object->Data->Now(); Channel->LastReadyTick = Channel->ReadySince; Channel->bReadyHitchProbeUsed = false; }
}
void FVaRestSocketStageContext::SetReady(FStringView Name) const
{
	if (auto* Object = StateOnGT(); Object && !bClosing)
	{
		const TStrongObjectPtr<UVaRestSocketState> Pin(Object);
		FOperation ReadyOperation{EOperation::StageReady}; ReadyOperation.Generation = Generation; ReadyOperation.Holder = Holder;
		ReadyOperation.Channel = Object->Data->ResolveChannel(Name); Object->Data->Queue(MoveTemp(ReadyOperation));
	}
}
void FVaRestSocketStageContext::HoldInbound(FStringView Name) const
{
	if (auto* Object = StateOnGT(); Object && !bClosing)
		if (auto Channel = Object->Data->FindChannel(Name)) { Channel->bHoldInbound = true; Channel->bReleaseInbound = false; HoldReady(Name); }
}
void FVaRestSocketStageContext::ChannelUp(FString Name, FVaRestSocketOpen Open) const
{
	if (auto* Object = StateOnGT(); Object && !bClosing)
	{
		const TStrongObjectPtr<UVaRestSocketState> Pin(Object);
		if (!Object->Data->FindChannel(Name)) return;
		FOperation Up{EOperation::ChannelUp}; Up.Generation = Generation; Up.Holder = Holder;
		Up.Channel = Object->Data->ResolveChannel(Name); Up.ChannelOpen = MoveTemp(Open); Object->Data->Queue(MoveTemp(Up));
	}
}
void FVaRestSocketStageContext::ChannelDown(FString Name, FVaRestSocketClose Close, EVaRestChannelEnd End) const
{
	if (auto* Object = StateOnGT(); Object && !bClosing)
	{
		const TStrongObjectPtr<UVaRestSocketState> Pin(Object);
		auto& Data = *Object->Data;
		const auto Channel = Data.FindChannel(Name); if (!Channel) return;
		check(Channel->Epoch != MAX_uint64); ++Channel->Epoch;
		Channel->bUp = false; Channel->bReady = false; Channel->bNeedsDisconnected = true;
		Channel->bWaitingReady = false;
		if (SameText(Channel->Name, Data.CodecContext.DefaultChannel)) { Data.Status.bReady = false; Data.ReadyHolds.Reset(); Data.bReadyRouteSeen = Data.ReadyRoute.IsEmpty(); }
		Data.PublishChannel(*Channel);
		FOperation Down{EOperation::ChannelDown}; Down.Generation = Generation; Down.Channel = Channel->Name;
		Down.ChannelClose = MoveTemp(Close); Down.ChannelEnd = End; Data.Queue(MoveTemp(Down));
	}
}
TArray<FString> FVaRestSocketStageContext::GetChannels() const
{
	TArray<FString> Out;
	if (const auto* Object = StateOnGT()) for (const auto& Pair : Object->Data->Channels) Out.Add(Pair.Value->Name);
	return Out;
}
bool FVaRestSocketStageContext::SetDefaultChannel(FString Name) const
{
	auto* Object = StateOnGT(); if (!Object || bClosing) return false;
	auto& Data = *Object->Data;
	if (SameText(Data.CodecContext.DefaultChannel, Name)) return true;
	if (Data.Status.State != EVaRestSocketState::Disconnected || Data.bChannelIdentityFrozen || !Data.Pending.IsEmpty() || Data.Outbox->Count()) return false;
	if (Data.Channels.Contains(FSocketTextKey(Name))) return false;
	const auto Default = Data.FindChannel({}); if (!Default) return false;
	const FString OldName = Data.CodecContext.DefaultChannel;
	Data.Channels.Remove(FSocketTextKey(OldName)); Default->Name = Name; Data.Channels.Add(FSocketTextKey(Name), Default);
	Data.Outbox->Clear(); // invalidate encoders that captured the old default before this GTK publication
	Data.Outbox->ForgetChannel(OldName);
	Data.CodecContext.DefaultChannel = MoveTemp(Name); Data.Outbox->SetEncoder(Data.Codec.ToSharedRef(), Data.CodecContext);
	Data.PublishChannel(*Default);
	return true;
}
FVaRestSocketOpen FVaRestSocketStageContext::ConnectionOpen() const { if (const auto* Object = StateOnGT()) return Object->Data->LastOpen; return {}; }
FVaRestSocketBatchPin FVaRestSocketStageContext::BatchPin(FStringView Name) const
{
	FVaRestSocketBatchPin Out;
	if (const auto* Object = StateOnGT())
		if (const auto Channel = Object->Data->FindChannel(Name)) { Out.ConnectionGeneration = Generation; Out.Channel = Channel->Name; Out.ChannelEpoch = Channel->Epoch; }
	return Out;
}
void FVaRestSocketStageContext::SetSendLimit(int64 Bytes) const { if (auto* Object = StateOnGT()) Object->Data->Outbox->SetSendLimit(Bytes); }
void FVaRestSocketStageContext::Close(int32 Code, FString Reason, EVaRestReconnect Reconnect, EVaRestErrorKind Kind) const
{
	if (auto* Object = StateOnGT(); Object && !bClosing)
	{
		const TStrongObjectPtr<UVaRestSocketState> Pin(Object);
		bool bReplaced = false;
		FOperation Operation{EOperation::StageClose}; Operation.Code = SanitizeCloseCode(Code, bReplaced); Operation.Reason = WireReason(Reason); Operation.Kind = Kind;
		if (bReplaced) UE_LOG(LogVaRestSocket, Warning, TEXT("VaRest socket stage: invalid close code replaced with 1000"));
		if (Reconnect != EVaRestReconnect::Default) Operation.Reconnect = Reconnect == EVaRestReconnect::Always;
		Object->Data->BumpGeneration(); Operation.Generation = Object->Data->Generation; Object->Data->Queue(MoveTemp(Operation));
	}
}
void FVaRestSocketStageContext::MarkDead(FString Reason) const
{
	if (auto* Object = StateOnGT(); Object && !bClosing)
	{
		const TStrongObjectPtr<UVaRestSocketState> Pin(Object);
		FOperation Operation{EOperation::StageClose}; Operation.Code = 1006; Operation.Reason = MoveTemp(Reason); Operation.Kind = EVaRestErrorKind::Timeout; Operation.bFlag = true;
		Object->Data->BumpGeneration(); Operation.Generation = Object->Data->Generation; Object->Data->Queue(MoveTemp(Operation));
	}
}
void FVaRestSocketStageContext::RequestRefresh() const
{
	if (auto* Object = StateOnGT(); Object && !bClosing)
	{
		const TStrongObjectPtr<UVaRestSocketState> Pin(Object);
		FOperation Operation{EOperation::StageRefresh}; Operation.Reason = TEXT("protocol requested auth refresh");
		Object->Data->BumpGeneration(); Operation.Generation = Object->Data->Generation; Object->Data->Queue(MoveTemp(Operation));
	}
}
void FVaRestSocketStageContext::RestartAfterRefresh() const
{
	if (auto* Object = StateOnGT(); Object && !bClosing)
	{
		const TStrongObjectPtr<UVaRestSocketState> Pin(Object);
		FOperation Operation{EOperation::Restart}; Operation.Reason = TEXT("Socket.IO credentials refreshed"); Operation.Holder = 1;
		Object->Data->Queue(MoveTemp(Operation));
	}
}
FVaRestTimerHandle FVaRestSocketStageContext::Timer(double Delay, TUniqueFunction<void()>&& Callback) const
{
	auto* Object = StateOnGT();
	if (!Object || bClosing || !Callback || Object->Data->StageTimers.Num() >= 1024 || !FMath::IsFinite(Delay)) return {};
	FStageTimer Timer; Timer.Id = VaRest::Http::NewTimerId(); Timer.Generation = Generation;
	Timer.Due = Object->Data->Now() + FMath::Clamp(Delay, 0.0, 1e9); Timer.Callback = MoveTemp(Callback);
	const FVaRestTimerHandle Handle{Timer.Id}; Object->Data->StageTimers.Add(MoveTemp(Timer)); return Handle;
}
void FVaRestSocketStageContext::CancelTimer(FVaRestTimerHandle& Timer) const
{
	if (auto* Object = StateOnGT()) Object->Data->StageTimers.RemoveAll([Id = Timer.Id](const auto& Row) { return Row.Id == Id; });
	Timer.Invalidate();
}

FVaRestSocketChannel FVaRestSocket::Channel(FStringView Name) const
{
	check(IsInGameThread());
	auto* Object = FSocketAccess::StateOnGT(*this); if (!Object) return {};
	const TStrongObjectPtr<UVaRestSocketState> Pin(Object);
	auto& Data = *Object->Data;
	const bool bExisting = Data.FindChannel(Name).IsValid();
	const auto Channel = Data.FindChannel(Name, true); if (!Channel) return {};
	Data.bChannelIdentityFrozen = true;
	if (!bExisting)
	{
		Data.EachStage([&](auto& Stage, auto& Context) { Stage.OnChannelAdded(Channel->Name, Context); });
		if (Data.Status.State == EVaRestSocketState::Connected && !Data.ControlsChannels())
		{
			FOperation Up{EOperation::ChannelUp}; Up.Generation = Data.Generation; Up.Channel = Channel->Name; Up.ChannelOpen = Data.LastOpen; Data.Queue(MoveTemp(Up));
		}
		Data.DrainOperations();
	}
	return IsValid() ? FVaRestSocketChannel(*this, FString(Name)) : FVaRestSocketChannel();
}

#define VAREST_CHANNEL_EDIT(Body) \
	check(IsInGameThread()); \
	if (auto* Object = FSocketAccess::StateOnGT(Socket)) \
	{ \
		const TStrongObjectPtr<UVaRestSocketState> Pin(Object); \
		auto& Data = *Object->Data; \
		if (auto Channel = Data.FindChannel(Name)) { Body; Data.DrainOperations(); } \
	} \
	return *this

FString FVaRestSocketChannel::GetName() const
{
	if (auto* Object = FSocketAccess::StateOnGT(Socket)) return Object->Data->ResolveChannel(Name);
	return Name;
}
bool FVaRestSocketChannel::IsConnected() const
{
	if (auto* Object = FSocketAccess::StateOnGT(Socket))
	{
		const auto Channel = Object->Data->FindChannel(Name);
		return Channel && Channel->bUp && Object->Data->Status.State == EVaRestSocketState::Connected;
	}
	return false;
}
bool FVaRestSocketChannel::IsReady() const
{
	if (auto* Object = FSocketAccess::StateOnGT(Socket))
	{
		const auto Channel = Object->Data->FindChannel(Name);
		return Channel && Channel->bReady && Object->Data->Status.State == EVaRestSocketState::Connected;
	}
	return false;
}
FVaRestSocketChannel FVaRestSocketChannel::ReadyWhen(FString Route)
{
	VAREST_CHANNEL_EDIT(
		if (SameText(Channel->Name, Data.CodecContext.DefaultChannel)) Socket.ReadyWhen(MoveTemp(Route));
		else
		{
			Channel->ReadyRoute = MoveTemp(Route); Channel->bReadyRouteSeen = Channel->ReadyRoute.IsEmpty();
			if (!Channel->bReadyRouteSeen) Channel->bReady = false;
			if (!Channel->bReady && Channel->bUp && !Channel->bWaitingReady) { Channel->bWaitingReady = true; Channel->ReadySince = Data.Now(); }
			Data.PublishChannel(*Channel); Data.ReadyChannel(Channel);
		}
	);
}
#define VAREST_CHANNEL_HANDLER(Method, List, Signature) \
	FVaRestSocketChannel FVaRestSocketChannel::Method(const UObject* Owner, TFunction<Signature> Handler) \
	{ VAREST_CHANNEL_EDIT(AddCallback(Channel->List, Owner, MoveTemp(Handler), false, Data.NextRouteOrder++)); }
VAREST_CHANNEL_HANDLER(OnConnected, Connected, void(const FVaRestSocketOpen&))
VAREST_CHANNEL_HANDLER(OnReady, Ready, void(const FVaRestSocketOpen&))
VAREST_CHANNEL_HANDLER(OnDisconnected, Disconnected, void(const FVaRestSocketClose&))
VAREST_CHANNEL_HANDLER(OnMessage, Message, void(const FVaRestSocketMessage&))
VAREST_CHANNEL_HANDLER(OnUnrouted, Unrouted, void(const FVaRestSocketMessage&))
#undef VAREST_CHANNEL_HANDLER
FVaRestSocketChannel FVaRestSocketChannel::On(FStringView Route, const UObject* Owner, TFunction<void(const FVaRestSocketMessage&)> Handler)
{
	VAREST_CHANNEL_EDIT(AddCallback(Channel->Routes.FindOrAdd(FSocketTextKey(Route)), Owner, MoveTemp(Handler), false, Data.NextRouteOrder++));
}
FVaRestSocketChannel FVaRestSocketChannel::On(FStringView Route, const UObject* Owner, TFunction<void(const FVaRestJson&)> Handler)
{
	check(IsInGameThread()); if (!Handler) return *this;
	TFunction<void(const FVaRestSocketMessage&)> Adapted = [Handler = MoveTemp(Handler)](const auto& Message) { Handler(Message.Data()); };
	return On(Route, Owner, MoveTemp(Adapted));
}
FVaRestSocketChannel FVaRestSocketChannel::OnRequest(FStringView Route, const UObject* Owner, TFunction<void(const FVaRestSocketMessage&)> Handler)
{
	VAREST_CHANNEL_EDIT(AddCallback(Channel->Routes.FindOrAdd(FSocketTextKey(Route)), Owner, MoveTemp(Handler), true, Data.NextRouteOrder++));
}
#undef VAREST_CHANNEL_EDIT

void FVaRestSocketChannel::Off(FStringView Route, const UObject* Owner)
{
	check(IsInGameThread());
	if (auto* Object = FSocketAccess::StateOnGT(Socket))
	{
		const TStrongObjectPtr<UVaRestSocketState> Pin(Object);
		if (auto Channel = Object->Data->FindChannel(Name))
		{
			const FSocketTextKey Key(Route);
			if (auto* List = Channel->Routes.Find(Key)) { RemoveCallbacks(*List, Owner); if (List->IsEmpty()) Channel->Routes.Remove(Key); }
			if (SameText(Channel->Name, Object->Data->CodecContext.DefaultChannel)) Socket.Off(Route, Owner);
		}
	}
}
void FVaRestSocketChannel::OffAll(const UObject* Owner)
{
	check(IsInGameThread());
	if (auto* Object = FSocketAccess::StateOnGT(Socket))
	{
		const TStrongObjectPtr<UVaRestSocketState> Pin(Object);
		if (auto Channel = Object->Data->FindChannel(Name))
		{
			RemoveCallbacks(Channel->Connected, Owner); RemoveCallbacks(Channel->Ready, Owner); RemoveCallbacks(Channel->Disconnected, Owner);
			RemoveCallbacks(Channel->Message, Owner); RemoveCallbacks(Channel->Unrouted, Owner); RemoveCallbacks(Channel->Error, Owner);
			for (auto It = Channel->Routes.CreateIterator(); It; ++It) { RemoveCallbacks(It.Value(), Owner); if (It.Value().IsEmpty()) It.RemoveCurrent(); }
			if (SameText(Channel->Name, Object->Data->CodecContext.DefaultChannel))
			{
				RemoveCallbacks(Object->Data->Ready, Owner); RemoveCallbacks(Object->Data->Unrouted, Owner);
				for (auto It = Object->Data->Routes.CreateIterator(); It; ++It) { RemoveCallbacks(It.Value(), Owner); if (It.Value().IsEmpty()) It.RemoveCurrent(); }
			}
		}
	}
}
EVaRestSendResult FVaRestSocketChannel::Emit(FStringView Route, const FVaRestJson& Payload, EVaRestSend Flags)
{
	if (!Socket.IsValid()) return EVaRestSendResult::Rejected;
	const auto Snapshot = Socket.Handle->Outbox->GetEncoder(Name); if (!Snapshot.Codec) return EVaRestSendResult::Rejected;
	auto Write = [&Payload](VaRest::Json::FCondensedWriter& Writer, bool bMembers, TConstArrayView<FString> Skip, FVaRestJsonError& Error) { return WriteJsonPayload(Writer, Payload, bMembers, Skip, Error); };
	FVaRestSocketPayloadWriter View(Write, Payload.IsObject()); View.Json = &Payload;
	auto Encoded = Snapshot.Codec->Encode(Snapshot.Channel, Route, View, {}, Snapshot.Context);
	if (!Encoded.HasValue()) { Socket.Handle->Outbox->EncodingFailed(); return EVaRestSendResult::Rejected; }
	return Socket.SendEncoded(Encoded.StealValue(), Flags, Snapshot.Epoch, {}, Snapshot.Channel, Snapshot.QueueEpoch);
}
FVaRestSocketJsonRequest FVaRestSocketChannel::Request(FStringView Route, FVaRestJson Payload)
{
	check(IsInGameThread()); FVaRestSocketJsonRequest Out(Socket, Route, MoveTemp(Payload)); Out.Options->Channel = Name; return Out;
}
int32 FVaRestSocketChannel::GetQueuedCount() const { check(IsInGameThread()); return Socket.IsValid() ? Socket.Handle->Outbox->CountChannel(Socket.Handle->Outbox->GetEncoder(Name).Channel) : 0; }
void FVaRestSocketChannel::ClearQueue() { check(IsInGameThread()); if (Socket.IsValid()) Socket.Handle->Outbox->ClearChannel(Socket.Handle->Outbox->GetEncoder(Name).Channel); }
