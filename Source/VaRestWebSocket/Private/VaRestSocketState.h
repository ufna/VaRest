// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "VaRestClock.h"
#include "VaRestSocket.h"
#include "VaRestSocketTransport.h"
#include "StructUtils/InstancedStruct.h"
#include <atomic>
#include "VaRestSocketState.generated.h"

class UVaRestSocketState;
struct FVaRestTraceSnapshot;

/** GTK request-builder input: reflected GC reaches nested references and the descriptor until synchronous encoding. */
UCLASS(Transient)
class UVaRestSocketStructPayload final : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(Transient) FInstancedStruct Value;
};
namespace VaRest::WebSocket
{
	struct FSocketModuleGate { std::atomic<bool> bStopped{false}; };
	class FSocketOutbox;
	/** No affine payload: dropping the last token on a worker only schedules a weak GT disposal. */
	struct FSocketHandle
	{
		~FSocketHandle();
		TWeakObjectPtr<UVaRestSocketState> State;
		TSharedPtr<FSocketModuleGate, ESPMode::ThreadSafe> Gate;
		TSharedPtr<FSocketOutbox, ESPMode::ThreadSafe> Outbox;
		std::atomic<bool> bDisposed{false};
	};
	struct FSocketData;
	/** UHT's vtable helper must destroy this member without seeing the private payload definition. */
	struct FSocketDataDeleter
	{
		void operator()(FSocketData* Pointer) const;
	};
	struct FSocketAccess
	{
		static FVaRestSocket CreateOnGT(UVaRestHub* CoreHub, FName Name, FString Url,
			TSharedPtr<IVaRestSocketTransport, ESPMode::ThreadSafe> Transport = {},
			TSharedPtr<IVaRestClock, ESPMode::ThreadSafe> Clock = {}, int32 Seed = 0);
		static UVaRestSocketState* StateOnGT(const FVaRestSocket& Socket);
		static void DisposeOnGT(const FVaRestSocket& Socket, FStringView Reason);
		static void SetEntryOnGT(const FVaRestSocket& Socket, FName Entry);
	};
	void StartSocketsOnGT();
	void StopSocketsOnGT();
}

/** Affine state lives in the real core Hub's retention map, never in a shared-pointer deleter. */
UCLASS(Transient)
class UVaRestSocketState final : public UObject
{
	GENERATED_BODY()
public:
	UVaRestSocketState();
	virtual ~UVaRestSocketState() override;
	void TickOnGT();
	void DisposeOnGT(FStringView Reason);
	void BackgroundOnGT(bool bBackground);
	bool IsDisposedOnGT() const;
	FVaRestSocketStats GetStatsOnGT() const;
	FString DescribeOnGT() const;
	bool MatchesConsoleKeyOnGT(FStringView Key) const;
	bool AbortOnGT(FStringView Reason);
	void ContributeSnapshotOnGT(FVaRestTraceSnapshot& Snapshot);
	virtual void BeginDestroy() override;
private:
	friend class FVaRestSocket;
	friend class FVaRestSocketJsonRequest;
	friend class FVaRestSocketStageContext;
	friend class FVaRestSocketChannel;
	friend struct VaRest::WebSocket::FSocketAccess;
	friend struct VaRest::WebSocket::FSocketData;
	TUniquePtr<VaRest::WebSocket::FSocketData, VaRest::WebSocket::FSocketDataDeleter> Data;
};
