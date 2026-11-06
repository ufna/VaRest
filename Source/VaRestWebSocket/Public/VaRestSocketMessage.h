// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestJson.h"
#include "VaRestHttpTypes.h"
#include "VaRestSocketTypes.h"
#include "VaRestSocketClientTypes.h"
#include "VaRestJsonWriterInternal.h"
#include "VaRestSocketMessage.generated.h"

namespace VaRest::WebSocket { struct FSocketMessagePayload; struct FSocketReplyToken; struct FSocketData; class FSocketInbound; }

UENUM(BlueprintType)
enum class EVaRestSocketMessageKind : uint8 { Message, Request, Reply, ErrorReply, Control };

/** Borrowed synchronous protocol argument list, using the reply encoder's native conversion snapshot. */
using FVaRestSocketArgumentsWriter = TFunctionRef<bool(VaRest::Json::FCondensedWriter&, const FVaRestJsonConvertOptions&, FVaRestJsonError&, TFunctionRef<bool()>)>;

/** Cheap immutable payload view. JSON is prepared by the socket's native decode pipe, never by these accessors.
 * A copied Json handle aliases its document; clone it before editing a message retained by other consumers. */
USTRUCT(BlueprintType)
struct VARESTWEBSOCKET_API FVaRestSocketMessage
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") FString Channel;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") FString Route;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") FString Id;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") EVaRestSocketMessageKind Kind = EVaRestSocketMessageKind::Message;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") EVaRestSocketFrameKind FrameKind = EVaRestSocketFrameKind::Text;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") FName ProtocolTag;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") bool bAutoReplied = false;
	FVaRestJson Json() const;
	FVaRestJson Data() const;
	FVaRestError GetReplyError() const;
	static FVaRestSocketMessage FromDecoded(EVaRestSocketFrameKind FrameType, TArray<uint8> Raw, FVaRestJson Root, FVaRestJson Data, FVaRestError ReplyError = {});
	TArray<TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>> Attachments;
	/** True for an id actually present on the wire, including an empty string id. */
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") bool bHasId = false;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") bool bNumericId = false;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") int64 NumericId = 0;
	TConstArrayView<uint8> Bytes() const;
	FString Text() const;
	bool IsBinary() const { return FrameKind == EVaRestSocketFrameKind::Binary; }
	/** Any thread. Encodes once and queues a reply pinned to the original connection. A second call is dropped. */
	EVaRestSendResult Reply(const FVaRestJson& InPayload) const;
	EVaRestSendResult ReplyArguments(FVaRestSocketArgumentsWriter Arguments) const;
	/** GTK snapshot of the once token and original connection/namespace generation. */
	bool CanReplyOnGT() const;
	template <typename T> requires (VaRest::Json::bHasScriptStruct<T> && !std::is_same_v<T, FVaRestJson>)
	EVaRestSendResult Reply(const T& Value) const { return ReplyStruct(VaRest::Json::GetScriptStruct<T>(), &Value); }
	EVaRestSendResult ReplyError(FString Code, FString Message) const;
	EVaRestSendResult ReplyError(int64 Code, FString Message) const;
private:
	friend class UVaRestSocketLibrary;
	friend class VaRest::WebSocket::FSocketInbound;
	friend struct VaRest::WebSocket::FSocketData;
	TSharedPtr<const VaRest::WebSocket::FSocketMessagePayload, ESPMode::ThreadSafe> Payload;
	TSharedPtr<VaRest::WebSocket::FSocketReplyToken, ESPMode::ThreadSafe> ReplyToken;
	EVaRestSendResult ReplyStruct(const UScriptStruct* Struct, const void* Value) const;
	EVaRestSendResult ReplyPropertyOnGT(const FProperty* Property, const void* Value) const;
};
