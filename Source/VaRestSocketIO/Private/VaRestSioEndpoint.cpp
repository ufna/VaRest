// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSioEndpoint.h"
#include "VaRestSioSocket.h"
#include "VaRestJsonRedact.h"

namespace
{
	void Invalid(FVaRestApiValidationContext& Context, FStringView At, const TCHAR* Message)
	{ Context.Error(TEXT("Sio.Configuration"), At, FText::FromString(Message)); }
	void ValidateArgs(FVaRestApiValidationContext& Context, FStringView At, const FVaRestTypeRef& Type, EVaRestSioArgs Mode)
	{
		Type.Validate(Context, At);
		if (static_cast<uint8>(Mode) > static_cast<uint8>(EVaRestSioArgs::Array))
			Invalid(Context, At, TEXT("The argument mode is outside its declared enum range."));
		if (Mode == EVaRestSioArgs::Positional && (Type.Kind != EVaRestTypeKind::Struct || Type.Container != EVaRestTypeContainer::None))
			Invalid(Context, At, TEXT("Positional arguments require a struct without a container."));
		if (Mode == EVaRestSioArgs::Array && (Type.Container != EVaRestTypeContainer::None || (Type.Kind != EVaRestTypeKind::Json && Type.Kind != EVaRestTypeKind::None)))
			Invalid(Context, At, TEXT("Array argument mode requires a JSON value."));
	}
}

UVaRestSioEndpoint::UVaRestSioEndpoint()
{
	Path = TEXT("/socket.io/"); Limits = FVaRestSioLimits();
}
FVaRestSioLimits UVaRestSioEndpoint::GetSioLimits() const
{
	FVaRestSioLimits Result; static_cast<FVaRestSocketLimits&>(Result) = Limits;
	Result.MaxAttachments = MaxAttachments; Result.MaxPacketBytes = MaxPacketBytes;
	Result.MaxOutboundFrameBytes = MaxOutboundFrameBytes; Result.bOverrideFallbackPayloadLimit = bOverrideFallbackPayloadLimit;
	return Result;
}
bool UVaRestSioEndpoint::ConfigureSio(FVaRestSioSocket& Socket) const
{
	check(IsInGameThread());
	Socket.WithAuthPlacement(AuthPlacement).WithRecovery(bRecovery).WithAckTimeout(AckTimeout);
	if (bOverrideReconnect) Socket.WithReconnect(Reconnect);
	if (bOverrideLimits) Socket.WithLimits(GetSioLimits());
	for (const auto& Declaration : Namespaces)
	{
		auto Namespace = Socket.Namespace(Declaration.Name);
		if (!Namespace.IsValid()) return false;
		Namespace.WithSessionAuth(Declaration.bSendSession).WithConnectOnStart(Declaration.bConnectOnStart);
		if (!Declaration.AuthPayload.IsEmpty())
		{
			auto Parsed = FVaRestJson::Parse(FStringView(Declaration.AuthPayload));
			if (Parsed.HasValue() && Parsed.GetValue().IsObject()) Namespace.WithAuth(Parsed.GetValue());
		}
	}
	return Socket.IsValid();
}
void UVaRestSioEndpoint::Validate(FVaRestApiValidationContext& Context) const
{
	Super::Validate(Context);
	const FString At = TEXT("Sockets.") + Name.ToString();
	if (Path.Contains(TEXT("?")) || Path.Contains(TEXT("#"))) Invalid(Context, At + TEXT(".Path"), TEXT("The Engine.IO path cannot contain a query or fragment; use Query instead."));
	if (!FVaRestSioCodec::IsValidNamespace(DefaultNamespace)) Invalid(Context, At + TEXT(".DefaultNamespace"), TEXT("A namespace must begin with '/', be at most 256 UTF-8 bytes, and contain no comma, query or control character."));
	if (!FMath::IsFinite(AckTimeout) || AckTimeout < 0) Invalid(Context, At + TEXT(".AckTimeout"), TEXT("Ack timeout must be finite and nonnegative; zero disables it."));
	if (static_cast<uint8>(AuthPlacement.Mode) > static_cast<uint8>(EVaRestSioAuthPlacement::None)
		|| ((AuthPlacement.Mode == EVaRestSioAuthPlacement::Payload || AuthPlacement.Mode == EVaRestSioAuthPlacement::Query)
			&& (AuthPlacement.Name.IsEmpty() || !FVaRestRedactRules::IsAcceptableKey(AuthPlacement.Name))))
		Invalid(Context, At + TEXT(".AuthPlacement"), TEXT("Authentication placement requires a valid mode and redaction-compatible field name."));
	if (AuthPlacement.Mode == EVaRestSioAuthPlacement::Query && (AuthPlacement.Name.Equals(TEXT("EIO"), ESearchCase::CaseSensitive)
		|| AuthPlacement.Name.Equals(TEXT("transport"), ESearchCase::CaseSensitive) || AuthPlacement.Name.Equals(TEXT("sid"), ESearchCase::CaseSensitive)))
		Invalid(Context, At + TEXT(".AuthPlacement"), TEXT("Authentication cannot replace an Engine.IO query parameter."));
	if (bOverrideLimits) { FString Error; if (!GetSioLimits().Validate(Error)) Invalid(Context, At + TEXT(".Limits"), TEXT("Socket.IO limits are outside their declared ranges.")); }
	bool bHasDefault = Namespaces.IsEmpty();
	for (int32 Index = 0; Index < Namespaces.Num(); ++Index)
	{
		const auto& Namespace = Namespaces[Index]; const FString Here = FString::Printf(TEXT("%s.Namespaces[%d]"), *At, Index);
		if (!FVaRestSioCodec::IsValidNamespace(Namespace.Name)) Invalid(Context, Here, TEXT("Invalid namespace name."));
		for (int32 Previous = 0; Previous < Index; ++Previous)
			if (Namespaces[Previous].Name.Equals(Namespace.Name, ESearchCase::CaseSensitive)) Invalid(Context, Here, TEXT("Namespace names must be distinct, including case."));
		if (Namespace.Name.Equals(DefaultNamespace, ESearchCase::CaseSensitive))
		{
			bHasDefault = true;
			if (!Namespace.bConnectOnStart) Context.Warning(TEXT("Sio.DefaultConnect"), Here, FText::FromString(TEXT("Connect always starts the default namespace; its ConnectOnStart flag is ignored.")));
		}
		if (!Namespace.AuthPayload.IsEmpty())
		{
			auto Parsed = FVaRestJson::Parse(FStringView(Namespace.AuthPayload));
			if (!Parsed.HasValue() || !Parsed.GetValue().IsObject()) Invalid(Context, Here + TEXT(".AuthPayload"), TEXT("Static authentication must be a JSON object."));
		}
		for (int32 EventIndex = 0; EventIndex < Namespace.Events.Num(); ++EventIndex)
		{
			const auto& Event = Namespace.Events[EventIndex]; const auto EventAt = FString::Printf(TEXT("%s.Events[%d]"), *Here, EventIndex);
			if (!FVaRestSioCodec::IsValidEventName(Event.Event) || FVaRestSioCodec::IsReservedEvent(Event.Event)) Invalid(Context, EventAt, TEXT("Event names must be nonempty, nonreserved and at most 256 UTF-8 bytes."));
			if (static_cast<uint8>(Event.Direction) > static_cast<uint8>(EVaRestSioEventDirection::InWithAck)) Invalid(Context, EventAt, TEXT("The event direction is outside its declared enum range."));
			for (int32 Previous = 0; Previous < EventIndex; ++Previous)
				if (Namespace.Events[Previous].Direction == Event.Direction && Namespace.Events[Previous].Event.Equals(Event.Event, ESearchCase::CaseSensitive)) Invalid(Context, EventAt, TEXT("Event names must be distinct for each namespace and direction."));
			ValidateArgs(Context, EventAt + TEXT(".PayloadType"), Event.PayloadType, Event.ArgsMode);
			if (Event.Direction == EVaRestSioEventDirection::OutWithAck) ValidateArgs(Context, EventAt + TEXT(".ReplyType"), Event.ReplyType, Event.ReplyArgsMode);
			else if (Event.ReplyType.Kind != EVaRestTypeKind::None || Event.ReplyType.Container != EVaRestTypeContainer::None || Event.ReplyType.Type
				|| Event.ReplyType.bJsonValue || Event.ReplyArgsMode != EVaRestSioArgs::First || Event.bErrorFirstReply)
				Invalid(Context, EventAt, TEXT("Reply fields are only available for OutWithAck declarations."));
		}
	}
	if (!bHasDefault) Invalid(Context, At + TEXT(".DefaultNamespace"), TEXT("The default namespace must be declared when Namespaces is nonempty."));
}
