// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSocketEndpoint.h"
#include "VaRestSocket.h"
#include "VaRestSocketTarget.h"
#include "VaRestSocketText.h"
#include "VaRestUrl.h"
#include "VaRestJsonRedact.h"

namespace
{
	FString HttpScheme(FString Text)
	{
		if (Text.StartsWith(TEXT("wss://"), ESearchCase::IgnoreCase)) return TEXT("https://") + Text.Mid(6);
		if (Text.StartsWith(TEXT("ws://"), ESearchCase::IgnoreCase)) return TEXT("http://") + Text.Mid(5);
		return Text;
	}
	bool IsTopKey(const FString& Text)
	{ return !Text.IsEmpty() && !Text.StartsWith(TEXT("/")) && !Text.Contains(TEXT(".")) && !Text.Contains(TEXT("[")) && !Text.Contains(TEXT("]")); }
	void ValidationError(FVaRestApiValidationContext& Context, FName Code, FStringView Location, const TCHAR* Text)
	{ Context.Error(Code, Location, FText::FromString(Text)); }
}

FString UVaRestSocketEndpointBase::ResolveUrl(FStringView BaseUrl) const
{
	FString Result = !Path.StartsWith(TEXT("/")) && Path.Contains(TEXT("://")) ? Path : VaRest::Http::JoinUrl(HttpScheme(FString(BaseUrl)), Path);
	if (Result.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase)) return TEXT("wss://") + Result.Mid(8);
	if (Result.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase)) return TEXT("ws://") + Result.Mid(7);
	return Result;
}
void UVaRestSocketEndpointBase::ConfigureSocket(FVaRestSocket& Socket) const
{
	if (bOverrideProtocolPing) Socket.WithProtocolPing(ProtocolPing);
	for (const auto& Header : Headers) Socket.WithHeader(Header.Name, Header.Value);
	for (const auto& Param : Query) Socket.WithQuery(Param.Name, Param.Value);
	Socket.WithOrigin(bOverrideOrigin ? TOptional<FString>(Origin) : TOptional<FString>());
	if (bOverrideReconnect) Socket.WithReconnect(Reconnect);
	if (bOverrideLimits) Socket.WithLimits(Limits);
	if (bOverrideQueue) Socket.WithQueue(Queue);
	if (bOverrideBackground) Socket.WithBackground(Background);
}
void UVaRestSocketEndpointBase::Validate(FVaRestApiValidationContext& Context) const
{
	const FString Location = TEXT("Sockets.") + Name.ToString();
	if (bOverrideProtocolPing && (!FMath::IsFinite(ProtocolPing.Interval) || ProtocolPing.Interval < 0 || !FMath::IsFinite(ProtocolPing.Timeout) || ProtocolPing.Timeout < 0.05))
		ValidationError(Context, TEXT("Socket.Ping"), Location, TEXT("Protocol ping interval must be nonnegative and its timeout at least 50 ms."));
	if (Path.IsEmpty() || Path.StartsWith(TEXT("//")) || (!Path.Contains(TEXT("://")) && !Path.StartsWith(TEXT("/"))))
		ValidationError(Context, TEXT("Socket.Path"), Location, TEXT("A socket path must begin with '/' or be an absolute URL."));
	FVaRestSocketConnectRequest Request; Request.Url = ResolveUrl(TEXTVIEW("https://varest.invalid"));
	for (const auto& Header : Headers) Request.Headers.Emplace(Header.Name, Header.Value);
	if (bOverrideOrigin) Request.Origin = Origin;
	const auto Target = VaRest::WebSocket::PrepareTarget(Request);
	if (!Target.HasValue()) Context.Error(TEXT("Socket.Connection"), Location, FText::FromString(Target.GetError().Message));
	if (bOverrideQueue && (Queue.MaxQueued < 0 || Queue.MaxQueuedBytes < 0))
		ValidationError(Context, TEXT("Socket.Queue"), Location, TEXT("Socket queue limits must be nonnegative."));
	if (bOverrideLimits && (Limits.MaxMessageBytes < 0 || Limits.MaxPendingInboundBytes < 0 || Limits.MaxSendBytes < 0
		|| Limits.MaxBufferedBytes < 0 || Limits.SendHighWater < 0 || Limits.MaxPendingRequests < 0 || Limits.MaxChannels < 1
		|| !FMath::IsFinite(Limits.ReadyTimeout) || Limits.ReadyTimeout < 0))
		ValidationError(Context, TEXT("Socket.Limits"), Location, TEXT("Socket limits must be finite and within their declared ranges."));
	if (bOverrideReconnect && (Reconnect.MaxAttempts < 0 || !FMath::IsFinite(Reconnect.BaseDelay) || Reconnect.BaseDelay < 0
		|| !FMath::IsFinite(Reconnect.Multiplier) || Reconnect.Multiplier < 1 || !FMath::IsFinite(Reconnect.MaxDelay) || Reconnect.MaxDelay < 0
		|| !FMath::IsFinite(Reconnect.StableAfter) || Reconnect.StableAfter < 0 || !FMath::IsFinite(Reconnect.JitterRatio) || Reconnect.JitterRatio < 0 || Reconnect.JitterRatio > 1))
		ValidationError(Context, TEXT("Socket.Reconnect"), Location, TEXT("Socket reconnect values must be finite and within their declared ranges."));
}
void UVaRestSocketEndpoint::ConfigureSocket(FVaRestSocket& Socket) const
{
	Super::ConfigureSocket(Socket);
	Socket.WithProtocols(Protocols).WithRouting(Routing).ReadyWhen(ReadyRoute).WithAuthPlacement(GetAuthPlacement());
	if (bOverrideHeartbeat) Socket.WithHeartbeat(Heartbeat);
}
void UVaRestSocketEndpoint::Validate(FVaRestApiValidationContext& Context) const
{
	Super::Validate(Context);
	const FString Location = TEXT("Sockets.") + Name.ToString();
	FVaRestSocketConnectRequest Request; Request.Url = TEXT("ws://varest.invalid/"); Request.Protocols = Protocols;
	if (!VaRest::WebSocket::PrepareTarget(Request).HasValue())
		ValidationError(Context, TEXT("Socket.Protocols"), Location, TEXT("Socket subprotocols must be distinct RFC token names."));
	if (AuthPlacement.Mode != EVaRestSocketAuthPlacement::Header && (AuthPlacement.Name.IsEmpty() || !FVaRestRedactRules::IsAcceptableKey(AuthPlacement.Name)))
		ValidationError(Context, TEXT("Socket.AuthPlacement"), Location, TEXT("The authentication field must be a nonempty redaction-compatible key."));
	if (bOverrideHeartbeat && (!FMath::IsFinite(Heartbeat.IntervalSeconds) || Heartbeat.IntervalSeconds < 0 || !FMath::IsFinite(Heartbeat.TimeoutSeconds) || Heartbeat.TimeoutSeconds < 0))
		ValidationError(Context, TEXT("Socket.Heartbeat"), Location, TEXT("Heartbeat times must be finite and nonnegative."));
	if (static_cast<uint8>(AuthPlacement.Mode) > static_cast<uint8>(EVaRestSocketAuthPlacement::FirstMessage)
		|| static_cast<uint8>(Routing.IdFormat) > static_cast<uint8>(EVaRestSocketIdFormat::Number))
		ValidationError(Context, TEXT("Socket.Enum"), Location, TEXT("A socket selection is outside its declared enum range."));
	if ((!Routing.IdField.IsEmpty() && VaRest::WebSocket::SameText(Routing.IdField, Routing.RouteField))
		|| (!Routing.Envelope.DataPath.IsEmpty() && (VaRest::WebSocket::SameText(Routing.Envelope.DataPath, Routing.RouteField)
			|| VaRest::WebSocket::SameText(Routing.Envelope.DataPath, Routing.IdField))))
		ValidationError(Context, TEXT("Socket.IdentityFields"), Location, TEXT("Socket route, id and data identity fields must be distinct."));
	TSet<VaRest::WebSocket::FSocketTextKey> Identities;
	for (int32 Index = 0; Index < Messages.Num(); ++Index)
	{
		const auto& Declaration = Messages[Index];
		const FString At = FString::Printf(TEXT("%s.Messages[%d]"), *Location, Index);
		const FString Identity = FString::Printf(TEXT("%d:%d:%s%d:%s"), static_cast<int32>(Declaration.Direction), Declaration.Channel.Len(), *Declaration.Channel, Declaration.Route.Len(), *Declaration.Route);
		const VaRest::WebSocket::FSocketTextKey Key(Identity);
		if (Declaration.Route.IsEmpty() || Identities.Contains(Key))
			ValidationError(Context, TEXT("Socket.MessageIdentity"), At, TEXT("Message routes must be nonempty and distinct for each channel and direction."));
		Identities.Add(Key);
		if (static_cast<uint8>(Declaration.Direction) > static_cast<uint8>(EVaRestSocketMessageDirection::InRequest))
			ValidationError(Context, TEXT("Socket.MessageDirection"), At, TEXT("The message direction is outside its declared enum range."));
		if (!Declaration.Channel.IsEmpty())
			ValidationError(Context, TEXT("Socket.Channel"), At, TEXT("The JSON WebSocket codec supports only the default channel; use a protocol entry for named channels."));
		Declaration.PayloadType.Validate(Context, At + TEXT(".PayloadType"));
		if (Declaration.Direction == EVaRestSocketMessageDirection::Request || Declaration.Direction == EVaRestSocketMessageDirection::InRequest)
		{
			Declaration.ReplyType.Validate(Context, At + TEXT(".ReplyType"));
			if (Routing.IdField.IsEmpty()) ValidationError(Context, TEXT("Socket.RequestId"), At, TEXT("Request declarations require IdField."));
			if (Routing.IdFormat == EVaRestSocketIdFormat::Number && !Routing.bJsonRpc && Routing.ReplyRoute.IsEmpty())
				ValidationError(Context, TEXT("Socket.RequestReplyRoute"), At, TEXT("Numeric request ids require ReplyRoute or the JSON-RPC preset."));
		}
		if (Declaration.Direction != EVaRestSocketMessageDirection::In)
		{
			if (!IsTopKey(Routing.RouteField) || (!Routing.IdField.IsEmpty() && !IsTopKey(Routing.IdField)) || (!Routing.Envelope.DataPath.IsEmpty() && !IsTopKey(Routing.Envelope.DataPath)))
				ValidationError(Context, TEXT("Socket.OutboundPath"), At, TEXT("Outbound route, id and data fields must be top-level keys."));
		}
	}
}
