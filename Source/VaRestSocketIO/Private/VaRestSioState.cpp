// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSioState.h"
#include "VaRestSioLog.h"
#include "UObject/StrongObjectPtr.h"
#include "VaRestSioAckState.h"

namespace VaRest::Sio
{
template <typename Signature> int32 Remove(TObservers<Signature>& List, const UObject* Owner)
{
	return List.RemoveAll([&](const auto& Entry)
	{
		if (Entry->bOwned == (Owner != nullptr) && Entry->Owner.Get() == Owner) { Entry->bActive = false; return true; }
		return false;
	});
}
}

FVaRestSioStats UVaRestSioState::GetStatsOnGT(FStringView Namespace) const
{
	check(IsInGameThread()); const auto HeldStage = Stage.Pin(); if (bRetired || !HeldStage) return {};
	auto Out = HeldStage->GetStats(Namespace);
	for (const auto& Pair : Acks)
	{
		const auto& Ack = Pair.Value;
		if (Ack->bStarted && !Ack->bRetired && !Ack->Control->bCancelRequested.load(std::memory_order_acquire)
			&& (Namespace.IsEmpty() || Namespace.Equals(Ack->Namespace, ESearchCase::CaseSensitive))) ++Out.AcksPending;
	}
	const auto Transport = Socket.Pin().GetStats();
	Out.PingsAnswered = HeldStage->GetStats().PingsAnswered + Transport.AutoRepliesOut;
	Out.LongestPongDelay = Transport.LongestAutoReplyDelay; Out.bHasPongDelay = Transport.AutoRepliesOut > 0;
	return Out;
}

TSharedPtr<VaRest::Sio::FNamespaceView> UVaRestSioState::Find(FStringView Name) const
{
	check(IsInGameThread()); if (Name.IsEmpty()) Name = Options.DefaultNamespace;
	for (const auto& Row : Namespaces) if (Name.Equals(Row->Name, ESearchCase::CaseSensitive)) return Row;
	return {};
}
TSharedPtr<VaRest::Sio::FNamespaceView> UVaRestSioState::Ensure(FStringView Name)
{
	check(IsInGameThread()); if (const auto Existing = Find(Name)) return Existing;
	if (bRetired || !FVaRestSioCodec::IsValidNamespace(Name) || Namespaces.Num() >= Options.MaxNamespaces || !Stage.IsValid()) return {};
	const auto Native = Socket.Pin(); if (!Native.IsValid()) return {};
	const TStrongObjectPtr<UVaRestSioState> Pin(this);
	auto Row = MakeShared<VaRest::Sio::FNamespaceView>(); Row->Name = FString(Name); Namespaces.Add(Row);
	auto Channel = Native.Channel(Name);
	if (!Channel.IsValid() || !Apply(Row)) { Namespaces.Remove(Row); return {}; }
	const TWeakObjectPtr<UVaRestSioState> Weak(this);
	Channel.OnMessage(this, [Weak](const FVaRestSocketMessage& Message) { if (auto* State = Weak.Get()) State->Dispatch(Message); });
	Channel.OnReady(this, [Weak, Name = Row->Name](const FVaRestSocketOpen& Open)
	{
		if (auto* State = Weak.Get())
		{
			const TStrongObjectPtr<UVaRestSioState> StatePin(State); const auto Found = State->Find(Name); if (!Found) return;
			FVaRestSioConnect Info; Info.Namespace = Name; Info.Sid = Open.PublicId; Info.bRecovered = Open.bRecovered; Info.bReconnect = Open.bReconnect; Info.Attempt = Open.Attempt;
			TGuardValue<int32> DispatchGuard(State->DispatchDepth, State->DispatchDepth + 1);
			const auto DispatchPin = State->Socket.Pin().GetProtocolDispatchPinOnGT(Name);
			State->NotifyBound(State->AnyConnect, DispatchPin, Info);
			State->NotifyBound(Found->Connect, DispatchPin, Info);
		}
	});
	return Row;
}
bool UVaRestSioState::Apply(const TSharedRef<VaRest::Sio::FNamespaceView>& Row)
{
	const auto HeldStage = Stage.Pin(); if (!HeldStage) return false;
	auto Config = Row->Config; const TWeakObjectPtr<UVaRestSioState> Weak(this);
	Config.OnConnectError = [Weak, Name = Row->Name](const FVaRestError& Error, const FVaRestSioConnectError& Info)
	{
		if (auto* State = Weak.Get()) if (const auto Found = State->Find(Name))
		{
			const TStrongObjectPtr<UVaRestSioState> Pin(State); TGuardValue<int32> DispatchGuard(State->DispatchDepth, State->DispatchDepth + 1);
			const auto DispatchPin = State->Socket.Pin().GetProtocolDispatchPinOnGT(Name);
			State->NotifyBound(State->AnyConnectError, DispatchPin, Error, Info);
			State->NotifyBound(Found->Error, DispatchPin, Error, Info);
		}
	};
	Config.OnDisconnect = [Weak, Name = Row->Name](const FVaRestSioDisconnect& Info)
	{
		if (auto* State = Weak.Get()) if (const auto Found = State->Find(Name))
		{
			const TStrongObjectPtr<UVaRestSioState> Pin(State); TGuardValue<int32> DispatchGuard(State->DispatchDepth, State->DispatchDepth + 1);
			const auto DispatchPin = State->Socket.Pin().GetProtocolDispatchPinOnGT(Name);
			State->NotifyBound(State->AnyDisconnect, DispatchPin, Info);
			State->NotifyBound(Found->Disconnect, DispatchPin, Info);
		}
	};
	Config.OnConnectAuth = [Weak, Name = Row->Name](FVaRestJson& Payload, const FVaRestSioConnectContext& Info)
	{
		if (auto* State = Weak.Get()) if (const auto Found = State->Find(Name))
		{
			const TStrongObjectPtr<UVaRestSioState> Pin(State); TGuardValue<int32> DispatchGuard(State->DispatchDepth, State->DispatchDepth + 1);
			State->NotifyBound(Found->Auth, State->Socket.Pin().GetProtocolDispatchPinOnGT(Name), Payload, Info);
		}
	};
	return HeldStage->ConfigureNamespace(Row->Name, MoveTemp(Config));
}
bool UVaRestSioState::UpdateOptions(FVaRestSioStageOptions NewOptions)
{
	check(IsInGameThread()); if (auto HeldStage = Stage.Pin(); HeldStage && HeldStage->UpdateOptions(NewOptions)) { Options = MoveTemp(NewOptions); return true; }
	ReportConfig(TEXT("Socket.IO stage configuration refused")); return false;
}
void UVaRestSioState::ReportConfig(FString Message) const
{
	FVaRestError Error; Error.Kind = EVaRestErrorKind::Config; Error.Message = MoveTemp(Message);
	Socket.Pin().ReportProtocolErrorOnGT(MoveTemp(Error));
}
void UVaRestSioState::Prune()
{
	check(IsInGameThread());
	auto Clean = [&](auto& List) { CallbackCount -= List.RemoveAll([](const auto& Entry) { return !Entry->Alive(); }); };
	Clean(AnyConnect); Clean(AnyConnectError); Clean(AnyDisconnect); Clean(AnyEvent);
	for (const auto& Row : Namespaces)
	{
		Clean(Row->Connect); Clean(Row->Error); Clean(Row->Disconnect); Clean(Row->Auth); Clean(Row->Any); Clean(Row->Unhandled);
		CallbackCount -= Row->Routes.RemoveAll([](const auto& Route) { return !Route.Observer->Alive(); });
	}
}
bool UVaRestSioState::AddRoute(const TSharedRef<VaRest::Sio::FNamespaceView>& Row, FStringView Event, const UObject* Owner, TFunction<void(const FVaRestSocketMessage&)> Handler, bool bOnce)
{
	Prune();
	if (!Handler || !FVaRestSioCodec::IsValidEventName(Event) || CallbackCount >= 4096) return false;
	auto Entry = MakeShared<VaRest::Sio::TObserver<void(const FVaRestSocketMessage&)>>();
	Entry->Owner = Owner; Entry->bOwned = Owner != nullptr; Entry->bOnce = bOnce; Entry->Function = MoveTemp(Handler);
	Row->Routes.Add({FString(Event), Entry}); ++CallbackCount; return true;
}
void UVaRestSioState::Dispatch(const FVaRestSocketMessage& Message)
{
	check(IsInGameThread()); const auto Row = Find(Message.Channel); if (!Row || !Socket.Pin().IsValid()) return;
	const TStrongObjectPtr<UVaRestSioState> Pin(this); TGuardValue<int32> DispatchGuard(DispatchDepth, DispatchDepth + 1);
	const auto AnySnapshot = Row->Any, UnhandledSnapshot = Row->Unhandled; const auto Routes = Row->Routes;
	const auto Native = Socket.Pin(); const auto DispatchPin = Native.GetProtocolDispatchPinOnGT(Message.Channel);
	NotifyBound(AnyEvent, DispatchPin, Message);
	NotifyBound(AnySnapshot, DispatchPin, Message);
	if (!Native.IsProtocolDispatchCurrentOnGT(DispatchPin)) return;
	bool bMatched = false;
	for (const auto& Route : Routes) if (Route.Event.Equals(Message.Route, ESearchCase::CaseSensitive) && Route.Observer->Alive())
	{
		bMatched = true; if (Route.Observer->bOnce) Route.Observer->bActive = false; auto Function = Route.Observer->Function; Function(Message);
		if (!Native.IsProtocolDispatchCurrentOnGT(DispatchPin)) return;
	}
	if (!bMatched) NotifyBound(UnhandledSnapshot, DispatchPin, Message);
}
void UVaRestSioState::Off(FStringView Namespace, FStringView Event, const UObject* Owner, bool bAll)
{
	check(IsInGameThread()); const auto Row = Find(Namespace); if (!Row) return;
	CallbackCount -= Row->Routes.RemoveAll([&](const auto& Route)
	{
		if ((bAll || Event.Equals(Route.Event, ESearchCase::CaseSensitive)) && Route.Observer->bOwned == (Owner != nullptr) && Route.Observer->Owner.Get() == Owner)
		{ Route.Observer->bActive = false; return true; } return false;
	});
	if (bAll)
	{
		CallbackCount -= VaRest::Sio::Remove(Row->Connect, Owner) + VaRest::Sio::Remove(Row->Error, Owner) + VaRest::Sio::Remove(Row->Disconnect, Owner)
			+ VaRest::Sio::Remove(Row->Auth, Owner) + VaRest::Sio::Remove(Row->Any, Owner) + VaRest::Sio::Remove(Row->Unhandled, Owner);
	}
}
void UVaRestSioState::BeginDestroy()
{
	RetireOnGT(); Super::BeginDestroy();
}
void UVaRestSioState::OffAny(FStringView Namespace, const UObject* Owner)
{
	check(IsInGameThread()); if (const auto Row = Find(Namespace)) CallbackCount -= VaRest::Sio::Remove(Row->Any, Owner);
}
void UVaRestSioState::OffGlobal(const UObject* Owner)
{
	check(IsInGameThread());
	CallbackCount -= VaRest::Sio::Remove(AnyConnect, Owner) + VaRest::Sio::Remove(AnyConnectError, Owner)
		+ VaRest::Sio::Remove(AnyDisconnect, Owner) + VaRest::Sio::Remove(AnyEvent, Owner);
}
void UVaRestSioState::RetireOnGT()
{
	check(IsInGameThread()); if (bRetired) return; bRetired = true;
	const auto PendingAcks = Acks; for (const auto& Pair : PendingAcks) Pair.Value->Retire(); Acks.Reset(); Codec.Reset();
	auto Clear = [](auto& List) { for (const auto& Entry : List) { Entry->bActive = false; Entry->Function = nullptr; } List.Reset(); };
	Clear(AnyConnect); Clear(AnyConnectError); Clear(AnyDisconnect); Clear(AnyEvent); BlueprintClient.Reset();
	// A UObject becoming unreachable is insufficient: the Core Hub stops HTTP before a later GC may run.
	// Retire every user capture synchronously while its socket stage is being destroyed on GT.
	for (const auto& Row : Namespaces)
	{
		Clear(Row->Connect); Clear(Row->Error); Clear(Row->Disconnect); Clear(Row->Auth); Clear(Row->Any); Clear(Row->Unhandled);
		for (const auto& Route : Row->Routes) { Route.Observer->bActive = false; Route.Observer->Function = nullptr; }
		Row->Routes.Reset(); Row->Config = {};
	}
	Namespaces.Reset(); CallbackCount = 0; Stage.Reset(); Socket = {};
}
