// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestManagedRefreshDelivery.h"
#include "Tasks/Task.h"

namespace VaRest::Http
{
/** No affine callback, State or module lookup. Every method/task/ticket retains this exact Control allocation. */
class FManagedRefreshNativeAccess final
{
public:
	FManagedRefreshNativeAccess(FManagedRefreshDelivery& InStore,
		TSharedRef<FNativeLifetimeControl, ESPMode::ThreadSafe> InControl, int32 InIndex, uint64 InGeneration, bool bInWake)
		: Store(InStore)
		, Control(MoveTemp(InControl))
		, Index(InIndex)
		, Generation(InGeneration)
		, bWake(bInWake)
	{
		check(IsInGameThread());
		const uint64 Before = Store.Authorities.fetch_add(1, std::memory_order_relaxed);
		check(Before != MAX_uint64);
		if (Index >= 0)
		{
			check(Store.Slots[Index].Generation == Generation);
			const uint64 EntryBefore = Store.Slots[Index].Authorities.fetch_add(1, std::memory_order_relaxed);
			check(EntryBefore != MAX_uint64);
		}
	}
	~FManagedRefreshNativeAccess() { Store.ReleaseAccess(Index, Generation, bWake); }
	void ReadyOnGT(const FRefreshEnded& End) { Store.SetReadyOnGT(static_cast<uint32>(Index), Generation, End); }
	void WakeOnGT() { Store.WakeOnGT(); }

private:
	FManagedRefreshDelivery& Store;
	TSharedRef<FNativeLifetimeControl, ESPMode::ThreadSafe> Control;
	int32 Index;
	uint64 Generation;
	bool bWake;
};

FManagedRefreshDelivery::FManagedRefreshDelivery(TSharedRef<FNativeLifetimeControl, ESPMode::ThreadSafe> Control,
	TSharedRef<FClientAdmissionGate, ESPMode::ThreadSafe> InGate)
	: Owner(Control)
	, Gate(MoveTemp(InGate))
{
	check(IsInGameThread());
}
FManagedRefreshDelivery::~FManagedRefreshDelivery()
{
	check(Authorities.load(std::memory_order_acquire) == 0 && !RefusedHead && !RefusedTail);
	for (const auto& Slot : Slots)
		check(!Slot.Original && Slot.Authorities.load(std::memory_order_acquire) == 0);
}
TSharedRef<FManagedRefreshNativeAccess, ESPMode::ThreadSafe> FManagedRefreshDelivery::AccessOnGT(int32 Index, uint64 Generation, bool bWake)
{
	check(IsInGameThread());
	return MakeShared<FManagedRefreshNativeAccess, ESPMode::ThreadSafe>(*this, Owner.Pin().ToSharedRef(), Index, Generation, bWake);
}
void FManagedRefreshDelivery::ReleaseAccess(int32 Index, uint64 Generation, bool bWake)
{
	if (Index >= 0)
	{
		auto& Slot = Slots[Index];
		check(Slot.Generation == Generation); // this authority prevents GT reuse through the entire method
		const uint64 Before = Slot.Authorities.fetch_sub(1, std::memory_order_release);
		check(Before != 0); // no later access to that slot
	}
	if (bWake)
		WakeEnded.store(true, std::memory_order_release); // after ALL task/method use
	const uint64 Before = Authorities.fetch_sub(1, std::memory_order_release);
	check(Before != 0); // LAST store access; captured Control lives until this native access destructor returns
}
bool FManagedRefreshDelivery::IsClosed() const
{
	const auto Control = Owner.Pin();
	return bRetired || !Control || Control->IsStopped() || Gate->IsClosed();
}
bool FManagedRefreshDelivery::TryReserveOnGT(FRefreshWaiter& Original)
{
	check(IsInGameThread());
	const auto Method = AccessOnGT();
	if (IsClosed())
		return false;
	CollectOnGT();
	if (IsClosed())
		return false; // expired callback destruction may reenter retirement
	for (uint32 Index = 0; Index < Capacity; ++Index)
	{
		auto& Slot = Slots[Index];
		if (Slot.Phase != EPhase::Free)
			continue;
		check(!Slot.Original && Slot.Authorities.load(std::memory_order_acquire) == 0);
		const auto Native = AccessOnGT(static_cast<int32>(Index), Slot.Generation);
		Slot.Original = MoveTemp(Original);
		Slot.Phase = EPhase::Waiting;
		Original = [Native](const FRefreshEnded& End) { Native->ReadyOnGT(End); };
		return true;
	}
	return false;
}
void FManagedRefreshDelivery::SetReadyOnGT(uint32 Index, uint64 Generation, const FRefreshEnded& End)
{
	check(IsInGameThread() && Index < Capacity);
	const auto Method = AccessOnGT();
	auto& Slot = Slots[Index];
	check(Slot.Generation == Generation && Slot.Authorities.load(std::memory_order_acquire) != 0);
	if (IsClosed() || Slot.Phase != EPhase::Waiting)
		return;
	Slot.Result = End;
	Slot.Phase = EPhase::Ready;
	PublishWakeOnGT();
}
void FManagedRefreshDelivery::RefuseLaterOnGT(FRefreshWaiter&& Original)
{
	check(IsInGameThread());
	const auto Method = AccessOnGT();
	if (IsClosed())
		return; // caller's actual callback destruction remains on this GT call stack
	auto* Node = new FRefusal{MoveTemp(Original), nullptr};
	if (RefusedTail)
		RefusedTail->Next = Node;
	else
		RefusedHead = Node;
	RefusedTail = Node;
	PublishWakeOnGT();
}
bool FManagedRefreshDelivery::HasReadyOnGT() const
{
	if (RefusedHead)
		return true;
	for (const auto& Slot : Slots)
		if (Slot.Phase == EPhase::Ready)
			return true;
	return false;
}
void FManagedRefreshDelivery::PublishWakeOnGT()
{
	check(IsInGameThread());
	if (IsClosed())
		return;
	if (bWakeOutstanding && WakeEnded.load(std::memory_order_acquire))
		bWakeOutstanding = false;
	if (bWakeOutstanding || !HasReadyOnGT())
		return;
	bWakeOutstanding = true;
	bWakeRan = false;
	WakeEnded.store(false, std::memory_order_relaxed);
	const auto Native = AccessOnGT(-1, 0, true);
	if (IsClosed())
		return; // the native owner ends normally; the GT service owns all callbacks independently
	UE::Tasks::Launch(TEXT("VaRest.ManagedRefreshDelivery"), [Native] { Native->WakeOnGT(); }, UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
}
void FManagedRefreshDelivery::WakeOnGT()
{
	check(IsInGameThread());
	const auto Method = AccessOnGT();
	bWakeRan = true;
	if (IsClosed())
	{
		RetireOnGT();
		return;
	}
	DeliverPendingOnGT();
}
void FManagedRefreshDelivery::DeliverPendingOnGT()
{
	check(IsInGameThread());
	if (bDelivering || IsClosed())
		return;
	TGuardValue<bool> Delivering(bDelivering, true);
	uint32 Delivered = 0;
	const uint32 AcceptedBudget = RefusedHead ? Capacity / 2 : Capacity;
	const uint32 Start = NextDelivery;
	for (uint32 Offset = 0; Offset < Capacity; ++Offset)
	{
		if (IsClosed() || Delivered == AcceptedBudget)
			break;
		const uint32 Index = (Start + Offset) % Capacity;
		auto& Slot = Slots[Index];
		if (Slot.Phase != EPhase::Ready)
			continue;
		Slot.Phase = EPhase::Delivering; // reentrant retirement/collection cannot reuse this actual callback frame
		FRefreshWaiter Callback = MoveTemp(Slot.Original);
		if (Callback)
			Callback(Slot.Result);
		Callback.Reset(); // arbitrary capture destruction is GT, before freeing the position or ending method authority
		Slot.Phase = EPhase::Retiring;
		NextDelivery = (Index + 1) % Capacity;
		++Delivered;
	}
	// A finite prefix: reentrant overflow cannot turn one GTK task into an unbounded delivery loop.
	FRefusal* Last = RefusedTail;
	while (RefusedHead && Delivered < Capacity && !IsClosed())
	{
		TUniquePtr<FRefusal> Node(RefusedHead);
		RefusedHead = Node->Next;
		if (!RefusedHead)
			RefusedTail = nullptr;
		const bool bLast = Node.Get() == Last;
		FRefreshEnded End;
		End.End = ERefreshEnd::Failed;
		End.Kind = EVaRestErrorKind::Config;
		End.Message = TEXT("managed External refresh waiter capacity exceeded");
		if (Node->Original)
			Node->Original(End);
		Node.Reset(); // the real refusal payload is destroyed on GT, not by the wake task's native capture
		++Delivered;
		if (bLast)
			break;
	}
	CollectOnGT();
}
void FManagedRefreshDelivery::CollectOnGT()
{
	check(IsInGameThread());
	if (bCollecting)
		return;
	TGuardValue<bool> Collecting(bCollecting, true);
	for (auto& Slot : Slots)
	{
		if (Slot.Authorities.load(std::memory_order_acquire) != 0 || Slot.Phase == EPhase::Delivering)
			continue;
		if (Slot.Phase == EPhase::Waiting)
			Slot.Phase = EPhase::Retiring; // dropped native session ticket, no invented result
		if (Slot.Phase != EPhase::Retiring)
			continue;
		Slot.Phase = EPhase::Delivering;
		FRefreshWaiter Dropped = MoveTemp(Slot.Original);
		Dropped.Reset(); // reentrant retirement/collection cannot destroy the same callable twice or reuse its frame
		Slot.Phase = EPhase::Retiring;
		Slot.Result = FRefreshEnded();
		if (Slot.Generation == MAX_uint64)
			Slot.Phase = EPhase::Exhausted;
		else
		{
			++Slot.Generation;
			Slot.Phase = EPhase::Free;
		}
	}
}
void FManagedRefreshDelivery::DropPendingOnGT()
{
	for (auto& Slot : Slots)
	{
		if (Slot.Phase != EPhase::Waiting && Slot.Phase != EPhase::Ready)
			continue;
		Slot.Phase = EPhase::Delivering;
		FRefreshWaiter Dropped = MoveTemp(Slot.Original);
		Dropped.Reset(); // native tickets can remain, but carry no actual user/config captures
		Slot.Phase = EPhase::Retiring;
	}
	while (RefusedHead)
	{
		TUniquePtr<FRefusal> Node(RefusedHead);
		RefusedHead = Node->Next;
		if (!RefusedHead)
			RefusedTail = nullptr;
		Node.Reset();
	}
	CollectOnGT();
}
void FManagedRefreshDelivery::RetireOnGT()
{
	check(IsInGameThread());
	const auto Method = AccessOnGT();
	bRetired = true;
	if (bDropping)
		return;
	TGuardValue<bool> Dropping(bDropping, true);
	DropPendingOnGT();
}
void FManagedRefreshDelivery::ServiceOnGT()
{
	check(IsInGameThread());
	const auto Method = AccessOnGT();
	if (IsClosed())
		RetireOnGT();
	if (bWakeOutstanding && WakeEnded.load(std::memory_order_acquire))
	{
		bWakeOutstanding = false;
		if (!bWakeRan && !IsClosed())
			DeliverPendingOnGT(); // actual discarded native task, independent GT fallback
	}
	CollectOnGT();
	PublishWakeOnGT();
}
bool FManagedRefreshDelivery::IsQuiescentOnGT() const
{
	check(IsInGameThread());
	if (Authorities.load(std::memory_order_acquire) != 0 || RefusedHead || RefusedTail)
		return false;
	for (const auto& Slot : Slots)
		if (Slot.Phase != EPhase::Free && Slot.Phase != EPhase::Exhausted)
			return false;
	return true;
}
bool FManagedRefreshDelivery::ResetForNextClientOnGT(TSharedRef<FClientAdmissionGate, ESPMode::ThreadSafe> InGate)
{
	check(IsInGameThread());
	if (!IsQuiescentOnGT() || !Gate->IsClosed())
		return false;
	Gate = MoveTemp(InGate);
	bRetired = bWakeOutstanding = bWakeRan = false;
	NextDelivery = 0;
	WakeEnded.store(false, std::memory_order_relaxed);
	return true;
}
} // namespace VaRest::Http
