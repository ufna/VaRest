// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreTypes.h"
#include "Misc/Optional.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"
#include "VaRestInspectorAllocation.h"
#include "VaRestInspectorPacket.h"
#include <atomic>

class FEvent;

#if VAREST_WITH_INSPECTOR
namespace VaRest::Inspector::Private
{
enum class EPush : uint8
{
	Accepted,
	ScalarOnly,
	Dropped,
	Closed,
	Unsupported
};
#if WITH_DEV_AUTOMATION_TESTS
// Owned barriers only; no application callback from the worker data path.
struct FQueueTestHooks
{
	uint64 ReservedRequestId = 0, BeforeEnqueueRequestId = 0;
	FEvent* Reserved = nullptr;
	FEvent* ReleaseReserved = nullptr;
	FEvent* BeforeEnqueue = nullptr;
	FEvent* ReleaseEnqueue = nullptr;
};
#endif
class FCaptureQueue
{
public:
	// Stack/member wrapper. A production heap owner must independently admit
	// its wrapper/controller BEFORE construction; this baseline cannot do so
	// retroactively. The service factory owns that later integration boundary.
	FCaptureQueue(const FCapturePolicy& InPolicy, uint64 InGeneration,
		TSharedRef<FPendingAccount, ESPMode::ThreadSafe> InAccount, FAllocationAdapter& InAllocator);
	~FCaptureQueue();
	FCaptureQueue(const FCaptureQueue&) = delete;
	FCaptureQueue& operator=(const FCaptureQueue&) = delete;
	static TOptional<int64> BaselineCharge(const FAllocationAdapter& Allocator);
	bool IsSupported() const;
	EPush TryPush(const FVaRestTraceEvent& Borrowed);
	TOptional<FPacket> TryPopMetadata();
	TOptional<FPacket> TryPopExcerpt();
	void Close();
	uint64 LastPublishedMetadata() const;
	uint64 LossEpoch() const;
	int64 OutstandingExcerptReservations() const;
#if WITH_DEV_AUTOMATION_TESTS
	void SetTestHooks(FQueueTestHooks* InHooks);
#endif
private:
	struct FImpl;
	struct FImplDeleter
	{
		FAllocationAdapter* Allocator = nullptr;
		void operator()(FImpl* Ptr) const;
	};
	const FCapturePolicy Policy;
	const uint64 Generation;
	TSharedRef<FPendingAccount, ESPMode::ThreadSafe> Account;
	FAllocationAdapter* Allocator;
	FPendingBaseline Baseline;
	TUniquePtr<FImpl, FImplDeleter> Impl;
	std::atomic<bool> bClosed{false};
};
} // namespace VaRest::Inspector::Private
#endif
