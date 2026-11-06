// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSocketPressureTesting.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace VaRest::WebSocket::PressureTesting
{

TSharedRef<FProbe, ESPMode::ThreadSafe> MakeProbe()
{
	static std::atomic<uint64> NextOwner{0};
	const uint64 Owner = NextOwner.fetch_add(1, std::memory_order_relaxed) + 1;
	check(Owner != 0);
	return MakeShared<FProbe, ESPMode::ThreadSafe>(Owner);
}

FString OwnedPath(const FProbe& Probe)
{
	return FString::Printf(TEXT("/__varest_pressure/%llu"), static_cast<unsigned long long>(Probe.Owner));
}

void FProbe::NotePush(EVaRestSocketEventType Type, int32 Queued)
{
	int32 Peak = PeakQueued.load(std::memory_order_relaxed);
	while (Peak < Queued && !PeakQueued.compare_exchange_weak(Peak, Queued, std::memory_order_relaxed)) {}
	if (Type == EVaRestSocketEventType::Closed || Type == EVaRestSocketEventType::Error) TerminalQueued.store(Queued, std::memory_order_release);
	Published[static_cast<uint8>(Type)].fetch_add(1, std::memory_order_release);
}

void FProbe::NoteTransition(const FTransition& Transition)
{
	const int32 Index = TransitionCount.load(std::memory_order_relaxed);
	if (Index == TransitionCapacity)
	{
		bRecordOverflow.store(true, std::memory_order_release);
		return;
	}
	Transitions[Index] = Transition;
	TransitionCount.store(Index + 1, std::memory_order_release);
}

void FProbe::Acknowledge(uint64 Request, EFenceStatus Status, const FSnapshot& Value)
{
	check(ActiveRequest.load(std::memory_order_acquire) == Request);
	Snapshot = Value;
	AckStatus.store(Status, std::memory_order_relaxed);
	ActiveRequest.store(0, std::memory_order_release);
	InFlightRequests.fetch_sub(1, std::memory_order_acq_rel);
	AckRequest.store(Request, std::memory_order_release);
}

void FProbe::NoteReceive(const FReceiveWitness& Value)
{
	if (Value.MessageOrdinal < ReceiveWitnessStartOrdinal) return;
	const int32 Index = ReceiveWitnessCount.load(std::memory_order_relaxed);
	if (Index == ReceiveWitnessCapacity) return;
	ReceiveWitnesses[Index] = Value;
	ReceiveWitnessCount.store(Index + 1, std::memory_order_release);
}

}

#endif
