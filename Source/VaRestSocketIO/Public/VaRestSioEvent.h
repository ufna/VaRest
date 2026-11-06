// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestSioArguments.h"
#include "VaRestSocketMessage.h"
#include "VaRestSioEvent.generated.h"

/** Every BP/C++ copy shares the original native reply token, its once flag and connection/namespace pin. */
USTRUCT(BlueprintType)
struct VARESTSOCKETIO_API FVaRestSioAck
{
	GENERATED_BODY()
	bool IsValid() const { return Message.bHasId && Message.Kind == EVaRestSocketMessageKind::Request; }
	bool CanReplyOnGT() const { return IsValid() && Message.CanReplyOnGT(); }
	EVaRestSendResult SendArguments(FVaRestSocketArgumentsWriter Arguments) const { return Message.ReplyArguments(Arguments); }
	template <typename... A> EVaRestSendResult Send(A&&... Args) const
	{
		auto Write = [&](auto& Writer, const FVaRestJsonConvertOptions& Options, FVaRestJsonError& Error, TFunctionRef<bool()> WithinBudget)
		{ VaRest::Sio::FArgumentWriter Into{Writer, Options, Error, WithinBudget}; return (Into.Value(Args) && ...); };
		return Message.ReplyArguments(Write);
	}
	/** An already parsed argument array, including the empty array for ack(). */
	EVaRestSendResult SendArray(const FVaRestJson& Args) const;
	static FVaRestSioAck FromMessage(FVaRestSocketMessage Value) { FVaRestSioAck Out; Out.Message = MoveTemp(Value); return Out; }
private:
	FVaRestSocketMessage Message;
};

/** Immutable native event view; placeholders and the optional recovery offset remain visible in Args. */
USTRUCT(BlueprintType)
struct VARESTSOCKETIO_API FVaRestSioEvent
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") FString Namespace;
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") FString Name;
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") FVaRestJson Args;
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") int32 NumAttachments = 0;
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") FVaRestSioAck Ack;
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") FString Offset;
	TConstArrayView<uint8> Attachment(int32 Index) const;
	template <typename T> TOptional<T> TryArg(int32 Index) const
	{
		const auto Value = Args.At(Index);
		if constexpr (std::is_same_v<T, FVaRestJson>) { if (Value.IsValid()) return Value; return {}; }
		else if constexpr (std::is_same_v<T, TArray<uint8>>)
		{
			const auto Placeholder = Value.Get(TEXT("_placeholder")), Number = Value.Get(TEXT("num"));
			const auto Id = Number.TryGet<int32>();
			if (Placeholder.IsBool() && Placeholder.AsBool() && Id.IsSet() && Native.Attachments.IsValidIndex(Id.GetValue()) && Native.Attachments[Id.GetValue()]) return *Native.Attachments[Id.GetValue()];
			return Value.TryGet<T>();
		}
		else if constexpr (VaRest::Json::bHasScriptStruct<T> && !std::is_same_v<T, FVaRestJson>)
		{
			T Out{}; FVaRestJsonConvertOptions Options; Options.BytesResolver = VaRest::WebSocket::MakePlaceholderBytesReader(Native.Attachments);
			if (FVaRestJsonConvert::JsonToStruct(Value, VaRest::Json::GetScriptStruct<T>(), &Out, Options)) return MoveTemp(Out);
			return {};
		}
		else return Value.TryGet<T>();
	}
	template <typename T> T Arg(int32 Index) const { return TryArg<T>(Index).Get(T{}); }
	static FVaRestSioEvent FromMessage(FVaRestSocketMessage Message, FString RecoveryOffset = {});
	const FVaRestSocketMessage& GetNativeMessage() const { return Native; }
private:
	FVaRestSocketMessage Native;
};
