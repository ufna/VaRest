// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "VaRestSioSocket.h"
#include "VaRestSioStage.h"
#include "VaRestSioState.generated.h"

namespace VaRest::Sio
{
struct FLogicalAck;
template <typename Signature> struct TObserver
{
	TWeakObjectPtr<const UObject> Owner;
	bool bOwned = false, bActive = true, bOnce = false;
	TFunction<Signature> Function;
	bool Alive() const { return bActive && (!bOwned || Owner.IsValid()); }
};
template <typename Signature> using TObservers = TArray<TSharedRef<TObserver<Signature>>>;
struct FNamespaceView
{
	FString Name;
	FVaRestSioNamespaceConfig Config;
	TObservers<void(const FVaRestSioConnect&)> Connect;
	TObservers<void(const FVaRestError&, const FVaRestSioConnectError&)> Error;
	TObservers<void(const FVaRestSioDisconnect&)> Disconnect;
	TObservers<void(FVaRestJson&, const FVaRestSioConnectContext&)> Auth;
	struct FRoute
	{
		FString Event;
		TSharedRef<TObserver<void(const FVaRestSocketMessage&)>> Observer;
	};
	TArray<FRoute> Routes;
	TObservers<void(const FVaRestSocketMessage&)> Any, Unhandled;
};
}

/** Strongly owned only by the GT protocol stage; never holds a strong native socket or client. */
UCLASS(Transient)
class UVaRestSioState final : public UObject
{
	GENERATED_BODY()
public:
	FVaRestSocketWeak Socket;
	TWeakPtr<FVaRestSioStage> Stage;
	FVaRestSioStageOptions Options;
	FVaRestSioLimits Limits;
	TSharedPtr<FVaRestSioCodec, ESPMode::ThreadSafe> Codec;
	double AckTimeout = 10.0;
	uint64 NextAckSlot = 0;
	TMap<uint64, TSharedRef<VaRest::Sio::FLogicalAck>> Acks;
	TArray<TSharedRef<VaRest::Sio::FNamespaceView>> Namespaces;
	VaRest::Sio::TObservers<void(const FVaRestSioConnect&)> AnyConnect;
	VaRest::Sio::TObservers<void(const FVaRestError&, const FVaRestSioConnectError&)> AnyConnectError;
	VaRest::Sio::TObservers<void(const FVaRestSioDisconnect&)> AnyDisconnect;
	VaRest::Sio::TObservers<void(const FVaRestSocketMessage&)> AnyEvent;
	TWeakObjectPtr<class UVaRestSioClient> BlueprintClient;
	int32 CallbackCount = 0, DispatchDepth = 0;
	bool bRetired = false;
	void RetireOnGT();
	TSharedPtr<VaRest::Sio::FNamespaceView> Find(FStringView Name) const;
	TSharedPtr<VaRest::Sio::FNamespaceView> Ensure(FStringView Name);
	bool Apply(const TSharedRef<VaRest::Sio::FNamespaceView>& Row);
	bool UpdateOptions(FVaRestSioStageOptions NewOptions);
	void Dispatch(const FVaRestSocketMessage& Message);
	void Off(FStringView Namespace, FStringView Event, const UObject* Owner, bool bAll);
	void OffAny(FStringView Namespace, const UObject* Owner);
	void OffGlobal(const UObject* Owner);
	void ReportConfig(FString Message) const;
	void Prune();
	FVaRestSioStats GetStatsOnGT(FStringView Namespace = {}) const;
	bool AddRoute(const TSharedRef<VaRest::Sio::FNamespaceView>& Row, FStringView Event, const UObject* Owner, TFunction<void(const FVaRestSocketMessage&)> Handler, bool bOnce);
	template <typename Signature, typename... A> void NotifyBound(const VaRest::Sio::TObservers<Signature>& List, const FVaRestSocketBatchPin& DispatchPin, A&&... Args)
	{
		const auto Native = Socket.Pin(); const auto Snapshot = List;
		for (const auto& Entry : Snapshot)
		{
			if (!Native.IsProtocolDispatchCurrentOnGT(DispatchPin)) break;
			if (Entry->Alive()) { if (Entry->bOnce) Entry->bActive = false; auto Function = Entry->Function; Function(Forward<A>(Args)...); }
		}
	}
	template <typename Signature> bool Add(VaRest::Sio::TObservers<Signature>& Into, const UObject* Owner, TFunction<Signature> Function, bool bOnce = false)
	{
		Prune();
		if (!Function || CallbackCount >= 4096) return false;
		auto Entry = MakeShared<VaRest::Sio::TObserver<Signature>>(); Entry->Owner = Owner; Entry->bOwned = Owner != nullptr;
		Entry->bOnce = bOnce; Entry->Function = MoveTemp(Function); Into.Add(MoveTemp(Entry)); ++CallbackCount; return true;
	}
	virtual void BeginDestroy() override;
};
