// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestSocketCodec.h"
#include <atomic>
#include "VaRestSioCodec.generated.h"

UENUM(BlueprintType)
enum class EVaRestSioArgs : uint8 { First, Positional, Array };

/** Native codec limits, copied into every per-connection decoder. Session/heartbeat belong to the stage. */
struct FVaRestSioCodecOptions
{
	int32 MaxAttachments = 10;
	int64 MaxMessageBytes = 8 * 1024 * 1024;
	int64 MaxPacketBytes = 16 * 1024 * 1024;
	int64 MaxPendingInboundBytes = 32 * 1024 * 1024;
	int64 MaxOutboundFrameBytes = 1000000;
	EVaRestSioArgs Args = EVaRestSioArgs::First;
};

/** EIO v4/SIO v5. It retains no reflection, client shell, user handler or service-thread callback. */
class VARESTSOCKETIO_API FVaRestSioCodec final : public IVaRestSocketCodec
{
public:
	explicit FVaRestSioCodec(FVaRestSioCodecOptions Options = {});
	virtual FName GetProtocolTag() const override;
	virtual int32 GetExcerptJsonOffset(TConstArrayView<uint8> Bytes) const override;
	virtual bool RedactAllOutgoingExcerptValues(TConstArrayView<uint8> Bytes) const override;
	virtual void ExtendRedactionRules(FVaRestRedactRules& Rules) const override;
	virtual TUniquePtr<IVaRestSocketDecoder> MakeDecoder() const override;
	virtual TValueOrError<TArray<FVaRestSocketFrame>, FVaRestError> Encode(FStringView Channel, FStringView Route,
		const FVaRestSocketPayloadWriter& Payload, const FVaRestSocketId& Id, const FVaRestSocketCodecContext& Context) const override;
	virtual TValueOrError<TArray<FVaRestSocketFrame>, FVaRestError> EncodeReply(FStringView Channel, const FVaRestSocketId& Id,
		const FVaRestSocketPayloadWriter& Payload, bool bError, const FVaRestSocketCodecContext& Context) const override;
	/** Stage CONNECT/DISCONNECT packet; emits one EIO message, without a handler/payload writer. */
	TValueOrError<TArray<FVaRestSocketFrame>, FVaRestError> EncodeControl(uint8 Type, FStringView Namespace, const FVaRestJson& Payload = {}) const;
	static bool IsReservedEvent(FStringView Name);
	static bool IsValidEventName(FStringView Name);
	static bool IsValidNamespace(FStringView Name);
	bool ValidateOptions(FString& Error) const;
	static bool ValidateOptions(const FVaRestSioCodecOptions& Options, FString& Error);
	FVaRestSioCodecOptions GetOptionsSnapshot() const;
	bool UpdateOptions(FVaRestSioCodecOptions Options, FString& Error);
	/** Publishes only the outbound frame limit; active binary decoder state is never replaced by OPEN negotiation. */
	void SetOutboundFrameLimit(int64 Bytes);
	/** Session stages require OPEN before any other data; standalone packet decoding remains available. */
	void RequireEngineOpen() { bRequireEngineOpen.store(true, std::memory_order_release); }
	/** Add a fresh client ack id to a prepared EVENT/BINARY_EVENT using the encoder's reserved header gap. */
	TValueOrError<TArray<FVaRestSocketFrame>, FVaRestError> WithAckId(TArray<FVaRestSocketFrame> Frames, const FVaRestSocketId& Id) const;
private:
	TValueOrError<TArray<FVaRestSocketFrame>, FVaRestError> EncodePacket(FStringView Channel, FStringView Route,
		const FVaRestSocketPayloadWriter& Payload, const FVaRestSocketId& Id, const FVaRestSocketCodecContext& Context, bool bReply, bool bError) const;
	mutable FCriticalSection OptionsMutex;
	FVaRestSioCodecOptions StoredOptions;
	std::atomic<int64> OutboundFrameLimit;
	std::atomic<bool> bRequireEngineOpen{false};
};
