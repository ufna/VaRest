// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSioStage.h"
#include "VaRestSioStatsInternal.h"
#include "VaRestSioLog.h"
#include "VaRestClient.h"
#include "VaRestAuth.h"
#include "VaRestTrace.h"
#include "VaRestRedactor.h"
#include "VaRestJsonUtf8.h"
#include "VaRestUrl.h"
#include "UObject/StrongObjectPtr.h"

namespace VaRest::Sio
{
static bool Same(FStringView A, FStringView B) { return A.Equals(B, ESearchCase::CaseSensitive); }
static FVaRestError Error(EVaRestErrorKind Kind, FString Text)
{ FVaRestError Out; Out.Kind = Kind; Out.Message = MoveTemp(Text); return Out; }
static bool ReservedQuery(FStringView Key)
{ return Same(Key, TEXTVIEW("EIO")) || Same(Key, TEXTVIEW("transport")) || Same(Key, TEXTVIEW("sid")); }
static FString ReasonText(EVaRestSioDisconnectReason Reason)
{
	switch (Reason)
	{
	case EVaRestSioDisconnectReason::ClientDisconnect:
	case EVaRestSioDisconnectReason::SocketClosed:
	case EVaRestSioDisconnectReason::SessionReplaced: return TEXT("io client disconnect");
	case EVaRestSioDisconnectReason::ServerDisconnect: return TEXT("io server disconnect");
	case EVaRestSioDisconnectReason::PingTimeout: return TEXT("ping timeout");
	case EVaRestSioDisconnectReason::ParseError: return TEXT("parse error");
	default: return TEXT("transport close");
	}
}
static FName ReasonName(EVaRestSioDisconnectReason Reason)
{
	static const FName Names[] = {TEXT("ClientDisconnect"), TEXT("SocketClosed"), TEXT("SessionReplaced"), TEXT("ServerDisconnect"), TEXT("PingTimeout"), TEXT("ParseError"), TEXT("TransportClose")};
	return Names[static_cast<uint8>(Reason)];
}
static bool AuthLooking(const FVaRestSioConnectError& Failure)
{
	const auto Code = Failure.Data.Get(TEXT("code"));
	if ((Code.IsNumber() && (Code.AsInt() == 401 || Code.AsInt() == 403))
		|| (Code.IsString() && (Same(Code.AsString(), TEXTVIEW("401")) || Same(Code.AsString(), TEXTVIEW("403"))))) return true;
	for (const TCHAR* Word : {TEXT("unauthori"), TEXT("forbidden"), TEXT("jwt"), TEXT("token"), TEXT("expired"), TEXT("auth")})
		if (Failure.Message.Contains(Word, ESearchCase::IgnoreCase)) return true;
	return false;
}
} // namespace VaRest::Sio

struct FVaRestSioStage::FImpl
{
	struct FNamespace
	{
		FString Name, Sid, Pid, Offset;
		FName LastTraceReason;
		FVaRestSioNamespaceConfig Config;
		EVaRestSioNamespaceState State = EVaRestSioNamespaceState::Disconnected;
		bool bWanted = false, bSent = false, bLeaveOnAck = false, bRefreshing = false;
		bool bRefreshSpent = false, bSentSession = false, bRecovered = false, bEverConnected = false;
		bool bTimeoutReported = false;
		bool bHasOffset = false;
		double RejoinAt = 0.0;
		uint64 Cycle = 0;
		int32 RetryAttempts = 0;
		FVaRestTimerHandle RejoinTimer, RetryTimer;
		TOptional<FVaRestError> Refusal;
		FVaRestSioConnectError RefusalEvent;
		FVaRestSioStats Counters;
		bool bRecoveryAttempted = false;
	};
	FVaRestSioStage& Owner;
	TStrongObjectPtr<UObject> OwnerState;
	TUniqueFunction<void()> RetireOwnerState;
	TSharedRef<FVaRestSioCodec, ESPMode::ThreadSafe> Codec;
	FVaRestSioStageOptions Options;
	TArray<TSharedRef<FNamespace>> Namespaces;
	TOptional<FVaRestSocketStageContext> Context;
	FVaRestSocketOpen Open;
	FString ConfigError, AttemptToken, RedactedUrl;
	bool bOpen = false, bHaveOpen = false, bTerminal = false, bUpgrade400Spent = false, bStarted = false;
	bool bInternalConnect = false;
	bool bRetired = false;
	int32 DispatchDepth = 0, NoResponseCount = 0, ProtocolFailures = 0;
	double OpenedAt = 0.0;
	FVaRestTimerHandle EmptyTimer;
	FRandomStream Random{FMath::Rand()};
	int64 OpenPayloadLimit = 1000000;
	bool bOpenPayloadAdvertised = false;
	FVaRestSioStats Counters;

	FImpl(FVaRestSioStage& InOwner, TSharedRef<FVaRestSioCodec, ESPMode::ThreadSafe> InCodec, FVaRestSioStageOptions InOptions)
		: Owner(InOwner), Codec(MoveTemp(InCodec)), Options(MoveTemp(InOptions)) {}
	TSharedPtr<FNamespace> Find(FStringView Name) const
	{
		for (const auto& Row : Namespaces) if (VaRest::Sio::Same(Row->Name, Name)) return Row;
		return {};
	}
	bool Current() const { return Context.IsSet() && Context->IsCurrent(); }
	bool AnyWanted() const { for (const auto& Row : Namespaces) if (Row->bWanted) return true; return false; }
	bool AnyConnected() const { for (const auto& Row : Namespaces) if (Row->State == EVaRestSioNamespaceState::Connected) return true; return false; }
	int64 ApplySendLimit()
	{
		const int64 Limit = bHaveOpen && bOpenPayloadAdvertised ? FMath::Min(OpenPayloadLimit, Options.MaxOutboundFrameBytes)
			: (Options.bOverrideFallbackPayloadLimit ? Options.MaxOutboundFrameBytes : FMath::Min<int64>(1000000, Options.MaxOutboundFrameBytes));
		Codec->SetOutboundFrameLimit(Limit); if (Current()) Context->SetSendLimit(Limit); return Limit;
	}
	bool Defer(TUniqueFunction<void(FVaRestSioStage&)>&& Function)
	{
		if (!Current()) return false;
		return Context->Timer(0.0, [Weak = Owner.AsWeak(), Function = MoveTemp(Function)]() mutable
		{ if (auto Self = Weak.Pin()) Function(*Self); }).Id != 0;
	}
	void Log(FStringView Text) const
	{
		if (Current() && Context->GetLogLevel() >= EVaRestLogLevel::Basic)
		{
			const FString Safe = Context->ScrubText(Text); UE_LOG(LogVaRestSio, Log, TEXT("%s"), *Safe);
		}
	}
	void Fail(EVaRestErrorKind Kind, FString Text, bool bRetry = false, int32 Code = 1002)
	{
		if (!Current()) return;
		const auto Control = Context.GetValue();
		bTerminal = !bRetry;
		Log(Text);
		Control.Close(Code, MoveTemp(Text), bRetry ? EVaRestReconnect::Always : EVaRestReconnect::Never, Kind);
	}
	void ClearRecovery()
	{ for (const auto& Row : Namespaces) { Row->Pid.Reset(); Row->Offset.Reset(); Row->bHasOffset = false; Row->bRecovered = false; } }
	bool SendControl(uint8 Type, FStringView Name, const FVaRestJson& Payload = {})
	{
		if (!Current()) return false;
		auto Encoded = Codec->EncodeControl(Type, Name, Payload);
		if (!Encoded.HasValue()) { Fail(EVaRestErrorKind::Config, Encoded.GetError().Message); return false; }
		for (auto& Frame : Encoded.GetValue()) if (!Context->SendBypass(MoveTemp(Frame)))
		{ Fail(EVaRestErrorKind::Connection, TEXT("Socket.IO control frame admission failed"), true, 1001); return false; }
		return true;
	}
	bool RememberValues(const FVaRestJson& Root)
	{
		// Bounded iterative walk; CONNECT values also join the per-attempt text redactor.
		TArray<FVaRestJson> Pending; Pending.Add(Root); int32 Visited = 0;
		while (!Pending.IsEmpty())
		{
			if (++Visited > 4096 || !Current()) return false;
			auto Value = Pending.Pop(EAllowShrinking::No);
			if (Value.IsString()) { if (!Context->RememberSecret(Value.AsString())) return false; }
			else if (Value.IsBool() || Value.IsNumber()) { if (!Context->RememberSecret(Value.ToString())) return false; }
			else if (Value.IsObject()) { for (const auto Member : Value.Members()) { if (Pending.Num() >= 4096) return false; Pending.Add(Member.Value); } }
			else if (Value.IsArray()) { for (int32 I = 0; I < Value.Num(); ++I) { if (Pending.Num() >= 4096) return false; Pending.Add(Value.At(I)); } }
		}
		return true;
	}
	void EmptyLater(EVaRestErrorKind Kind = EVaRestErrorKind::None, FString Detail = TEXT("no namespace"))
	{
		if (!Current() || AnyWanted() || EmptyTimer.Id) return;
		EmptyTimer = Context->Timer(0.0, [Weak = Owner.AsWeak(), Kind, Detail = MoveTemp(Detail)]() mutable
		{
			if (auto Self = Weak.Pin())
			{
				auto& Data = *Self->Impl; Data.EmptyTimer.Invalidate();
				if (!Data.AnyWanted()) Data.Fail(Kind, MoveTemp(Detail), false, 1000);
			}
		});
		if (!EmptyTimer.Id) Fail(EVaRestErrorKind::Config, TEXT("Socket.IO namespace close timer admission failed"));
	}
	void DisconnectEvent(const TSharedRef<FNamespace>& Row, EVaRestSioDisconnectReason Reason, const FVaRestSocketClose& Close)
	{
		FVaRestSioDisconnect Event; Event.Namespace = Row->Name; Event.Reason = Reason;
		Event.ReasonText = VaRest::Sio::ReasonText(Reason); Event.Close = Close; Event.bWillReconnect = Close.bWillReconnect;
		Log(FString::Printf(TEXT("namespace %s disconnected: %s, reconnect=%d, read-paused=%d"), *Row->Name, *Event.ReasonText, Event.bWillReconnect, Close.bWhileReadPaused));
		if (Current())
		{
			FVaRestTraceSocketNote Note; Note.Kind = EVaRestTraceSocketNoteKind::Channel; Note.Channel = Row->Name;
			Note.Reason = VaRest::Sio::ReasonName(Reason); Row->LastTraceReason = Note.Reason; Note.ChannelState = EVaRestTraceSocketChannelState::Down;
			Context->EmitNote(MoveTemp(Note));
		}
		const auto Callback = Row->Config.OnDisconnect; if (Callback && Current()) Callback(Event);
	}
	void Refused(const TSharedRef<FNamespace>& Row, FVaRestError Failure, FVaRestSioConnectError Event)
	{
		Row->bWanted = false; Row->bSent = false; Row->bRefreshing = false; Row->bLeaveOnAck = false;
		Row->State = EVaRestSioNamespaceState::Disconnected; Row->Pid.Reset(); Row->Offset.Reset(); Row->bHasOffset = false; Row->Sid.Reset(); ++Row->Cycle;
		Event.bWillRetry = false;
		const auto Control = Context.GetValue();
		FVaRestSocketClose Close; Close.Attempt = Open.Attempt; Close.Reason = Failure.Message;
		Control.ChannelDown(Row->Name, Close, EVaRestChannelEnd::Ended);
		FVaRestTraceSocketNote Note; Note.Kind = EVaRestTraceSocketNoteKind::Channel; Note.Channel = Row->Name;
		Row->LastTraceReason = TEXT("ConnectError");
		Note.ChannelState = EVaRestTraceSocketChannelState::Refused; Note.ErrorKind = Failure.Kind; Note.Message = Failure.Message;
		Control.EmitNote(MoveTemp(Note));
		Log(FString::Printf(TEXT("namespace %s connect error: %s"), *Row->Name, *Failure.Message));
		const auto Callback = Row->Config.OnConnectError; if (Callback && Current()) Callback(Failure, Event);
		if (!Control.IsCurrent()) return;
		Control.ReportError(Row->Name, Failure);
		if (Control.IsCurrent()) EmptyLater(Failure.Kind, Failure.Message);
	}
	void Refresh(const TSharedRef<FNamespace>& Row, bool bReactive)
	{
		if (Row->bRefreshing || !Current()) return;
		Row->bRefreshing = true;
		if (bReactive) Row->bRefreshSpent = true;
		const uint64 Cycle = Row->Cycle;
		const auto Control = Context.GetValue();
		const bool bAdmitted = Control.RefreshAuth([Weak = Owner.AsWeak(), Row, Cycle, bReactive](EVaRestAuthRefreshOutcome Outcome, EVaRestErrorKind Kind)
		{
			if (auto Self = Weak.Pin())
			{
				auto& Data = *Self->Impl; TGuardValue<int32> Dispatch(Data.DispatchDepth, Data.DispatchDepth + 1);
				if (!Data.Current() || !Row->bWanted || Row->Cycle != Cycle) return;
				Row->bRefreshing = false;
				if (Outcome == EVaRestAuthRefreshOutcome::Refreshed)
				{
					Row->bSent = false;
					if (Data.Options.Auth.Mode == EVaRestSioAuthPlacement::Header || Data.Options.Auth.Mode == EVaRestSioAuthPlacement::Query) Data.Context->RestartAfterRefresh();
					else Data.SendConnect(Row, true);
				}
				else
				{
					if (Outcome == EVaRestAuthRefreshOutcome::Failed && (Kind == EVaRestErrorKind::Connection || Kind == EVaRestErrorKind::Timeout))
					{
						Row->bRefreshSpent = false; Row->bSent = false;
						FVaRestSioConnectError Event; Event.Namespace = Row->Name; Event.Message = TEXT("Socket.IO auth refresh is temporarily unavailable"); Event.bWillRetry = Data.Options.NamespaceRetry.bEnabled;
						const auto Failure = VaRest::Sio::Error(Kind, Event.Message); const auto Callback = Row->Config.OnConnectError;
						if (Callback) Callback(Failure, Event);
						if (Data.Current()) Data.Fail(Kind, Event.Message, Event.bWillRetry, 1001);
						return;
					}
					if (Outcome == EVaRestAuthRefreshOutcome::Unavailable && Row->Refusal.IsSet())
					{ Data.Refused(Row, Row->Refusal.GetValue(), Row->RefusalEvent); return; }
					FVaRestSioConnectError Event; Event.Namespace = Row->Name; Event.Message = TEXT("Socket.IO session refresh could not provide credentials");
					auto Failure = VaRest::Sio::Error(Outcome == EVaRestAuthRefreshOutcome::Failed && Kind != EVaRestErrorKind::None ? Kind : EVaRestErrorKind::Auth, Event.Message);
					Data.Refused(Row, Failure, Event);
				}
			}
		});
		if (!bAdmitted)
		{
			Row->bRefreshing = false;
			FVaRestSioConnectError Event; Event.Namespace = Row->Name; Event.Message = TEXT("Socket.IO auth continuation admission failed");
			Refused(Row, VaRest::Sio::Error(EVaRestErrorKind::Config, Event.Message), Event);
		}
	}
	void SendConnect(const TSharedRef<FNamespace>& Row, bool bSkipProactive = false)
	{
		if (!bHaveOpen || !Current() || !Row->bWanted || Row->bSent || Row->bRefreshing || Row->RetryTimer.Id) return;
		if (Context->Now() < Row->RejoinAt)
		{
			if (!Row->RejoinTimer.Id)
			{
				Row->RejoinTimer = Context->Timer(Row->RejoinAt - Context->Now(), [Weak = Owner.AsWeak(), Row]()
				{ if (auto Self = Weak.Pin()) { Row->RejoinTimer.Invalidate(); Self->Impl->SendConnect(Row); } });
				if (!Row->RejoinTimer.Id) Fail(EVaRestErrorKind::Config, TEXT("Socket.IO rejoin timer admission failed"));
			}
			return;
		}
		if (!bSkipProactive && Row->Config.bSendSession && Options.Auth.Mode != EVaRestSioAuthPlacement::None && Context->NeedsAuthRefresh())
		{ Refresh(Row, false); return; }
		FVaRestJson Payload = FVaRestJson::Object();
		if (Options.bRecovery && !Row->Pid.IsEmpty() && Row->bHasOffset) Payload.Set(TEXT("pid"), Row->Pid).Set(TEXT("offset"), Row->Offset);
		if (Row->Config.AuthPayload.IsValid())
		{
			if (!Row->Config.AuthPayload.IsObject()) { Fail(EVaRestErrorKind::Config, TEXT("Socket.IO auth payload must be an object")); return; }
			for (const auto Member : Row->Config.AuthPayload.Members()) Payload.Set(Member.Key, Member.Value);
		}
		const FString SessionToken = Context->Session().AccessToken;
		const FString Token = !SessionToken.IsEmpty() ? SessionToken : AttemptToken;
		Row->bSentSession = Row->Config.bSendSession && Options.Auth.Mode != EVaRestSioAuthPlacement::None && !Token.IsEmpty();
		if (Row->bSentSession && Options.Auth.Mode == EVaRestSioAuthPlacement::Payload) Payload.Set(Options.Auth.Name, Options.Auth.Prefix + Token);
		const auto Hook = Row->Config.OnConnectAuth;
		if (Hook)
		{
			FVaRestSioConnectContext Info; Info.Namespace = Row->Name; Info.Attempt = Open.Attempt; Info.bReconnect = Open.bReconnect;
			const auto Control = Context.GetValue(); TGuardValue<int32> Dispatch(DispatchDepth, DispatchDepth + 1); Hook(Payload, Info);
			if (!Control.IsCurrent()) return;
		}
		if (!Payload.IsObject()) { Fail(EVaRestErrorKind::Config, TEXT("OnConnectAuth must leave a JSON object")); return; }
		if (!RememberValues(Payload)) { Fail(EVaRestErrorKind::Config, TEXT("Socket.IO CONNECT secret registration exceeded its bound")); return; }
		if (Context->GetLogLevel() >= EVaRestLogLevel::Headers)
		{
			FString Keys; for (const auto& Key : Payload.GetKeys()) { if (!Keys.IsEmpty()) Keys += TEXT(", "); Keys += Key + TEXT("=***"); }
			Log(FString::Printf(TEXT("CONNECT %s {%s}"), *Row->Name, *Keys));
		}
		Context->HoldInbound(Row->Name);
		if (SendControl(0, Row->Name, Payload.Num() ? Payload : FVaRestJson()))
		{ Row->bSent = true; Row->bRecoveryAttempted = Options.bRecovery && !Row->Pid.IsEmpty() && Row->bHasOffset; Row->bTimeoutReported = false; Context->ResetReadyTimeout(Row->Name); }
	}
	void ConnectError(const TSharedRef<FNamespace>& Row, const FVaRestJson& Root)
	{
		if (!Row->bSent || Row->State == EVaRestSioNamespaceState::Connected) return;
		++Counters.ConnectErrors; ++Row->Counters.ConnectErrors;
		if (Row->bRecoveryAttempted) { ++Counters.RecoveriesFailed; ++Row->Counters.RecoveriesFailed; Row->bRecoveryAttempted = false; }
		Row->bSent = false;
		FVaRestSioConnectError Event; Event.Namespace = Row->Name; Event.Message = Root.Get(TEXT("message")).AsString(); Event.Data = Root.Get(TEXT("data"));
		if (Event.Message.IsEmpty()) Event.Message = TEXT("Socket.IO namespace refused the connection");
		FVaRestError Failure = VaRest::Sio::Error(EVaRestErrorKind::Server, Event.Message); Failure.Body = Event.Data;
		const auto Code = Event.Data.Get(TEXT("code")); if (Code.IsString()) Failure.Code = Code.AsString(); else if (Code.IsNumber()) Failure.Code = Code.ToString();
		if (Event.Message.Equals(TEXT("Invalid namespace"), ESearchCase::IgnoreCase))
		{
			Failure.Kind = EVaRestErrorKind::Config;
			Failure.Message += TEXT("; the URL path selects a namespace; use WithPath or asset Path for the engine path (default /socket.io/)");
			Refused(Row, Failure, Event); return;
		}
		if (!Row->bWanted || Row->bLeaveOnAck) { Refused(Row, Failure, Event); return; }
		EVaRestSioRetry Policy = EVaRestSioRetry::Default;
		const auto Predicate = Row->Config.ConnectErrorIf;
		if (Predicate)
		{
			const auto Control = Context.GetValue(); Policy = Predicate(Event); if (!Control.IsCurrent()) return;
		}
		const bool bAuthDefault = Row->bSentSession && !Row->bRefreshSpent
			&& (Options.Auth.Mode == EVaRestSioAuthPlacement::Payload || VaRest::Sio::AuthLooking(Event));
		if ((Policy == EVaRestSioRetry::RefreshAndRetry && !Row->bRefreshSpent) || (Policy == EVaRestSioRetry::Default && bAuthDefault))
		{ Row->Refusal = Failure; Row->RefusalEvent = Event; Refresh(Row, true); return; }
		if (Policy == EVaRestSioRetry::Retry)
		{
			const auto& Retry = Options.NamespaceRetry;
			if (Row->RetryAttempts < MAX_int32) ++Row->RetryAttempts;
			if (!Retry.bEnabled || (Retry.MaxAttempts > 0 && Row->RetryAttempts >= Retry.MaxAttempts)) { Refused(Row, Failure, Event); return; }
			const double Raw = FMath::Min(Retry.MaxDelay, Retry.BaseDelay * FMath::Pow(Retry.Multiplier, FMath::Min(1024, Row->RetryAttempts - 1)));
			const double Delay = Retry.Jitter == EVaRestSocketJitter::Full ? Raw * Random.FRand() : Raw * (1.0 + Retry.JitterRatio * (2.0 * Random.FRand() - 1.0));
			const uint64 Cycle = Row->Cycle;
			Row->RetryTimer = Context->Timer(FMath::Clamp(Delay, 0.0, Retry.MaxDelay), [Weak = Owner.AsWeak(), Row, Cycle]()
			{ if (auto Self = Weak.Pin(); Self && Row->Cycle == Cycle) { Row->RetryTimer.Invalidate(); Self->Impl->SendConnect(Row); } });
			if (!Row->RetryTimer.Id) { Failure.Kind = EVaRestErrorKind::Config; Failure.Message = TEXT("Socket.IO namespace retry timer admission failed"); Refused(Row, Failure, Event); return; }
			Event.bWillRetry = true; const auto Callback = Row->Config.OnConnectError; if (Callback) Callback(Failure, Event);
			return;
		}
		Refused(Row, Failure, Event);
	}
};

FVaRestSioStage::FVaRestSioStage(TSharedRef<FVaRestSioCodec, ESPMode::ThreadSafe> Codec, FVaRestSioStageOptions Options)
	: Impl(MakeUnique<FImpl>(*this, MoveTemp(Codec), MoveTemp(Options)))
{
	check(IsInGameThread());
	Impl->Codec->RequireEngineOpen();
	FString Invalid;
	if (!Impl->Codec->ValidateOptions(Invalid)) Impl->ConfigError = MoveTemp(Invalid);
	if (!FVaRestSioCodec::IsValidNamespace(Impl->Options.DefaultNamespace)) Impl->ConfigError = TEXT("Invalid Socket.IO default namespace");
	if (Impl->Options.MaxNamespaces < 1 || Impl->Options.MaxNamespaces > 4096 || Impl->Options.MaxOutboundFrameBytes < 1
		|| !FMath::IsFinite(Impl->Options.ReadyTimeout) || Impl->Options.ReadyTimeout <= 0.0
		|| !FMath::IsFinite(Impl->Options.StableAfter) || Impl->Options.StableAfter < 0.0) Impl->ConfigError = TEXT("Invalid Socket.IO stage limits or timeout");
	if (Impl->Options.Path.IsEmpty() || !Impl->Options.Path.StartsWith(TEXT("/")) || Impl->Options.Path.Contains(TEXT("?")) || Impl->Options.Path.Contains(TEXT("#"))) Impl->ConfigError = TEXT("Socket.IO engine path must be an absolute path without query or fragment");
	if (!Impl->Options.Path.EndsWith(TEXT("/"))) Impl->Options.Path += TEXT("/");
	if ((Impl->Options.Auth.Mode == EVaRestSioAuthPlacement::Payload || Impl->Options.Auth.Mode == EVaRestSioAuthPlacement::Query) && Impl->Options.Auth.Name.IsEmpty()) Impl->ConfigError = TEXT("Socket.IO auth field cannot be empty");
	if (static_cast<uint8>(Impl->Options.Auth.Mode) > static_cast<uint8>(EVaRestSioAuthPlacement::None)) Impl->ConfigError = TEXT("Invalid Socket.IO auth placement");
	if (Impl->Options.Auth.Mode == EVaRestSioAuthPlacement::Query && VaRest::Sio::ReservedQuery(Impl->Options.Auth.Name)) Impl->ConfigError = TEXT("Socket.IO auth cannot use an Engine.IO reserved query key");
	const auto& Retry = Impl->Options.NamespaceRetry;
	if (Retry.MaxAttempts < 0 || !FMath::IsFinite(Retry.BaseDelay) || Retry.BaseDelay < 0.0 || !FMath::IsFinite(Retry.MaxDelay) || Retry.MaxDelay < 0.0
		|| !FMath::IsFinite(Retry.Multiplier) || Retry.Multiplier < 1.0 || !FMath::IsFinite(Retry.JitterRatio) || Retry.JitterRatio < 0.0 || Retry.JitterRatio > 1.0) Impl->ConfigError = TEXT("Invalid Socket.IO namespace retry policy");
	ConfigureNamespace(Impl->Options.DefaultNamespace);
}
FVaRestSioStage::~FVaRestSioStage()
{
	OnRetire();
}
void FVaRestSioStage::OnRetire()
{
	check(IsInGameThread()); if (Impl->bRetired) return; Impl->bRetired = true;
	Impl->Context.Reset(); Impl->bOpen = false; Impl->bHaveOpen = false; Impl->AttemptToken.Reset();
	for (const auto& Row : Impl->Namespaces) { Row->Config = {}; Row->bWanted = false; Row->bRefreshing = false; ++Row->Cycle; }
	Impl->Namespaces.Reset();
	if (Impl->RetireOwnerState) { auto Retire = MoveTemp(Impl->RetireOwnerState); Retire(); }
	Impl->OwnerState.Reset();
}
void FVaRestSioStage::OnSessionChanged(EVaRestSessionChange Change, FVaRestSocketStageContext& Context)
{
	check(IsInGameThread()); if (Change == EVaRestSessionChange::Replaced || Change == EVaRestSessionChange::Cleared) Impl->ClearRecovery();
}

bool FVaRestSioStage::ConfigureNamespace(FString Name, FVaRestSioNamespaceConfig Config)
{
	check(IsInGameThread());
	if (Impl->bRetired || !FVaRestSioCodec::IsValidNamespace(Name) || (Config.AuthPayload.IsValid() && !Config.AuthPayload.IsObject())) return false;
	if (Impl->DispatchDepth) return Impl->Defer([Name = MoveTemp(Name), Config = MoveTemp(Config)](auto& Self) mutable { Self.ConfigureNamespace(MoveTemp(Name), MoveTemp(Config)); });
	auto Row = Impl->Find(Name);
	if (!Row)
	{
		if (Impl->Namespaces.Num() >= Impl->Options.MaxNamespaces) return false;
		auto Added = MakeShared<FImpl::FNamespace>(); Added->Name = MoveTemp(Name); Impl->Namespaces.Add(Added); Row = Added;
	}
	Config.AuthPayload = Config.AuthPayload.Clone(); Row->Config = MoveTemp(Config); return true;
}
void FVaRestSioStage::KeepOwnerStateOnGT(UObject* State, TUniqueFunction<void()>&& Retire)
{
	check(IsInGameThread());
	if (Impl->bRetired) { if (Retire) Retire(); return; }
	if (Impl->RetireOwnerState) { auto Previous = MoveTemp(Impl->RetireOwnerState); Previous(); }
	Impl->OwnerState.Reset(State); Impl->RetireOwnerState = MoveTemp(Retire);
}
bool FVaRestSioStage::UpdateOptions(FVaRestSioStageOptions Options)
{
	check(IsInGameThread());
	if (Impl->bRetired) return false;
	if (!VaRest::Sio::Same(Options.DefaultNamespace, Impl->Options.DefaultNamespace) || Options.MaxNamespaces < Impl->Namespaces.Num()
		|| Options.MaxNamespaces > 4096 || Options.MaxOutboundFrameBytes < 1 || Options.MaxOutboundFrameBytes > MAX_int32 - VaRest::WebSocket::FrameHeadroom
		|| !FMath::IsFinite(Options.ReadyTimeout) || Options.ReadyTimeout <= 0 || !FMath::IsFinite(Options.StableAfter) || Options.StableAfter < 0
		|| !Options.Path.StartsWith(TEXT("/")) || Options.Path.Contains(TEXT("?")) || Options.Path.Contains(TEXT("#"))) return false;
	if ((Options.Auth.Mode == EVaRestSioAuthPlacement::Payload || Options.Auth.Mode == EVaRestSioAuthPlacement::Query) && Options.Auth.Name.IsEmpty()) return false;
	if (static_cast<uint8>(Options.Auth.Mode) > static_cast<uint8>(EVaRestSioAuthPlacement::None)) return false;
	if (Options.Auth.Mode == EVaRestSioAuthPlacement::Query && VaRest::Sio::ReservedQuery(Options.Auth.Name)) return false;
	const auto& Retry = Options.NamespaceRetry;
	if (Retry.MaxAttempts < 0 || !FMath::IsFinite(Retry.BaseDelay) || Retry.BaseDelay < 0 || !FMath::IsFinite(Retry.MaxDelay) || Retry.MaxDelay < 0
		|| !FMath::IsFinite(Retry.Multiplier) || Retry.Multiplier < 1 || !FMath::IsFinite(Retry.JitterRatio) || Retry.JitterRatio < 0 || Retry.JitterRatio > 1) return false;
	if (Impl->DispatchDepth) return Impl->Defer([Options = MoveTemp(Options)](auto& Self) mutable { Self.UpdateOptions(MoveTemp(Options)); });
	if (!Options.Path.EndsWith(TEXT("/"))) Options.Path += TEXT("/");
	if (Impl->Current() && !Options.Auth.Name.IsEmpty() && !Impl->Context->AddRedactedKey(Options.Auth.Name)) return false;
	if (!Options.bRecovery) Impl->ClearRecovery();
	Impl->Options = MoveTemp(Options); Impl->ApplySendLimit(); return true;
}
bool FVaRestSioStage::ConnectNamespace(FStringView Name)
{
	check(IsInGameThread());
	const auto Row = Impl->Find(Name); if (!Row) return false;
	if (Impl->DispatchDepth || (!Impl->bInternalConnect && Impl->Current() && Impl->Context->IsDispatching())) return Impl->Defer([Name = FString(Name)](auto& Self) { Self.ConnectNamespace(Name); });
	Impl->bStarted = true;
	if (Row->bSent) { Row->bWanted = true; Row->bLeaveOnAck = false; if (Row->State == EVaRestSioNamespaceState::Disconnected) Row->State = EVaRestSioNamespaceState::Connecting; return true; }
	if (Row->bWanted) return true;
	Row->bWanted = true; Row->bLeaveOnAck = false; Row->bRefreshSpent = false; Row->bRefreshing = false; ++Row->Cycle;
	Row->RetryAttempts = 0;
	Row->Refusal.Reset(); Row->RefusalEvent = {};
	if (Impl->Current()) { Impl->Context->CancelTimer(Row->RetryTimer); Impl->Context->CancelTimer(Row->RejoinTimer); }
	Row->State = EVaRestSioNamespaceState::Connecting; Impl->bTerminal = false;
	if (Impl->Current()) { Impl->Context->HoldInbound(Row->Name); Impl->Context->CancelTimer(Impl->EmptyTimer); Impl->SendConnect(Row.ToSharedRef()); }
	return true;
}
bool FVaRestSioStage::DisconnectNamespace(FStringView Name)
{
	check(IsInGameThread()); const auto Row = Impl->Find(Name); if (!Row) return false;
	if (Impl->DispatchDepth || (Impl->Current() && Impl->Context->IsDispatching())) return Impl->Defer([Name = FString(Name)](auto& Self) { Self.DisconnectNamespace(Name); });
	if (!Row->bWanted) return true;
	const bool bConnected = Row->State == EVaRestSioNamespaceState::Connected;
	Row->bWanted = false; Row->bRefreshing = false; Row->bLeaveOnAck = Row->bSent && !bConnected;
	Row->State = EVaRestSioNamespaceState::Disconnected; Row->Pid.Reset(); Row->Offset.Reset(); Row->bHasOffset = false; Row->Sid.Reset(); ++Row->Cycle;
	if (Impl->Current())
	{
		Impl->Context->CancelTimer(Row->RetryTimer); Impl->Context->CancelTimer(Row->RejoinTimer);
		TGuardValue<int32> Dispatch(Impl->DispatchDepth, Impl->DispatchDepth + 1);
		if (bConnected) { if (!Impl->SendControl(1, Row->Name)) return false; Row->bSent = false; Row->RejoinAt = Impl->Context->Now() + FMath::Max(0.1, Impl->Context->GetLatency()); }
		FVaRestSocketClose Close; Close.Code = 1000; Close.By = EVaRestSocketCloseBy::Client; Close.Reason = TEXT("io client disconnect");
		Impl->Context->ChannelDown(Row->Name, Close, EVaRestChannelEnd::Ended);
		if (bConnected) Impl->DisconnectEvent(Row.ToSharedRef(), EVaRestSioDisconnectReason::ClientDisconnect, Close);
		Impl->EmptyLater();
	}
	return true;
}
void FVaRestSioStage::ConnectDefaults()
{
	check(IsInGameThread()); ConnectNamespace(Impl->Options.DefaultNamespace);
	for (const auto& Row : Impl->Namespaces) if (Row->Config.bConnectOnStart) ConnectNamespace(Row->Name);
}
void FVaRestSioStage::ResetRecovery() { check(IsInGameThread()); Impl->ClearRecovery(); }
EVaRestSioNamespaceState FVaRestSioStage::GetNamespaceState(FStringView Name) const
{ check(IsInGameThread()); const auto Row = Impl->Find(Name); return Row ? Row->State : EVaRestSioNamespaceState::Disconnected; }
FVaRestSioConnect FVaRestSioStage::GetNamespaceInfo(FStringView Name) const
{
	check(IsInGameThread()); FVaRestSioConnect Out; if (const auto Row = Impl->Find(Name))
	{ Out.Namespace = Row->Name; Out.Sid = Row->Sid; Out.bRecovered = Row->bRecovered; Out.bReconnect = Impl->Open.bReconnect; Out.Attempt = Impl->Open.Attempt; } return Out;
}
TArray<FString> FVaRestSioStage::GetNamespaces() const
{ check(IsInGameThread()); TArray<FString> Out; for (const auto& Row : Impl->Namespaces) Out.Add(Row->Name); return Out; }
FVaRestSioStats FVaRestSioStage::GetStats(FStringView Namespace) const
{
	check(IsInGameThread());
	if (!Namespace.IsEmpty())
	{
		const auto Row = Impl->Find(Namespace); if (!Row) return {};
		auto Out = Row->Counters; Out.ConnectedNamespaces = Row->State == EVaRestSioNamespaceState::Connected ? 1 : 0; return Out;
	}
	auto Out = Impl->Counters; for (const auto& Row : Impl->Namespaces) Out.ConnectedNamespaces += Row->State == EVaRestSioNamespaceState::Connected ? 1 : 0; return Out;
}
void FVaRestSioStage::RecordAckTimeout(FStringView Namespace)
{
	check(IsInGameThread()); ++Impl->Counters.AcksTimedOut; if (const auto Row = Impl->Find(Namespace)) ++Row->Counters.AcksTimedOut;
}
FString FVaRestSioStage::RecoveryOffsetFor(const FVaRestSocketMessage& Message) const
{
	check(IsInGameThread()); if (!Impl->Options.bRecovery || Message.bHasId || Message.Kind != EVaRestSocketMessageKind::Message) return {};
	const auto Row = Impl->Find(Message.Channel); const auto Args = Message.Data();
	return Row && !Row->Pid.IsEmpty() && Args.Num() && Args.At(Args.Num() - 1).IsString() ? Args.At(Args.Num() - 1).AsString() : FString();
}

void FVaRestSioStage::OnAttach(FVaRestSocketStageContext& Context)
{
	if (Impl->bRetired) { Context.Close(1000, TEXT("Socket.IO stage is retired"), EVaRestReconnect::Never, EVaRestErrorKind::Canceled); return; }
	check(IsInGameThread()); Impl->Context = Context;
	Impl->ApplySendLimit();
	if (!Context.SetDefaultChannel(Impl->Options.DefaultNamespace)) Impl->ConfigError = TEXT("Attach the Socket.IO stage before freezing channel identities");
	if (!Context.AddRedactedKey(TEXT("pid")) || !Context.AddRedactedKey(TEXT("sid"))
		|| (!Impl->Options.Auth.Name.IsEmpty() && !Context.AddRedactedKey(Impl->Options.Auth.Name))) Impl->ConfigError = TEXT("Socket.IO protocol redaction registration failed");
	for (const auto& Name : Context.GetChannels()) if (!Impl->Find(Name)) ConfigureNamespace(Name);
}
void FVaRestSioStage::OnHandshake(FVaRestSocketHandshake& Handshake, FVaRestSocketStageContext& Context)
{
	if (Impl->bRetired) { Context.Close(1000, TEXT("Socket.IO stage is retired"), EVaRestReconnect::Never, EVaRestErrorKind::Canceled); return; }
	check(IsInGameThread()); Impl->Context = Context; Impl->bOpen = false; Impl->bHaveOpen = false; Impl->bTerminal = false; Impl->EmptyTimer.Invalidate();
	Impl->ApplySendLimit();
	if (!Impl->ConfigError.IsEmpty()) { Impl->Fail(EVaRestErrorKind::Config, Impl->ConfigError); return; }
	if (!Impl->bStarted) { TGuardValue<bool> Internal(Impl->bInternalConnect, true); ConnectDefaults(); }
	for (const auto& Row : Impl->Namespaces)
	{
		Row->bSent = false; Row->bRefreshing = false; Row->bTimeoutReported = false; Row->RejoinTimer.Invalidate(); Row->RetryTimer.Invalidate(); Row->RejoinAt = 0.0; ++Row->Cycle;
		if (Row->bLeaveOnAck) { Row->bWanted = false; Row->bLeaveOnAck = false; }
		Row->State = Row->bWanted ? EVaRestSioNamespaceState::Connecting : EVaRestSioNamespaceState::Disconnected;
	}
	const int32 Scheme = Handshake.Url.Find(TEXT("://"));
	if (Scheme == INDEX_NONE) { Impl->Fail(EVaRestErrorKind::Config, TEXT("Socket.IO URL needs an absolute http(s) or ws(s) origin")); return; }
	const int32 AuthorityStart = Scheme + 3;
	int32 AuthorityEnd = AuthorityStart;
	while (AuthorityEnd < Handshake.Url.Len() && Handshake.Url[AuthorityEnd] != '/' && Handshake.Url[AuthorityEnd] != '?' && Handshake.Url[AuthorityEnd] != '#') ++AuthorityEnd;
	FString Query;
	const int32 QueryAt = Handshake.Url.Find(TEXT("?"), ESearchCase::CaseSensitive);
	if (QueryAt != INDEX_NONE) { Query = Handshake.Url.Mid(QueryAt + 1); const int32 Fragment = Query.Find(TEXT("#")); if (Fragment != INDEX_NONE) Query.LeftInline(Fragment); }
	Handshake.Url = Handshake.Url.Left(AuthorityEnd) + Impl->Options.Path;
	if (!Query.IsEmpty())
	{
		TArray<FString> Parts; Query.ParseIntoArray(Parts, TEXT("&"), true);
		for (const auto& Part : Parts)
		{
			FString Key, Value; if (!Part.Split(TEXT("="), &Key, &Value)) Key = Part;
			// Raw encoded query is preserved for the core URL builder, which performs canonical validation.
			if (!VaRest::Sio::ReservedQuery(VaRest::Http::PercentDecode(Key, true))) { if (!Handshake.Url.Contains(TEXT("?"))) Handshake.Url += TEXT("?"); else Handshake.Url += TEXT("&"); Handshake.Url += Part; }
			else Impl->Log(TEXTVIEW("removed EIO/transport/sid from the Socket.IO URL query"));
		}
	}
	Handshake.Query.RemoveAll([](const auto& Pair) { return VaRest::Sio::ReservedQuery(Pair.Key); });
	Handshake.Query.Emplace(TEXT("EIO"), TEXT("4")); Handshake.Query.Emplace(TEXT("transport"), TEXT("websocket"));
	const FString Authorization = Handshake.Headers.Get(TEXTVIEW("Authorization"));
	Impl->AttemptToken = Authorization.StartsWith(TEXT("Bearer "), ESearchCase::IgnoreCase) ? Authorization.Mid(7) : Context.Session().AccessToken;
	if (!Context.RememberSecret(Impl->AttemptToken)) { Impl->Fail(EVaRestErrorKind::Config, TEXT("Socket.IO bearer secret registration failed")); return; }
	if (Impl->Options.Auth.Mode != EVaRestSioAuthPlacement::Header && Authorization.StartsWith(TEXT("Bearer "), ESearchCase::IgnoreCase)) Handshake.Headers.Remove(TEXTVIEW("Authorization"));
	if (Impl->Options.Auth.Mode == EVaRestSioAuthPlacement::Query && !Impl->AttemptToken.IsEmpty())
	{
		Handshake.Query.RemoveAll([&](const auto& Pair) { return VaRest::Sio::Same(Pair.Key, Impl->Options.Auth.Name); });
		const FString Value = Impl->Options.Auth.Prefix + Impl->AttemptToken;
		if (!Context.RememberSecret(Value)) { Impl->Fail(EVaRestErrorKind::Config, TEXT("Socket.IO query secret registration failed")); return; }
		Handshake.Query.Emplace(Impl->Options.Auth.Name, Value);
	}
	Impl->RedactedUrl = Context.RedactUrl(Handshake.Url);
}
void FVaRestSioStage::OnOpen(const FVaRestSocketOpen& Open, FVaRestSocketStageContext& Context)
{
	check(IsInGameThread()); Impl->Context = Context; Impl->Open = Open; Impl->OpenedAt = Context.Now(); Impl->bOpen = true;
	Impl->NoResponseCount = 0;
	for (const auto& Row : Impl->Namespaces) if (Row->bWanted) Context.HoldInbound(Row->Name);
}
EVaRestSocketStageResult FVaRestSioStage::OnInbound(const FVaRestSocketMessage& Message, FVaRestSocketStageContext& Context)
{
	check(IsInGameThread()); Impl->Context = Context; TGuardValue<int32> Dispatch(Impl->DispatchDepth, Impl->DispatchDepth + 1);
	const auto Tag = Message.ProtocolTag;
	if (Tag == TEXT("sio:EVENT") || Tag == TEXT("sio:BINARY_EVENT"))
	{
		++Impl->Counters.EventsIn; if (const auto Row = Impl->Find(Message.Channel)) ++Row->Counters.EventsIn;
		VaRest::Sio::Stats::EventIn();
	}
	if (!Message.Attachments.IsEmpty())
	{
		int64 Bytes = 0; for (const auto& Attachment : Message.Attachments) if (Attachment) Bytes += Attachment->Num();
		Impl->Counters.AttachmentsIn += Message.Attachments.Num(); Impl->Counters.AttachmentBytesIn += Bytes;
		if (const auto Row = Impl->Find(Message.Channel)) { Row->Counters.AttachmentsIn += Message.Attachments.Num(); Row->Counters.AttachmentBytesIn += Bytes; }
	}
	if (!Impl->bHaveOpen)
	{
		if (Tag != TEXT("eio:open")) { Impl->Fail(EVaRestErrorKind::Config, TEXT("not an Engine.IO v4 endpoint: first data packet is not OPEN")); return EVaRestSocketStageResult::Consumed; }
		const auto Root = Message.Data(); const auto Sid = Root.Get(TEXT("sid"));
		const auto Interval = Root.Get(TEXT("pingInterval")).TryGet<int64>(), Timeout = Root.Get(TEXT("pingTimeout")).TryGet<int64>();
		if (!Root.IsObject() || !Sid.IsString() || Sid.AsString().IsEmpty() || !Interval.IsSet() || !Timeout.IsSet() || Interval.GetValue() <= 0 || Timeout.GetValue() <= 0)
		{ Impl->Fail(EVaRestErrorKind::Config, TEXT("invalid Engine.IO OPEN: sid and positive integer pingInterval/pingTimeout required")); return EVaRestSocketStageResult::Consumed; }
		const double PingInterval = FMath::Max(0.05, Interval.GetValue() / 1000.0), PingTimeout = FMath::Max(0.05, Timeout.GetValue() / 1000.0);
		const auto Advertised = Root.Get(TEXT("maxPayload")); Impl->bOpenPayloadAdvertised = Advertised.IsValid();
		Impl->OpenPayloadLimit = 1000000;
		if (Advertised.IsValid())
		{
			const auto Number = Advertised.TryGet<double>();
			if (!Number.IsSet() || !FMath::IsFinite(Number.GetValue()) || Number.GetValue() < 1.0 || Number.GetValue() > static_cast<double>(MAX_int64))
			{ Impl->Fail(EVaRestErrorKind::Config, TEXT("invalid Engine.IO OPEN maxPayload")); return EVaRestSocketStageResult::Consumed; }
			Impl->OpenPayloadLimit = static_cast<int64>(FMath::Min(Number.GetValue(), static_cast<double>(MAX_int32)));
		}
		else
		{
			UE_LOG(LogVaRestSio, Verbose, TEXT("Engine.IO OPEN omitted maxPayload; applying the 1 MB default or explicit fallback override"));
		}
		if (!Context.RememberSecret(Sid.AsString())) { Impl->Fail(EVaRestErrorKind::Config, TEXT("Engine.IO sid secret registration failed")); return EVaRestSocketStageResult::Consumed; }
		if (!Context.SetIdleDeadline(PingInterval + PingTimeout)) { Impl->Fail(EVaRestErrorKind::Config, TEXT("transport refused the Engine.IO service idle deadline")); return EVaRestSocketStageResult::Consumed; }
		if (Context.GetLogLevel() >= EVaRestLogLevel::Basic && (PingInterval < 1.0 || PingTimeout < 1.0)) { UE_LOG(LogVaRestSio, Warning, TEXT("Engine.IO advertises sub-second heartbeat timings (%g/%g s)"), PingInterval, PingTimeout); }
		Impl->bHaveOpen = true; const int64 Limit = Impl->ApplySendLimit();
		const double Required = (Limit + 262144.0) * 8.0 / PingTimeout;
		if (Context.GetLogLevel() >= EVaRestLogLevel::Basic && Required > 1000000.0) { UE_LOG(LogVaRestSio, Warning, TEXT("Engine.IO maxPayload=%lld and pingTimeout=%g s need uplink above %g Mbit/s for a full frame and pong"), Limit, PingTimeout, Required / 1000000.0); }
		const auto Snapshot = Impl->Namespaces; for (const auto& Row : Snapshot) { Impl->SendConnect(Row); if (!Context.IsCurrent()) break; }
		return EVaRestSocketStageResult::Consumed;
	}
	if (Tag == TEXT("eio:open")) { Impl->ClearRecovery(); Impl->Fail(EVaRestErrorKind::Decode, TEXT("second Engine.IO OPEN in one connection"), true); return EVaRestSocketStageResult::Consumed; }
	if (Tag == TEXT("eio:ping"))
	{
		if (!Message.bAutoReplied)
		{
			auto Pong = FVaRestSocketFrame::Text(TEXTVIEW("3")); auto Metadata = MakeShared<FVaRestSocketFrameMetadata, ESPMode::ThreadSafe>();
			Metadata->ProtocolTag = TEXT("eio:pong"); Metadata->Kind = EVaRestSocketMessageKind::Control; Pong.Metadata = MoveTemp(Metadata);
			if (!Context.SendBypass(MoveTemp(Pong))) Context.MarkDead(TEXT("Engine.IO fallback pong admission failed"));
		}
		return EVaRestSocketStageResult::Consumed;
	}
	if (Tag == TEXT("eio:close")) { Context.Close(1000, TEXT("Engine.IO close"), EVaRestReconnect::Always); return EVaRestSocketStageResult::Consumed; }
	if (Tag == TEXT("eio:pong") || Tag == TEXT("eio:noop") || Tag == TEXT("eio:upgrade")) return EVaRestSocketStageResult::Consumed;
	if (Tag == TEXT("sio:CONNECT") && (!Message.Data().Get(TEXT("sid")).IsString() || Message.Data().Get(TEXT("sid")).AsString().IsEmpty()))
	{ Impl->ClearRecovery(); Impl->Fail(EVaRestErrorKind::Config, TEXT("the server speaks Socket.IO v2 (EIO=3); VaRest supports Socket.IO 3.x and 4.x servers")); return EVaRestSocketStageResult::Consumed; }
	if (Tag == TEXT("sio:CONNECT_ERROR") && Message.Data().IsString())
	{ Impl->ClearRecovery(); Impl->Fail(EVaRestErrorKind::Config, TEXT("the server speaks Socket.IO v2 (EIO=3); VaRest supports Socket.IO 3.x and 4.x servers")); return EVaRestSocketStageResult::Consumed; }
	const auto Row = Impl->Find(Message.Channel);
	if (Tag == TEXT("sio:CONNECT"))
	{
		if (!Row || !Row->bSent) { Impl->SendControl(1, Message.Channel); Impl->Log(TEXTVIEW("unrequested namespace CONNECT ack; sent DISCONNECT")); return EVaRestSocketStageResult::Consumed; }
		if (Row->State == EVaRestSioNamespaceState::Connected) { Impl->Log(TEXTVIEW("duplicate namespace CONNECT ack ignored")); return EVaRestSocketStageResult::Consumed; }
		if (!Row->bWanted || Row->bLeaveOnAck)
		{ Impl->SendControl(1, Row->Name); Row->bSent = false; Row->bLeaveOnAck = false; Row->RejoinAt = Context.Now() + FMath::Max(0.1, Context.GetLatency()); return EVaRestSocketStageResult::Consumed; }
		const FString Pid = Impl->Options.bRecovery ? Message.Data().Get(TEXT("pid")).AsString() : FString();
		if (!Context.RememberSecret(Pid)) { Impl->Fail(EVaRestErrorKind::Config, TEXT("Socket.IO recovery pid registration failed")); return EVaRestSocketStageResult::Consumed; }
		Row->bRecovered = !Pid.IsEmpty() && VaRest::Sio::Same(Pid, Row->Pid); if (!Row->bRecovered) { Row->Offset.Reset(); Row->bHasOffset = false; }
		if (Row->bRecoveryAttempted)
		{
			if (Row->bRecovered) { ++Impl->Counters.RecoveriesSucceeded; ++Row->Counters.RecoveriesSucceeded; }
			else { ++Impl->Counters.RecoveriesFailed; ++Row->Counters.RecoveriesFailed; }
			Row->bRecoveryAttempted = false;
		}
		Row->Pid = Pid; Row->Sid = Message.Data().Get(TEXT("sid")).AsString(); Row->State = EVaRestSioNamespaceState::Connected; Row->bEverConnected = true;
		auto Info = Impl->Open; Info.PublicId = Row->Sid; Info.bRecovered = Row->bRecovered;
		Impl->Log(FString::Printf(TEXT("namespace %s connected sid=%s recovered=%d"), *Row->Name, *Row->Sid, Row->bRecovered));
		Context.ChannelUp(Row->Name, MoveTemp(Info)); return EVaRestSocketStageResult::Consumed;
	}
	if (!Row || (!Row->bWanted && !Row->bLeaveOnAck)) return EVaRestSocketStageResult::Consumed;
	if (Tag == TEXT("sio:CONNECT_ERROR")) { Impl->ConnectError(Row.ToSharedRef(), Message.Data()); return EVaRestSocketStageResult::Consumed; }
	if (Tag == TEXT("sio:DISCONNECT"))
	{
		const bool bConnected = Row->State == EVaRestSioNamespaceState::Connected;
		FVaRestSioConnectError Event; Event.Namespace = Row->Name; Event.Message = TEXT("io server disconnect");
		if (!bConnected) { Impl->Refused(Row.ToSharedRef(), VaRest::Sio::Error(EVaRestErrorKind::Server, Event.Message), Event); return EVaRestSocketStageResult::Consumed; }
		Row->bWanted = false; Row->bSent = false; Row->State = EVaRestSioNamespaceState::Disconnected; Row->Pid.Reset(); Row->Offset.Reset(); Row->bHasOffset = false; Row->Sid.Reset(); ++Row->Cycle;
		FVaRestSocketClose Close; Close.Code = 1000; Close.By = EVaRestSocketCloseBy::Server; Close.Reason = TEXT("io server disconnect");
		Context.ChannelDown(Row->Name, Close, EVaRestChannelEnd::Ended); Impl->DisconnectEvent(Row.ToSharedRef(), EVaRestSioDisconnectReason::ServerDisconnect, Close);
		if (Context.IsCurrent()) Impl->EmptyLater(EVaRestErrorKind::Server, Close.Reason);
		return EVaRestSocketStageResult::Consumed;
	}
	if (Row->State == EVaRestSioNamespaceState::Disconnected) return EVaRestSocketStageResult::Consumed;
	return EVaRestSocketStageResult::Continue;
}
void FVaRestSioStage::OnDispatched(const FVaRestSocketMessage& Message, FVaRestSocketStageContext& Context)
{
	if (!Impl->Options.bRecovery || Message.bHasId || (Message.Kind != EVaRestSocketMessageKind::Message && Message.Kind != EVaRestSocketMessageKind::Request)) return;
	if (const auto Row = Impl->Find(Message.Channel); Row && !Row->Pid.IsEmpty() && Row->bWanted)
	{
		const auto Args = Message.Data(); if (Args.Num() && Args.At(Args.Num() - 1).IsString()) { Row->Offset = Args.At(Args.Num() - 1).AsString(); Row->bHasOffset = true; }
	}
}
void FVaRestSioStage::ClassifyClose(const FVaRestSocketClose& Close, FVaRestSocketStageCloseDecision& Decision, FVaRestSocketStageContext& Context)
{
	Impl->Context = Context;
	if (Impl->bTerminal) { Decision.bRetry = false; return; }
	if (Impl->bOpen && !Impl->bHaveOpen && (Close.Code == 1002 || Close.Code == 1007))
	{ Decision.bRetry = false; Decision.Kind = EVaRestErrorKind::Config; Decision.Detail = TEXT("not an Engine.IO v4 endpoint: invalid first data packet or OPEN"); return; }
	const bool bStable = Impl->bOpen && Impl->Current() && Impl->Context->Now() - Impl->OpenedAt >= Impl->Options.StableAfter;
	if (bStable) { Impl->ProtocolFailures = 0; Impl->bUpgrade400Spent = false; }
	if (Close.Failure == EVaRestSocketFailure::NoUpgradeResponse)
	{
		++Impl->NoResponseCount; Decision.bRetry = Impl->NoResponseCount < 3; Decision.Kind = EVaRestErrorKind::Config;
		Decision.Detail = FString::Printf(TEXT("no Engine.IO server at %s; the engine path (WithPath / asset Path, default /socket.io/) is not the namespace"), *Impl->RedactedUrl); return;
	}
	Impl->NoResponseCount = 0;
	if (Close.Failure == EVaRestSocketFailure::UpgradeRefused && Close.HttpStatus == 400)
	{
		const bool bRefresh = !Impl->bUpgrade400Spent && !Impl->AttemptToken.IsEmpty()
			&& (Impl->Options.Auth.Mode == EVaRestSioAuthPlacement::Header || Impl->Options.Auth.Mode == EVaRestSioAuthPlacement::Query);
		Impl->bUpgrade400Spent = true; Decision.bRetry = bRefresh; Decision.bRefresh = bRefresh; Decision.Kind = bRefresh ? EVaRestErrorKind::Auth : EVaRestErrorKind::Http;
		Decision.Detail = TEXT("Engine.IO refused the upgrade (400): websocket transport not enabled, refused by allowRequest or middleware (Origin, auth), or bad request"); return;
	}
	if (Close.By != EVaRestSocketCloseBy::Server && (Close.Code == 1002 || Close.Code == 1007 || Close.Code == 1009))
	{
		Impl->ClearRecovery(); Decision.Kind = EVaRestErrorKind::Decode;
		if (Close.Code != 1009) ++Impl->ProtocolFailures;
		Decision.bRetry = Close.Code == 1009 || Impl->ProtocolFailures < 3;
	}
}
void FVaRestSioStage::OnSent(const FVaRestSocketFrameMetadata& Metadata, EVaRestSocketFrameKind Kind, int64 Bytes, FVaRestSocketStageContext& Context)
{
	check(IsInGameThread()); if (Impl->bRetired) return;
	const auto Row = Impl->Find(Metadata.Channel);
	if (Kind == EVaRestSocketFrameKind::Text && Metadata.ProtocolTag == TEXT("eio:pong")) ++Impl->Counters.PingsAnswered;
	if (Kind == EVaRestSocketFrameKind::Text && (Metadata.ProtocolTag == TEXT("sio:EVENT") || Metadata.ProtocolTag == TEXT("sio:BINARY_EVENT")))
	{ ++Impl->Counters.EventsOut; if (Row) ++Row->Counters.EventsOut; VaRest::Sio::Stats::EventOut(); }
	if (Kind == EVaRestSocketFrameKind::Binary && !Metadata.AttachmentSizes.IsEmpty())
	{
		++Impl->Counters.AttachmentsOut; Impl->Counters.AttachmentBytesOut += Bytes;
		if (Row) { ++Row->Counters.AttachmentsOut; Row->Counters.AttachmentBytesOut += Bytes; }
	}
}
void FVaRestSioStage::OnConnectionClosed(const FVaRestSocketClose& Close, FVaRestSocketStageContext& Context)
{
	Impl->Context = Context; TGuardValue<int32> Dispatch(Impl->DispatchDepth, Impl->DispatchDepth + 1);
	Impl->bOpen = false; Impl->bHaveOpen = false; Impl->AttemptToken.Reset();
	const auto Snapshot = Impl->Namespaces;
	for (const auto& Row : Snapshot)
	{
		const bool bConnected = Row->State == EVaRestSioNamespaceState::Connected;
		if (Row->bLeaveOnAck) Row->bWanted = false;
		Row->bSent = false; Row->bRefreshing = false; Row->bLeaveOnAck = false; Row->Sid.Reset(); Row->RejoinTimer.Invalidate(); Row->RetryTimer.Invalidate(); ++Row->Cycle;
		Row->State = Row->bWanted && Close.bWillReconnect ? EVaRestSioNamespaceState::Connecting : EVaRestSioNamespaceState::Disconnected;
		if (!Close.bWillReconnect) { Row->bWanted = false; Row->Pid.Reset(); Row->Offset.Reset(); Row->bHasOffset = false; }
		if (bConnected)
		{
			const auto Reason = Close.By == EVaRestSocketCloseBy::Heartbeat ? EVaRestSioDisconnectReason::PingTimeout
				: ((Close.Code == 1002 || Close.Code == 1007 || Close.Code == 1009) ? EVaRestSioDisconnectReason::ParseError : EVaRestSioDisconnectReason::TransportClose);
			Impl->DisconnectEvent(Row, Reason, Close); if (!Context.IsCurrent()) break;
		}
	}
}
void FVaRestSioStage::OnGiveUp(FVaRestSocketStageContext& Context)
{
	Impl->bStarted = false;
	Impl->ClearRecovery(); Impl->bOpen = false; Impl->bHaveOpen = false; Impl->AttemptToken.Reset();
	for (const auto& Row : Impl->Namespaces) { Row->bWanted = false; Row->bSent = false; Row->bRefreshing = false; Row->Sid.Reset(); Row->State = EVaRestSioNamespaceState::Disconnected; ++Row->Cycle; }
}
void FVaRestSioStage::OnClosing(FVaRestSocketStageContext& Context, const FVaRestSocketCloseRequest& Request)
{
	if (!Request.bSessionReplacement) Impl->bStarted = false;
	Impl->Context = Context; TGuardValue<int32> Dispatch(Impl->DispatchDepth, Impl->DispatchDepth + 1);
	const auto Snapshot = Impl->Namespaces;
	for (const auto& Row : Snapshot)
	{
		const bool bConnected = Row->State == EVaRestSioNamespaceState::Connected;
		if (bConnected) Impl->SendControl(1, Row->Name);
		Row->bSent = false; Row->bLeaveOnAck = false; Row->bRefreshing = false; Row->Pid.Reset(); Row->Offset.Reset(); Row->bHasOffset = false; Row->Sid.Reset(); ++Row->Cycle;
		if (!Request.bSessionReplacement) Row->bWanted = false;
		Row->State = Row->bWanted ? EVaRestSioNamespaceState::Connecting : EVaRestSioNamespaceState::Disconnected;
		if (bConnected)
		{
			FVaRestSocketClose Close; Close.Code = Request.Code; Close.Reason = Request.Reason; Close.By = EVaRestSocketCloseBy::Client; Close.bWillReconnect = Request.bSessionReplacement;
			Impl->DisconnectEvent(Row, Request.bSessionReplacement ? EVaRestSioDisconnectReason::SessionReplaced : EVaRestSioDisconnectReason::SocketClosed, Close);
			if (!Context.IsCurrent()) break;
		}
	}
	Impl->bOpen = false; Impl->bHaveOpen = false; Impl->AttemptToken.Reset();
}
void FVaRestSioStage::OnChannelAdded(FStringView Channel, FVaRestSocketStageContext& Context)
{
	Impl->Context = Context;
	if (!Impl->Find(Channel) && !ConfigureNamespace(FString(Channel))) { Impl->Fail(EVaRestErrorKind::Config, TEXT("Socket.IO namespace registration refused")); return; }
	if (const auto Row = Impl->Find(Channel); Row && Row->bWanted) Context.HoldInbound(Channel);
}
bool FVaRestSioStage::OnInboundOverflow(FStringView Channel, FVaRestSocketStageContext& Context)
{
	Impl->Context = Context; Impl->ClearRecovery(); Impl->Fail(EVaRestErrorKind::Decode, TEXT("Socket.IO namespace inbound hold overflow"), true, 1009); return true;
}
bool FVaRestSioStage::OnChannelReadyTimeout(FStringView Channel, FVaRestSocketStageContext& Context)
{
	Impl->Context = Context;
	const auto Row = Impl->Find(Channel); if (!Row || !Row->bWanted || Row->State == EVaRestSioNamespaceState::Connected || Row->bTimeoutReported) return true;
	Row->bTimeoutReported = true; TGuardValue<int32> Dispatch(Impl->DispatchDepth, Impl->DispatchDepth + 1);
	++Impl->Counters.ConnectErrors; ++Row->Counters.ConnectErrors;
	FVaRestSioConnectError Event; Event.Namespace = Row->Name; Event.Message = TEXT("Socket.IO namespace CONNECT timed out"); Event.bWillRetry = true;
	const auto Failure = VaRest::Sio::Error(EVaRestErrorKind::Timeout, Event.Message);
	const auto Callback = Row->Config.OnConnectError; if (Callback) Callback(Failure, Event);
	if (Context.IsCurrent()) { Context.ReportError(Row->Name, Failure); if (Context.IsCurrent() && !Impl->AnyConnected()) Impl->Fail(EVaRestErrorKind::Timeout, Event.Message, true, 1001); }
	return true;
}

void FVaRestSioStage::AppendTraceSnapshot(FVaRestTraceSnapshot& Snapshot,uint64 SocketId,uint64 AttemptId,const FVaRestRedactor& Redactor) const
{
 check(IsInGameThread()); if (Impl->bRetired) return;
 for (const auto& Row:Impl->Namespaces)
 {
  FVaRestTraceSocketNote Note; Note.SocketId=SocketId; Note.AttemptId=AttemptId; Note.Kind=EVaRestTraceSocketNoteKind::Channel; Note.Channel=Row->Name;
  Note.ChannelState=Row->State==EVaRestSioNamespaceState::Connected ? EVaRestTraceSocketChannelState::Up : Row->Refusal.IsSet() ? EVaRestTraceSocketChannelState::Refused : EVaRestTraceSocketChannelState::Down;
  Note.PublicId=Redactor.ScrubText(Row->Sid); Note.bRecovered=Row->bRecovered;
  Note.Reason=Row->State==EVaRestSioNamespaceState::Connected ? FName(TEXT("Connected")) : Row->State==EVaRestSioNamespaceState::Connecting ? FName(TEXT("Connecting")) : Row->LastTraceReason;
  if (Row->Refusal.IsSet()) { Note.ErrorKind=Row->Refusal->Kind; Note.Message=Redactor.ScrubText(Row->Refusal->Message); }
  Snapshot.SocketNotes.Add(MoveTemp(Note));
 }
}
