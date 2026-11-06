// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestAuthTypes.h"
#include "VaRestClock.h"
#include "VaRestSocketClientTypes.h"
#include "VaRestSocketMessage.h"

class UVaRestSocketState;
enum class EVaRestAuthRefreshOutcome : uint8;
struct FVaRestTraceSocketNote;
struct FVaRestTraceSnapshot;
class FVaRestRedactor;
namespace VaRest::WebSocket { struct FSocketData; }

enum class EVaRestSocketStageResult : uint8 { Continue, Consumed };
enum class EVaRestReconnect : uint8 { Default, Always, Never };

struct FVaRestSocketCloseRequest
{
	int32 Code = 1000;
	FString Reason;
	bool bSessionReplacement = false;
};

/** Optional protocol classification layered over the transport's close defaults. */
struct FVaRestSocketStageCloseDecision
{
	TOptional<bool> bRetry;
	TOptional<EVaRestErrorKind> Kind;
	TOptional<bool> bRefresh;
	FString Detail;
};

/** Borrowed GTK-only control. Retaining a copy does not keep the socket alive; old generations refuse work. */
class VARESTWEBSOCKET_API FVaRestSocketStageContext
{
public:
	bool IsCurrent() const;
	bool IsDispatching() const;
	double Now() const;
	double GetLatency() const;
	FVaRestSessionData Session() const;
	bool NeedsAuthRefresh() const;
	void NotifySessionExpired() const;
	EVaRestLogLevel GetLogLevel() const;
	FString ScrubText(FStringView Text) const;
	FString RedactUrl(FStringView Url) const;
	void ReportError(FStringView Channel, FVaRestError Error) const;
	void EmitNote(FVaRestTraceSocketNote Note) const;
	/** Callback is retained and destroyed on GT; delivery is deferred through the owned socket timer queue. */
	bool RefreshAuth(TUniqueFunction<void(EVaRestAuthRefreshOutcome, EVaRestErrorKind)>&& Callback) const;
	bool SendBypass(FVaRestSocketFrame Frame) const;
	void HoldReady(FStringView Channel = {}) const;
	void ResetReadyTimeout(FStringView Channel = {}) const;
	void SetReady(FStringView Channel = {}) const;
	void HoldInbound(FStringView Channel = {}) const;
	void ChannelUp(FString Channel, FVaRestSocketOpen Open) const;
	void ChannelDown(FString Channel, FVaRestSocketClose Close, EVaRestChannelEnd End) const;
	TArray<FString> GetChannels() const;
	bool SetDefaultChannel(FString Name) const;
	FVaRestSocketOpen ConnectionOpen() const;
	FVaRestSocketBatchPin BatchPin(FStringView Channel = {}) const;
	void SetSendLimit(int64 Bytes) const;
	/** Returns service-command admission; false requires a protocol-specific failure decision. */
	bool SetIdleDeadline(double Seconds) const;
	/** Persistent protocol key and current-connection value masks. False means invalid/bounded registration refusal. */
	bool AddRedactedKey(FString Key) const;
	bool RememberSecret(FString Value) const;
	void Close(int32 Code, FString Reason, EVaRestReconnect Reconnect = EVaRestReconnect::Default,
		EVaRestErrorKind Kind = EVaRestErrorKind::Server) const;
	void MarkDead(FString Reason) const;
	void RequestRefresh() const;
	/** New credentials are already installed; reconnect immediately without a second refresh/backoff step. */
	void RestartAfterRefresh() const;
	FVaRestTimerHandle Timer(double Delay, TUniqueFunction<void()>&& Callback) const;
	void CancelTimer(FVaRestTimerHandle& Timer) const;
private:
	friend struct VaRest::WebSocket::FSocketData;
	friend class FVaRestSocket;
	FVaRestSocketStageContext(UVaRestSocketState* State, uint64 Generation, bool bClosing = false, uint64 Holder = 0);
	UVaRestSocketState* StateOnGT() const;
	TWeakObjectPtr<UVaRestSocketState> State;
	uint64 Generation = 0;
	bool bClosing = false;
	uint64 Holder = 0;
};

/** Stateful GTK middleware. Own and destroy stages on GTK; only codecs/decoders belong to workers.
 * Inbound is already decoded. Outbound hooks run in reverse registration order before transport admission. */
class IVaRestSocketStage
{
public:
	virtual ~IVaRestSocketStage() = default;
	/** Called on GT before Hub HTTP teardown, even if an external GT stage reference remains alive. */
	virtual void OnRetire() {}
	/** GTK-only read: append native protocol metadata; borrowed arguments expire at return. */
	virtual void AppendTraceSnapshot(FVaRestTraceSnapshot& Snapshot, uint64 SocketId, uint64 AttemptId, const FVaRestRedactor& Redactor) const {}
	/** Session changes also reach a stage between transport attempts, when no closing hook can run. */
	virtual void OnSessionChanged(EVaRestSessionChange Change, FVaRestSocketStageContext& Context) {}
	virtual void OnAttach(FVaRestSocketStageContext& Context) {}
	virtual void OnHandshake(FVaRestSocketHandshake& Handshake, FVaRestSocketStageContext& Context) {}
	virtual void OnOpen(const FVaRestSocketOpen& Open, FVaRestSocketStageContext& Context) {}
	virtual EVaRestSocketStageResult OnInbound(const FVaRestSocketMessage& Message, FVaRestSocketStageContext& Context) { return EVaRestSocketStageResult::Continue; }
	virtual void OnDispatched(const FVaRestSocketMessage& Message, FVaRestSocketStageContext& Context) {}
	virtual EVaRestSocketStageResult OnOutbound(FVaRestSocketFrame& Frame, FVaRestSocketStageContext& Context) { return EVaRestSocketStageResult::Continue; }
	/** GTK notification after connection Send accepts a frame. Metadata is native and borrowed for this call only. */
	virtual void OnSent(const FVaRestSocketFrameMetadata& Metadata, EVaRestSocketFrameKind Kind, int64 Bytes, FVaRestSocketStageContext& Context) {}
	virtual TOptional<bool> ShouldReconnect(const FVaRestSocketClose& Close) { return {}; }
	virtual void ClassifyClose(const FVaRestSocketClose& Close, FVaRestSocketStageCloseDecision& Decision, FVaRestSocketStageContext& Context) {}
	virtual void OnConnectionClosed(const FVaRestSocketClose& Close, FVaRestSocketStageContext& Context) {}
	virtual void OnGiveUp(FVaRestSocketStageContext& Context) {}
	virtual void OnClosing(FVaRestSocketStageContext& Context, const FVaRestSocketCloseRequest& Close) {}
	virtual bool ProvidesHeartbeat() const { return false; }
	virtual bool ControlsChannels() const { return false; }
	virtual void OnChannelAdded(FStringView Channel, FVaRestSocketStageContext& Context) {}
	virtual bool OnInboundOverflow(FStringView Channel, FVaRestSocketStageContext& Context) { return false; }
	virtual bool OnChannelReadyTimeout(FStringView Channel, FVaRestSocketStageContext& Context) { return false; }
};
