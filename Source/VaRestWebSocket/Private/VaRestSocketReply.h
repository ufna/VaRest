// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestSocket.h"
#include <atomic>

namespace VaRest::WebSocket
{
	/** Native only. Shared by every copy of the inbound message; never retains its GTK state. */
	struct FSocketReplyToken
	{
		FVaRestSocketWeak Socket;
		TSharedPtr<IVaRestSocketCodec, ESPMode::ThreadSafe> Codec;
		FVaRestSocketCodecContext Context;
		FVaRestSocketId Id;
		FVaRestSocketBatchPin Pin;
		std::atomic<bool> bUsed{false};
		EVaRestSendResult Send(const FVaRestJson& Payload, bool bError);
		EVaRestSendResult SendWriter(const FVaRestSocketPayloadWriter& Payload, bool bError);
		EVaRestSendResult SendStruct(const UScriptStruct* Struct, const void* Value);
		EVaRestSendResult SendPropertyOnGT(const FProperty* Property, const void* Value);
		EVaRestSendResult SendArguments(FVaRestSocketArgumentsWriter Arguments);
		using FEncodeReply = TFunctionRef<TValueOrError<TArray<FVaRestSocketFrame>, FVaRestError>(const FVaRestJsonConvertOptions&)>;
		EVaRestSendResult SendWith(FEncodeReply Encode, bool bRejectDuplicate = false);
	};
}
