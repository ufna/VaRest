// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestSocketStage.h"
#include "VaRestSioCodec.h"
#include "VaRestSioTypes.h"
#include "VaRestSioStats.h"

struct FVaRestSioStageOptions
{
	FString DefaultNamespace = TEXT("/");
	FString Path = TEXT("/socket.io/");
	FVaRestSioAuthPlacement Auth;
	bool bRecovery = true;
	bool bOverrideFallbackPayloadLimit = false;
	int64 MaxOutboundFrameBytes = 16 * 1024 * 1024;
	int32 MaxNamespaces = 256;
	double ReadyTimeout = 20.0;
	double StableAfter = 5.0;
	FVaRestReconnectPolicy NamespaceRetry;
	FVaRestSioStageOptions()
	{ NamespaceRetry.BaseDelay = 1.0; NamespaceRetry.Multiplier = 2.0; NamespaceRetry.MaxDelay = 5.0; NamespaceRetry.Jitter = EVaRestSocketJitter::Ratio; NamespaceRetry.JitterRatio = 0.5; }
};

/** These callbacks and their captures live exclusively on GT, in the socket-owned stage. */
struct FVaRestSioNamespaceConfig
{
	FVaRestJson AuthPayload;
	bool bSendSession = true;
	bool bConnectOnStart = false;
	TFunction<void(FVaRestJson&, const FVaRestSioConnectContext&)> OnConnectAuth;
	TFunction<EVaRestSioRetry(const FVaRestSioConnectError&)> ConnectErrorIf;
	TFunction<void(const FVaRestError&, const FVaRestSioConnectError&)> OnConnectError;
	TFunction<void(const FVaRestSioDisconnect&)> OnDisconnect;
};

/** Engine.IO session and namespace controller. Create, use and destroy on GT. */
class VARESTSOCKETIO_API FVaRestSioStage final : public IVaRestSocketStage, public TSharedFromThis<FVaRestSioStage>
{
public:
	FVaRestSioStage(TSharedRef<FVaRestSioCodec, ESPMode::ThreadSafe> Codec, FVaRestSioStageOptions Options = {});
	virtual ~FVaRestSioStage() override;
	virtual void OnRetire() override;
	virtual void AppendTraceSnapshot(FVaRestTraceSnapshot&, uint64 SocketId, uint64 AttemptId, const FVaRestRedactor&) const override;
	virtual void OnSessionChanged(EVaRestSessionChange Change, FVaRestSocketStageContext& Context) override;
	bool ConfigureNamespace(FString Name, FVaRestSioNamespaceConfig Config = {});
	bool UpdateOptions(FVaRestSioStageOptions Options);
	void KeepOwnerStateOnGT(UObject* State, TUniqueFunction<void()>&& Retire = {});
	bool ConnectNamespace(FStringView Name);
	bool DisconnectNamespace(FStringView Name);
	void ConnectDefaults();
	void ResetRecovery();
	EVaRestSioNamespaceState GetNamespaceState(FStringView Name) const;
	FVaRestSioConnect GetNamespaceInfo(FStringView Name) const;
	FString RecoveryOffsetFor(const FVaRestSocketMessage& Message) const;
	TArray<FString> GetNamespaces() const;
	FVaRestSioStats GetStats(FStringView Namespace = {}) const;
	void RecordAckTimeout(FStringView Namespace);
	virtual void OnAttach(FVaRestSocketStageContext& Context) override;
	virtual void OnHandshake(FVaRestSocketHandshake& Handshake, FVaRestSocketStageContext& Context) override;
	virtual void OnOpen(const FVaRestSocketOpen& Open, FVaRestSocketStageContext& Context) override;
	virtual EVaRestSocketStageResult OnInbound(const FVaRestSocketMessage& Message, FVaRestSocketStageContext& Context) override;
	virtual void OnDispatched(const FVaRestSocketMessage& Message, FVaRestSocketStageContext& Context) override;
	virtual void OnSent(const FVaRestSocketFrameMetadata& Metadata, EVaRestSocketFrameKind Kind, int64 Bytes, FVaRestSocketStageContext& Context) override;
	virtual void ClassifyClose(const FVaRestSocketClose& Close, FVaRestSocketStageCloseDecision& Decision, FVaRestSocketStageContext& Context) override;
	virtual void OnConnectionClosed(const FVaRestSocketClose& Close, FVaRestSocketStageContext& Context) override;
	virtual void OnGiveUp(FVaRestSocketStageContext& Context) override;
	virtual void OnClosing(FVaRestSocketStageContext& Context, const FVaRestSocketCloseRequest& Close) override;
	virtual bool ProvidesHeartbeat() const override { return true; }
	virtual bool ControlsChannels() const override { return true; }
	virtual void OnChannelAdded(FStringView Channel, FVaRestSocketStageContext& Context) override;
	virtual bool OnInboundOverflow(FStringView Channel, FVaRestSocketStageContext& Context) override;
	virtual bool OnChannelReadyTimeout(FStringView Channel, FVaRestSocketStageContext& Context) override;
private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
