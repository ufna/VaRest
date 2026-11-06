// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSioCodec.h"
#include "VaRestJsonUtf8.h"
#include "UObject/UnrealType.h"
#include "Misc/ScopeLock.h"

namespace VaRest::SocketIO::CodecPrivate
{
constexpr int64 MaxAckId = 9007199254740991LL;
FVaRestError Config(FString Message)
{
	FVaRestError Error; Error.Kind = EVaRestErrorKind::Config; Error.Message = MoveTemp(Message); return Error;
}
bool Same(FStringView A, FStringView B) { return A.Len() == B.Len() && (!A.Len() || FMemory::Memcmp(A.GetData(), B.GetData(), A.Len() * sizeof(TCHAR)) == 0); }
void WriteString(VaRest::Json::FCondensedWriter& Writer, FStringView Value)
{
	const FString Copy(Value); const FTCHARToUTF8 Utf8(*Copy, Copy.Len()); Writer.String(Utf8.Get(), static_cast<rapidjson::SizeType>(Utf8.Length()));
}
FString Text(TConstArrayView<uint8> Bytes)
{
	if (Bytes.IsEmpty()) return {};
	const FUTF8ToTCHAR Utf8(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num()); return FString(Utf8.Length(), Utf8.Get());
}
FName Tag(uint8 Type)
{
	static const FName Tags[] = {TEXT("sio:CONNECT"), TEXT("sio:DISCONNECT"), TEXT("sio:EVENT"), TEXT("sio:ACK"), TEXT("sio:CONNECT_ERROR"), TEXT("sio:BINARY_EVENT"), TEXT("sio:BINARY_ACK")};
	return Tags[Type];
}
bool ReadUnsigned(TConstArrayView<uint8> Bytes, int32& Cursor, int64 Limit, int64& Out)
{
	const int32 Start = Cursor; Out = 0;
	while (Cursor < Bytes.Num() && Bytes[Cursor] >= '0' && Bytes[Cursor] <= '9')
	{
		const int32 Digit = Bytes[Cursor++] - '0';
		if (Out > (Limit - Digit) / 10) return false;
		Out = Out * 10 + Digit;
	}
	return Cursor > Start && Out <= Limit;
}
bool PlaceholdersValid(const FVaRestJson& Json, int32 Count, int32 Depth = 0)
{
	if (Depth > VaRest::Json::MaxDepth) return false;
	if (Json.IsObject())
	{
		const auto Flag = Json.Get(TEXT("_placeholder"));
		if (Flag.IsBool() && Flag.AsBool())
		{
			const auto Number = Json.Get(TEXT("num")).TryGet<int64>();
			return Number.IsSet() && Number.GetValue() >= 0 && Number.GetValue() < Count;
		}
		for (const auto& Key : Json.GetKeys()) if (!PlaceholdersValid(Json.Get(Key), Count, Depth + 1)) return false;
	}
	else if (Json.IsArray()) for (int32 Index = 0; Index < Json.Num(); ++Index) if (!PlaceholdersValid(Json.At(Index), Count, Depth + 1)) return false;
	return true;
}
struct FPacket
{
	FVaRestSocketMessage Message;
	int32 Expected = 0;
	int64 Bytes = 0;
};
class FDecoder final : public IVaRestSocketDecoder
{
public:
	explicit FDecoder(FVaRestSioCodecOptions InOptions, bool bRequireOpen) : Limits(MoveTemp(InOptions)), bAwaitingOpen(bRequireOpen) {}
	virtual FVaRestSocketDecodeResult Decode(FVaRestSocketInboundFrame Frame, const FVaRestSocketCodecContext&) override
	{
		check(!IsInGameThread());
		if (bPoisoned) return Fail(TEXT("Socket.IO decoder is poisoned"));
		if (Frame.Bytes.Num() > Limits.MaxMessageBytes) return Fail(TEXT("Socket.IO frame exceeds MaxMessageBytes"), 1009);
		if (bAwaitingOpen)
		{
			bAwaitingOpen = false;
			if (Frame.Kind != EVaRestSocketFrameKind::Text || Frame.Bytes.IsEmpty() || Frame.Bytes[0] != '0')
			{
				bPoisoned = true;
				auto First = FVaRestSocketMessage::FromDecoded(Frame.Kind, MoveTemp(Frame.Bytes), {}, {});
				First.Kind = EVaRestSocketMessageKind::Control; First.ProtocolTag = TEXT("eio:invalidfirst");
				return Delivered(MoveTemp(First));
			}
		}
		if (Frame.Kind == EVaRestSocketFrameKind::Binary)
		{
			if (!Pending.IsSet()) return Fail(TEXT("Socket.IO binary attachment arrived without a packet header"));
			auto& Packet = Pending.GetValue();
			if (Frame.Bytes.Num() > Limits.MaxPacketBytes - Packet.Bytes) return Fail(TEXT("Socket.IO packet exceeds MaxPacketBytes"), 1009);
			Packet.Bytes += Frame.Bytes.Num();
			Packet.Message.Attachments.Add(MakeShared<const TArray<uint8>, ESPMode::ThreadSafe>(MoveTemp(Frame.Bytes)));
			if (Packet.Message.Attachments.Num() < Packet.Expected) return {};
			if (!PlaceholdersValid(Packet.Message.Json(), Packet.Expected)) return Fail(TEXT("Socket.IO packet has an invalid attachment placeholder"));
			auto Message = MoveTemp(Packet.Message); Pending.Reset(); return Delivered(MoveTemp(Message));
		}
		if (Frame.Bytes.IsEmpty()) return Fail(TEXT("An Engine.IO packet cannot be empty"));
		if (!VaRest::Json::IsValidUtf8(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Frame.Bytes.GetData()), Frame.Bytes.Num()))) return Fail(TEXT("Engine.IO text is not UTF-8"));
		const uint8 EngineType = Frame.Bytes[0];
		if (EngineType < '0' || EngineType > '6') return Fail(TEXT("Unknown Engine.IO packet type"));
		if (EngineType != '4')
		{
			FVaRestJson Root;
			if (EngineType == '0')
			{
				auto Parsed = FVaRestJson::Parse(TConstArrayView<uint8>(Frame.Bytes).RightChop(1));
				// The GT session stage diagnoses malformed OPEN as terminal Config, including a wrong endpoint/version.
				if (Parsed.HasValue()) Root = Parsed.StealValue();
			}
			else if (Frame.Bytes.Num() > 1) Root = FVaRestJson::From(Text(TConstArrayView<uint8>(Frame.Bytes).RightChop(1)));
			auto Message = FVaRestSocketMessage::FromDecoded(Frame.Kind, MoveTemp(Frame.Bytes), Root, Root);
			Message.Kind = EVaRestSocketMessageKind::Control; Message.bAutoReplied = Frame.bAutoReplied;
			static const FName Tags[] = {TEXT("eio:open"), TEXT("eio:close"), TEXT("eio:ping"), TEXT("eio:pong"), NAME_None, TEXT("eio:upgrade"), TEXT("eio:noop")};
			Message.ProtocolTag = Tags[EngineType - '0'];
			auto Result = Delivered(MoveTemp(Message)); Result.bInterleavedControl = Pending.IsSet(); return Result;
		}
		if (Pending.IsSet()) return Fail(TEXT("Socket.IO text message arrived while binary attachments were expected"));
		if (Frame.Bytes.Num() > Limits.MaxPacketBytes) return Fail(TEXT("Socket.IO packet exceeds MaxPacketBytes"), 1009);
		return ParsePacket(MoveTemp(Frame));
	}
private:
	FVaRestSocketDecodeResult Fail(FString Reason, int32 Code = 1002)
	{
		bPoisoned = true; Pending.Reset(); FVaRestSocketDecodeResult Out; Out.Status = EVaRestSocketDecode::ProtocolError;
		Out.Reason = MoveTemp(Reason); Out.CloseCode = Code; return Out;
	}
	static FVaRestSocketDecodeResult Delivered(FVaRestSocketMessage Message)
	{
		FVaRestSocketDecodeResult Out; Out.Status = EVaRestSocketDecode::Message; Out.Message = MoveTemp(Message); return Out;
	}
	FVaRestSocketDecodeResult ParsePacket(FVaRestSocketInboundFrame Frame)
	{
		const TConstArrayView<uint8> Bytes(Frame.Bytes);
		if (Bytes.Num() < 2 || Bytes[1] < '0' || Bytes[1] > '6') return Fail(TEXT("Unknown Socket.IO packet type"));
		const uint8 Type = Bytes[1] - '0'; int32 Cursor = 2, AttachmentCount = 0;
		if (Type == 5 || Type == 6)
		{
			int64 Count = 0;
			if (!ReadUnsigned(Bytes, Cursor, Limits.MaxAttachments, Count) || Count == 0 || Cursor >= Bytes.Num() || Bytes[Cursor++] != '-') return Fail(TEXT("Invalid Socket.IO attachment count"));
			AttachmentCount = static_cast<int32>(Count);
		}
		FString Namespace = TEXT("/");
		if (Cursor < Bytes.Num() && Bytes[Cursor] == '/')
		{
			const int32 Start = Cursor; while (Cursor < Bytes.Num() && Bytes[Cursor] != ',') ++Cursor;
			if (Cursor == Bytes.Num()) return Fail(TEXT("Socket.IO namespace is missing its comma delimiter"));
			Namespace = Text(Bytes.Slice(Start, Cursor - Start)); ++Cursor;
			if (!FVaRestSioCodec::IsValidNamespace(Namespace)) return Fail(TEXT("Invalid Socket.IO namespace"));
		}
		FVaRestSocketId Id;
		if (Cursor < Bytes.Num() && Bytes[Cursor] >= '0' && Bytes[Cursor] <= '9')
		{
			int64 Number = 0; if (!ReadUnsigned(Bytes, Cursor, MaxAckId, Number)) return Fail(TEXT("Socket.IO ack id exceeds the exact JavaScript integer range"));
			Id = FVaRestSocketId::Numeric(Number);
		}
		FVaRestJson Root;
		if (Cursor < Bytes.Num())
		{
			FVaRestJsonParseOptions ParseOptions; ParseOptions.MaxInputBytes = Limits.MaxPacketBytes;
			auto Parsed = FVaRestJson::Parse(Bytes.RightChop(Cursor), ParseOptions);
			if (!Parsed.HasValue()) return Fail(TEXT("Malformed Socket.IO JSON payload")); Root = Parsed.StealValue();
		}
		const bool bEvent = Type == 2 || Type == 5, bAck = Type == 3 || Type == 6;
		if (!bEvent && !bAck && Id.bSet) return Fail(TEXT("Only Socket.IO events and acks can carry an ack id"));
		if (bAck && (!Id.bSet || !Root.IsArray())) return Fail(TEXT("A Socket.IO ack requires an id and an argument array"));
		if (Type == 0 && Root.IsValid() && !Root.IsObject()) return Fail(TEXT("Socket.IO CONNECT payload must be an object"));
		if (Type == 1 && Root.IsValid()) return Fail(TEXT("Socket.IO DISCONNECT cannot carry a payload"));
		// Legacy string CONNECT_ERROR is retained for the stage's precise v2/EIO3 diagnostic.
		if (Type == 4 && !Root.IsObject() && !Root.IsString()) return Fail(TEXT("Socket.IO CONNECT_ERROR requires an object"));
		FString Route; FVaRestJson Data = Root;
		if (bEvent)
		{
			if (!Root.IsArray() || Root.Num() == 0) return Fail(TEXT("Socket.IO EVENT requires a nonempty array"));
			const auto Name = Root.At(0);
			if (Name.IsString()) Route = Name.AsString(); else if (Name.IsNumber()) Route = Name.ToString(); else return Fail(TEXT("Socket.IO event name must be a string or number"));
			if (!FVaRestSioCodec::IsValidEventName(Route)) return Fail(TEXT("Socket.IO event name is empty, reserved or over 256 UTF-8 bytes"));
			Data = FVaRestJson::Array(); for (int32 Index = 1; Index < Root.Num(); ++Index) Data.Add(Root.At(Index));
		}
		auto Message = FVaRestSocketMessage::FromDecoded(Frame.Kind, MoveTemp(Frame.Bytes), Root, Data);
		Message.Channel = MoveTemp(Namespace); Message.Route = MoveTemp(Route); Message.ProtocolTag = Tag(Type);
		Message.Kind = bAck ? EVaRestSocketMessageKind::Reply : (bEvent ? (Id.bSet ? EVaRestSocketMessageKind::Request : EVaRestSocketMessageKind::Message) : EVaRestSocketMessageKind::Control);
		Message.bHasId = Id.bSet; Message.bNumericId = Id.bSet; Message.NumericId = Id.Number; Message.Id = Id.bSet ? LexToString(Id.Number) : FString();
		if (AttachmentCount)
		{
			FPacket Packet; Packet.Bytes = Bytes.Num(); Packet.Expected = AttachmentCount; Packet.Message = MoveTemp(Message);
			Packet.Message.Attachments.Reserve(AttachmentCount); Pending = MoveTemp(Packet); return {};
		}
		return Delivered(MoveTemp(Message));
	}
	const FVaRestSioCodecOptions Limits;
	TOptional<FPacket> Pending;
	bool bPoisoned = false;
	bool bAwaitingOpen = false;
};
}

namespace SioWirePrivate = VaRest::SocketIO::CodecPrivate;

FVaRestSioCodec::FVaRestSioCodec(FVaRestSioCodecOptions InOptions) : StoredOptions(MoveTemp(InOptions)), OutboundFrameLimit(StoredOptions.MaxOutboundFrameBytes) {}
FVaRestSioCodecOptions FVaRestSioCodec::GetOptionsSnapshot() const { FScopeLock Lock(&OptionsMutex); return StoredOptions; }
bool FVaRestSioCodec::UpdateOptions(FVaRestSioCodecOptions Options, FString& Error)
{
	if (!ValidateOptions(Options, Error)) return false;
	FScopeLock Lock(&OptionsMutex); StoredOptions = MoveTemp(Options); return true;
}
void FVaRestSioCodec::SetOutboundFrameLimit(int64 Bytes) { OutboundFrameLimit.store(FMath::Clamp<int64>(Bytes, 1, MAX_int32 - VaRest::WebSocket::FrameHeadroom), std::memory_order_release); }
FName FVaRestSioCodec::GetProtocolTag() const { return TEXT("SocketIO"); }
int32 FVaRestSioCodec::GetExcerptJsonOffset(TConstArrayView<uint8> Bytes) const
{
	if (Bytes.IsEmpty()) return 0;
	if (Bytes[0] >= '0' && Bytes[0] <= '6' && Bytes[0] != '4') return 1;
	if (Bytes.Num() < 2 || Bytes[0] != '4' || Bytes[1] < '0' || Bytes[1] > '6') return Bytes.Num();
	int32 Cursor = 2;
	if (Bytes[1] == '5' || Bytes[1] == '6')
	{
		while (Cursor < Bytes.Num() && Bytes[Cursor] >= '0' && Bytes[Cursor] <= '9') ++Cursor;
		if (Cursor == 2 || Cursor >= Bytes.Num() || Bytes[Cursor++] != '-') return Bytes.Num();
	}
	if (Cursor < Bytes.Num() && Bytes[Cursor] == '/')
	{
		while (Cursor < Bytes.Num() && Bytes[Cursor] != ',') ++Cursor;
		if (Cursor == Bytes.Num()) return Cursor; ++Cursor;
	}
	while (Cursor < Bytes.Num() && Bytes[Cursor] >= '0' && Bytes[Cursor] <= '9') ++Cursor;
	return Cursor;
}
TUniquePtr<IVaRestSocketDecoder> FVaRestSioCodec::MakeDecoder() const { return MakeUnique<SioWirePrivate::FDecoder>(GetOptionsSnapshot(), bRequireEngineOpen.load(std::memory_order_acquire)); }
bool FVaRestSioCodec::RedactAllOutgoingExcerptValues(TConstArrayView<uint8> Bytes) const { return Bytes.Num() >= 2 && Bytes[0] == '4' && Bytes[1] == '0'; }
void FVaRestSioCodec::ExtendRedactionRules(FVaRestRedactRules& Rules) const { Rules.AddKey(TEXTVIEW("sid")); Rules.AddKey(TEXTVIEW("pid")); }
bool FVaRestSioCodec::IsReservedEvent(FStringView Name)
{
	for (const auto Reserved : {TEXTVIEW("connect"), TEXTVIEW("connect_error"), TEXTVIEW("disconnect"), TEXTVIEW("disconnecting"), TEXTVIEW("newListener"), TEXTVIEW("removeListener")}) if (SioWirePrivate::Same(Name, Reserved)) return true;
	return false;
}
bool FVaRestSioCodec::IsValidNamespace(FStringView Name)
{
	if (Name.IsEmpty() || Name[0] != '/' || VaRest::Json::Utf8Length(Name) > 256) return false;
	for (TCHAR Character : Name) if (Character == TEXT(',') || Character == TEXT('?') || Character < TEXT(' ') || Character == 127) return false;
	return true;
}
bool FVaRestSioCodec::ValidateOptions(FString& Error) const
{ return ValidateOptions(GetOptionsSnapshot(), Error); }
bool FVaRestSioCodec::IsValidEventName(FStringView Name)
{ return !Name.IsEmpty() && VaRest::Json::Utf8Length(Name) <= 256 && !IsReservedEvent(Name); }
bool FVaRestSioCodec::ValidateOptions(const FVaRestSioCodecOptions& Options, FString& Error)
{
	Error.Reset();
	if (Options.MaxAttachments < 0 || Options.MaxAttachments > 4095 || Options.MaxMessageBytes <= 0 || Options.MaxMessageBytes > MAX_int32 - 1
		|| Options.MaxPacketBytes <= 0 || Options.MaxPacketBytes > Options.MaxPendingInboundBytes / 2 || Options.MaxOutboundFrameBytes <= 0
		|| Options.MaxOutboundFrameBytes > MAX_int32 - VaRest::WebSocket::FrameHeadroom || static_cast<uint8>(Options.Args) > static_cast<uint8>(EVaRestSioArgs::Array))
	{ Error = TEXT("Invalid Socket.IO limits; MaxPacketBytes must not exceed half MaxPendingInboundBytes"); return false; }
	return true;
}
TValueOrError<TArray<FVaRestSocketFrame>, FVaRestError> FVaRestSioCodec::Encode(FStringView Channel, FStringView Route,
	const FVaRestSocketPayloadWriter& Payload, const FVaRestSocketId& Id, const FVaRestSocketCodecContext& Context) const
{
	return EncodePacket(Channel, Route, Payload, Id, Context, false, false);
}
TValueOrError<TArray<FVaRestSocketFrame>, FVaRestError> FVaRestSioCodec::EncodeReply(FStringView Channel, const FVaRestSocketId& Id,
	const FVaRestSocketPayloadWriter& Payload, bool bError, const FVaRestSocketCodecContext& Context) const
{
	return EncodePacket(Channel, {}, Payload, Id, Context, true, bError);
}
TValueOrError<TArray<FVaRestSocketFrame>, FVaRestError> FVaRestSioCodec::EncodePacket(FStringView Channel, FStringView Route,
	const FVaRestSocketPayloadWriter& Payload, const FVaRestSocketId& Id, const FVaRestSocketCodecContext& Context, bool bReply, bool bError) const
{
	const auto Options = GetOptionsSnapshot();
	FString ErrorText; if (!ValidateOptions(Options, ErrorText)) return MakeError(SioWirePrivate::Config(MoveTemp(ErrorText)));
	const int64 FrameLimit = OutboundFrameLimit.load(std::memory_order_acquire);
	const FString Namespace(Channel.IsEmpty() ? (Context.DefaultChannel.IsEmpty() ? FString(TEXT("/")) : Context.DefaultChannel) : FString(Channel));
	if (!IsValidNamespace(Namespace)) return MakeError(SioWirePrivate::Config(TEXT("A Socket.IO namespace must start with '/' and contain neither ',' nor '?'")));
	if (!bReply && !IsValidEventName(Route)) return MakeError(SioWirePrivate::Config(TEXT("Socket.IO event names must be nonempty, non-reserved and at most 256 UTF-8 bytes")));
	if ((bReply && !Id.bSet) || (Id.bSet && (!Id.bNumber || Id.Number < 0 || Id.Number > SioWirePrivate::MaxAckId))) return MakeError(SioWirePrivate::Config(TEXT("Socket.IO ack ids must be exact nonnegative JavaScript integers")));
	FVaRestJsonConvertOptions Convert = Payload.ConvertOptions ? *Payload.ConvertOptions : FVaRestJsonConvertOptions();
	if (!Convert.Rules) Convert.Rules = FVaRestJsonRules::DefaultSnapshot();
	Convert.MaxOutputBytes = Convert.MaxOutputBytes > 0 ? FMath::Min(Convert.MaxOutputBytes, Options.MaxPacketBytes) : Options.MaxPacketBytes;
	TArray<const FProperty*> Fields; FVaRestJsonError WriteError;
	const auto ArgsMode = Payload.bPositionalArguments ? EVaRestSioArgs::Positional : Options.Args;
	if (!Payload.Arguments.IsSet() && ArgsMode == EVaRestSioArgs::Positional && (!Payload.NativeStruct || !Payload.NativeValue
		|| !VaRest::Json::TryGetConvertedFieldsInternal(Payload.NativeStruct, Fields, Convert, &WriteError))) return MakeError(SioWirePrivate::Config(TEXT("Positional Socket.IO arguments require an ordinary struct with valid converted fields")));
	if (!Payload.Arguments.IsSet() && ArgsMode == EVaRestSioArgs::Array && (!Payload.Json || !Payload.Json->IsArray())) return MakeError(SioWirePrivate::Config(TEXT("Array Socket.IO arguments require a JSON array")));
	TArray<FVaRestSocketFrame> Attachments; bool bAttachmentOverflow = false; int64 AttachmentBytes = 0;
	Convert.BytesWriter = [&](TConstArrayView<uint8> Bytes, FVaRestJson& Replacement)
	{
		if (Attachments.Num() >= Options.MaxAttachments || Bytes.Num() > FrameLimit || Bytes.Num() > Options.MaxPacketBytes - AttachmentBytes)
		{ bAttachmentOverflow = true; Replacement = FVaRestJson::Null(); return true; }
		Replacement = FVaRestJson::Object().Set(TEXT("_placeholder"), true).Set(TEXT("num"), Attachments.Num());
		Attachments.Add(FVaRestSocketFrame::Binary(Bytes)); AttachmentBytes += Bytes.Num(); return true;
	};
	const FTCHARToUTF8 NamespaceUtf8(*Namespace, Namespace.Len());
	const int64 Gap = static_cast<int64>(NamespaceUtf8.Length()) + 64;
	if (Gap > MAX_int32 - VaRest::WebSocket::FrameHeadroom) return MakeError(SioWirePrivate::Config(TEXT("Socket.IO namespace is too large to encode")));
	const int32 BodyStart = VaRest::WebSocket::FrameHeadroom + static_cast<int32>(Gap);
	FVaRestSocketFrame Frame; Frame.Storage.SetNumUninitialized(BodyStart);
	VaRest::Json::FUtf8Sink Sink{Frame.Storage}; VaRest::Json::FCondensedWriter Writer(Sink); Writer.StartArray();
	if (!bReply) SioWirePrivate::WriteString(Writer, Route);
	bool bWritten = true;
	if (Payload.Arguments.IsSet())
	{
		auto WithinBudget = [&]() { return !bAttachmentOverflow && Frame.Storage.Num() - BodyStart <= Options.MaxPacketBytes - AttachmentBytes; };
		bWritten = Payload.Arguments.GetValue()(Writer, Convert, WriteError, WithinBudget);
	}
	else if (ArgsMode == EVaRestSioArgs::Array)
	{
		for (int32 Index = 0; Index < Payload.Json->Num() && bWritten; ++Index) bWritten = VaRest::WebSocket::WriteJsonPayload(Writer, Payload.Json->At(Index), false, {}, WriteError);
	}
	else if (ArgsMode == EVaRestSioArgs::Positional)
	{
		for (const auto* Field : Fields) if (!(bWritten = VaRest::Json::WritePropertyInternal(Writer, Field, Field->ContainerPtrToValuePtr<void>(Payload.NativeValue), Convert, 1, &WriteError))) break;
	}
	else if (Payload.NativeStruct && Payload.NativeValue) bWritten = VaRest::Json::WriteStructInternal(Writer, Payload.NativeStruct, Payload.NativeValue, Convert, 1, &WriteError);
	else bWritten = Payload.Write(Writer, false, {}, WriteError);
	if (!bWritten || bAttachmentOverflow) return MakeError(SioWirePrivate::Config(TEXT("Socket.IO arguments exceed their attachment, frame, packet or JSON conversion limits")));
	Writer.EndArray();
	const uint8 Type = bReply ? (Attachments.IsEmpty() ? 3 : 6) : (Attachments.IsEmpty() ? 2 : 5);
	FString Header = TEXT("4") + LexToString(Type);
	if (!Attachments.IsEmpty()) Header += LexToString(Attachments.Num()) + TEXT("-");
	if (!SioWirePrivate::Same(Namespace, TEXTVIEW("/"))) Header += Namespace + TEXT(",");
	if (Id.bSet) Header += LexToString(Id.Number);
	const FTCHARToUTF8 HeaderUtf8(*Header, Header.Len());
	check(HeaderUtf8.Length() <= Gap);
	Frame.Offset = BodyStart - HeaderUtf8.Length(); Frame.Length = Frame.Storage.Num() - Frame.Offset;
	FMemory::Memcpy(Frame.Storage.GetData() + Frame.Offset, HeaderUtf8.Get(), HeaderUtf8.Length());
	if (Frame.Length > FrameLimit || Frame.Length > Options.MaxPacketBytes - AttachmentBytes) return MakeError(SioWirePrivate::Config(TEXT("Socket.IO encoded frame or packet exceeds its limit")));
	auto Metadata = MakeShared<FVaRestSocketFrameMetadata, ESPMode::ThreadSafe>(); Metadata->Channel = Namespace; Metadata->Route = FString(Route);
	Metadata->Id = Id.bSet ? LexToString(Id.Number) : FString(); Metadata->Kind = bReply ? EVaRestSocketMessageKind::Reply : (Id.bSet ? EVaRestSocketMessageKind::Request : EVaRestSocketMessageKind::Message);
	Metadata->ProtocolTag = SioWirePrivate::Tag(Type); for (const auto& Attachment : Attachments) Metadata->AttachmentSizes.Add(Attachment.Length);
	Metadata->ExcerptJsonOffset = HeaderUtf8.Length();
	Frame.Metadata = Metadata; for (auto& Attachment : Attachments) Attachment.Metadata = Metadata;
	TArray<FVaRestSocketFrame> Frames; Frames.Reserve(Attachments.Num() + 1); Frames.Add(MoveTemp(Frame)); Frames.Append(MoveTemp(Attachments));
	(void)bError; // Socket.IO has no error packet for ACK; an error-first convention is an argument, interpreted by its caller.
	return MakeValue(MoveTemp(Frames));
}
TValueOrError<TArray<FVaRestSocketFrame>, FVaRestError> FVaRestSioCodec::EncodeControl(uint8 Type, FStringView Namespace, const FVaRestJson& Payload) const
{
	const auto Options = GetOptionsSnapshot();
	FString ErrorText; if (!ValidateOptions(Options, ErrorText)) return MakeError(SioWirePrivate::Config(MoveTemp(ErrorText)));
	const int64 FrameLimit = OutboundFrameLimit.load(std::memory_order_acquire);
	if ((Type != 0 && Type != 1) || !IsValidNamespace(Namespace) || (Type == 1 && Payload.IsValid()) || (Payload.IsValid() && !Payload.IsObject())) return MakeError(SioWirePrivate::Config(TEXT("Invalid Socket.IO CONNECT/DISCONNECT packet")));
	FString Prefix = TEXT("4") + LexToString(Type); if (!SioWirePrivate::Same(Namespace, TEXTVIEW("/"))) Prefix += FString(Namespace) + TEXT(",");
	auto Frame = FVaRestSocketFrame::Text(FStringView(Prefix));
	if (Payload.IsValid() && Payload.Num())
	{
		const auto Written = Payload.TryToUtf8(Frame.Storage); if (!Written.HasValue()) return MakeError(SioWirePrivate::Config(TEXT("Socket.IO CONNECT payload could not be encoded")));
		Frame.Length = Frame.Storage.Num() - Frame.Offset;
	}
	if (Frame.Length > FrameLimit || Frame.Length > Options.MaxPacketBytes) return MakeError(SioWirePrivate::Config(TEXT("Socket.IO control packet exceeds its limit")));
	auto Metadata = MakeShared<FVaRestSocketFrameMetadata, ESPMode::ThreadSafe>(); Metadata->Channel = FString(Namespace); Metadata->Kind = EVaRestSocketMessageKind::Control; Metadata->ProtocolTag = SioWirePrivate::Tag(Type);
	Metadata->ExcerptJsonOffset = GetExcerptJsonOffset(Frame.GetPayload()); Metadata->bRedactAllExcerptValues = Type == 0; Frame.Metadata = MoveTemp(Metadata);
	TArray<FVaRestSocketFrame> Frames; Frames.Add(MoveTemp(Frame)); return MakeValue(MoveTemp(Frames));
}
TValueOrError<TArray<FVaRestSocketFrame>, FVaRestError> FVaRestSioCodec::WithAckId(TArray<FVaRestSocketFrame> Frames, const FVaRestSocketId& Id) const
{
	const auto Options = GetOptionsSnapshot();
	if (Frames.IsEmpty() || !Id.bSet || !Id.bNumber || Id.Number < 0 || Id.Number > SioWirePrivate::MaxAckId)
		return MakeError(SioWirePrivate::Config(TEXT("Invalid Socket.IO prepared ack id or packet")));
	auto& Header = Frames[0];
	const auto Bytes = Header.GetPayload();
	if (!Header.IsValid() || Header.Kind != EVaRestSocketFrameKind::Text || !Header.Metadata || !Header.Metadata->Id.IsEmpty()
		|| Bytes.Num() < 3 || Bytes[0] != '4' || (Bytes[1] != '2' && Bytes[1] != '5')
		|| Header.Metadata->ExcerptJsonOffset < 2 || Header.Metadata->ExcerptJsonOffset >= Bytes.Num()
		|| Bytes[Header.Metadata->ExcerptJsonOffset] != '[')
		return MakeError(SioWirePrivate::Config(TEXT("Socket.IO ack requires a prepared EVENT packet without an id")));
	const FString Number = LexToString(Id.Number); const FTCHARToUTF8 Utf8(*Number);
	if (Header.Offset - Utf8.Length() < VaRest::WebSocket::FrameHeadroom)
		return MakeError(SioWirePrivate::Config(TEXT("Socket.IO prepared packet has insufficient ack header headroom")));
	const int32 Prefix = Header.Metadata->ExcerptJsonOffset, OldOffset = Header.Offset;
	Header.Offset -= Utf8.Length(); Header.Length += Utf8.Length();
	FMemory::Memmove(Header.Storage.GetData() + Header.Offset, Header.Storage.GetData() + OldOffset, Prefix);
	FMemory::Memcpy(Header.Storage.GetData() + Header.Offset + Prefix, Utf8.Get(), Utf8.Length());
	auto Metadata = MakeShared<FVaRestSocketFrameMetadata, ESPMode::ThreadSafe>(*Header.Metadata);
	Metadata->Id = Number; Metadata->Kind = EVaRestSocketMessageKind::Request; Metadata->ExcerptJsonOffset += Utf8.Length();
	const int64 FrameLimit = OutboundFrameLimit.load(std::memory_order_acquire); int64 Total = 0;
	for (auto& Frame : Frames)
	{
		if (!Frame.IsValid() || Frame.Length > FrameLimit || Frame.Length > Options.MaxPacketBytes - Total)
			return MakeError(SioWirePrivate::Config(TEXT("Socket.IO acknowledged packet exceeds its negotiated frame or packet limit")));
		Total += Frame.Length; Frame.Metadata = Metadata;
	}
	return MakeValue(MoveTemp(Frames));
}
