// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSocketClient.h"
#include "VaRestSocketBlueprint.h"
#include "UObject/Stack.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

void UVaRestSocketClient::InitializeOnGT(FVaRestSocket Socket, bool bTransportEvents)
{
	check(IsInGameThread() && !Native.IsValid());
	Native = MoveTemp(Socket);
	const TWeakObjectPtr<UVaRestSocketClient> Weak(this);
#define SOCKET_FORWARD(Event, Signature, Arguments) \
	Native.Event(this, [Weak] Signature { if (auto* View = Weak.Get()) { const TStrongObjectPtr<UVaRestSocketClient> Pin(View); View->Event.Broadcast Arguments; } })
	if (bTransportEvents)
	{
		SOCKET_FORWARD(OnConnected, (const FVaRestSocketOpen& Open), (Open));
		SOCKET_FORWARD(OnReady, (const FVaRestSocketOpen& Open), (Open, Open.bReconnect));
		SOCKET_FORWARD(OnDisconnected, (const FVaRestSocketClose& End), (End));
		SOCKET_FORWARD(OnMessage, (const FVaRestSocketMessage& Incoming), (Incoming));
		SOCKET_FORWARD(OnUnrouted, (const FVaRestSocketMessage& Incoming), (Incoming));
	}
	SOCKET_FORWARD(OnRetryScheduled, (const FVaRestSocketStatus& Status, const FVaRestSocketClose& End), (Status, End));
	SOCKET_FORWARD(OnGaveUp, (const FVaRestError& Error, const FVaRestSocketClose& End), (Error, End));
	SOCKET_FORWARD(OnStateChanged, (EVaRestSocketState Old, EVaRestSocketState New), (Old, New));
	SOCKET_FORWARD(OnError, (const FVaRestError& Error), (Error));
	SOCKET_FORWARD(OnDrained, (), ());
#undef SOCKET_FORWARD
}
void UVaRestSocketClient::BeginDestroy()
{
	check(IsInGameThread());
	Native.OffAll(this); Native = {};
	Super::BeginDestroy();
}
void UVaRestSocketClient::Connect() { Native.Connect(); }
void UVaRestSocketClient::Close(int32 Code, const FString& Reason) { Native.Close(Code, Reason); }
void UVaRestSocketClient::ReconnectNow() { Native.ReconnectNow(); }
void UVaRestSocketClient::StopReconnecting() { Native.StopReconnecting(); }
void UVaRestSocketClient::ResetSession() { Native.ResetSession(); }
EVaRestSendResult UVaRestSocketClient::SendText(const FString& Text, EVaRestSend Flags) { return Native.Send(FStringView(Text), Flags); }
EVaRestSendResult UVaRestSocketClient::SendBytes(const TArray<uint8>& Bytes, EVaRestSend Flags) { TArray<uint8> Copy = Bytes; return Native.SendBinary(MoveTemp(Copy), Flags); }
EVaRestSendResult UVaRestSocketClient::SendJson(const FVaRestJson& Json, EVaRestSend Flags) { return Native.Send(Json, Flags); }
EVaRestSendResult UVaRestSocketClient::EmitJson(const FString& Route, const FVaRestJson& Json, EVaRestSend Flags) { return Native.Emit(Route, Json, Flags); }
EVaRestSendResult UVaRestSocketClient::ReplyToMessage(const FVaRestSocketMessage& Message, const FVaRestJson& Json) { return Message.Reply(Json); }
bool UVaRestSocketClient::IsValidSocket() const { return Native.IsValid(); }
EVaRestSocketState UVaRestSocketClient::GetState() const { return Native.GetState(); }
FVaRestSocketStatus UVaRestSocketClient::GetStatus() const { return Native.GetStatus(); }
FVaRestSocketStats UVaRestSocketClient::GetStats() const { return Native.GetStats(); }
bool UVaRestSocketClient::IsConnected() const { return Native.IsConnected(); }
bool UVaRestSocketClient::IsReady() const { return Native.IsReady(); }
double UVaRestSocketClient::GetLatency() const { return Native.GetLatency(); }
double UVaRestSocketClient::GetLatencyAverage() const { return Native.GetLatencyAverage(); }
int64 UVaRestSocketClient::GetBufferedAmount() const { return Native.GetBufferedAmount(); }
int32 UVaRestSocketClient::GetQueuedCount() const { return Native.GetQueuedCount(); }
void UVaRestSocketClient::ClearQueue() { Native.ClearQueue(); }
void UVaRestSocketClient::SetHeader(const FString& Header, const FString& Value) { Native.WithHeader(Header, Value); }
void UVaRestSocketClient::RemoveHeader(const FString& Header) { Native.RemoveHeader(Header); }
void UVaRestSocketClient::SetQueryParam(const FString& Param, const FString& Value) { Native.WithQuery(Param, Value); }
void UVaRestSocketClient::RemoveQueryParam(const FString& Param) { Native.RemoveQuery(Param); }
void UVaRestSocketClient::SetUrl(const FString& Url) { Native.WithUrl(Url); }
void UVaRestSocketClient::SetOrigin(const FString& Origin, bool bDeriveFromUrl) { Native.WithOrigin(bDeriveFromUrl ? TOptional<FString>() : TOptional<FString>(Origin)); }
void UVaRestSocketClient::SetProtocols(const TArray<FString>& Protocols) { Native.WithProtocols(Protocols); }
void UVaRestSocketClient::SetReconnectPolicy(const FVaRestReconnectPolicy& Policy) { Native.WithReconnect(Policy); }
void UVaRestSocketClient::SetHeartbeat(const FVaRestHeartbeat& Heartbeat) { Native.WithHeartbeat(Heartbeat); }
void UVaRestSocketClient::SetProtocolPing(const FVaRestProtocolPing& Ping) { Native.WithProtocolPing(Ping); }
void UVaRestSocketClient::SetLogLevel(EVaRestLogLevel Level) { Native.WithLogLevel(Level); }
void UVaRestSocketClient::SetRouting(const FVaRestSocketRouting& Routing) { Native.WithRouting(Routing); }
void UVaRestSocketClient::SetAuthPlacement(const FVaRestSocketAuthPlacement& Placement) { Native.WithAuthPlacement(Placement); }
void UVaRestSocketClient::SetLimits(const FVaRestSocketLimits& Limits) { Native.WithLimits(Limits); }
void UVaRestSocketClient::SetQueuePolicy(const FVaRestSocketQueuePolicy& Policy) { Native.WithQueue(Policy); }
void UVaRestSocketClient::ReadyWhen(const FString& Route) { Native.ReadyWhen(Route); }

EVaRestSendResult UVaRestSocketClient::SendStruct(const int32&, EVaRestSend) { checkNoEntry(); return EVaRestSendResult::Rejected; }
EVaRestSendResult UVaRestSocketClient::Emit(const FString&, const int32&, EVaRestSend) { checkNoEntry(); return EVaRestSendResult::Rejected; }
DEFINE_FUNCTION(UVaRestSocketClient::execSendStruct)
{
	const auto Value = VaRest::WebSocket::Blueprint::StepWildcard(Stack);
	P_GET_ENUM(EVaRestSend, Flags);
	P_FINISH;
	P_NATIVE_BEGIN;
	*static_cast<EVaRestSendResult*>(RESULT_PARAM) = P_THIS->Native.SendPropertyOnGT(Value.Property, Value.Address, static_cast<EVaRestSend>(Flags));
	P_NATIVE_END;
}
DEFINE_FUNCTION(UVaRestSocketClient::execEmit)
{
	P_GET_PROPERTY(FStrProperty, Route);
	const auto Value = VaRest::WebSocket::Blueprint::StepWildcard(Stack);
	P_GET_ENUM(EVaRestSend, Flags);
	P_FINISH;
	P_NATIVE_BEGIN;
	*static_cast<EVaRestSendResult*>(RESULT_PARAM) = P_THIS->Native.EmitPropertyOnGT(Route, Value.Property, Value.Address, static_cast<EVaRestSend>(Flags));
	P_NATIVE_END;
}
