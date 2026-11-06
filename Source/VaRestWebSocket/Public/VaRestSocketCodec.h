// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestEnvelope.h"
#include "VaRestJsonWriterInternal.h"
#include "VaRestJsonRedact.h"
#include "VaRestSocketMessage.h"
#include "VaRestSocketCodec.generated.h"

UENUM(BlueprintType)
enum class EVaRestSocketIdFormat : uint8 { String, Number };

USTRUCT(BlueprintType)
struct VARESTWEBSOCKET_API FVaRestSocketRouting
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket") FString RouteField = TEXT("type");
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket") FVaRestEnvelope Envelope;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket") FString IdField;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket") EVaRestSocketIdFormat IdFormat = EVaRestSocketIdFormat::String;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket") FString ReplyRoute;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket") bool bDecodeBinaryAsJson = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket") bool bJsonRpc = false;
	static FVaRestSocketRouting ByField(FString RouteField);
	static FVaRestSocketRouting JsonRpc();
	FVaRestSocketRouting DataAt(FString Path) const;
};

/** Exact correlation identity. String "1" and int64 1 are different keys. Unset is distinct from string "". */
struct VARESTWEBSOCKET_API FVaRestSocketId
{
	bool bSet = false, bNumber = false;
	FString Text;
	int64 Number = 0;
	static FVaRestSocketId String(FString Value);
	static FVaRestSocketId Numeric(int64 Value);
	static FVaRestSocketId FromJson(const FVaRestJson& Json);
	FString Key() const;
};

/** Optional encoder annotations; native strings and sizes only, including on Socket.IO attachment packets. */
struct FVaRestSocketFrameMetadata
{
	FString Channel, Route, Id;
	EVaRestSocketMessageKind Kind = EVaRestSocketMessageKind::Message;
	FName ProtocolTag;
	TArray<int64> AttachmentSizes;
	bool bAutoReplied = false;
	/** Nonnegative for framed JSON protocols; excerpts redact this suffix as JSON, without copying the wire header. */
	int32 ExcerptJsonOffset = -1;
	bool bRedactAllExcerptValues = false;
};

/** Synchronous encoder view; codecs must not retain the view, writer, or referenced payload. */
struct FVaRestSocketPayloadWriter
{
	using FWrite = TFunctionRef<bool(VaRest::Json::FCondensedWriter&, bool bMembers, TConstArrayView<FString> SkipKeys, FVaRestJsonError&)>;
	FWrite Write;
	bool bCanWriteMembers = false;
	/** Optional synchronous views for protocol codecs (positional fields and binary attachment hooks).
	 * These pointers share the writer's call lifetime. A codec must never retain them. */
	const UScriptStruct* NativeStruct = nullptr;
	const void* NativeValue = nullptr;
	const FVaRestJsonConvertOptions* ConvertOptions = nullptr;
	const FVaRestJson* Json = nullptr;
	/** Protocol argument list, written inside the codec's array using its negotiated byte/attachment options. */
	using FWriteArguments = FVaRestSocketArgumentsWriter;
	TOptional<FWriteArguments> Arguments;
	bool bPositionalArguments = false;
	FVaRestSocketPayloadWriter(FWrite InWrite, bool bInCanWriteMembers) : Write(InWrite), bCanWriteMembers(bInCanWriteMembers) {}
};

struct FVaRestSocketCodecContext
{
	FVaRestSocketRouting Routing;
	/** Immutable encoder snapshot; body logging needs native route annotations even with trace capture off. */
	EVaRestLogLevel LogLevel = EVaRestLogLevel::Basic;
	FString ReplyIdPrefix;
	FString DefaultChannel;
};

struct FVaRestSocketInboundFrame
{
	EVaRestSocketFrameKind Kind = EVaRestSocketFrameKind::Text;
	TArray<uint8> Bytes;
	TArray<TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>> Attachments;
	bool bAutoReplied = false;
};

enum class EVaRestSocketDecode : uint8 { Message, NeedMore, ProtocolError };
struct FVaRestSocketDecodeResult
{
	EVaRestSocketDecode Status = EVaRestSocketDecode::NeedMore;
	TOptional<FVaRestSocketMessage> Message;
	FString Reason;
	bool bJsonError = false;
	/** A control message delivered between a header and its attachments. Earlier packet charges stay held. */
	bool bInterleavedControl = false;
	/** Protocol refusal is 1002; a bounded packet/frame overflow is 1009. */
	int32 CloseCode = 1002;
};

/** Pure native per-connection owner. Creation, Decode and destruction all occur on the same socket pipe.
 * NeedMore retains packet fragments in this decoder; ProtocolError poisons it for the connection. */
class IVaRestSocketDecoder
{
public:
	virtual ~IVaRestSocketDecoder() = default;
	virtual FVaRestSocketDecodeResult Decode(FVaRestSocketInboundFrame Frame, const FVaRestSocketCodecContext& Context) = 0;
};

/** Thread-safe native codec; encode runs synchronously on its caller, decode belongs to MakeDecoder's owner pipe.
 * Do not retain UObjects, strong client shells, or affine user callbacks in a codec/decoder. */
class IVaRestSocketCodec
{
public:
	virtual ~IVaRestSocketCodec() = default;
	virtual FName GetProtocolTag() const = 0;
	/** Pure native header scan only. -1 keeps ordinary text redaction; an empty suffix can hide malformed framing. */
	virtual int32 GetExcerptJsonOffset(TConstArrayView<uint8> Bytes) const { return -1; }
	virtual bool RedactAllOutgoingExcerptValues(TConstArrayView<uint8> Bytes) const { return false; }
	virtual void ExtendRedactionRules(FVaRestRedactRules& Rules) const {}
	virtual TUniquePtr<IVaRestSocketDecoder> MakeDecoder() const = 0;
	virtual TValueOrError<TArray<FVaRestSocketFrame>, FVaRestError> Encode(FStringView Channel, FStringView Route,
		const FVaRestSocketPayloadWriter& Payload, const FVaRestSocketId& Id, const FVaRestSocketCodecContext& Context) const = 0;
	virtual TValueOrError<TArray<FVaRestSocketFrame>, FVaRestError> EncodeReply(FStringView Channel, const FVaRestSocketId& Id,
		const FVaRestSocketPayloadWriter& Payload, bool bError, const FVaRestSocketCodecContext& Context) const = 0;
};

namespace VaRest::WebSocket
{
	/** Native Socket.IO-style {_placeholder:true,num:N} resolution; copied attachment references are thread-safe.
	 * The converter invokes it on its caller's thread and copies bytes only for a requested mutable byte field. */
	VARESTWEBSOCKET_API FVaRestJsonBytesResolver MakePlaceholderBytesReader(TConstArrayView<TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>> Attachments);
	VARESTWEBSOCKET_API TSharedRef<IVaRestSocketCodec, ESPMode::ThreadSafe> MakeJsonCodec();
	/** One-pass DOM payload adapter; SkipKeys excludes envelope identity fields when writing members. */
	VARESTWEBSOCKET_API bool WriteJsonPayload(VaRest::Json::FCondensedWriter& Writer, const FVaRestJson& Json,
		bool bMembers, TConstArrayView<FString> SkipKeys, FVaRestJsonError& Error);
	VARESTWEBSOCKET_API bool WriteStructPayload(VaRest::Json::FCondensedWriter& Writer, const UScriptStruct* Struct, const void* Value,
		const FVaRestJsonConvertOptions& Options, bool bMembers, TConstArrayView<FString> SkipKeys, FVaRestJsonError& Error);
}
