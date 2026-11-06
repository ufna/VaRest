// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSioLibrary.h"
#include "VaRestSioBlueprint.h"
#include "VaRestSocketHub.h"
#include "UObject/StrongObjectPtr.h"

UVaRestSioClient* UVaRestSioLibrary::GetSocketIO(const UObject* WorldContextObject, UVaRestApi* Api, FName Entry, FVaRestError& Error)
{
	check(IsInGameThread()); auto* Hub = UVaRestSocketHub::Get(WorldContextObject);
	auto Native = FVaRestSio::Socket(Hub, Api, Entry, Error);
	auto* View = UVaRestSioClient::WrapOnGT(Hub, MoveTemp(Native));
	if (!View && Error.Kind == EVaRestErrorKind::None) { Error.Kind = EVaRestErrorKind::Config; Error.Message = TEXT("Socket.IO view capacity was exhausted or the scope ended"); }
	return View;
}
UVaRestSioClient* UVaRestSioLibrary::MakeSocketIO(const UObject* WorldContextObject, const FString& Url, const FString& Namespace,
	const TArray<FVaRestHeader>& Headers, const FVaRestJson& Auth, FVaRestError& Error, FName Name)
{
	check(IsInGameThread()); Error = {}; auto* Hub = UVaRestSocketHub::Get(WorldContextObject);
	if (!Hub || (!Namespace.IsEmpty() && !FVaRestSioCodec::IsValidNamespace(Namespace)) || (Auth.IsValid() && !Auth.IsObject()))
	{ Error.Kind = EVaRestErrorKind::Config; Error.Message = TEXT("Socket.IO requires a game instance, valid namespace and object authentication payload"); return nullptr; }
	if (!Name.IsNone())
	{
		auto Existing = FVaRestSio::Find(Hub, Name); if (Existing.IsValid()) return UVaRestSioClient::WrapOnGT(Hub, MoveTemp(Existing));
	}
	auto Socket = FVaRestSio::Make(Hub, Name, Url, Namespace);
	if (!Socket.IsValid()) { Error.Kind = EVaRestErrorKind::Config; Error.Message = TEXT("Socket.IO creation was refused; check its scope, path and protocol identity"); return nullptr; }
	for (const auto& Header : Headers) Socket.WithHeader(Header.Name, Header.Value);
	if (Auth.IsValid()) Socket.WithAuth(Auth);
	auto* View = UVaRestSioClient::WrapOnGT(Hub, MoveTemp(Socket));
	if (!View) { Error.Kind = EVaRestErrorKind::Config; Error.Message = TEXT("Socket.IO view could not be admitted"); }
	return View;
}
EVaRestSendResult UVaRestSioLibrary::AckReply(const FVaRestSioAck&, const int32&, EVaRestSioArgs) { checkNoEntry(); return EVaRestSendResult::Rejected; }
EVaRestSendResult UVaRestSioLibrary::EmitAssetEvent(const UObject*, UVaRestApi*, FName, const FString&, const int32&, FVaRestError&, const FString&, bool, EVaRestSioArgs, UVaRestSioClient*)
{ checkNoEntry(); return EVaRestSendResult::Rejected; }
DEFINE_FUNCTION(UVaRestSioLibrary::execEmitAssetEvent)
{
	P_GET_OBJECT(UObject, WorldContextObject);
	P_GET_OBJECT(UVaRestApi, Api);
	P_GET_PROPERTY(FNameProperty, Entry);
	P_GET_PROPERTY(FStrProperty, Event);
	const auto Payload = VaRest::Sio::Blueprint::StepWildcard(Stack);
	P_GET_STRUCT_REF(FVaRestError, Error);
	P_GET_PROPERTY(FStrProperty, Namespace);
	P_GET_UBOOL(Volatile);
	P_GET_ENUM(EVaRestSioArgs, ArgsMode);
	P_GET_OBJECT(UVaRestSioClient, Socket);
	P_FINISH;
	P_NATIVE_BEGIN;
	check(IsInGameThread()); Error = {};
	const TStrongObjectPtr<UVaRestSioClient> Client(IsValid(Socket) ? Socket : GetSocketIO(WorldContextObject, Api, Entry, Error));
	const auto Result = Client.Get() ? Client->EmitPropertyOnGT(Event, Namespace, Payload.Property, Payload.Address, static_cast<EVaRestSioArgs>(ArgsMode), Volatile) : EVaRestSendResult::Rejected;
	*static_cast<EVaRestSendResult*>(RESULT_PARAM) = Result;
	P_NATIVE_END;
}
EVaRestSendResult UVaRestSioLibrary::AckReplyArgs(const FVaRestSioAck& Ack, const FVaRestJson& Args) { return Ack.SendArray(Args); }
bool UVaRestSioLibrary::IsAckValid(const FVaRestSioAck& Ack) { return Ack.CanReplyOnGT(); }
int32 UVaRestSioLibrary::GetEventArgCount(const FVaRestSioEvent& Event) { return Event.Args.IsArray() ? Event.Args.Num() : 0; }
TArray<uint8> UVaRestSioLibrary::GetEventAttachment(const FVaRestSioEvent& Event, int32 Num) { return TArray<uint8>(Event.Attachment(Num)); }
FString UVaRestSioLibrary::DescribeDisconnect(const FVaRestSioDisconnect& Disconnect)
{
	const auto* Enum = StaticEnum<EVaRestSioDisconnectReason>();
	const FString Reason = Enum ? Enum->GetNameStringByValue(static_cast<int64>(Disconnect.Reason)) : TEXT("TransportClose");
	return FString::Printf(TEXT("%s: %s%s%s"), *Disconnect.Namespace, *Reason, Disconnect.ReasonText.IsEmpty() ? TEXT("") : TEXT(": "), *Disconnect.ReasonText);
}
bool UVaRestSioLibrary::EventArgToStruct(const FVaRestSioEvent&, int32, int32&, EVaRestJsonNaming, bool) { checkNoEntry(); return false; }
DEFINE_FUNCTION(UVaRestSioLibrary::execAckReply)
{
	P_GET_STRUCT_REF(FVaRestSioAck, Ack);
	const auto Payload = VaRest::Sio::Blueprint::StepWildcard(Stack);
	P_GET_ENUM(EVaRestSioArgs, ArgsMode);
	P_FINISH;
	P_NATIVE_BEGIN;
	check(IsInGameThread()); const TStrongObjectPtr<UStruct> TypePin(Payload.Property ? Payload.Property->GetOwnerStruct() : nullptr);
	auto Write = [&](auto& Writer, const FVaRestJsonConvertOptions& Options, FVaRestJsonError& Error, TFunctionRef<bool()> Budget)
	{ return VaRest::Sio::Blueprint::WritePayload(Writer, Options, Error, Budget, Payload.Property, Payload.Address, static_cast<EVaRestSioArgs>(ArgsMode)); };
	*static_cast<EVaRestSendResult*>(RESULT_PARAM) = Ack.SendArguments(Write);
	P_NATIVE_END;
}
DEFINE_FUNCTION(UVaRestSioLibrary::execEventArgToStruct)
{
	P_GET_STRUCT_REF(FVaRestSioEvent, Event);
	P_GET_PROPERTY(FIntProperty, Index);
	const auto Value = VaRest::Sio::Blueprint::StepWildcard(Stack);
	P_GET_ENUM(EVaRestJsonNaming, Naming);
	P_GET_UBOOL(Strict);
	P_FINISH;
	P_NATIVE_BEGIN;
	check(IsInGameThread()); const TStrongObjectPtr<UStruct> TypePin(Value.Property ? Value.Property->GetOwnerStruct() : nullptr);
	FVaRestJsonConvertOptions Options; Options.Naming = static_cast<EVaRestJsonNaming>(Naming); Options.bStrict = Strict;
	Options.BytesResolver = VaRest::WebSocket::MakePlaceholderBytesReader(Event.GetNativeMessage().Attachments);
	const auto Argument = Event.Args.At(Index);
	*static_cast<bool*>(RESULT_PARAM) = Argument.IsValid() && Value.Property && Value.Address && FVaRestJsonConvert::JsonToProperty(Argument, Value.Property, Value.Address, Options);
	P_NATIVE_END;
}
