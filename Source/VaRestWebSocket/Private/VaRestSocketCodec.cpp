// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSocketCodec.h"
#include "VaRestSocketText.h"
#include "VaRestWebSocketLog.h"
#include "VaRestTrace.h"
#include "VaRestSocketLog.h"
#include <atomic>

FVaRestSocketRouting FVaRestSocketRouting::ByField(FString RouteField)
{
	FVaRestSocketRouting Out; Out.RouteField = MoveTemp(RouteField); return Out;
}
FVaRestSocketRouting FVaRestSocketRouting::JsonRpc()
{
	FVaRestSocketRouting Out; Out.bJsonRpc = true; Out.RouteField = TEXT("method"); Out.IdField = TEXT("id"); Out.Envelope.DataPath = TEXT("params"); return Out;
}
FVaRestSocketRouting FVaRestSocketRouting::DataAt(FString Path) const
{
	auto Out = *this; Out.Envelope.DataPath = MoveTemp(Path); return Out;
}
FVaRestSocketId FVaRestSocketId::String(FString Value) { FVaRestSocketId Out; Out.bSet = true; Out.Text = MoveTemp(Value); return Out; }
FVaRestSocketId FVaRestSocketId::Numeric(int64 Value) { FVaRestSocketId Out; Out.bSet = true; Out.bNumber = true; Out.Number = Value; return Out; }
FVaRestSocketId FVaRestSocketId::FromJson(const FVaRestJson& Json)
{
	if (Json.IsString()) return String(Json.AsString());
	if (const auto Number = Json.TryGet<int64>(); Number.IsSet()) return Numeric(Number.GetValue());
	return {};
}
FString FVaRestSocketId::Key() const { return !bSet ? FString() : (bNumber ? TEXT("n:") + LexToString(Number) : TEXT("s:") + Text); }

namespace VaRest::WebSocket
{
namespace
{
	bool TopKey(const FString& Key)
	{
		return !Key.IsEmpty() && !Key.StartsWith(TEXT("/")) && !Key.Contains(TEXT(".")) && !Key.Contains(TEXT("[")) && !Key.Contains(TEXT("]"));
	}
	void Key(VaRest::Json::FCondensedWriter& Writer, FStringView Text)
	{
		const FString String(Text); const FTCHARToUTF8 Utf8(*String, String.Len());
		Writer.Key(Utf8.Get(), static_cast<rapidjson::SizeType>(Utf8.Length()));
	}
	void String(VaRest::Json::FCondensedWriter& Writer, FStringView Text)
	{
		const FString String(Text); const FTCHARToUTF8 Utf8(*String, String.Len());
		Writer.String(Utf8.Get(), static_cast<rapidjson::SizeType>(Utf8.Length()));
	}
	void WriteId(VaRest::Json::FCondensedWriter& Writer, const FVaRestSocketId& Id)
	{
		if (Id.bNumber) Writer.Int64(Id.Number); else String(Writer, Id.Text);
	}
	FVaRestError ConfigError(FString Message)
	{
		FVaRestError Error; Error.Kind = EVaRestErrorKind::Config; Error.Message = MoveTemp(Message); return Error;
	}
	class FJsonDecoder final : public IVaRestSocketDecoder
	{
	public:
		virtual FVaRestSocketDecodeResult Decode(FVaRestSocketInboundFrame Frame, const FVaRestSocketCodecContext& Context) override
		{
			FVaRestSocketDecodeResult Out; Out.Status = EVaRestSocketDecode::Message;
			FVaRestJson Root, Data;
			FVaRestError ReplyError;
			TArray<uint8> Raw = MoveTemp(Frame.Bytes);
			if (Frame.Kind == EVaRestSocketFrameKind::Text || Context.Routing.bDecodeBinaryAsJson)
			{
				TArray<uint8> Parsing = Raw; Parsing.Reserve(Parsing.Num() + 1);
				auto Parsed = FVaRestJson::ParseOwned(MoveTemp(Parsing));
				if (Parsed.HasValue()) Root = Parsed.StealValue(); else Out.bJsonError = true;
			}
			const auto& Routing = Context.Routing;
			const auto RouteValue = Routing.RouteField.IsEmpty() ? FVaRestJson() : Root.Path(Routing.RouteField);
			const auto Route = RouteValue.IsString() ? RouteValue.AsString() : (RouteValue.IsValid() ? RouteValue.ToString() : FString());
			const auto Id = Routing.IdField.IsEmpty() ? FVaRestSocketId() : FVaRestSocketId::FromJson(Root.Path(Routing.IdField));
			EVaRestSocketMessageKind Kind = EVaRestSocketMessageKind::Message;
			if (Id.bSet)
			{
				const bool bReply = Routing.bJsonRpc ? !RouteValue.IsValid()
					: (!Routing.ReplyRoute.IsEmpty() ? SameText(Route, Routing.ReplyRoute) : (!Id.bNumber && !Context.ReplyIdPrefix.IsEmpty() && Id.Text.StartsWith(Context.ReplyIdPrefix, ESearchCase::CaseSensitive)));
				Kind = bReply ? EVaRestSocketMessageKind::Reply : EVaRestSocketMessageKind::Request;
			}
			auto Evaluated = Routing.Envelope.Evaluate(Root);
			Data = Evaluated.Data;
			if (Routing.bJsonRpc)
				Data = Kind == EVaRestSocketMessageKind::Request ? Root.Get(TEXT("params")) : Root.Get(TEXT("result"));
			if (Kind == EVaRestSocketMessageKind::Reply && (!Evaluated.bSuccess || Root.Has(TEXT("error"))))
			{
				Kind = EVaRestSocketMessageKind::ErrorReply;
				ReplyError.Kind = EVaRestErrorKind::Server; ReplyError.Body = Root;
				const auto Error = Root.Get(TEXT("error"));
				ReplyError.Code = Evaluated.Code.IsEmpty() ? FVaRestEnvelope::ValueText(Error.Get(TEXT("code"))) : Evaluated.Code;
				ReplyError.Message = Evaluated.Message.IsEmpty() ? FVaRestEnvelope::ValueText(Error.Get(TEXT("message"))) : Evaluated.Message;
				if (ReplyError.Message.IsEmpty()) ReplyError.Message = TEXT("WebSocket peer returned an error reply");
			}
			auto Message = FVaRestSocketMessage::FromDecoded(Frame.Kind, MoveTemp(Raw), MoveTemp(Root), MoveTemp(Data), MoveTemp(ReplyError));
			Message.Route = Route; Message.Kind = Kind; Message.bHasId = Id.bSet; Message.ProtocolTag = TEXT("ws:json");
			Message.bNumericId = Id.bNumber; Message.NumericId = Id.Number; Message.Id = Id.bNumber ? LexToString(Id.Number) : Id.Text;
			Message.Attachments = MoveTemp(Frame.Attachments);
			Out.Message = MoveTemp(Message);
			return Out;
		}
	};
	class FJsonCodec final : public IVaRestSocketCodec
	{
	public:
		virtual FName GetProtocolTag() const override { return TEXT("WebSocket"); }
		virtual TUniquePtr<IVaRestSocketDecoder> MakeDecoder() const override { return MakeUnique<FJsonDecoder>(); }
		virtual TValueOrError<TArray<FVaRestSocketFrame>, FVaRestError> Encode(FStringView Channel, FStringView Route,
			const FVaRestSocketPayloadWriter& Payload, const FVaRestSocketId& Id, const FVaRestSocketCodecContext& Context) const override
		{
			return Write(Channel, Route, Payload, Id, Context, false, false);
		}
		virtual TValueOrError<TArray<FVaRestSocketFrame>, FVaRestError> EncodeReply(FStringView Channel, const FVaRestSocketId& Id,
			const FVaRestSocketPayloadWriter& Payload, bool bError, const FVaRestSocketCodecContext& Context) const override
		{
			return Write(Channel, Context.Routing.ReplyRoute, Payload, Id, Context, true, bError);
		}
	private:
		TValueOrError<TArray<FVaRestSocketFrame>, FVaRestError> Write(FStringView Channel, FStringView Route,
			const FVaRestSocketPayloadWriter& Payload, const FVaRestSocketId& Id, const FVaRestSocketCodecContext& Context, bool bReply, bool bError) const
		{
			const auto& Routing = Context.Routing;
			if (!Channel.IsEmpty()) return MakeError(ConfigError(TEXT("The JSON WebSocket codec has only the default channel")));
			if (((!bReply || !Routing.ReplyRoute.IsEmpty()) && !TopKey(Routing.RouteField)) || (!Routing.IdField.IsEmpty() && !TopKey(Routing.IdField))
				|| (!Routing.Envelope.DataPath.IsEmpty() && !TopKey(Routing.Envelope.DataPath)))
				return MakeError(ConfigError(TEXT("Outbound route, id and data paths must be single top-level keys")));
			if (Id.bSet && Routing.IdField.IsEmpty()) return MakeError(ConfigError(TEXT("A correlated socket packet requires IdField")));
			if (bReply && !Id.bSet) return MakeError(ConfigError(TEXT("A socket reply requires a correlation id")));
			if (Id.bSet && Id.bNumber && !Routing.bJsonRpc && Routing.ReplyRoute.IsEmpty())
				return MakeError(ConfigError(TEXT("Numeric ids require ReplyRoute or the JSON-RPC preset")));
			const FString DataKey = Routing.bJsonRpc ? (bReply ? (bError ? TEXT("error") : TEXT("result")) : TEXT("params"))
				: (bError && bReply ? FString(TEXT("error")) : Routing.Envelope.DataPath);
			if (DataKey.IsEmpty() && !Payload.bCanWriteMembers) return MakeError(ConfigError(TEXT("An unwrapped Emit payload must be an object")));
			if ((!Routing.IdField.IsEmpty() && SameText(Routing.IdField, Routing.RouteField))
				|| (!DataKey.IsEmpty() && (SameText(DataKey, Routing.RouteField) || SameText(DataKey, Routing.IdField))))
				return MakeError(ConfigError(TEXT("Outbound route, id and data keys must be distinct")));
			TArray<FString> Skip;
			if (!Routing.RouteField.IsEmpty()) Skip.Add(Routing.RouteField);
			if (!Routing.IdField.IsEmpty()) Skip.Add(Routing.IdField);
			if (Routing.bJsonRpc) Skip.Add(TEXT("jsonrpc"));
			FVaRestSocketFrame Frame; Frame.Storage.SetNumUninitialized(FrameHeadroom);
			VaRest::Json::FUtf8Sink Sink{Frame.Storage}; VaRest::Json::FCondensedWriter Writer(Sink);
			Writer.StartObject();
			if (Routing.bJsonRpc) { Key(Writer, TEXTVIEW("jsonrpc")); String(Writer, TEXTVIEW("2.0")); }
			if (!bReply || !Routing.ReplyRoute.IsEmpty()) { Key(Writer, Routing.RouteField); String(Writer, Route); }
			if (Id.bSet) { Key(Writer, Routing.IdField); WriteId(Writer, Id); }
			if (!DataKey.IsEmpty()) Key(Writer, DataKey);
			FVaRestJsonError Error;
			if (!Payload.Write(Writer, DataKey.IsEmpty(), Skip, Error))
				return MakeError(ConfigError(TEXT("The socket payload could not be encoded within the JSON limits"))); // writer unusable: never close it
			Writer.EndObject(); Frame.Length = Frame.Storage.Num() - Frame.Offset;
			if (FVaRestTrace::Detail() != EVaRestTraceDetail::None || SocketLogLevel(Context.LogLevel) == EVaRestLogLevel::Body)
			{
				auto Metadata = MakeShared<FVaRestSocketFrameMetadata, ESPMode::ThreadSafe>();
				Metadata->Channel = FString(Channel); Metadata->Route = FString(Route); Metadata->Id = Id.bSet ? (Id.bNumber ? LexToString(Id.Number) : Id.Text) : FString();
				Metadata->Kind = bReply ? (bError ? EVaRestSocketMessageKind::ErrorReply : EVaRestSocketMessageKind::Reply) : (Id.bSet ? EVaRestSocketMessageKind::Request : EVaRestSocketMessageKind::Message);
				Metadata->ProtocolTag = TEXT("ws:json"); Frame.Metadata = MoveTemp(Metadata);
			}
			TArray<FVaRestSocketFrame> Frames; Frames.Add(MoveTemp(Frame)); return MakeValue(MoveTemp(Frames));
		}
	};
}

FVaRestJsonBytesResolver MakePlaceholderBytesReader(TConstArrayView<TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>> Attachments)
{
	TArray<TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>> Held(Attachments);
	return [Held = MoveTemp(Held)](const FVaRestJson& Value, const FString&, TArray<uint8>& Out)
	{
		if (!Value.IsObject() || !Value.Get(TEXT("_placeholder")).IsBool() || !Value.Get(TEXT("_placeholder")).AsBool()) return false;
		const auto Index = Value.Get(TEXT("num")).TryGet<int64>();
		if (!Index.IsSet() || Index.GetValue() < 0 || Index.GetValue() >= Held.Num() || !Held[static_cast<int32>(Index.GetValue())]) return false;
		Out = *Held[static_cast<int32>(Index.GetValue())]; return true;
	};
}

TSharedRef<IVaRestSocketCodec, ESPMode::ThreadSafe> MakeJsonCodec() { return MakeShared<FJsonCodec, ESPMode::ThreadSafe>(); }
bool WriteJsonPayload(VaRest::Json::FCondensedWriter& Writer, const FVaRestJson& Json, bool bMembers, TConstArrayView<FString> SkipKeys, FVaRestJsonError& Error)
{
	if (!bMembers) return VaRest::Json::WriteJsonInternal(Writer, Json, 1, &Error);
	if (!Json.IsObject()) return false;
	static std::atomic<bool> bWarnedCollision{false};
	for (const auto& Member : Json.Members())
	{
		if (SkipKeys.ContainsByPredicate([&Member](const auto& Skip) { return SameText(Skip, Member.GetKey()); }))
		{
			if (!bWarnedCollision.exchange(true, std::memory_order_acq_rel)) UE_LOG(LogVaRestSocket, Warning, TEXT("VaRest socket: envelope identity keys in a payload were skipped"));
			continue;
		}
		Writer.Key(reinterpret_cast<const ANSICHAR*>(Member.Key.GetData()), static_cast<rapidjson::SizeType>(Member.Key.Len()));
		if (!VaRest::Json::WriteJsonInternal(Writer, Member.Value, 1, &Error)) return false;
	}
	return true;
}
bool WriteStructPayload(VaRest::Json::FCondensedWriter& Writer, const UScriptStruct* Struct, const void* Value,
	const FVaRestJsonConvertOptions& Options, bool bMembers, TConstArrayView<FString> SkipKeys, FVaRestJsonError& Error)
{
	if (!bMembers) return VaRest::Json::WriteStructInternal(Writer, Struct, Value, Options, 1, &Error);
	bool bSkipped = false;
	const bool bWritten = VaRest::Json::WriteStructMembersFilteredInternal(Writer, Struct, Value, SkipKeys, bSkipped, Options, 1, &Error);
	static std::atomic<bool> bWarned{false};
	if (bSkipped && !bWarned.exchange(true, std::memory_order_acq_rel)) UE_LOG(LogVaRestSocket, Warning, TEXT("VaRest socket: envelope identity keys in a struct payload were skipped"));
	return bWritten;
}
}
