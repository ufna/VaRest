// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSocketActions.h"
#include "VaRestSocketHub.h"
#include "VaRestSocketLibrary.h"
#include "VaRestSocketTyped.h"
#include "VaRestHub.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "UObject/StrongObjectPtr.h"
#include "Misc/ScopeExit.h"
#include "UObject/Stack.h"
#include "UObject/UnrealType.h"

namespace
{
	FVaRestError ActionError(EVaRestErrorKind Kind, const TCHAR* Message)
	{ FVaRestError Out; Out.Kind = Kind; Out.Message = Message; return Out; }
	struct FSocketWildcard { const FProperty* Property = nullptr; void* Address = nullptr; };
	FSocketWildcard StepSocketPayload(FFrame& Stack)
	{
		const FProperty* Chain = Stack.Code ? nullptr : CastField<FProperty>(Stack.PropertyChainForCompiledIn);
		uint8 Placeholder = 0; Stack.MostRecentProperty = nullptr; Stack.MostRecentPropertyAddress = nullptr;
		Stack.StepCompiledIn<FProperty>(Stack.Code ? nullptr : &Placeholder);
		return {Stack.MostRecentProperty ? Stack.MostRecentProperty : Chain, Stack.MostRecentPropertyAddress};
	}
}
void UVaRestSocketAsyncAction::InitializeContextOnGT(const UObject* Context, UVaRestSocketClient* Socket)
{
	check(IsInGameThread()); ContextOwner = Context; Client = Socket;
}
bool UVaRestSocketAsyncAction::StartScopeOnGT(FVaRestError& Error)
{
	check(IsInGameThread()); bActivated = true;
	if (InitialError.Kind != EVaRestErrorKind::None) { Error = InitialError; return false; }
	auto* Scope = ContextOwner.IsValid() ? UVaRestSocketHub::Get(ContextOwner.Get()) : nullptr;
	auto* World = ContextOwner.IsValid() && GEngine ? GEngine->GetWorldFromContextObject(ContextOwner.Get(), EGetWorldErrorMode::ReturnNull) : nullptr;
	auto* Instance = World ? World->GetGameInstance() : nullptr;
	if (!Scope || !Instance || World->bIsTearingDown || !IsValid(Client) || Client->GetTypedOuter<UVaRestSocketHub>() != Scope)
	{ Error = ActionError(EVaRestErrorKind::Config, TEXT("The action and socket must belong to the same live GameInstance")); return false; }
	if (!Client->IsValidSocket()) { Error = ActionError(EVaRestErrorKind::Canceled, TEXT("The socket has been released")); return false; }
	CoreHub = Scope->GetCoreHubOnGT(); ContextWorld = World;
	if (!CoreHub.IsValid()) { Error = ActionError(EVaRestErrorKind::Canceled, TEXT("The socket Hub is closing")); return false; }
	OutputOptions = VaRest::WebSocket::FSocketTypedAccess::Options(Client->GetNativeSocket());
	if (!AdmitPreparedOutputOnGT(ContextOwner.Get(), nullptr, CoreHub->GetDynamicBudget(), OutputOptions, Error)) return false;
	const TWeakObjectPtr<UVaRestSocketAsyncAction> Weak(this);
	Registration = CoreHub->RetainInFlight(this, [Weak]
	{
		if (auto* Action = Weak.Get()) Action->Stop();
	});
	if (!Registration) { Error = ActionError(EVaRestErrorKind::Config, TEXT("Socket action capacity was exhausted or the Hub closed")); return false; }
	RegisterWithGameInstance(Instance);
	OwnerTick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this, [Weak](float Seconds)
	{ if (auto* Action = Weak.Get()) return Action->TickOwner(Seconds); return false; }));
	return true;
}
bool UVaRestSocketAsyncAction::CanDeliverPreparedOutputOnGT() const
{
	check(IsInGameThread());
	return bActivated && !bFinished && ContextOwner.IsValid() && CoreHub.IsValid() && ContextWorld.IsValid()
		&& !ContextWorld->bIsTearingDown && IsValid(Client) && Client->IsValidSocket();
}
FVaRestSocketMessage UVaRestSocketAsyncAction::GetPreparedMessage(UVaRestPreparedOutputAction* Action, const FString& RequestIdText)
{
	check(IsInGameThread()); const auto* Socket = Cast<UVaRestSocketAsyncAction>(Action);
	return Socket ? Socket->PreparedMessages.FindRef(RequestIdText) : FVaRestSocketMessage();
}
FString UVaRestSocketAsyncAction::PrepareMessageOutputOnGT(const FVaRestSocketMessage& Message, const FVaRestJson& Value, FVaRestError& Error)
{
	check(IsInGameThread()); Error = {}; if (!HasPreparedOutputOnGT()) return {};
	const auto Native = IsValid(Client) ? Client->GetNativeSocket() : FVaRestSocket();
	const auto Pin = VaRest::WebSocket::FSocketTypedAccess::Pin(Native, Message.Channel);
	const auto Weak = Native.AsWeak();
	auto Options = OutputOptions;
	if (!Message.Attachments.IsEmpty()) Options.BytesResolver = VaRest::WebSocket::MakePlaceholderBytesReader(Message.Attachments);
	const FString Cookie = PrepareOutputOnGT(Value, Options, [Weak, Pin]
	{ const auto Current = Weak.Pin(); return Current.IsValid() && VaRest::WebSocket::FSocketTypedAccess::Current(Current, Pin); }, Error);
	if (!Cookie.IsEmpty()) PreparedMessages.Add(Cookie, Message); return Cookie;
}
bool UVaRestSocketAsyncAction::DeliverMessageOutputOnGT(const FString& Cookie)
{
	check(IsInGameThread()); ON_SCOPE_EXIT { PreparedMessages.Remove(Cookie); };
	return DeliverPreparedOutputOnGT(Cookie);
}
bool UVaRestSocketAsyncAction::TickOwner(float)
{
	check(IsInGameThread());
	if (bFinished) return false;
	if (!ContextOwner.IsValid() || !ContextWorld.IsValid() || ContextWorld->bIsTearingDown || !Client || !Client->IsValidSocket())
	{ Stop(); return false; }
	return true;
}
void UVaRestSocketAsyncAction::FinishOnGT()
{
	check(IsInGameThread());
	if (bFinished) return;
	bFinished = true;
	RetirePreparedOutputOnGT(); PreparedMessages.Reset(); OutputOptions = {};
	FTSTicker::GetCoreTicker().RemoveTicker(OwnerTick); OwnerTick.Reset();
	if (Client) Client->GetNativeSocket().OffAll(this);
	const uint64 OldRegistration = Registration; Registration = 0;
	if (auto* Hub = CoreHub.Get(); Hub && OldRegistration) Hub->ReleaseInFlight(OldRegistration);
	CoreHub.Reset(); ContextOwner.Reset(); ContextWorld.Reset(); Client = nullptr;
	SetReadyToDestroy();
}
void UVaRestSocketAsyncAction::Stop()
{
	check(IsInGameThread()); if (bFinished) return;
	const TStrongObjectPtr<UVaRestSocketAsyncAction> Pin(this);
	CanceledOnGT(ActionError(EVaRestErrorKind::Canceled, TEXT("Socket action stopped or its context ended")));
}
void UVaRestSocketAsyncAction::CanceledOnGT(const FVaRestError&) { FinishOnGT(); }
void UVaRestSocketAsyncAction::BeginDestroy() { FinishOnGT(); Super::BeginDestroy(); }

UVaRestConnectSocketAction* UVaRestConnectSocketAction::ConnectSocket(const UObject* WorldContextObject, UVaRestSocketClient* Socket)
{
	check(IsInGameThread()); auto* Action = NewObject<UVaRestConnectSocketAction>(); Action->InitializeContextOnGT(WorldContextObject, Socket); return Action;
}
void UVaRestConnectSocketAction::Activate()
{
	check(IsInGameThread()); if (bActivated || bFinished) return;
	const TStrongObjectPtr<UVaRestConnectSocketAction> Pin(this);
	FVaRestError Error; if (!StartScopeOnGT(Error)) { EndOnGT(Error, {}); return; }
	auto Native = Client->GetNativeSocket(); bOpened = Native.IsConnected();
	const TWeakObjectPtr<UVaRestConnectSocketAction> Weak(this);
	Native.OnConnected(this, [Weak](const FVaRestSocketOpen&)
	{ if (auto* Action = Weak.Get(); Action && !Action->bFinished) Action->bOpened = true; });
	Native.OnReady(this, [Weak](const FVaRestSocketOpen& Open)
	{
		if (auto* Action = Weak.Get(); Action && !Action->bFinished)
		{ const TStrongObjectPtr<UVaRestConnectSocketAction> Kept(Action); Action->bOpened = true; Action->OnReady.Broadcast(Open, Open.bReconnect); }
	});
	Native.OnGaveUp(this, [Weak](const FVaRestError& Failure, const FVaRestSocketClose& Close)
	{ if (auto* Action = Weak.Get(); Action && !Action->bFinished) Action->EndOnGT(Failure, Close); });
	Native.OnDisconnected(this, [Weak](const FVaRestSocketClose& Close)
	{
		if (!Close.bWillReconnect) if (auto* Action = Weak.Get(); Action && !Action->bFinished)
			Action->EndOnGT(ActionError(Close.By == EVaRestSocketCloseBy::Client ? EVaRestErrorKind::Canceled : EVaRestErrorKind::Connection, TEXT("Socket ended before becoming ready")), Close);
	});
	if (Native.IsReady()) { const auto Open = Native.GetLastOpen(); OnReady.Broadcast(Open, Open.bReconnect); }
	else Native.Connect();
}
void UVaRestConnectSocketAction::EndOnGT(const FVaRestError& Error, const FVaRestSocketClose& Close)
{
	if (bFinished) return;
	const TStrongObjectPtr<UVaRestConnectSocketAction> Pin(this);
	const bool bFail = !bOpened; FinishOnGT(); if (bFail) OnFailed.Broadcast(Error, Close);
}
void UVaRestConnectSocketAction::CanceledOnGT(const FVaRestError& Error) { EndOnGT(Error, {}); }

UVaRestListenSocketAction* UVaRestListenSocketAction::Listen(const UObject* WorldContextObject, UVaRestSocketClient* Socket, const FString& Route, const FString& Channel, bool bRequestsOnly)
{
	check(IsInGameThread()); auto* Action = NewObject<UVaRestListenSocketAction>(); Action->InitializeContextOnGT(WorldContextObject, Socket);
	Action->RouteName = Route; Action->ChannelName = Channel; Action->bOnlyRequests = bRequestsOnly; return Action;
}
UVaRestListenSocketAction* UVaRestListenSocketAction::ListenAsset(const UObject* WorldContextObject, UVaRestApi* Api, FName Entry, const FString& Route, const FString& Channel, bool bRequestsOnly)
{
	check(IsInGameThread());
	const TStrongObjectPtr<UVaRestListenSocketAction> Action(Listen(WorldContextObject, nullptr, Route, Channel, bRequestsOnly));
	Action->Client = UVaRestSocketLibrary::GetSocket(WorldContextObject, Api, Entry, Action->InitialError);
	return Action.Get(); // the factory never fires delegates; Activate runs after Blueprint bindings exist
}
void UVaRestListenSocketAction::Activate()
{
	check(IsInGameThread()); if (bActivated || bFinished) return;
	const TStrongObjectPtr<UVaRestListenSocketAction> Pin(this);
	FVaRestError Error; if (!StartScopeOnGT(Error)) { FinishOnGT(); OnError.Broadcast(Error); return; }
	auto Channel = Client->GetNativeSocket().Channel(ChannelName);
	if (!Channel.IsValid() || RouteName.IsEmpty())
	{ FinishOnGT(); OnError.Broadcast(ActionError(EVaRestErrorKind::Config, TEXT("A listener needs a route and an admitted channel"))); return; }
	const TWeakObjectPtr<UVaRestListenSocketAction> Weak(this);
	TFunction<void(const FVaRestSocketMessage&)> Callback = [Weak](const FVaRestSocketMessage& Message)
	{
		if (auto* Action = Weak.Get(); Action && !Action->bFinished)
		{
			const TStrongObjectPtr<UVaRestListenSocketAction> Kept(Action);
			FVaRestError Error; const FString Cookie = Action->PrepareMessageOutputOnGT(Message, Message.Data(), Error);
			if (Error.IsError()) { Action->OnError.Broadcast(Error); return; }
			if (Action->HasPreparedOutputOnGT()) Action->DeliverMessageOutputOnGT(Cookie);
			if (!Action->bFinished) Action->OnMessage.Broadcast(Message);
		}
	};
	if (bOnlyRequests) Channel.OnRequest(RouteName, this, MoveTemp(Callback)); else Channel.On(RouteName, this, MoveTemp(Callback));
}
void UVaRestListenSocketAction::CanceledOnGT(const FVaRestError&) { FinishOnGT(); }

UVaRestSocketRequestAction* UVaRestSocketRequestAction::Request(const UObject* WorldContextObject, UVaRestSocketClient* Socket, const FString& Route, const FVaRestJson& Payload, double Timeout, const FString& Channel)
{
	check(IsInGameThread()); auto* Action = NewObject<UVaRestSocketRequestAction>(); Action->InitializeContextOnGT(WorldContextObject, Socket);
	Action->RouteName = Route; Action->ChannelName = Channel; Action->Body = Payload; Action->TimeoutSeconds = Timeout; return Action;
}
UVaRestSocketRequestAction* UVaRestSocketRequestAction::RequestAsset(const UObject* WorldContextObject, UVaRestApi* Api, FName Entry, const FString& Route, const FVaRestJson& Payload, double Timeout, const FString& Channel)
{
	check(IsInGameThread());
	const TStrongObjectPtr<UVaRestSocketRequestAction> Action(Request(WorldContextObject, nullptr, Route, Payload, Timeout, Channel));
	Action->Client = UVaRestSocketLibrary::GetSocket(WorldContextObject, Api, Entry, Action->InitialError);
	return Action.Get();
}
UVaRestSocketRequestAction* UVaRestSocketRequestAction::RequestValue(const UObject*, UVaRestSocketClient*, const FString&, const int32&, double, const FString&)
{ checkNoEntry(); return nullptr; }
UVaRestSocketRequestAction* UVaRestSocketRequestAction::RequestAssetValue(const UObject*, UVaRestApi*, FName, const FString&, const int32&, double, const FString&)
{ checkNoEntry(); return nullptr; }
UVaRestSocketRequestAction* UVaRestSocketRequestAction::PrepareOnGT(const UObject* Context, UVaRestSocketClient* Socket, const FString& Route,
	const FProperty* Property, const void* Address, double Timeout, const FString& Channel)
{
	check(IsInGameThread()); const TStrongObjectPtr<UVaRestSocketRequestAction> Action(Request(Context, Socket, Route, {}, Timeout, Channel));
	if (!IsValid(Socket) || !FMath::IsFinite(Timeout) || Timeout < 0)
		Action->InitialError = ActionError(EVaRestErrorKind::Config, TEXT("A request needs a live socket and nonnegative finite timeout"));
	else Action->PreparedInput = MakeUnique<FVaRestSocketJsonRequest>(Socket->GetNativeSocket().PreparePropertyRequestOnGT(Route, Channel, Property, Address));
	return Action.Get();
}
DEFINE_FUNCTION(UVaRestSocketRequestAction::execRequestValue)
{
	P_GET_OBJECT(UObject, WorldContextObject); P_GET_OBJECT(UVaRestSocketClient, Socket); P_GET_PROPERTY(FStrProperty, Route);
	const auto Payload = StepSocketPayload(Stack); P_GET_PROPERTY(FDoubleProperty, Timeout); P_GET_PROPERTY(FStrProperty, Channel); P_FINISH;
	P_NATIVE_BEGIN;
	*static_cast<UVaRestSocketRequestAction**>(RESULT_PARAM) = PrepareOnGT(WorldContextObject, Socket, Route, Payload.Property, Payload.Address, Timeout, Channel);
	P_NATIVE_END;
}
DEFINE_FUNCTION(UVaRestSocketRequestAction::execRequestAssetValue)
{
	P_GET_OBJECT(UObject, WorldContextObject); P_GET_OBJECT(UVaRestApi, Api); P_GET_PROPERTY(FNameProperty, Entry); P_GET_PROPERTY(FStrProperty, Route);
	const auto Payload = StepSocketPayload(Stack); P_GET_PROPERTY(FDoubleProperty, Timeout); P_GET_PROPERTY(FStrProperty, Channel); P_FINISH;
	P_NATIVE_BEGIN;
	const TStrongObjectPtr<UStruct> PropertyOwner(Payload.Property ? Payload.Property->GetOwnerStruct() : nullptr);
	FVaRestError Error; const TStrongObjectPtr<UVaRestSocketClient> Socket(UVaRestSocketLibrary::GetSocket(WorldContextObject, Api, Entry, Error));
	auto* Action = PrepareOnGT(WorldContextObject, Socket.Get(), Route, Payload.Property, Payload.Address, Timeout, Channel);
	if (Error.IsError()) Action->InitialError = MoveTemp(Error);
	*static_cast<UVaRestSocketRequestAction**>(RESULT_PARAM) = Action;
	P_NATIVE_END;
}
void UVaRestSocketRequestAction::Activate()
{
	check(IsInGameThread()); if (bActivated || bFinished) return;
	const TStrongObjectPtr<UVaRestSocketRequestAction> Pin(this);
	FVaRestError Error; if (!StartScopeOnGT(Error)) { Body = {}; PreparedInput.Reset(); FinishOnGT(); OnError.Broadcast(Error); return; }
	const TWeakObjectPtr<UVaRestSocketRequestAction> Weak(this);
	auto Builder = PreparedInput ? MoveTemp(*PreparedInput) : Client->GetNativeSocket().Channel(ChannelName).Request(RouteName, Body);
	PreparedInput.Reset(); Body = {};
	const auto PreparedCookie = MakeShared<FString>();
	if (HasPreparedOutputOnGT()) Builder.TransformReply([Weak, PreparedCookie](const FVaRestSocketMessage& Message, FVaRestJson& Reply)
	{
		auto* Action = Weak.Get();
		if (!Action || Action->bFinished) return ActionError(EVaRestErrorKind::Canceled, TEXT("The request output scope closed"));
		const TStrongObjectPtr<UVaRestSocketRequestAction> Kept(Action);
		FVaRestError Failure; *PreparedCookie = Action->PrepareMessageOutputOnGT(Message, Reply, Failure); return Failure;
	});
	Builder.Timeout(TimeoutSeconds).Owner(this).OnSuccess([Weak, PreparedCookie](const FVaRestJson& Reply)
	{
		if (auto* Action = Weak.Get(); Action && !Action->bFinished)
		{
			const TStrongObjectPtr<UVaRestSocketRequestAction> Kept(Action);
			if (Action->HasPreparedOutputOnGT()) Action->DeliverMessageOutputOnGT(*PreparedCookie);
			if (!Action->bFinished) { Action->FinishOnGT(); Action->OnReply.Broadcast(Reply); }
		}
	}).OnError([Weak](const FVaRestError& Failure)
	{
		if (auto* Action = Weak.Get(); Action && !Action->bFinished)
		{ const TStrongObjectPtr<UVaRestSocketRequestAction> Kept(Action); Action->FinishOnGT(); Action->OnError.Broadcast(Failure); }
	}).OnRetired([Weak]
	{
		// A trace callback can replace the connection and suppress public completion. Retire the action silently too.
		if (auto* Action = Weak.Get(); Action && !Action->bFinished) Action->FinishOnGT();
	});
	RequestHandle = Builder.Send();
}
void UVaRestSocketRequestAction::CanceledOnGT(const FVaRestError& Error)
{
	RequestHandle.Cancel(); Body = {}; PreparedInput.Reset(); FinishOnGT(); OnError.Broadcast(Error);
}
void UVaRestSocketRequestAction::BeginDestroy() { PreparedInput.Reset(); RequestHandle.Cancel(); Super::BeginDestroy(); }
