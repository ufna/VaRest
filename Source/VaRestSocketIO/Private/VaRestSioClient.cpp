// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSioClient.h"
#include "VaRestSioState.h"
#include "VaRestSioBlueprint.h"
#include "VaRestSocketHub.h"
#include "UObject/StrongObjectPtr.h"

UVaRestSioClient* UVaRestSioClient::WrapOnGT(UVaRestSocketHub* Hub, FVaRestSioSocket Native)
{
	check(IsInGameThread()); if (!IsValid(Hub) || !Hub->GetCoreHubOnGT()) return nullptr;
	auto* State = Native.StateOnGT(); if (!State || State->bRetired) return nullptr;
	if (auto* Existing = State->BlueprintClient.Get()) return Existing;
	const TStrongObjectPtr<UVaRestSioState> Pin(State);
	auto* View = NewObject<UVaRestSioClient>(Hub);
	if (!View->InitializeSioOnGT(MoveTemp(Native))) return nullptr;
	State->BlueprintClient = View; return View;
}
bool UVaRestSioClient::InitializeSioOnGT(FVaRestSioSocket Native)
{
	check(IsInGameThread()); Socket = MoveTemp(Native); auto* State = Socket.StateOnGT(); if (!State) return false;
	InitializeOnGT(Socket.Native, false); const TWeakObjectPtr<UVaRestSioClient> Weak(this);
	const bool bConnect = State->Add<void(const FVaRestSioConnect&)>(State->AnyConnect, this, [Weak](const FVaRestSioConnect& Info)
	{ if (auto* View = Weak.Get()) { const TStrongObjectPtr<UVaRestSioClient> Pin(View); View->OnConnect.Broadcast(Info); } });
	const bool bError = State->Add<void(const FVaRestError&, const FVaRestSioConnectError&)>(State->AnyConnectError, this, [Weak](const FVaRestError& Error, const FVaRestSioConnectError& Info)
	{ if (auto* View = Weak.Get()) { const TStrongObjectPtr<UVaRestSioClient> Pin(View); View->OnConnectError.Broadcast(Error, Info); } });
	const bool bDisconnect = State->Add<void(const FVaRestSioDisconnect&)>(State->AnyDisconnect, this, [Weak](const FVaRestSioDisconnect& Info)
	{ if (auto* View = Weak.Get()) { const TStrongObjectPtr<UVaRestSioClient> Pin(View); View->OnDisconnect.Broadcast(Info); } });
	const bool bEvent = State->Add<void(const FVaRestSocketMessage&)>(State->AnyEvent, this, [Weak](const FVaRestSocketMessage& Message)
	{
		if (auto* View = Weak.Get())
		{
			const TStrongObjectPtr<UVaRestSioClient> Pin(View); auto* State = View->Socket.StateOnGT(); if (!State) return;
			const auto Stage = State->Stage.Pin(); if (!Stage) return;
			const auto Event = FVaRestSioEvent::FromMessage(Message, Stage->RecoveryOffsetFor(Message)); View->OnEvent.Broadcast(Event);
		}
	});
	if (!bConnect || !bError || !bDisconnect || !bEvent) { Socket.OffAll(this); Socket = {}; return false; }
	return true;
}
void UVaRestSioClient::BeginDestroy() { Socket.OffAll(this); Socket = {}; Super::BeginDestroy(); }
void UVaRestSioClient::Connect() { Socket.Connect(); }
FVaRestSioConnect UVaRestSioClient::GetLastConnectInfoOnGT(FStringView Namespace) const
{
	check(IsInGameThread()); const auto* State = Socket.StateOnGT(); if (!State) return {};
	const auto Stage = State->Stage.Pin(); return Stage ? Stage->GetNamespaceInfo(Namespace.IsEmpty() ? FStringView(State->Options.DefaultNamespace) : Namespace) : FVaRestSioConnect();
}
FVaRestSioNamespace UVaRestSioClient::ResolveNamespace(FStringView Name, bool bCreate) const
{
	if (!bCreate) { const auto* State = Socket.StateOnGT(); if (!State || !State->Find(Name)) return {}; }
	return Name.IsEmpty() ? Socket.DefaultNamespace() : Socket.Namespace(Name);
}
void UVaRestSioClient::ConnectNamespace(const FString& Name, const FVaRestJson& Auth)
{
	const TStrongObjectPtr<UVaRestSioClient> Pin(this); auto Namespace = ResolveNamespace(Name);
	if (Auth.IsValid()) Namespace.WithAuth(Auth);
	Namespace.Connect();
}
void UVaRestSioClient::DisconnectNamespace(const FString& Name) { ResolveNamespace(Name).Disconnect(); }
bool UVaRestSioClient::IsNamespaceConnected(const FString& Name) const { return ResolveNamespace(Name, false).IsConnected(); }
EVaRestSioNamespaceState UVaRestSioClient::GetNamespaceState(const FString& Name) const { return ResolveNamespace(Name, false).GetState(); }
FString UVaRestSioClient::GetSid(const FString& Name) const { return ResolveNamespace(Name, false).GetSid(); }
bool UVaRestSioClient::WasRecovered(const FString& Name) const { return ResolveNamespace(Name, false).WasRecovered(); }
FVaRestSioStats UVaRestSioClient::GetSioStats(const FString& Namespace) const { return Socket.GetStats(Namespace); }
int32 UVaRestSioClient::GetNamespaceQueuedCount(const FString& Name) const { return ResolveNamespace(Name, false).GetQueuedCount(); }
void UVaRestSioClient::ClearNamespaceQueue(const FString& Name) { ResolveNamespace(Name, false).ClearQueue(); }
void UVaRestSioClient::SetAuthPayload(const FString& Name, const FVaRestJson& Auth) { ResolveNamespace(Name).WithAuth(Auth); }
void UVaRestSioClient::SetSioAuthPlacement(const FVaRestSioAuthPlacement& Placement) { Socket.WithAuthPlacement(Placement); }
void UVaRestSioClient::SetSioLimits(const FVaRestSioLimits& Limits) { Socket.WithLimits(Limits); }
void UVaRestSioClient::SetSioReconnectPolicy(const FVaRestReconnectPolicy& Policy) { Socket.WithReconnect(Policy); }
EVaRestSendResult UVaRestSioClient::EmitArgs(const FString& Event, const FVaRestJson& Args, const FString& Namespace, bool bVolatile)
{
	check(IsInGameThread()); const TStrongObjectPtr<UVaRestSioClient> Pin(this);
	if (!Args.IsArray()) return EVaRestSendResult::Rejected;
	const auto Target = ResolveNamespace(Namespace); if (!Target.IsValid()) return EVaRestSendResult::Rejected;
	auto Write = [&](auto& Writer, const FVaRestJsonConvertOptions& Options, FVaRestJsonError& Error, TFunctionRef<bool()> Budget)
	{
		VaRest::Sio::FArgumentWriter Into{Writer, Options, Error, Budget};
		for (int32 Index = 0; Index < Args.Num(); ++Index) if (!Into.Value(Args.At(Index))) return false;
		return true;
	};
	return Socket.EmitArguments(Target.GetName(), Event, Write, bVolatile ? EVaRestSend::Volatile : EVaRestSend::Default);
}
EVaRestSendResult UVaRestSioClient::EmitPropertyOnGT(FStringView Event, FStringView Namespace, const FProperty* Property, const void* Address, EVaRestSioArgs Mode, bool bVolatile)
{
	check(IsInGameThread()); const TStrongObjectPtr<UVaRestSioClient> Pin(this);
	const TStrongObjectPtr<UStruct> TypePin(Property ? Property->GetOwnerStruct() : nullptr);
	const auto Target = ResolveNamespace(Namespace); if (!Target.IsValid()) return EVaRestSendResult::Rejected;
	auto Write = [&](auto& Writer, const FVaRestJsonConvertOptions& Options, FVaRestJsonError& Error, TFunctionRef<bool()> Budget)
	{ return VaRest::Sio::Blueprint::WritePayload(Writer, Options, Error, Budget, Property, Address, Mode); };
	return Socket.EmitArguments(Target.GetName(), Event, Write, bVolatile ? EVaRestSend::Volatile : EVaRestSend::Default);
}
FVaRestSioEmitWithAck UVaRestSioClient::PrepareAckOnGT(FStringView Event, FStringView Namespace, const FProperty* Property, const void* Address, EVaRestSioArgs Mode)
{
	check(IsInGameThread()); const TStrongObjectPtr<UVaRestSioClient> Pin(this);
	const TStrongObjectPtr<UStruct> TypePin(Property ? Property->GetOwnerStruct() : nullptr);
	const auto Target = ResolveNamespace(Namespace);
	auto Write = [&](auto& Writer, const FVaRestJsonConvertOptions& Options, FVaRestJsonError& Error, TFunctionRef<bool()> Budget)
	{ return Target.IsValid() && VaRest::Sio::Blueprint::WritePayload(Writer, Options, Error, Budget, Property, Address, Mode); };
	return Socket.MakeAckBuilder(Namespace, Event, Write);
}
bool UVaRestSioClient::ListenOnGT(FStringView Namespace, FStringView Event, const UObject* Owner, TFunction<void(const FVaRestSioEvent&)> Handler, bool bOnce)
{
	check(IsInGameThread()); if (!Handler || !FVaRestSioCodec::IsValidEventName(Event) || FVaRestSioCodec::IsReservedEvent(Event)) return false;
	auto* State = Socket.StateOnGT(); if (!State) return false;
	const TStrongObjectPtr<UVaRestSioState> Pin(State); const auto Row = State->Ensure(Namespace.IsEmpty() ? FStringView(State->Options.DefaultNamespace) : Namespace); if (!Row) return false;
	return State->AddRoute(Row.ToSharedRef(), Event, Owner, [Weak = Socket.AsWeak(), Handler = MoveTemp(Handler)](const FVaRestSocketMessage& Message)
	{
		const auto Socket = Weak.Pin(); const auto* State = Socket.StateOnGT(); if (!State) return;
		const auto Stage = State->Stage.Pin(); if (Stage) Handler(FVaRestSioEvent::FromMessage(Message, Stage->RecoveryOffsetFor(Message)));
	}, bOnce);
}
EVaRestSendResult UVaRestSioClient::EmitEvent(const FString&, const int32&, const FString&, bool, EVaRestSioArgs) { checkNoEntry(); return EVaRestSendResult::Rejected; }

EVaRestSendResult UVaRestSioClient::EmitArguments(const FString&, const FString&, bool, int32,
	const int32&, const int32&, const int32&, const int32&, const int32&, const int32&, const int32&, const int32&, const int32&, const int32&, const int32&, const int32&, const int32&, const int32&, const int32&, const int32&, const int32&, const int32&, const int32&, const int32&, const int32&, const int32&, const int32&, const int32&, const int32&, const int32&, const int32&, const int32&, const int32&, const int32&, const int32&, const int32&)
{ checkNoEntry(); return EVaRestSendResult::Rejected; }
DEFINE_FUNCTION(UVaRestSioClient::execEmitArguments)
{
	P_GET_PROPERTY(FStrProperty, Event);
	P_GET_PROPERTY(FStrProperty, Namespace);
	P_GET_UBOOL(Volatile);
	P_GET_PROPERTY(FIntProperty, ArgumentCount);
	const auto Arg0 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	const auto Arg1 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	const auto Arg2 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	const auto Arg3 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	const auto Arg4 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	const auto Arg5 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	const auto Arg6 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	const auto Arg7 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	const auto Arg8 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	const auto Arg9 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	const auto Arg10 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	const auto Arg11 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	const auto Arg12 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	const auto Arg13 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	const auto Arg14 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	const auto Arg15 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	const auto Arg16 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	const auto Arg17 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	const auto Arg18 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	const auto Arg19 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	const auto Arg20 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	const auto Arg21 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	const auto Arg22 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	const auto Arg23 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	const auto Arg24 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	const auto Arg25 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	const auto Arg26 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	const auto Arg27 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	const auto Arg28 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	const auto Arg29 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	const auto Arg30 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	const auto Arg31 = VaRest::Sio::Blueprint::StepWildcard(Stack);
	P_FINISH;
	P_NATIVE_BEGIN;
	check(IsInGameThread()); const TStrongObjectPtr<UVaRestSioClient> Pin(P_THIS);
	const VaRest::Sio::Blueprint::FWildcard Arguments[] = {Arg0, Arg1, Arg2, Arg3, Arg4, Arg5, Arg6, Arg7, Arg8, Arg9, Arg10, Arg11, Arg12, Arg13, Arg14, Arg15, Arg16, Arg17, Arg18, Arg19, Arg20, Arg21, Arg22, Arg23, Arg24, Arg25, Arg26, Arg27, Arg28, Arg29, Arg30, Arg31};
	EVaRestSendResult Result = EVaRestSendResult::Rejected;
	if (ArgumentCount >= 0 && ArgumentCount <= static_cast<int32>(UE_ARRAY_COUNT(Arguments)))
	{
		const auto Target = P_THIS->ResolveNamespace(Namespace);
		TArray<TStrongObjectPtr<UStruct>> TypePins; TypePins.Reserve(ArgumentCount);
		bool bValid = Target.IsValid();
		for (int32 Index = 0; Index < ArgumentCount; ++Index)
		{
			const auto& Value = Arguments[Index]; bValid &= Value.Property != nullptr && Value.Address != nullptr;
			if (Value.Property) TypePins.Emplace(Value.Property->GetOwnerStruct());
		}
		if (bValid)
		{
			auto Write = [&](auto& Writer, const FVaRestJsonConvertOptions& Options, FVaRestJsonError& Error, TFunctionRef<bool()> Budget)
			{
				for (int32 Index = 0; Index < ArgumentCount; ++Index)
					if (!VaRest::Sio::Blueprint::WritePayload(Writer, Options, Error, Budget, Arguments[Index].Property, Arguments[Index].Address, EVaRestSioArgs::First)) return false;
				return true;
			};
			Result = P_THIS->Socket.EmitArguments(Target.GetName(), Event, Write, Volatile ? EVaRestSend::Volatile : EVaRestSend::Default);
		}
	}
	*static_cast<EVaRestSendResult*>(RESULT_PARAM) = Result;
	P_NATIVE_END;
}
DEFINE_FUNCTION(UVaRestSioClient::execEmitEvent)
{
	P_GET_PROPERTY(FStrProperty, Event);
	const auto Payload = VaRest::Sio::Blueprint::StepWildcard(Stack);
	P_GET_PROPERTY(FStrProperty, Namespace);
	P_GET_UBOOL(Volatile);
	P_GET_ENUM(EVaRestSioArgs, ArgsMode);
	P_FINISH;
	P_NATIVE_BEGIN;
	*static_cast<EVaRestSendResult*>(RESULT_PARAM) = P_THIS->EmitPropertyOnGT(Event, Namespace, Payload.Property, Payload.Address, static_cast<EVaRestSioArgs>(ArgsMode), Volatile);
	P_NATIVE_END;
}
