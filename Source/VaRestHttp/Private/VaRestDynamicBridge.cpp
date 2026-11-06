// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestDynamicBridge.h"
#include "VaRestFrameDispatcher.h"
#include "VaRestDynamicClosure.h"

namespace VaRest::Http
{
struct FDynamicCompletionPayload
{
	~FDynamicCompletionPayload()
	{
		check(IsInGameThread());
		Kernel.Reset(); // real finish/config/callback destruction happens here, never in the native producer
		Types.Reset();
		auto Cleanup=MoveTemp(ActionCleanup);
		if (Cleanup) Cleanup();
	}
	TUniqueFunction<void()> Kernel;
	TUniqueFunction<void()> ActionCleanup;
	TSharedPtr<FDynamicTypedState,ESPMode::ThreadSafe> Types;
	bool bSelected=false;
	bool bInvoked=false;
};

FDynamicCompletion::FDynamicCompletion(TSharedRef<FDynamicFrameTicket, ESPMode::ThreadSafe> InTicket,
	TWeakPtr<FDynamicCompletionPayload, ESPMode::ThreadSafe> InPayload)
	: Ticket(MoveTemp(InTicket)),Payload(MoveTemp(InPayload)) {}

TSharedPtr<FDynamicCompletion, ESPMode::ThreadSafe> FDynamicCompletion::ReserveOnGT()
{
	check(IsInGameThread());
	const auto Owned=MakeShared<FDynamicCompletionPayload,ESPMode::ThreadSafe>();
	TUniqueFunction<void()> Invoke=[Owned] {
		check(IsInGameThread());
		if (Owned->bInvoked || !Owned->bSelected) return;
		Owned->bInvoked=true;
		auto Kernel=MoveTemp(Owned->Kernel);
		if (Kernel) Kernel(); // actual kernel is GT-local across callback reentrancy/producer release/Stop
		Kernel.Reset();
	};
	const auto Reserved=ReserveDynamicFrameOnGT(Invoke);
	if (!Reserved) return nullptr;
	return MakeShareable(new FDynamicCompletion(Reserved.ToSharedRef(),Owned));
}
bool FDynamicCompletion::DeferOnGT(TUniqueFunction<void()>& Kernel)
{
	check(IsInGameThread());
	const auto Self=AsShared();
	const auto Owned=Payload.Pin(); // only a GT method ever pins this actual payload
	if (!Owned || Owned->bSelected || !Kernel) return false;
	if (!Ticket->ReadyOnGT()) return false;
	// Ready's state update has no callout. No delegate/drain can interleave these same-thread, non-reentrant moves.
	Owned->bSelected=true;
	Owned->Kernel=MoveTemp(Kernel);
	return true;
}
void FDynamicCompletion::CancelOnGT()
{
	check(IsInGameThread());
	const auto Self=AsShared();
	Ticket->CancelOnGT();
}
bool FDynamicCompletion::BindWorkerOnGT(TUniqueFunction<void()>& Kernel)
{
	check(IsInGameThread());
	const auto Owned=Payload.Pin();
	if (!Owned || Owned->bSelected || !Kernel || Ticket->IsStopped()) return false;
	Owned->bSelected=true; Owned->Kernel=MoveTemp(Kernel); return true;
}
void FDynamicCompletion::NotifyReadyFromWorker() { Ticket->NotifyReadyFromWorker(); }
bool FDynamicCompletion::IsStopped() const { return Ticket->IsStopped(); }
bool FDynamicCompletion::SetTypesOnGT(TSharedRef<FDynamicTypedState,ESPMode::ThreadSafe> Types)
{
	check(IsInGameThread());
	const auto Owned=Payload.Pin();
	if (!Owned || Owned->bSelected || Owned->Types) return false;
	Owned->Types=MoveTemp(Types); return true;
}
TSharedPtr<FDynamicTypedState,ESPMode::ThreadSafe> FDynamicCompletion::TypesOnGT() const
{
	check(IsInGameThread());
	const auto Owned=Payload.Pin();
	return Owned ? Owned->Types : nullptr;
}
bool FDynamicCompletion::SetActionLifetimeOnGT(TUniqueFunction<void()>&& Cleanup)
{
	check(IsInGameThread());
	const auto Owned=Payload.Pin();
	if (!Owned || Owned->bSelected || Owned->ActionCleanup || !Cleanup) return false;
	Owned->ActionCleanup=MoveTemp(Cleanup); return true;
}
}
