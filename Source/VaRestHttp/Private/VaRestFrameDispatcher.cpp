// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestFrameDispatcher.h"
#include "Misc/CoreDelegates.h"
#include "CoreGlobals.h"
#include <array>
#include <atomic>

namespace VaRest::Http
{
class FDynamicFrameControl final : public TSharedFromThis<FDynamicFrameControl, ESPMode::ThreadSafe>
{
public:
	static constexpr uint16 Capacity = 256;
	explicit FDynamicFrameControl(uint16 InLimit) : Limit(InLimit) {}
	~FDynamicFrameControl()
	{
		// Late native tickets may release this EMPTY allocation on a worker. No callback/type/static lookup here.
		for (const auto& Slot : Slots) check(!Slot.Callback);
	}
	TSharedPtr<FDynamicFrameTicket, ESPMode::ThreadSafe> ReserveOnGT(TUniqueFunction<void()>& Callback)
	{
		check(IsInGameThread());
		if (bStopped.load(std::memory_order_acquire) || !Callback) return nullptr;
		for (auto& Slot:Slots) ReclaimOnGT(Slot);
		if (Occupied>=Limit) return nullptr;
		for (uint16 Step=0; Step<Capacity; ++Step)
		{
			const uint16 Index=(Next+Step)%Capacity;
			auto& Slot=Slots[Index];
			ReclaimOnGT(Slot);
			if (Slot.Phase!=EPhase::Free) continue;
			Slot.Phase=EPhase::Reserved;
			Slot.bAccessEnded.store(false,std::memory_order_relaxed);
			Slot.bWorkerReady.store(false,std::memory_order_relaxed);
			Slot.Callback=MoveTemp(Callback);
			++Occupied;
			Next=(Index+1)%Capacity;
			return MakeShareable(new FDynamicFrameTicket(AsShared(),Index,Slot.Generation));
		}
		return nullptr;
	}
	bool ReadyOnGT(uint16 Index,uint64 Generation)
	{
		check(IsInGameThread());
		if (bStopped.load(std::memory_order_acquire)) return false;
		auto& Slot=Slots[Index];
		if (Slot.Generation!=Generation || Slot.Phase!=EPhase::Reserved) return false;
		Slot.ReadyFrame=GFrameCounter;
		Slot.Phase=EPhase::Ready;
		return true;
	}
	void CancelOnGT(uint16 Index,uint64 Generation)
	{
		check(IsInGameThread());
		auto& Slot=Slots[Index];
		if (Slot.Generation!=Generation || Slot.Phase==EPhase::Running) return;
		DisposeOnGT(Slot);
	}
	void EndNativeUse(uint16 Index)
	{
		Slots[Index].bAccessEnded.store(true,std::memory_order_release); // LAST slot access; ticket retains Control allocation
	}
	void NotifyReadyFromWorker(uint16 Index) { Slots[Index].bWorkerReady.store(true,std::memory_order_release); }
	bool IsStopped() const { return bStopped.load(std::memory_order_acquire); }
	void DrainOnGT()
	{
		check(IsInGameThread());
		const auto Self=AsShared();
		if (bDraining || Occupied==0 || bStopped.load(std::memory_order_acquire)) return;
		bDraining=true;
		const uint64 Now=GFrameCounter;
		std::array<TPair<uint16,uint64>,Capacity> Ready;
		uint16 Count=0;
		for (uint16 Index=0; Index<Capacity; ++Index)
		{
			auto& Slot=Slots[Index];
			if (Slot.Phase==EPhase::Reserved && Slot.bWorkerReady.load(std::memory_order_acquire))
			{
				Slot.ReadyFrame=Now; Slot.Phase=EPhase::Ready; // no native worker can choose a GT frame stamp
			}
			if (Slot.Phase==EPhase::Reserved && Slot.bAccessEnded.load(std::memory_order_acquire)) DisposeOnGT(Slot);
			ReclaimOnGT(Slot);
			if (Slot.Phase==EPhase::Ready && Slot.ReadyFrame<Now) Ready[Count++]={Index,Slot.Generation};
		}
		// Snapshot before ANY invocation. A callback that becomes ready reentrantly cannot enter this same drain.
		for (uint16 Offset=0; Offset<Count && !bStopped.load(std::memory_order_acquire); ++Offset)
		{
			const auto Entry=Ready[Offset];
			auto& Slot=Slots[Entry.Key];
			if (Slot.Generation!=Entry.Value || Slot.Phase!=EPhase::Ready || !Slot.Callback) continue;
			Slot.Phase=EPhase::Running;
			auto Invoke=MoveTemp(Slot.Callback);
			Invoke();
			Invoke.Reset(); // actual GTK payload destruction, before this slot can become reusable
			Slot.Phase=EPhase::Spent;
			ReclaimOnGT(Slot);
		}
		bDraining=false;
	}
	void StopOnGT()
	{
		check(IsInGameThread());
		const auto Self=AsShared();
		if (bStopped.exchange(true,std::memory_order_acq_rel)) return;
		for (auto& Slot:Slots) if (Slot.Phase!=EPhase::Running) DisposeOnGT(Slot);
	}

private:
	enum class EPhase:uint8 { Free,Reserved,Ready,Running,Cleaning,Spent,Exhausted };
	struct FSlot
	{
		uint64 Generation=1,ReadyFrame=0;
		EPhase Phase=EPhase::Free;
		std::atomic<bool> bAccessEnded{true};
		std::atomic<bool> bWorkerReady{false};
		TUniqueFunction<void()> Callback;
	};
	void DisposeOnGT(FSlot& Slot)
	{
		if (Slot.Phase==EPhase::Free || Slot.Phase==EPhase::Exhausted || Slot.Phase==EPhase::Running || Slot.Phase==EPhase::Cleaning) return;
		Slot.Phase=EPhase::Cleaning;
		auto Dropped=MoveTemp(Slot.Callback);
		Dropped.Reset(); // can reenter Stop/reserve; Cleaning bars this exact slot until destruction returns
		Slot.Phase=EPhase::Spent;
		ReclaimOnGT(Slot);
	}
	void ReclaimOnGT(FSlot& Slot)
	{
		if (Slot.Phase!=EPhase::Spent || !Slot.bAccessEnded.load(std::memory_order_acquire)) return;
		check(!Slot.Callback);
		check(Occupied!=0);
		--Occupied;
		if (Slot.Generation==MAX_uint64) Slot.Phase=EPhase::Exhausted;
		else { ++Slot.Generation;Slot.Phase=EPhase::Free; }
	}
	std::array<FSlot,Capacity> Slots;
	std::atomic<bool> bStopped{false};
	uint16 Next=0;
	const uint16 Limit;
	uint16 Occupied=0;
	bool bDraining=false;
};

FDynamicFrameTicket::FDynamicFrameTicket(TSharedRef<FDynamicFrameControl,ESPMode::ThreadSafe> InControl,uint16 InIndex,uint64 InGeneration)
	: Control(MoveTemp(InControl)),Index(InIndex),Generation(InGeneration) {}
FDynamicFrameTicket::~FDynamicFrameTicket() { Control->EndNativeUse(Index); }
bool FDynamicFrameTicket::ReadyOnGT() { const auto Self=AsShared();return Control->ReadyOnGT(Index,Generation); }
void FDynamicFrameTicket::NotifyReadyFromWorker() { Control->NotifyReadyFromWorker(Index); }
bool FDynamicFrameTicket::IsStopped() const { return Control->IsStopped(); }
void FDynamicFrameTicket::CancelOnGT() { const auto Self=AsShared();Control->CancelOnGT(Index,Generation); }

TUniquePtr<FFrameDispatcher> FFrameDispatcher::CreateOnGT(uint32 AdmissionLimit)
{
	check(IsInGameThread());
	if (AdmissionLimit==0 || AdmissionLimit>FDynamicFrameControl::Capacity) return nullptr;
	TUniquePtr<FFrameDispatcher> Dispatcher(new FFrameDispatcher());
	Dispatcher->Control=MakeShared<FDynamicFrameControl,ESPMode::ThreadSafe>(static_cast<uint16>(AdmissionLimit));
	TWeakPtr<FDynamicFrameControl,ESPMode::ThreadSafe> Weak=Dispatcher->Control;
	Dispatcher->EndFrameHandle=FCoreDelegates::OnEndFrame.AddLambda([Weak] { if (const auto Kept=Weak.Pin()) Kept->DrainOnGT(); });
	if (!Dispatcher->EndFrameHandle.IsValid()) return nullptr;
	return Dispatcher;
}
FFrameDispatcher::~FFrameDispatcher() { StopOnGT(); }
TSharedPtr<FDynamicFrameTicket,ESPMode::ThreadSafe> FFrameDispatcher::TryReserveOnGT(TUniqueFunction<void()>& Callback)
{
	check(IsInGameThread());
	const auto Kept=Control;
	return Kept ? Kept->ReserveOnGT(Callback) : nullptr;
}
void FFrameDispatcher::StopOnGT()
{
	check(IsInGameThread());
	const auto Kept=MoveTemp(Control);
	const FDelegateHandle Removing=EndFrameHandle;
	EndFrameHandle.Reset();
	FCoreDelegates::OnEndFrame.Remove(Removing);
	if (Kept) Kept->StopOnGT(); // reentrant payload destruction can delete the dispatcher; no this/member access follows
}
}
