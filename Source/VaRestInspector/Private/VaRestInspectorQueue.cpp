// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestInspectorQueue.h"
#include "VaRestInspectorService.h"

#include "Containers/MpscQueue.h"
#include "CoreGlobals.h"
#include "HAL/Event.h"
#include "HAL/UnrealMemory.h"
#include "Misc/AssertionMacros.h"
#include "Misc/ScopeExit.h"
#include "Templates/TypeCompatibleBytes.h"

#if VAREST_WITH_INSPECTOR
namespace VaRest::Inspector::Private
{
namespace
{
// Exact engine node shape; MpscQueue.h:124-133. Malloc checks every actual
// request. The prefix permits context-free Free on a different thread.
struct FNodeMirror
{
	std::atomic<void*> Next;
	TTypeCompatibleBytes<FPacket> Value;
};
struct alignas(16) FNodePrefix
{
	FAllocationAdapter* Allocator;
};
static_assert(alignof(FNodeMirror) <= alignof(FNodePrefix));
thread_local FAllocationAdapter* NodeAllocationContext = nullptr;
class FScopedNodeContext
{
public:
	explicit FScopedNodeContext(FAllocationAdapter& Allocator)
	{
		check(NodeAllocationContext == nullptr);
		NodeAllocationContext = &Allocator;
	}
	~FScopedNodeContext() { NodeAllocationContext = nullptr; }
};
struct FNodeAllocator
{
	static void* Malloc(SIZE_T Size, uint32 Alignment)
	{
		checkf(Size == sizeof(FNodeMirror) && Alignment == alignof(FNodeMirror), TEXT("Inspector MPSC node layout changed"));
		check(NodeAllocationContext);
		FAllocationAdapter& Allocator = *NodeAllocationContext;
		const SIZE_T Request = sizeof(FNodePrefix) + sizeof(FNodeMirror);
		void* Base = Allocator.AllocateReserved(Request, alignof(FNodePrefix), Allocator.QueueNodeBound());
		checkf(Base, TEXT("Inspector MPSC node allocation capability failed"));
		new (Base) FNodePrefix{&Allocator};
		return static_cast<uint8*>(Base) + sizeof(FNodePrefix);
	}
	static void Free(void* Node)
	{
		auto* Prefix = reinterpret_cast<FNodePrefix*>(static_cast<uint8*>(Node) - sizeof(FNodePrefix));
		FAllocationAdapter* Allocator = Prefix->Allocator;
		Prefix->~FNodePrefix();
		// Baseline or the still-held dequeue packet keeps the context alive.
		// No TLS lookup, allocation or ticket release occurs before this free.
		Allocator->Free(Prefix);
	}
};
void RecordLoss(FPendingAccount& Account, int64 Bytes, bool bDropped, bool bMetadata, bool bExcerpt)
{
	if (bDropped)
		AddSaturated(Account.DroppedCount);
	if (Bytes > 0)
		AddSaturated(Account.DroppedBytes, uint64(Bytes));
	if (bMetadata)
		AddSaturated(Account.MetadataOmitted);
	if (bExcerpt)
		AddSaturated(Account.ExcerptsSkipped);
	AddSaturated(Account.LossEpoch);
 if (Account.Domain && Account.LossEpoch.load(std::memory_order_acquire)==MAX_uint64) Account.Domain->LossSaturated.store(true,std::memory_order_release);
}
bool ScalarLayout(FPacketLayout& Layout, const FAllocationAdapter& Allocator, bool bBacklog)
{
	const FPacketHeader Header = Layout.Fixed.Header;
	const FPacketScalars Scalars = Layout.Fixed.Scalars;
	Layout = FPacketLayout{};
	Layout.Fixed.Header = Header;
	Layout.Fixed.Scalars = Scalars;
	Layout.Fixed.Header.bMetadataOmitted = true;
	if (bBacklog)
	{
		Layout.Fixed.Scalars.ExcerptKind = EVaRestExcerptKind::Skipped;
		Layout.Fixed.Scalars.Skip = EVaRestExcerptSkip::Backlog;
	}
	const auto Bound = Allocator.Bound(sizeof(FPacketFixed), alignof(FPacketFixed));
	const SIZE_T Node = Allocator.QueueNodeBound();
	if (!Bound.IsSet() || Node == 0 || Bound.GetValue() > SIZE_T(MAX_int64) || Node > SIZE_T(MAX_int64))
		return false;
	Layout.BackingRequest = sizeof(FPacketFixed);
	Layout.BackingBound = Bound.GetValue();
	Layout.PendingCharge = int64(Bound.GetValue());
	return AddChecked(Layout.PendingCharge, int64(Node), MAX_int64);
}
} // namespace

SIZE_T FAllocationAdapter::QueueNodeBound() const
{
	const auto Result = Bound(sizeof(FNodePrefix) + sizeof(FNodeMirror), alignof(FNodePrefix));
	return Result.IsSet() ? Result.GetValue() : 0;
}
struct FCaptureQueue::FImpl
{
	TMpscQueue<FPacket, FNodeAllocator> Metadata;
	TMpscQueue<FPacket, FNodeAllocator> Excerpts;
	uint64 LastPublished = 0;
#if WITH_DEV_AUTOMATION_TESTS
	FQueueTestHooks* Hooks = nullptr;
#endif
	~FImpl()
	{
		// MpscQueue.h:32-43 destroys a queued value before freeing its node.
		// Drain first: Dequeue frees the old sentinel before returning its
		// still-charged packet. Only empty sentinels reach engine destruction.
		while (Metadata.Dequeue().IsSet())
		{
		}
		while (Excerpts.Dequeue().IsSet())
		{
		}
	}
};
void FCaptureQueue::FImplDeleter::operator()(FImpl* Ptr) const
{
	if (!Ptr)
		return;
	Ptr->~FImpl();
	Allocator->Free(Ptr);
}
TOptional<int64> FCaptureQueue::BaselineCharge(const FAllocationAdapter& Allocator)
{
	const auto Wrapper = Allocator.Bound(sizeof(FCaptureQueue), alignof(FCaptureQueue));
	const auto Implementation = Allocator.Bound(sizeof(FImpl), alignof(FImpl));
	const SIZE_T Node = Allocator.QueueNodeBound();
	if (!Wrapper.IsSet() || !Implementation.IsSet() || Node == 0 ||
		Wrapper.GetValue() > SIZE_T(MAX_int64) || Implementation.GetValue() > SIZE_T(MAX_int64) || Node > SIZE_T(MAX_int64))
		return {};
	int64 Charge = int64(Wrapper.GetValue());
	if (!AddChecked(Charge, int64(Implementation.GetValue()), MAX_int64) ||
		!AddChecked(Charge, int64(Node), MAX_int64) || !AddChecked(Charge, int64(Node), MAX_int64))
		return {};
	return Charge;
}
TOptional<int64> FCaptureQueue::InternalBaselineCharge(const FAllocationAdapter& A)
{
 auto All=BaselineCharge(A); auto Wrapper=A.Bound(sizeof(FCaptureQueue),alignof(FCaptureQueue));
 if (!All.IsSet() || !Wrapper.IsSet() || All.GetValue()<int64(Wrapper.GetValue())) return {};
 return All.GetValue()-int64(Wrapper.GetValue());
}
FCaptureQueue::FCaptureQueue(const FCapturePolicy& P,uint64 G,TSharedRef<FPendingAccount,ESPMode::ThreadSafe> A,FAllocationAdapter& L)
 : FCaptureQueue(P,G,MoveTemp(A),L,false) {}
FCaptureQueue::FCaptureQueue(FQueueWrapperAdmission,const FCapturePolicy& P,uint64 G,TSharedRef<FPendingAccount,ESPMode::ThreadSafe> A,FAllocationAdapter& L)
 : FCaptureQueue(P,G,MoveTemp(A),L,true) {}
FCaptureQueue::FCaptureQueue(const FCapturePolicy& InPolicy, uint64 InGeneration,
	TSharedRef<FPendingAccount, ESPMode::ThreadSafe> InAccount,FAllocationAdapter& InAllocator,bool bFactoryWrapper)
	: Policy(InPolicy)
	, Generation(InGeneration)
	, Account(MoveTemp(InAccount))
	, Allocator(&InAllocator)
	, Impl(static_cast<FImpl*>(nullptr), FImplDeleter{&InAllocator})
{
	const auto Charge=bFactoryWrapper ? InternalBaselineCharge(InAllocator) : BaselineCharge(InAllocator);
	if (!Charge.IsSet())
	{
		AddSaturated(InAllocator.Counters().UnsupportedAllocator);
		return;
	}
	auto Credit = FPendingBaseline::Acquire(Account, Charge.GetValue());
	if (!Credit.IsSet())
		return;
	Baseline = MoveTemp(Credit.GetValue());
	const auto Bound = InAllocator.Bound(sizeof(FImpl), alignof(FImpl));
	check(Bound.IsSet());
	void* Storage = InAllocator.AllocateReserved(sizeof(FImpl), alignof(FImpl), Bound.GetValue());
	if (!Storage)
	{
		Baseline.Reset();
		return;
	}
	{
		FScopedNodeContext Context(InAllocator);
		Impl.Reset(new (Storage) FImpl());
	}
}
FCaptureQueue::~FCaptureQueue()
{
	Close();
	// Last owner means all retained publisher calls have returned. Dequeue's
	// transient empty state alone never authorizes destruction or resets.
	Impl.Reset();
	Baseline.Reset();
}
bool FCaptureQueue::IsSupported() const
{
	return Impl.IsValid();
}
void FCaptureQueue::Close()
{
	bClosed.store(true, std::memory_order_release);
}
uint64 FCaptureQueue::LastPublishedMetadata() const
{
	check(IsInGameThread());
	return Impl ? Impl->LastPublished : 0;
}
uint64 FCaptureQueue::LossEpoch() const
{
	return Account->LossEpoch.load(std::memory_order_acquire);
}
int64 FCaptureQueue::OutstandingExcerptReservations() const
{
	return Account->ExcerptReservations.load(std::memory_order_acquire);
}
#if WITH_DEV_AUTOMATION_TESTS
void FCaptureQueue::SetPublishedOrdinalForTest(uint64 Ordinal) { check(IsInGameThread() && Impl); Impl->LastPublished=Ordinal; }
void FCaptureQueue::SetTestHooks(FQueueTestHooks* InHooks)
{
	check(IsInGameThread() && Impl);
	Impl->Hooks = InHooks;
}
#endif
EPush FCaptureQueue::TryPush(const FVaRestTraceEvent& Borrowed)
{
 FAdmissionEntry Entry(*Account);
 if (!Entry) return EPush::Closed;
	if (bClosed.load(std::memory_order_acquire))
		return EPush::Closed;
	if (!Impl)
		return EPush::Unsupported;
	FPacketLayout Layout;
	const EMeasure Measured = FPacketCodec::Measure(Borrowed, Policy, *Allocator, Layout);
	if (Measured == EMeasure::Unsupported)
		return EPush::Unsupported;
	const bool bExcerpt = Borrowed.Kind == EVaRestTraceKind::Excerpt;
	if (!bExcerpt && !IsInGameThread())
	{
		RecordLoss(*Account, Layout.PendingCharge, true, false, false);
		return EPush::Dropped;
	}
	bool bScalar = Measured == EMeasure::ScalarOnly;
	const int64 OriginalCharge = Layout.PendingCharge;
	// Count saturation must never be masked by a smaller fallback descriptor.
	if (Account->Count.load(std::memory_order_acquire) >= Account->CountLimit)
	{
		RecordLoss(*Account, OriginalCharge, true, bScalar, bExcerpt);
		return EPush::Dropped;
	}
	bool bCountRefused = false;
	auto Ticket = FPendingTicket::Acquire(Account, Layout.PendingCharge, &bCountRefused);
	if (!Ticket.IsSet() && !bCountRefused && bExcerpt && !bScalar)
	{
		if (ScalarLayout(Layout, *Allocator, true))
		{
			Ticket = FPendingTicket::Acquire(Account, Layout.PendingCharge);
			bScalar = true;
		}
	}
	if (!Ticket.IsSet())
	{
		RecordLoss(*Account, OriginalCharge, true, Measured == EMeasure::ScalarOnly, bExcerpt);
		return EPush::Dropped;
	}
	// Complete admission before exposing a held reservation. After this point,
	// the entry, ticket and queue owner keep the old generation alive through
	// publication, even if Close or a same-bank resume changes the gate.
	if (bClosed.load(std::memory_order_acquire) || !Entry.StillCurrent())
		return EPush::Closed;
	if (bScalar)
		RecordLoss(*Account, OriginalCharge, false, !bExcerpt, bExcerpt);
	if (bExcerpt)
		Account->ExcerptReservations.fetch_add(1, std::memory_order_acq_rel);
	ON_SCOPE_EXIT
	{
		if (bExcerpt)
			Account->ExcerptReservations.fetch_sub(1, std::memory_order_acq_rel);
	};
#if WITH_DEV_AUTOMATION_TESTS
	if (Impl->Hooks && bExcerpt && Layout.Fixed.Scalars.RequestId == Impl->Hooks->ReservedRequestId)
	{
		Impl->Hooks->Reserved->Trigger();
		Impl->Hooks->ReleaseReserved->Wait();
	}
#endif
	Layout.Fixed.Header.Generation = Generation;
	if (!bExcerpt)
	{
		if (Impl->LastPublished == MAX_uint64)
		{
			// The GT service rotates through the same BeginGeneration/attach path.
			bOrdinalOverflow.store(true,std::memory_order_release);
			Close();
			RecordLoss(*Account, Layout.PendingCharge, true, false, false);
			return EPush::Dropped;
		}
		Layout.Fixed.Header.MetadataOrdinal = Impl->LastPublished + 1;
	}
	FPacket Packet = FPacketCodec::Write(Borrowed, Layout, MoveTemp(Ticket.GetValue()), *Allocator);
	if (!Packet.IsValid())
	{
		RecordLoss(*Account, Layout.PendingCharge, true, false, bExcerpt);
		return EPush::Dropped;
	}
#if WITH_DEV_AUTOMATION_TESTS
	if (Impl->Hooks && bExcerpt && Layout.Fixed.Scalars.RequestId == Impl->Hooks->BeforeEnqueueRequestId)
	{
		Impl->Hooks->BeforeEnqueue->Trigger();
		Impl->Hooks->ReleaseEnqueue->Wait();
	}
#endif
	{
		FScopedNodeContext Context(*Allocator);
		if (bExcerpt)
			Impl->Excerpts.Enqueue(MoveTemp(Packet));
		else
			Impl->Metadata.Enqueue(MoveTemp(Packet));
	}
	if (!bExcerpt)
		Impl->LastPublished = Layout.Fixed.Header.MetadataOrdinal;
	return bScalar ? EPush::ScalarOnly : EPush::Accepted;
}
TOptional<FPacket> FCaptureQueue::TryPopMetadata()
{
	check(IsInGameThread());
	return Impl ? Impl->Metadata.Dequeue() : TOptional<FPacket>();
}
TOptional<FPacket> FCaptureQueue::TryPopExcerpt()
{
	check(IsInGameThread());
	return Impl ? Impl->Excerpts.Dequeue() : TOptional<FPacket>();
}
uint32 FCaptureQueue::DescribeTypes(FTypeLayoutRow* Out,uint32 Capacity)
{
 const FTypeLayoutRow Rows[]={{TEXT("QueueWrapper"),sizeof(FCaptureQueue),alignof(FCaptureQueue),sizeof(FCaptureQueue)},{TEXT("QueueImpl"),sizeof(FImpl),alignof(FImpl),sizeof(FImpl)},{TEXT("QueueNode"),sizeof(FNodeMirror),alignof(FNodeMirror),sizeof(FNodePrefix)+sizeof(FNodeMirror)},{TEXT("QueuePrefix"),sizeof(FNodePrefix),alignof(FNodePrefix),sizeof(FNodePrefix)}};
 for (uint32 N=0;N<UE_ARRAY_COUNT(Rows) && N<Capacity;++N) Out[N]=Rows[N]; return UE_ARRAY_COUNT(Rows);
}
} // namespace VaRest::Inspector::Private
#endif
