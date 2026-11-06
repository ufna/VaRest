// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestSocketTransport.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace VaRest::WebSocket::PressureTesting
{

enum class EFenceStatus : uint8 { Pending, Complete, Refused, Stale, Destroyed, Canceled };

/** Published by one service-thread request; AckRequest releases the complete snapshot to its GT reader. */
struct FSnapshot
{
	uint64 Owner = 0, Connection = 0, Request = 0, Pass = 0;
	int32 Queued = 0, Messages = 0, ReplyDebt = 0;
	int64 Bytes = 0;
	bool bPaused = false;
};

/** Inputs precede the transition's own Flow slot. Records have one producer and are never overwritten. */
struct FTransition
{
	int32 Queued = 0, ReplyDebt = 0;
	int64 Bytes = 0;
	bool bWasPaused = false, bPaused = false;
};

/** Finite diagnostic window, committed once per entry by the service thread and never overwritten. */
struct FReceiveWitness
{
	uint64 MessageOrdinal = 0, Length = 0, Remaining = 0;
	int32 AssemblyBytes = 0, Queued = 0;
	bool bFinal = false, bBinary = false, bAssembling = false;
};

/** Native-only attachment. No GT owner, callback, UObject, clock or WSI is retained here. */
struct FProbe final
{
	explicit FProbe(uint64 InOwner) : Owner(InOwner) {}
	static constexpr int32 TransitionCapacity = 128;
	const uint64 Owner;
	std::atomic<uint64> Connection{0}, NextRequest{0}, ActiveRequest{0}, AckRequest{0};
	std::atomic<int32> InFlightRequests{0};
	std::atomic<EFenceStatus> AckStatus{EFenceStatus::Pending};
	std::atomic<bool> bCanceled{false}, bWsiDestroyed{false}, bRetired{false}, bRecordOverflow{false};
	std::atomic<const IVaRestSocketConnection*> HandleIdentity{nullptr};
	std::atomic<int32> PopLimit{MAX_int32}, PopCalls{0}, LastPopBudget{0}, PeakQueued{0}, TerminalQueued{0};
	std::atomic<int64> Published[7]{};
	std::atomic<int64> AckBytes{0}, AckMessages{0};
	std::atomic<int32> WakePosts{0}, WakeRuns{0}, WakeCoalesced{0}, WakeRefused{0};
	std::atomic<int32> RxDisables{0}, RxEnables{0}, TransitionCount{0};
	FSnapshot Snapshot;
	FTransition Transitions[TransitionCapacity]{};
	static constexpr int32 ReceiveWitnessCapacity = 64;
	uint64 ReceiveWitnessStartOrdinal = MAX_uint64; // GT writes only before Connect publishes this probe.
	std::atomic<int32> ReceiveWitnessCount{0};
	FReceiveWitness ReceiveWitnesses[ReceiveWitnessCapacity]{};

	void NotePush(EVaRestSocketEventType Type, int32 Queued);
	void NoteTransition(const FTransition& Transition);
	void NoteReceive(const FReceiveWitness& Value);
	void Acknowledge(uint64 Request, EFenceStatus Status, const FSnapshot& Value = FSnapshot());
	int64 Count(EVaRestSocketEventType Type) const { return Published[static_cast<uint8>(Type)].load(std::memory_order_acquire); }
};

/** One request owns its native probe until it is acknowledged, including cancellation and stale-connection refusal. */
struct FPassFence
{
	TSharedRef<FProbe, ESPMode::ThreadSafe> Probe;
	uint64 Owner, Connection, Request, AfterPass = 0;
};

TSharedRef<FProbe, ESPMode::ThreadSafe> MakeProbe();
FString OwnedPath(const FProbe& Probe);

/** GT only. Exact literal-loopback target; reuses the default transport and admits only an empty live baseline. */
TSharedPtr<IVaRestSocketConnection, ESPMode::ThreadSafe> ConnectOnDefault(
	int32 OwnedPort, FVaRestSocketConnectRequest Request, TSharedRef<FProbe, ESPMode::ThreadSafe> Probe);

/** GT only. One outstanding request; zero means refused before enqueue. A fence never refreshes flow itself. */
uint64 RequestPassFence(const TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe>& Handle,
	const TSharedRef<FProbe, ESPMode::ThreadSafe>& Probe);
int32 DefaultLiveConnections();
int32 DefaultContextCreations();

}

#endif
