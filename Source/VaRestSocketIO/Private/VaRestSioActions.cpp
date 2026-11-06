// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSioActions.h"
#include "VaRestSioLibrary.h"
#include "VaRestSioBlueprint.h"
#include "UObject/StrongObjectPtr.h"
#include "Misc/ScopeExit.h"

namespace
{
	FVaRestError SioActionError(EVaRestErrorKind Kind, const TCHAR* Message)
	{ FVaRestError Error; Error.Kind = Kind; Error.Message = Message; return Error; }
}
void UVaRestSioAsyncAction::FinishSioOnGT()
{
	check(IsInGameThread()); if (auto* View = SioClient()) View->GetNativeSioSocket().OffAll(this);
	PreparedEvents.Reset();
	FinishOnGT();
}
FVaRestSioEvent UVaRestSioAsyncAction::GetPreparedEvent(UVaRestPreparedOutputAction* Action, const FString& RequestIdText)
{
	check(IsInGameThread()); const auto* Sio = Cast<UVaRestSioAsyncAction>(Action);
	return Sio ? Sio->PreparedEvents.FindRef(RequestIdText) : FVaRestSioEvent();
}
FString UVaRestSioAsyncAction::PrepareEventOutputOnGT(const FVaRestSioEvent& Event, EVaRestSioArgs ArgsMode, FVaRestError& Error)
{
	check(IsInGameThread()); Error = {}; if (!HasPreparedOutputOnGT()) return {};
	const auto Value = ArgsMode == EVaRestSioArgs::First ? Event.Args.At(0) : Event.Args;
	const FString Cookie = PrepareMessageOutputOnGT(Event.GetNativeMessage(), Value, Error);
	if (!Cookie.IsEmpty()) PreparedEvents.Add(Cookie, Event); return Cookie;
}
bool UVaRestSioAsyncAction::DeliverEventOutputOnGT(const FString& Cookie)
{
	check(IsInGameThread()); ON_SCOPE_EXIT { PreparedEvents.Remove(Cookie); };
	return DeliverMessageOutputOnGT(Cookie);
}
void UVaRestSioAsyncAction::CanceledOnGT(const FVaRestError&) { FinishSioOnGT(); }
void UVaRestSioAsyncAction::BeginDestroy() { FinishSioOnGT(); Super::BeginDestroy(); }

UVaRestConnectSioAction* UVaRestConnectSioAction::ConnectSocketIO(const UObject* Context, UVaRestSioClient* Socket)
{
	check(IsInGameThread()); auto* Action = NewObject<UVaRestConnectSioAction>(); Action->InitializeContextOnGT(Context, Socket); return Action;
}
UVaRestConnectSioAction* UVaRestConnectSioAction::ConnectAsset(const UObject* Context, UVaRestApi* Api, FName Entry)
{
	const TStrongObjectPtr<UVaRestConnectSioAction> Action(ConnectSocketIO(Context, nullptr));
	Action->Client = UVaRestSioLibrary::GetSocketIO(Context, Api, Entry, Action->InitialError); return Action.Get();
}
void UVaRestConnectSioAction::Activate()
{
	check(IsInGameThread()); if (bActivated || bFinished) return;
	const TStrongObjectPtr<UVaRestConnectSioAction> Pin(this);
	FVaRestError Error; if (!StartScopeOnGT(Error)) { EndOnGT(MoveTemp(Error)); return; }
	auto* View = SioClient(); if (!View) { EndOnGT(SioActionError(EVaRestErrorKind::Config, TEXT("A Socket.IO client is required"))); return; }
	auto Native = View->GetNativeSioSocket(); const TWeakObjectPtr<UVaRestConnectSioAction> Weak(this);
	Native.OnConnect(this, [Weak](const FVaRestSioConnect& Info)
	{
		if (auto* Action = Weak.Get(); Action && !Action->bFinished)
		{ const TStrongObjectPtr<UVaRestConnectSioAction> Pin(Action); Action->bConnected = true; Action->OnConnect.Broadcast(Info); }
	});
	Native.OnConnectError(this, [Weak](const FVaRestError& Error, const FVaRestSioConnectError& Info)
	{ if (!Info.bWillRetry) if (auto* Action = Weak.Get(); Action && !Action->bFinished) Action->EndOnGT(Error, Info); });
	Native.OnGaveUp(this, [Weak](const FVaRestError& Error, const FVaRestSocketClose&)
	{ if (auto* Action = Weak.Get(); Action && !Action->bFinished) Action->EndOnGT(Error); });
	View->GetNativeSocket().OnDisconnected(this, [Weak](const FVaRestSocketClose& Close)
	{
		if (!Close.bWillReconnect) if (auto* Action = Weak.Get(); Action && !Action->bFinished)
			Action->EndOnGT(SioActionError(Close.By == EVaRestSocketCloseBy::Client ? EVaRestErrorKind::Canceled : EVaRestErrorKind::Connection, TEXT("Socket.IO ended before connecting its default namespace")));
	});
	auto Namespace = Native.DefaultNamespace();
	if (Namespace.IsConnected())
	{
		bConnected = true; const auto Info = View->GetLastConnectInfoOnGT();
		OnConnect.Broadcast(Info);
	}
	if (!bFinished) Native.Connect();
}
void UVaRestConnectSioAction::EndOnGT(FVaRestError Error, FVaRestSioConnectError Info)
{
	if (bFinished) return; const TStrongObjectPtr<UVaRestConnectSioAction> Pin(this);
	if (Info.Namespace.IsEmpty()) if (auto* View = SioClient()) Info.Namespace = FString(View->GetNativeSioSocket().DefaultNamespace().GetName());
	if (Info.Message.IsEmpty()) Info.Message = Error.Message; const bool bFailed = !bConnected;
	FinishSioOnGT(); if (bFailed) OnFailed.Broadcast(Error, Info);
}
void UVaRestConnectSioAction::CanceledOnGT(const FVaRestError& Error) { EndOnGT(Error); }

UVaRestListenSioAction* UVaRestListenSioAction::Listen(const UObject* Context, UVaRestSioClient* Socket, const FString& Event, const FString& Namespace, bool bOnce, EVaRestSioArgs ArgsMode)
{
	check(IsInGameThread()); auto* Action = NewObject<UVaRestListenSioAction>(); Action->InitializeContextOnGT(Context, Socket);
	Action->EventName = Event; Action->NamespaceName = Namespace; Action->bSingle = bOnce; Action->Mode = ArgsMode; return Action;
}
UVaRestListenSioAction* UVaRestListenSioAction::ListenAsset(const UObject* Context, UVaRestApi* Api, FName Entry, const FString& Event, const FString& Namespace, bool bOnce, EVaRestSioArgs ArgsMode, UVaRestSioClient* Socket)
{
	const TStrongObjectPtr<UVaRestListenSioAction> Action(Listen(Context, Socket, Event, Namespace, bOnce, ArgsMode));
	if (!IsValid(Socket)) Action->Client = UVaRestSioLibrary::GetSocketIO(Context, Api, Entry, Action->InitialError); return Action.Get();
}
void UVaRestListenSioAction::Activate()
{
	check(IsInGameThread()); if (bActivated || bFinished) return;
	const TStrongObjectPtr<UVaRestListenSioAction> Pin(this);
	SetPositionalOutputOnGT(Mode == EVaRestSioArgs::Positional);
	if (static_cast<uint8>(Mode) > static_cast<uint8>(EVaRestSioArgs::Array) || (HasPreparedOutputOnGT() && Mode == EVaRestSioArgs::Array))
		InitialError = SioActionError(EVaRestErrorKind::Config, TEXT("Array argument mode requires a native JSON output"));
	FVaRestError Error; if (!StartScopeOnGT(Error)) { FinishSioOnGT(); OnError.Broadcast(Error); return; }
	const TWeakObjectPtr<UVaRestListenSioAction> Weak(this); auto* View = SioClient();
	if (!View || !View->ListenOnGT(NamespaceName, EventName, this, [Weak](const FVaRestSioEvent& Event)
	{
		if (auto* Action = Weak.Get(); Action && !Action->bFinished)
		{
			const TStrongObjectPtr<UVaRestListenSioAction> Pin(Action);
			FVaRestError Failure; const FString Cookie = Action->PrepareEventOutputOnGT(Event, Action->Mode, Failure);
			if (Failure.IsError()) { if (Action->bSingle) Action->FinishSioOnGT(); Action->OnError.Broadcast(Failure); return; }
			const auto Payload = Action->Mode == EVaRestSioArgs::First ? Event.Args.At(0) : Event.Args;
			if (Action->HasPreparedOutputOnGT()) Action->DeliverEventOutputOnGT(Cookie);
			if (!Action->bFinished)
			{ if (Action->bSingle) Action->FinishSioOnGT(); Action->OnEvent.Broadcast(Payload, Event, Event.Ack); }
		}
	}, bSingle))
	{ FinishSioOnGT(); OnError.Broadcast(SioActionError(EVaRestErrorKind::Config, TEXT("Socket.IO listener needs a valid event, namespace and available observer slot"))); }
}

UVaRestEmitSioAckAction* UVaRestEmitSioAckAction::EmitWithAck(const UObject*, UVaRestSioClient*, const FString&, const int32&, const FString&, double, bool, EVaRestSioArgs, EVaRestSioArgs)
{ checkNoEntry(); return nullptr; }
UVaRestEmitSioAckAction* UVaRestEmitSioAckAction::EmitAssetWithAck(const UObject*, UVaRestApi*, FName, const FString&, const int32&, const FString&, double, bool, EVaRestSioArgs, EVaRestSioArgs, UVaRestSioClient*)
{ checkNoEntry(); return nullptr; }
UVaRestEmitSioAckAction* UVaRestEmitSioAckAction::PrepareOnGT(const UObject* Context, UVaRestSioClient* Socket, FStringView Event, FStringView Namespace,
	const FProperty* Property, const void* Address, double Timeout, bool bErrorFirst, EVaRestSioArgs ArgsMode, EVaRestSioArgs ReplyArgsMode)
{
	check(IsInGameThread()); const TStrongObjectPtr<UVaRestEmitSioAckAction> Action(NewObject<UVaRestEmitSioAckAction>());
	Action->InitializeContextOnGT(Context, Socket); Action->ReplyMode = ReplyArgsMode;
	if (!IsValid(Socket) || !FMath::IsFinite(Timeout) || Timeout < 0 || static_cast<uint8>(ReplyArgsMode) > static_cast<uint8>(EVaRestSioArgs::Array))
		Action->InitialError = SioActionError(EVaRestErrorKind::Config, TEXT("Emit With Ack requires a Socket.IO client, valid argument mode and nonnegative finite timeout"));
	else
	{
		Action->Prepared = MakeUnique<FVaRestSioEmitWithAck>(Socket->PrepareAckOnGT(Event, Namespace, Property, Address, ArgsMode));
		Action->Prepared->Timeout(Timeout).ErrorFirst(bErrorFirst);
	}
	return Action.Get();
}
DEFINE_FUNCTION(UVaRestEmitSioAckAction::execEmitWithAck)
{
	P_GET_OBJECT(UObject, WorldContextObject);
	P_GET_OBJECT(UVaRestSioClient, Socket);
	P_GET_PROPERTY(FStrProperty, Event);
	const auto Payload = VaRest::Sio::Blueprint::StepWildcard(Stack);
	P_GET_PROPERTY(FStrProperty, Namespace);
	P_GET_PROPERTY(FDoubleProperty, Timeout);
	P_GET_UBOOL(ErrorFirst);
	P_GET_ENUM(EVaRestSioArgs, ArgsMode);
	P_GET_ENUM(EVaRestSioArgs, ReplyArgsMode);
	P_FINISH;
	P_NATIVE_BEGIN;
	*static_cast<UVaRestEmitSioAckAction**>(RESULT_PARAM) = PrepareOnGT(WorldContextObject, Socket, Event, Namespace, Payload.Property, Payload.Address, Timeout, ErrorFirst, static_cast<EVaRestSioArgs>(ArgsMode), static_cast<EVaRestSioArgs>(ReplyArgsMode));
	P_NATIVE_END;
}
DEFINE_FUNCTION(UVaRestEmitSioAckAction::execEmitAssetWithAck)
{
	P_GET_OBJECT(UObject, WorldContextObject);
	P_GET_OBJECT(UVaRestApi, Api);
	P_GET_PROPERTY(FNameProperty, Entry);
	P_GET_PROPERTY(FStrProperty, Event);
	const auto Payload = VaRest::Sio::Blueprint::StepWildcard(Stack);
	P_GET_PROPERTY(FStrProperty, Namespace);
	P_GET_PROPERTY(FDoubleProperty, Timeout);
	P_GET_UBOOL(ErrorFirst);
	P_GET_ENUM(EVaRestSioArgs, ArgsMode);
	P_GET_ENUM(EVaRestSioArgs, ReplyArgsMode);
	P_GET_OBJECT(UVaRestSioClient, Socket);
	P_FINISH;
	P_NATIVE_BEGIN;
	FVaRestError Error; const TStrongObjectPtr<UVaRestSioClient> Client(IsValid(Socket) ? Socket : UVaRestSioLibrary::GetSocketIO(WorldContextObject, Api, Entry, Error));
	auto* Action = PrepareOnGT(WorldContextObject, Client.Get(), Event, Namespace, Payload.Property, Payload.Address, Timeout, ErrorFirst, static_cast<EVaRestSioArgs>(ArgsMode), static_cast<EVaRestSioArgs>(ReplyArgsMode));
	if (Error.Kind != EVaRestErrorKind::None) Action->InitialError = MoveTemp(Error);
	*static_cast<UVaRestEmitSioAckAction**>(RESULT_PARAM) = Action;
	P_NATIVE_END;
}
void UVaRestEmitSioAckAction::Activate()
{
	check(IsInGameThread()); if (bActivated || bFinished) return;
	const TStrongObjectPtr<UVaRestEmitSioAckAction> Pin(this);
	SetPositionalOutputOnGT(ReplyMode == EVaRestSioArgs::Positional);
	if (HasPreparedOutputOnGT() && ReplyMode == EVaRestSioArgs::Array)
		InitialError = SioActionError(EVaRestErrorKind::Config, TEXT("Array reply mode requires a native JSON output"));
	FVaRestError Error; if (!StartScopeOnGT(Error) || !Prepared)
	{
		if (Prepared) { Prepared->Discard(); Prepared.Reset(); }
		if (Error.Kind == EVaRestErrorKind::None) Error = SioActionError(EVaRestErrorKind::Config, TEXT("Emit With Ack has no prepared packet"));
		FinishSioOnGT(); OnError.Broadcast(Error); return;
	}
	const TWeakObjectPtr<UVaRestEmitSioAckAction> Weak(this);
	auto Sending = MoveTemp(Prepared); // a synchronous failure may finish the action while Send is still on the stack
	const auto PreparedCookie = MakeShared<FString>();
	if (HasPreparedOutputOnGT()) Sending->TransformReply([Weak, PreparedCookie](const FVaRestSioEvent& Reply)
	{
		auto* Action = Weak.Get();
		if (!Action || Action->bFinished) return SioActionError(EVaRestErrorKind::Canceled, TEXT("The ack output scope closed"));
		const TStrongObjectPtr<UVaRestEmitSioAckAction> Pin(Action);
		FVaRestError Failure; *PreparedCookie = Action->PrepareEventOutputOnGT(Reply, Action->ReplyMode, Failure); return Failure;
	});
	Sending->Owner(this).OnSuccess(TFunction<void(const FVaRestSioEvent&)>([Weak, PreparedCookie](const FVaRestSioEvent& Reply)
	{
		if (auto* Action = Weak.Get(); Action && !Action->bFinished)
		{
			const TStrongObjectPtr<UVaRestEmitSioAckAction> Pin(Action);
			const auto Payload = Action->ReplyMode == EVaRestSioArgs::First ? Reply.Args.At(0) : Reply.Args;
			if (Action->HasPreparedOutputOnGT()) Action->DeliverEventOutputOnGT(*PreparedCookie);
			if (!Action->bFinished) { Action->FinishSioOnGT(); Action->OnAck.Broadcast(Payload, Reply.Args, Reply); }
		}
	})).OnError([Weak](const FVaRestError& Error)
	{
		if (auto* Action = Weak.Get(); Action && !Action->bFinished)
		{ const TStrongObjectPtr<UVaRestEmitSioAckAction> Pin(Action); Action->FinishSioOnGT(); Action->OnError.Broadcast(Error); }
	}).OnRetired([Weak]
	{
		if (auto* Action = Weak.Get(); Action && !Action->bFinished) Action->FinishSioOnGT();
	});
	Handle = Sending->Send(); if (bFinished) Handle.Cancel();
}
void UVaRestEmitSioAckAction::CanceledOnGT(const FVaRestError& Error)
{
	if (Prepared) { Prepared->Discard(); Prepared.Reset(); }
	FinishSioOnGT(); Handle.Cancel(); OnError.Broadcast(Error);
}
void UVaRestEmitSioAckAction::BeginDestroy()
{
	if (Prepared) { Prepared->Discard(); Prepared.Reset(); }
	Handle.Cancel(); Super::BeginDestroy();
}
