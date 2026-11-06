// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSocketLibrary.h"
#include "VaRestSocketHub.h"
#include "VaRestSocketClient.h"
#include "VaRestSocketBlueprint.h"
#include "VaRestApi.h"
#include "UObject/StrongObjectPtr.h"

UVaRestSocketClient* UVaRestSocketLibrary::GetSocket(const UObject* WorldContextObject, UVaRestApi* Api, FName Entry, FVaRestError& Error)
{
	check(IsInGameThread());
	auto* Hub = UVaRestSocketHub::Get(WorldContextObject);
	if (Hub) return Hub->GetSocket(Api, Entry, Error);
	Error = {}; Error.Kind = EVaRestErrorKind::Config; Error.Message = TEXT("A socket requires a real GameInstance context"); return nullptr;
}
UVaRestSocketClient* UVaRestSocketLibrary::MakeSocket(const UObject* WorldContextObject, const FString& Url, const TArray<FString>& Protocols, const TArray<FVaRestHeader>& Headers, FVaRestError& Error, FName Name)
{
	check(IsInGameThread()); Error = {};
	auto* Hub = UVaRestSocketHub::Get(WorldContextObject);
	if (!Hub) { Error.Kind = EVaRestErrorKind::Config; Error.Message = TEXT("A socket requires a real GameInstance context"); return nullptr; }
	if (!Name.IsNone()) { auto Existing = Hub->Find(Name); if (Existing.IsValid()) return Hub->Wrap(MoveTemp(Existing)); }
	auto Socket = Hub->Make(Name, Url);
	if (!Socket.IsValid()) { Error.Kind = EVaRestErrorKind::Config; Error.Message = TEXT("Socket creation was refused; check its scope and protocol accessor"); return nullptr; }
	Socket.WithProtocols(Protocols); for (const auto& Header : Headers) Socket.WithHeader(Header.Name, Header.Value);
	return Hub->Wrap(MoveTemp(Socket));
}
FString UVaRestSocketLibrary::DescribeSocketClose(const FVaRestSocketClose& Close)
{
	return FString::Printf(TEXT("%d%s%s%s"), Close.Code, Close.bWasClean ? TEXT(" (clean)") : TEXT(""), Close.Reason.IsEmpty() ? TEXT("") : TEXT(": "), *Close.Reason);
}
FVaRestJson UVaRestSocketLibrary::SocketMessageJson(const FVaRestSocketMessage& Message) { return Message.Json(); }
FVaRestJson UVaRestSocketLibrary::SocketMessageData(const FVaRestSocketMessage& Message) { return Message.Data(); }
FString UVaRestSocketLibrary::SocketMessageText(const FVaRestSocketMessage& Message) { return Message.Text(); }
TArray<uint8> UVaRestSocketLibrary::SocketMessageBytes(const FVaRestSocketMessage& Message) { return TArray<uint8>(Message.Bytes()); }
EVaRestSendResult UVaRestSocketLibrary::ReplySocketError(const FVaRestSocketMessage& Message, const FString& Code, const FString& Reason) { return Message.ReplyError(Code, Reason); }
FVaRestHeartbeat UVaRestSocketLibrary::TextHeartbeat(const FString& Ping, const FString& Pong) { return FVaRestHeartbeat::Text(Ping, Pong); }
FVaRestHeartbeat UVaRestSocketLibrary::JsonHeartbeat(const FString& PingRoute, const FString& PongRoute) { return FVaRestHeartbeat::Json(PingRoute, PongRoute); }
FVaRestHeartbeat UVaRestSocketLibrary::ServerHeartbeat(const FString& Ping, const FString& Pong, bool bJson) { return FVaRestHeartbeat::ServerPing(Ping, Pong, bJson); }
FVaRestSocketRouting UVaRestSocketLibrary::RoutingByField(const FString& Field) { return FVaRestSocketRouting::ByField(Field); }
FVaRestSocketRouting UVaRestSocketLibrary::JsonRpcRouting() { return FVaRestSocketRouting::JsonRpc(); }
bool UVaRestSocketLibrary::SocketMessageToStruct(const FVaRestSocketMessage&, int32&, EVaRestJsonNaming, bool) { checkNoEntry(); return false; }
EVaRestSendResult UVaRestSocketLibrary::EmitAssetMessage(const UObject*, UVaRestApi*, FName, const FString&, const int32&, FVaRestError&, EVaRestSend) { checkNoEntry(); return EVaRestSendResult::Rejected; }
DEFINE_FUNCTION(UVaRestSocketLibrary::execEmitAssetMessage)
{
	P_GET_OBJECT(UObject, WorldContextObject);
	P_GET_OBJECT(UVaRestApi, Api);
	P_GET_PROPERTY(FNameProperty, Entry);
	P_GET_PROPERTY(FStrProperty, Route);
	const auto Value = VaRest::WebSocket::Blueprint::StepWildcard(Stack);
	P_GET_STRUCT_REF(FVaRestError, Error);
	P_GET_ENUM(EVaRestSend, Flags);
	P_FINISH;
	P_NATIVE_BEGIN;
	check(IsInGameThread());
	const TStrongObjectPtr<UVaRestSocketClient> Client(GetSocket(WorldContextObject, Api, Entry, Error));
	*static_cast<EVaRestSendResult*>(RESULT_PARAM) = Client.Get()
		? Client->GetNativeSocket().EmitPropertyOnGT(Route, Value.Property, Value.Address, static_cast<EVaRestSend>(Flags)) : EVaRestSendResult::Rejected;
	P_NATIVE_END;
}
EVaRestSendResult UVaRestSocketLibrary::ReplyToSocketMessage(const FVaRestSocketMessage&, const int32&) { checkNoEntry(); return EVaRestSendResult::Rejected; }
DEFINE_FUNCTION(UVaRestSocketLibrary::execSocketMessageToStruct)
{
	P_GET_STRUCT_REF(FVaRestSocketMessage, Message);
	const auto Value = VaRest::WebSocket::Blueprint::StepWildcard(Stack);
	P_GET_ENUM(EVaRestJsonNaming, Naming);
	P_GET_UBOOL(Strict);
	P_FINISH;
	P_NATIVE_BEGIN;
	check(IsInGameThread());
	FVaRestJsonConvertOptions Options; Options.Naming = static_cast<EVaRestJsonNaming>(Naming); Options.bStrict = Strict;
	const TStrongObjectPtr<UStruct> TypePin(Value.Property ? Value.Property->GetOwnerStruct() : nullptr);
	*static_cast<bool*>(RESULT_PARAM) = Value.Property && Value.Address && FVaRestJsonConvert::JsonToProperty(Message.Data(), Value.Property, Value.Address, Options);
	P_NATIVE_END;
}
DEFINE_FUNCTION(UVaRestSocketLibrary::execReplyToSocketMessage)
{
	P_GET_STRUCT_REF(FVaRestSocketMessage, Message);
	const auto Value = VaRest::WebSocket::Blueprint::StepWildcard(Stack);
	P_FINISH;
	P_NATIVE_BEGIN;
	check(IsInGameThread());
	*static_cast<EVaRestSendResult*>(RESULT_PARAM) = Message.ReplyPropertyOnGT(Value.Property, Value.Address);
	P_NATIVE_END;
}
