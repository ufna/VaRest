// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSioSocket.h"
#include "UObject/Package.h"
#include "VaRestSioState.h"
#include "VaRestSioLog.h"
#include "VaRestSioSettings.h"
#include "VaRestSioEndpoint.h"
#include "VaRestSioStatsInternal.h"
#include "VaRestSocketHub.h"
#include "VaRestApi.h"
#include "VaRestHub.h"
#include "VaRestUrl.h"
#include "UObject/StrongObjectPtr.h"

FVaRestSioSocket::FVaRestSioSocket(FVaRestSocket Socket, UVaRestSioState* InState) : Native(MoveTemp(Socket)), State(InState) {}

UVaRestSioState* FVaRestSioSocket::StateOnGT() const
{
	check(IsInGameThread()); auto* View = State.Get();
	return Native.IsValid() && View && Native.GetProtocolStateOnGT() == View ? View : nullptr;
}
FVaRestSioSocketWeak FVaRestSioSocket::AsWeak() const
{ FVaRestSioSocketWeak Out; Out.Native = Native.AsWeak(); Out.State = State; return Out; }
FVaRestSioSocket FVaRestSioSocketWeak::Pin() const
{ FVaRestSioSocket Out; Out.Native = Native.Pin(); Out.State = State; return Out; }
FVaRestSioNamespace FVaRestSioSocket::Namespace(FStringView Name) const
{
	check(IsInGameThread());
	if (auto* View = StateOnGT()) { const TStrongObjectPtr<UVaRestSioState> Pin(View); if (const auto Row = View->Ensure(Name)) return FVaRestSioNamespace(*this, Row->Name); }
	return {};
}
FVaRestSioNamespace FVaRestSioSocket::DefaultNamespace() const
{ check(IsInGameThread()); const auto* View = StateOnGT(); return View ? FVaRestSioNamespace(*this, View->Options.DefaultNamespace) : FVaRestSioNamespace(); }
FVaRestSioStats FVaRestSioSocket::GetStats(FStringView Namespace) const
{ check(IsInGameThread()); const auto* View = StateOnGT(); return View ? View->GetStatsOnGT(Namespace) : FVaRestSioStats(); }

#define VAREST_SIO_OPTIONS(Body) \
	check(IsInGameThread()); if (auto* View = StateOnGT()) { const TStrongObjectPtr<UVaRestSioState> Pin(View); auto Options = View->Options; Body; View->UpdateOptions(MoveTemp(Options)); } return *this
FVaRestSioSocket FVaRestSioSocket::WithPath(FString Path) { VAREST_SIO_OPTIONS(Options.Path = MoveTemp(Path)); }
FVaRestSioSocket FVaRestSioSocket::WithAuthPlacement(FVaRestSioAuthPlacement Placement) { VAREST_SIO_OPTIONS(Options.Auth = MoveTemp(Placement)); }
FVaRestSioSocket FVaRestSioSocket::WithRecovery(bool bEnabled) { VAREST_SIO_OPTIONS(Options.bRecovery = bEnabled); }
FVaRestSioSocket FVaRestSioSocket::WithReconnect(FVaRestReconnectPolicy Policy)
{
	check(IsInGameThread()); if (auto* View = StateOnGT())
	{
		const TStrongObjectPtr<UVaRestSioState> Pin(View); auto Options = View->Options; Options.NamespaceRetry = Policy; Options.StableAfter = Policy.StableAfter;
		if (View->UpdateOptions(MoveTemp(Options))) Native.WithReconnect(MoveTemp(Policy));
	}
	return *this;
}
FVaRestSioSocket FVaRestSioSocket::NoReconnect()
{ check(IsInGameThread()); if (auto* View = StateOnGT()) { auto Policy = View->Options.NamespaceRetry; Policy.bEnabled = false; return WithReconnect(Policy); } return *this; }
#undef VAREST_SIO_OPTIONS

FVaRestSioSocket FVaRestSioSocket::WithQuery(FString Key, FString Value) { Native.WithQuery(MoveTemp(Key), MoveTemp(Value)); return *this; }
FVaRestSioSocket FVaRestSioSocket::WithHeader(FString Name, FString Value) { Native.WithHeader(MoveTemp(Name), MoveTemp(Value)); return *this; }
FVaRestSioSocket FVaRestSioSocket::WithOrigin(TOptional<FString> Origin) { Native.WithOrigin(MoveTemp(Origin)); return *this; }
FVaRestSioSocket FVaRestSioSocket::WithApi(UVaRestApi* Api) { Native.WithApi(Api); return *this; }
FVaRestSioSocket FVaRestSioSocket::WithClient(FVaRestClient Client) { Native.WithClient(MoveTemp(Client)); return *this; }
FVaRestSioSocket FVaRestSioSocket::WithBearer(TFunction<FString()> Token) { Native.WithBearer(MoveTemp(Token)); return *this; }
FVaRestSioSocket FVaRestSioSocket::WithProtocolPing(double Interval, double Timeout) { FVaRestProtocolPing Ping; Ping.Interval = Interval; Ping.Timeout = Timeout; Native.WithProtocolPing(Ping); return *this; }
FVaRestSioSocket FVaRestSioSocket::WithQueue(FVaRestSocketQueuePolicy Policy) { Native.WithQueue(MoveTemp(Policy)); return *this; }
FVaRestSioSocket FVaRestSioSocket::WithBackground(EVaRestSocketBackground Policy) { Native.WithBackground(Policy); return *this; }
FVaRestSioSocket FVaRestSioSocket::WithLimits(FVaRestSioLimits Limits)
{
	check(IsInGameThread()); if (auto* View = StateOnGT())
	{
		const TStrongObjectPtr<UVaRestSioState> Pin(View); FString Error;
		if (!Limits.Validate(Error)) { View->ReportConfig(MoveTemp(Error)); return *this; }
		auto Options = View->Options; Options.MaxOutboundFrameBytes = FMath::Min(Limits.MaxOutboundFrameBytes, Limits.MaxSendBytes);
		Options.bOverrideFallbackPayloadLimit = Limits.bOverrideFallbackPayloadLimit; Options.MaxNamespaces = Limits.MaxChannels;
		Options.ReadyTimeout = Limits.ReadyTimeout > 0.0 ? Limits.ReadyTimeout : Options.ReadyTimeout;
		if (!View->UpdateOptions(MoveTemp(Options)) || !View->Codec || !View->Codec->UpdateOptions(Limits.ToCodecOptions(), Error))
		{ if (!Error.IsEmpty()) View->ReportConfig(MoveTemp(Error)); return *this; }
		View->Limits = Limits; Native.WithLimits(Limits);
		if (Native.GetState() != EVaRestSocketState::Disconnected) Native.Restart(TEXTVIEW("Socket.IO limits changed"));
	}
	return *this;
}
FVaRestSioSocket FVaRestSioSocket::WithNaming(EVaRestJsonNaming Naming) { Native.WithNaming(Naming); return *this; }
FVaRestSioSocket FVaRestSioSocket::WithStrictDecode(bool bStrict) { Native.WithStrictDecode(bStrict); return *this; }
FVaRestSioSocket FVaRestSioSocket::WithLogLevel(EVaRestLogLevel Level) { Native.WithLogLevel(Level); return *this; }
FVaRestSioSocket FVaRestSioSocket::Owner(const UObject* Owner) { Native.Owner(Owner); return *this; }
FVaRestSioSocket FVaRestSioSocket::OnRetryScheduled(const UObject* Owner, TFunction<void(const FVaRestSocketStatus&, const FVaRestSocketClose&)> Handler) { Native.OnRetryScheduled(Owner, MoveTemp(Handler)); return *this; }
FVaRestSioSocket FVaRestSioSocket::OnGaveUp(const UObject* Owner, TFunction<void(const FVaRestError&, const FVaRestSocketClose&)> Handler) { Native.OnGaveUp(Owner, MoveTemp(Handler)); return *this; }
FVaRestSioSocket FVaRestSioSocket::OnStateChanged(const UObject* Owner, TFunction<void(EVaRestSocketState, EVaRestSocketState)> Handler) { Native.OnStateChanged(Owner, MoveTemp(Handler)); return *this; }
FVaRestSioSocket FVaRestSioSocket::OnError(const UObject* Owner, TFunction<void(const FVaRestError&)> Handler) { Native.OnError(Owner, MoveTemp(Handler)); return *this; }
FVaRestSioSocket FVaRestSioSocket::OnConnect(const UObject* Owner, TFunction<void(const FVaRestSioConnect&)> Handler) { DefaultNamespace().OnConnect(Owner, MoveTemp(Handler)); return *this; }
FVaRestSioSocket FVaRestSioSocket::OnConnectError(const UObject* Owner, TFunction<void(const FVaRestError&, const FVaRestSioConnectError&)> Handler) { DefaultNamespace().OnConnectError(Owner, MoveTemp(Handler)); return *this; }
FVaRestSioSocket FVaRestSioSocket::OnDisconnect(const UObject* Owner, TFunction<void(const FVaRestSioDisconnect&)> Handler) { DefaultNamespace().OnDisconnect(Owner, MoveTemp(Handler)); return *this; }
FVaRestSioSocket FVaRestSioSocket::WithAuth(FVaRestJson Payload) { DefaultNamespace().WithAuth(MoveTemp(Payload)); return *this; }
FVaRestSioSocket FVaRestSioSocket::WithSessionAuth(bool bSend) { DefaultNamespace().WithSessionAuth(bSend); return *this; }
FVaRestSioSocket FVaRestSioSocket::OnConnectAuth(const UObject* Owner, TFunction<void(FVaRestJson&, const FVaRestSioConnectContext&)> Handler) { DefaultNamespace().OnConnectAuth(Owner, MoveTemp(Handler)); return *this; }
FVaRestSioSocket FVaRestSioSocket::ConnectErrorIf(TFunction<EVaRestSioRetry(const FVaRestSioConnectError&)> Predicate) { DefaultNamespace().ConnectErrorIf(MoveTemp(Predicate)); return *this; }
FVaRestSioSocket FVaRestSioSocket::On(FStringView Event, const UObject* Owner, TFunction<void(const FVaRestSioEvent&)> Handler) { DefaultNamespace().On(Event, Owner, MoveTemp(Handler)); return *this; }
FVaRestSioSocket FVaRestSioSocket::Once(FStringView Event, const UObject* Owner, TFunction<void(const FVaRestSioEvent&)> Handler) { DefaultNamespace().Once(Event, Owner, MoveTemp(Handler)); return *this; }
FVaRestSioSocket FVaRestSioSocket::OnAny(const UObject* Owner, TFunction<void(const FVaRestSioEvent&)> Handler) { DefaultNamespace().OnAny(Owner, MoveTemp(Handler)); return *this; }
FVaRestSioSocket FVaRestSioSocket::OnUnhandled(const UObject* Owner, TFunction<void(const FVaRestSioEvent&)> Handler) { DefaultNamespace().OnUnhandled(Owner, MoveTemp(Handler)); return *this; }
void FVaRestSioSocket::Off(FStringView Event, const UObject* Owner) { DefaultNamespace().Off(Event, Owner); }
void FVaRestSioSocket::OffAny(const UObject* Owner) { DefaultNamespace().OffAny(Owner); }
void FVaRestSioSocket::OffAll(const UObject* Owner) { check(IsInGameThread()); if (auto* View = StateOnGT()) { View->OffGlobal(Owner); const auto Rows = View->Namespaces; for (const auto& Row : Rows) View->Off(Row->Name, {}, Owner, true); } Native.OffAll(Owner); }
FVaRestSioSocket FVaRestSioSocket::Connect()
{
	check(IsInGameThread()); if (auto* View = StateOnGT())
	{ const TStrongObjectPtr<UVaRestSioState> Pin(View); if (auto Held = View->Stage.Pin()) { Held->ConnectDefaults(); Native.Connect(); } } return *this;
}
void FVaRestSioSocket::Close() { Native.Close(); }
void FVaRestSioSocket::ReconnectNow() { Native.ReconnectNow(); }
void FVaRestSioSocket::StopReconnecting() { Native.StopReconnecting(); }
void FVaRestSioSocket::ResetSession()
{ check(IsInGameThread()); if (auto* View = StateOnGT()) if (auto Held = View->Stage.Pin()) Held->ResetRecovery(); Native.ResetSession(); }

EVaRestSendResult FVaRestSioSocket::EmitArguments(FStringView Namespace, FStringView Event, FVaRestSocketPayloadWriter::FWriteArguments Arguments, EVaRestSend Flags) const
{
	auto Copy = Native;
	return Copy.EncodeAndSend(Namespace, [&](const IVaRestSocketCodec& Codec, const FVaRestSocketCodecContext& Context, const FVaRestJsonConvertOptions& Convert, FStringView Channel) -> TValueOrError<TArray<FVaRestSocketFrame>, FVaRestError>
	{
		if (Codec.GetProtocolTag() != TEXT("SocketIO")) { FVaRestError Error; Error.Kind = EVaRestErrorKind::Config; Error.Message = TEXT("Socket.IO encoder unavailable"); return MakeError(MoveTemp(Error)); }
		auto Unused = [](auto&, bool, TConstArrayView<FString>, FVaRestJsonError&) { return false; };
		FVaRestSocketPayloadWriter View(Unused, false); View.Arguments.Emplace(Arguments); View.ConvertOptions = &Convert;
		return Codec.Encode(Channel, Event, View, {}, Context);
	}, Flags);
}
EVaRestSendResult FVaRestSioSocket::EmitStructArguments(FStringView Namespace, FStringView Event, const UScriptStruct* Struct, const void* Value, EVaRestSend Flags) const
{
	auto Copy = Native;
	return Copy.EncodeAndSend(Namespace, [&](const IVaRestSocketCodec& Codec, const FVaRestSocketCodecContext& Context, const FVaRestJsonConvertOptions& Convert, FStringView Channel) -> TValueOrError<TArray<FVaRestSocketFrame>, FVaRestError>
	{
		if (Codec.GetProtocolTag() != TEXT("SocketIO")) { FVaRestError Error; Error.Kind = EVaRestErrorKind::Config; Error.Message = TEXT("Socket.IO encoder unavailable"); return MakeError(MoveTemp(Error)); }
		auto Unused = [](auto&, bool, TConstArrayView<FString>, FVaRestJsonError&) { return false; };
		FVaRestSocketPayloadWriter View(Unused, true); View.bPositionalArguments = true; View.NativeStruct = Struct; View.NativeValue = Value; View.ConvertOptions = &Convert;
		return Codec.Encode(Channel, Event, View, {}, Context);
	}, Flags);
}

#define VAREST_SIO_NS_EDIT(Body) \
	check(IsInGameThread()); if (auto* View = Socket.StateOnGT()) { const TStrongObjectPtr<UVaRestSioState> Pin(View); if (auto Row = View->Find(Name)) { Body; } } return *this
FVaRestSioNamespace FVaRestSioNamespace::WithAuth(FVaRestJson Payload)
{
	VAREST_SIO_NS_EDIT(if (Payload.IsValid() && !Payload.IsObject()) View->ReportConfig(TEXT("Socket.IO auth payload must be an object")); else { Row->Config.AuthPayload = Payload.Clone(); View->Apply(Row.ToSharedRef()); });
}
FVaRestSioNamespace FVaRestSioNamespace::WithSessionAuth(bool bSend) { VAREST_SIO_NS_EDIT(Row->Config.bSendSession = bSend; View->Apply(Row.ToSharedRef())); }
FVaRestSioNamespace FVaRestSioNamespace::WithConnectOnStart(bool bConnect) { VAREST_SIO_NS_EDIT(Row->Config.bConnectOnStart = bConnect; View->Apply(Row.ToSharedRef())); }
FVaRestSioNamespace FVaRestSioNamespace::ConnectErrorIf(TFunction<EVaRestSioRetry(const FVaRestSioConnectError&)> Predicate) { VAREST_SIO_NS_EDIT(Row->Config.ConnectErrorIf = MoveTemp(Predicate); View->Apply(Row.ToSharedRef())); }
FVaRestSioNamespace FVaRestSioNamespace::OnConnectAuth(const UObject* Owner, TFunction<void(FVaRestJson&, const FVaRestSioConnectContext&)> Handler)
{ VAREST_SIO_NS_EDIT(if (!View->Add(Row->Auth, Owner, MoveTemp(Handler))) View->ReportConfig(TEXT("Socket.IO auth handler admission failed"))); }
FVaRestSioNamespace FVaRestSioNamespace::OnConnect(const UObject* Owner, TFunction<void(const FVaRestSioConnect&)> Handler)
{ VAREST_SIO_NS_EDIT(if (!View->Add(Row->Connect, Owner, MoveTemp(Handler))) View->ReportConfig(TEXT("Socket.IO connect handler admission failed"))); }
FVaRestSioNamespace FVaRestSioNamespace::OnConnectError(const UObject* Owner, TFunction<void(const FVaRestError&, const FVaRestSioConnectError&)> Handler)
{ VAREST_SIO_NS_EDIT(if (!View->Add(Row->Error, Owner, MoveTemp(Handler))) View->ReportConfig(TEXT("Socket.IO error handler admission failed"))); }
FVaRestSioNamespace FVaRestSioNamespace::OnDisconnect(const UObject* Owner, TFunction<void(const FVaRestSioDisconnect&)> Handler)
{ VAREST_SIO_NS_EDIT(if (!View->Add(Row->Disconnect, Owner, MoveTemp(Handler))) View->ReportConfig(TEXT("Socket.IO disconnect handler admission failed"))); }
FVaRestSioNamespace FVaRestSioNamespace::OnAny(const UObject* Owner, TFunction<void(const FVaRestSocketMessage&)> Handler)
{ VAREST_SIO_NS_EDIT(if (!View->Add(Row->Any, Owner, MoveTemp(Handler))) View->ReportConfig(TEXT("Socket.IO any handler admission failed"))); }
FVaRestSioNamespace FVaRestSioNamespace::OnUnhandled(const UObject* Owner, TFunction<void(const FVaRestSocketMessage&)> Handler)
{ VAREST_SIO_NS_EDIT(if (!View->Add(Row->Unhandled, Owner, MoveTemp(Handler))) View->ReportConfig(TEXT("Socket.IO unhandled handler admission failed"))); }
FVaRestSioNamespace FVaRestSioNamespace::On(FStringView Event, const UObject* Owner, TFunction<void(const FVaRestSocketMessage&)> Handler)
{
	VAREST_SIO_NS_EDIT(if (!View->AddRoute(Row.ToSharedRef(), Event, Owner, MoveTemp(Handler), false)) View->ReportConfig(TEXT("Socket.IO event handler admission failed")));
}
FVaRestSioNamespace FVaRestSioNamespace::Once(FStringView Event, const UObject* Owner, TFunction<void(const FVaRestSocketMessage&)> Handler)
{ VAREST_SIO_NS_EDIT(if (!View->AddRoute(Row.ToSharedRef(), Event, Owner, MoveTemp(Handler), true)) View->ReportConfig(TEXT("Socket.IO once handler admission failed"))); }
#undef VAREST_SIO_NS_EDIT
void FVaRestSioNamespace::Off(FStringView Event, const UObject* Owner) { check(IsInGameThread()); if (auto* View = Socket.StateOnGT()) View->Off(Name, Event, Owner, false); }
void FVaRestSioNamespace::OffAny(const UObject* Owner) { check(IsInGameThread()); if (auto* View = Socket.StateOnGT()) View->OffAny(Name, Owner); }
void FVaRestSioNamespace::OffAll(const UObject* Owner) { check(IsInGameThread()); if (auto* View = Socket.StateOnGT()) View->Off(Name, {}, Owner, true); }
FVaRestSioNamespace FVaRestSioNamespace::Connect()
{
	check(IsInGameThread()); if (auto* View = Socket.StateOnGT())
	{ const TStrongObjectPtr<UVaRestSioState> Pin(View); if (auto Held = View->Stage.Pin(); Held && Held->ConnectNamespace(Name)) Socket.Native.Connect(); } return *this;
}
void FVaRestSioNamespace::Disconnect()
{ check(IsInGameThread()); if (auto* View = Socket.StateOnGT()) { const TStrongObjectPtr<UVaRestSioState> Pin(View); if (auto Held = View->Stage.Pin()) Held->DisconnectNamespace(Name); } }
EVaRestSioNamespaceState FVaRestSioNamespace::GetState() const
{ check(IsInGameThread()); if (const auto* View = Socket.StateOnGT()) if (auto Held = View->Stage.Pin()) return Held->GetNamespaceState(Name); return EVaRestSioNamespaceState::Disconnected; }
FString FVaRestSioNamespace::GetSid() const
{ check(IsInGameThread()); if (const auto* View = Socket.StateOnGT()) if (auto Held = View->Stage.Pin()) return Held->GetNamespaceInfo(Name).Sid; return {}; }
bool FVaRestSioNamespace::WasRecovered() const
{ check(IsInGameThread()); if (const auto* View = Socket.StateOnGT()) if (auto Held = View->Stage.Pin()) return Held->GetNamespaceInfo(Name).bRecovered; return false; }
int32 FVaRestSioNamespace::GetQueuedCount() const { check(IsInGameThread()); return Socket.Native.Channel(Name).GetQueuedCount(); }
void FVaRestSioNamespace::ClearQueue() { check(IsInGameThread()); Socket.Native.Channel(Name).ClearQueue(); }
TFunction<void(const FVaRestSocketMessage&)> FVaRestSioNamespace::EventHandler(TFunction<void(const FVaRestSioEvent&)> Handler) const
{
	if (!Handler) return {};
	return [Weak = Socket.AsWeak(), Handler = MoveTemp(Handler)](const FVaRestSocketMessage& Message)
	{
		const auto HeldSocket = Weak.Pin(); auto* View = HeldSocket.StateOnGT(); if (!View) return;
		const TStrongObjectPtr<UVaRestSioState> Pin(View); const auto HeldStage = View->Stage.Pin();
		if (!HeldStage) return;
		const auto Event = FVaRestSioEvent::FromMessage(Message, HeldStage->RecoveryOffsetFor(Message)); Handler(Event);
	};
}
FVaRestSioNamespace FVaRestSioNamespace::On(FStringView Event, const UObject* Owner, TFunction<void(const FVaRestSioEvent&)> Handler)
{ return On(Event, Owner, EventHandler(MoveTemp(Handler))); }
FVaRestSioNamespace FVaRestSioNamespace::Once(FStringView Event, const UObject* Owner, TFunction<void(const FVaRestSioEvent&)> Handler)
{ return Once(Event, Owner, EventHandler(MoveTemp(Handler))); }
FVaRestSioNamespace FVaRestSioNamespace::OnAny(const UObject* Owner, TFunction<void(const FVaRestSioEvent&)> Handler)
{ return OnAny(Owner, EventHandler(MoveTemp(Handler))); }
FVaRestSioNamespace FVaRestSioNamespace::OnUnhandled(const UObject* Owner, TFunction<void(const FVaRestSioEvent&)> Handler)
{ return OnUnhandled(Owner, EventHandler(MoveTemp(Handler))); }

FVaRestSioSocket FVaRestSio::Make(const UObject* WorldContext, FStringView Url) { return Make(UVaRestSocketHub::Get(WorldContext), NAME_None, Url); }
FVaRestSioSocket FVaRestSio::Make(const UObject* WorldContext, FName Name, FStringView Url) { return Make(UVaRestSocketHub::Get(WorldContext), Name, Url); }
namespace
{
	bool ReadSioSettings(const UVaRestSioSettings& Settings, FVaRestSioStageOptions& Options)
	{
		FString Error;
		if (!Settings.Limits.Validate(Error) || !FMath::IsFinite(Settings.AckTimeout) || Settings.AckTimeout < 0
			|| !FMath::IsFinite(Settings.ConnectTimeout) || Settings.ConnectTimeout <= 0 || Settings.ConnectTimeout > 60
			|| !FMath::IsFinite(Settings.CloseTimeout) || Settings.CloseTimeout < 0 || Settings.CloseTimeout > 5)
		{ UE_LOG(LogVaRestSio, Error, TEXT("Config: invalid Socket.IO project limits or timeouts")); return false; }
		Options.Path = Settings.Path; Options.Auth = Settings.Auth; Options.bRecovery = Settings.bRecovery;
		Options.NamespaceRetry = Settings.Reconnect; Options.StableAfter = Settings.Reconnect.StableAfter;
		Options.MaxOutboundFrameBytes = FMath::Min(Settings.Limits.MaxOutboundFrameBytes, Settings.Limits.MaxSendBytes);
		Options.bOverrideFallbackPayloadLimit = Settings.Limits.bOverrideFallbackPayloadLimit; Options.MaxNamespaces = Settings.Limits.MaxChannels;
		Options.ReadyTimeout = Settings.Limits.ReadyTimeout > 0.0 ? Settings.Limits.ReadyTimeout : Settings.ConnectTimeout;
		return true;
	}
	FString SioEnginePath(FStringView Url)
	{
		const FString Text(Url); const int32 Scheme = Text.Find(TEXT("://")); if (Scheme == INDEX_NONE) return {};
		int32 Start = Scheme + 3; while (Start < Text.Len() && Text[Start] != '/' && Text[Start] != '?' && Text[Start] != '#') ++Start;
		if (Start == Text.Len() || Text[Start] != '/') return TEXT("/");
		int32 End = Start; while (End < Text.Len() && Text[End] != '?' && Text[End] != '#') ++End;
		return Text.Mid(Start, End - Start);
	}
}
bool FVaRestSio::InitializeOnGT(FVaRestSocket& Socket, FVaRestSioStageOptions Options, const UVaRestSioSettings& Settings)
{
	auto Codec = MakeShared<FVaRestSioCodec, ESPMode::ThreadSafe>(Settings.Limits.ToCodecOptions()); auto Stage = MakeShared<FVaRestSioStage>(Codec, Options);
	if (!FVaRestSioCodec::IsValidNamespace(Options.DefaultNamespace) || !Stage->UpdateOptions(Options)) return false;
	auto* View = NewObject<UVaRestSioState>(GetTransientPackage()); const TStrongObjectPtr<UVaRestSioState> Pin(View);
	View->Options = Options; View->Socket = Socket.AsWeak(); View->Stage = Stage; View->Codec = Codec;
	View->Limits = Settings.Limits; View->AckTimeout = Settings.AckTimeout;
	VaRest::Sio::Stats::RegisterOnGT(View);
	const TWeakObjectPtr<UVaRestSioState> WeakView(View);
	Stage->KeepOwnerStateOnGT(View, [WeakView]() { if (auto* Current = WeakView.Get()) Current->RetireOnGT(); }); Socket.SetProtocolStateOnGT(View);
	FVaRestSocketRouting Routing; Routing.IdFormat = EVaRestSocketIdFormat::Number; Routing.IdField = TEXT("id");
	FVaRestSocketAutoReply Pong; Pong.Match = {'2'}; Pong.Reply = {'3'};
	Socket.WithCodec(Codec).WithRouting(Routing).WithLimits(Settings.Limits).WithQueue(Settings.Queue).WithReconnect(Options.NamespaceRetry)
		.WithTimeouts(Settings.ConnectTimeout, Settings.CloseTimeout).WithProtocolPing(Settings.ProtocolPing)
		.WithBackground(Settings.Background).WithLogLevel(Settings.LogLevel).WithHeartbeat(FVaRestHeartbeat{}).WithAutoReplies({Pong}).Use(Stage);
	return View->Ensure(Options.DefaultNamespace).IsValid();
}
FVaRestSioSocket FVaRestSio::Make(UVaRestSocketHub* Hub, FName Name, FStringView Url)
{ return MakeOnGT(Hub, Name, Url, {}); }
FVaRestSioSocket FVaRestSio::Make(UVaRestSocketHub* Hub, FName Name, FStringView Url, FStringView Namespace)
{ return MakeOnGT(Hub, Name, Url, FString(Namespace.IsEmpty() ? TEXTVIEW("/") : Namespace)); }
FVaRestSioSocket FVaRestSio::MakeOnGT(UVaRestSocketHub* Hub, FName Name, FStringView Url, TOptional<FString> Namespace)
{
	check(IsInGameThread()); if (!IsValid(Hub)) return {};
	FString Input(Url); const int32 Scheme = Input.Find(TEXT("://")); if (Scheme == INDEX_NONE) return {};
	int32 Start = Scheme + 3; while (Start < Input.Len() && Input[Start] != '/' && Input[Start] != '?' && Input[Start] != '#') ++Start;
	int32 End = Start; while (End < Input.Len() && Input[End] != '?' && Input[End] != '#') ++End;
	const auto* Settings = GetDefault<UVaRestSioSettings>();
	FVaRestSioStageOptions Options; if (!ReadSioSettings(*Settings, Options)) return {};
	if (Start < Input.Len() && Input[Start] == '/') Options.DefaultNamespace = VaRest::Http::PercentDecode(FStringView(Input).Mid(Start, End - Start));
	if (Options.DefaultNamespace.Equals(TEXT("/socket.io"), ESearchCase::CaseSensitive) || Options.DefaultNamespace.Equals(TEXT("/socket.io/"), ESearchCase::CaseSensitive))
	{ Options.DefaultNamespace = TEXT("/"); Options.Path = TEXT("/socket.io/"); UE_LOG(LogVaRestSio, Log, TEXT("Socket.IO URL path /socket.io/ selects the engine path and default namespace /")); }
	if (Namespace.IsSet()) Options.DefaultNamespace = Namespace.GetValue();
	if (!FVaRestSioCodec::IsValidNamespace(Options.DefaultNamespace)) { UE_LOG(LogVaRestSio, Error, TEXT("Config: invalid Socket.IO namespace; query in a namespace is the EIO 3 auth style, use WithAuth or WithQuery")); return {}; }
	auto Native = Hub->MakeForProtocol(Name, Input, TEXT("SocketIO"), [&](FVaRestSocket& Socket)
	{
		InitializeOnGT(Socket, Options, *Settings);
	});
	auto* View = Cast<UVaRestSioState>(Native.GetProtocolStateOnGT()); return View ? FVaRestSioSocket(MoveTemp(Native), View) : FVaRestSioSocket();
}
FVaRestSioSocket FVaRestSio::Find(UVaRestSocketHub* Hub, FName Name)
{
	check(IsInGameThread()); if (!IsValid(Hub)) return {};
	auto Native = Hub->FindForProtocol(Name, TEXT("SocketIO")); auto* View = Cast<UVaRestSioState>(Native.GetProtocolStateOnGT());
	return View ? FVaRestSioSocket(MoveTemp(Native), View) : FVaRestSioSocket();
}
FVaRestSioSocket FVaRestSio::Socket(const UObject* WorldContext, UVaRestApi* Api, FName Entry, FVaRestError& Error)
{ return Socket(UVaRestSocketHub::Get(WorldContext), Api, Entry, Error); }
FVaRestSioSocket FVaRestSio::Socket(UVaRestSocketHub* Hub, UVaRestApi* Api, FName Entry, FVaRestError& Error)
{
	check(IsInGameThread()); Error = {};
	if (!IsValid(Hub)) { Error.Kind = EVaRestErrorKind::Config; Error.Message = TEXT("Socket.IO requires a real game instance or an explicit editor Hub"); return {}; }
	auto Native = Hub->SocketForProtocol(Api, Entry, TEXT("SocketIO"), Error,
		[](FVaRestSocket& Socket, const UVaRestSocketEndpointBase& Base, FStringView ResolvedUrl)
		{
			const auto* Endpoint = Cast<UVaRestSioEndpoint>(&Base); if (!Endpoint) return false;
			const auto* Settings = GetDefault<UVaRestSioSettings>(); FVaRestSioStageOptions Options;
			if (!ReadSioSettings(*Settings, Options)) return false;
			Options.DefaultNamespace = Endpoint->DefaultNamespace;
			// The Hub has joined the API base and engine path. Preserve a base such as /v2/socket.io/.
			Options.Path = SioEnginePath(ResolvedUrl);
			if (!InitializeOnGT(Socket, Options, *Settings)) return false;
			Endpoint->ConfigureSocket(Socket);
			auto* View = Cast<UVaRestSioState>(Socket.GetProtocolStateOnGT()); if (!View) return false;
			FVaRestSioSocket Sio(Socket, View); return Endpoint->ConfigureSio(Sio);
		},
		[](UVaRestSocketHub& Owner, UVaRestApi* Asset, FName Name, FVaRestSocket& Socket)
		{
			auto* Core = Owner.GetCoreHubOnGT(); if (!Core || !IsValid(Asset)) return;
			auto* Client = Core->GetClient(Asset); if (!Owner.GetCoreHubOnGT() || !Client || !Socket.IsValid()) return;
			const UVaRestSioEndpoint* Endpoint = nullptr; bool bDuplicate = false;
			for (const auto& Candidate : Asset->Sockets) if (Candidate && Candidate->Name == Name)
			{ if (Endpoint) bDuplicate = true; Endpoint = Cast<UVaRestSioEndpoint>(Candidate); if (!Endpoint) bDuplicate = true; }
			const auto* Environment = Asset->FindEnvironment(Client->GetEnvironment());
			FVaRestApiValidationContext Validation; if (Endpoint) Endpoint->Validate(Validation);
			auto* View = Cast<UVaRestSioState>(Socket.GetProtocolStateOnGT());
			if (!Endpoint || bDuplicate || Validation.HasErrors() || !Environment || !View)
			{ Socket.Close(1001, TEXTVIEW("invalid API environment")); return; }
			const TStrongObjectPtr<UVaRestSioState> Pin(View); auto Options = View->Options;
			const FString Url = Endpoint->ResolveUrl(Environment->SocketBaseUrl.IsEmpty() ? FStringView(Environment->BaseUrl) : FStringView(Environment->SocketBaseUrl));
			Options.Path = SioEnginePath(Url);
			if (auto Stage = View->Stage.Pin()) Stage->ResetRecovery();
			if (!View->UpdateOptions(MoveTemp(Options))) { Socket.Close(1001, TEXTVIEW("invalid API engine path")); return; }
			Socket.WithUrl(Url).WithClient(Client->GetNativeClient(), !Endpoint->bNoAuth).Restart(TEXTVIEW("environment changed"));
		});
	auto* View = Cast<UVaRestSioState>(Native.GetProtocolStateOnGT()); return View ? FVaRestSioSocket(MoveTemp(Native), View) : FVaRestSioSocket();
}
