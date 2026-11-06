// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestNativeOperationStore.h"
#include "VaRestResultHandler.h"

class FVaRestExchange;

namespace VaRest::Http
{
class FManagedOperationPayload;
class FManagedCompletionAccess;
class FManagedCompletionWorker;
class FManagedCancelTask;
class FClientState;
using FManagedCompletionRef = TSharedRef<FManagedCompletionAccess, ESPMode::ThreadSafe>;
using FManagedCompletionKernel = TUniqueFunction<void(FManagedCompletionRef)>;

/** One actual completion's affine data, inside the existing operation anchor. No handler/finish/task functor is a worker capture. */
class FManagedCompletionPayload final
{
public:
	~FManagedCompletionPayload();
	TUniquePtr<IResultHandler> Handler;
	FFinish Finish;
	FNativeLogicalTailLease DeliveryWorker;
	FManagedCompletionKernel CompleteKernel;
	TUniqueFunction<void()> DiscardKernel;
	TUniqueFunction<void(const FFinish&, const TArray<uint8>&)> TerminalObserver;
	TArray<uint8> ReplayBody;
	bool bFinalized = false;
	bool bFinalizingInternal = false;
	bool bQueuedGTK = false;
	bool bDidDecode = false;
	bool bLaterFramePending = false;
	bool bLaterFrameReady = false;
	bool WorkerDidRun() const { return bWorkerDidRun.load(std::memory_order_relaxed); } // GTK only AFTER ended acquire/prerequisite
	FVaRestExchange& Exchange() const { return *Target; }

private:
	friend class FManagedOperationPayload;
	friend class FManagedCompletionAccess;
	friend class FManagedCompletionWorker;
	FManagedCompletionPayload(FNativeOperationRef InRef, FVaRestExchange* InTarget);
	void ServiceReadyOnGT(bool bStopped);
	void DisposeDataOnGT(bool bDiscarded);
	void DestroyOnGT(bool bDiscarded);
	FNativeOperationRef Ref;
	FVaRestExchange* Target;
	FNativeLogicalTailLease ReturnedCompletion; // abandoned native continuation returns SAME permission before release-publishing access-ended
	FNativeLogicalTailLease WorkerPermission;
	TUniqueFunction<void()> WorkerKernel;
	FManagedCompletionKernel AfterWorker;
	std::atomic<bool> bAccessEnded{false};
	std::atomic<bool> bWorkerEnded{false};
	std::atomic<bool> bWorkerDidRun{false};
	uint64 WorkerGeneration = 0;
	bool bWorkerActive = false;
	bool bDataDisposed = false;
	bool bCleaning = false;
	bool bServicing = false;
};

/** Native-only shared access to the ONE already-reserved completion tail; actual worker has a separate existing permission. */
class FManagedCompletionAccess final : public TSharedFromThis<FManagedCompletionAccess, ESPMode::ThreadSafe>
{
public:
	static FManagedCompletionRef CreateOnGT(FManagedCompletionPayload& Owner, FNativeLogicalTailLease&& Completion);
	void BindFrameOnGT(FManagedCompletionPayload& Frame);
	FManagedOperationPayload& OperationOnGT() const;
	~FManagedCompletionAccess();
	/** One coalesced native named-GTK cancel under this SAME initial Completion, not an additional child. */
	void QueueCancel();
	void CancelOnGT();
	void ServicePendingCancelOnGT(); // discarded native task fallback only; default route is the named-GTK task
	void CompleteOnGT();
	void QueueCompleteOnGT();
	/** All three owners are consumed only while live; GTK caller publishes worker and prerequisite GTK return together. */
	bool StartWorkerOnGT(FNativeLogicalTailLease&& Permission, TUniqueFunction<void()>&& Work, FManagedCompletionKernel&& After);
	void DisposeOnGT(bool bDiscarded);
	FManagedCompletionPayload& Payload() const
	{
		auto* Frame = BoundFrame.load(std::memory_order_acquire);
		check(Frame);
		return *Frame;
	} // caller holds this actual shared access through EVERY use
	TSharedPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> Control() const { return Ref.Control; }
	const FNativeOperationRef& Reference() const { return Ref; } // identity only; this Access owns actual completion authority

private:
	friend class FManagedCompletionWorker;
	friend class FManagedCompletionPayload;
	friend class FManagedCancelTask;
	friend class FNativeOperationReservation;
	/** Sole caller PublishManaged, inside exclusive Claim and its infallible publication sequence. */
	static FManagedCompletionRef CreateForClaim(FManagedOperationPayload& Operation, FNativeLogicalTailLease&& Completion);
	FManagedCompletionAccess(FManagedCompletionPayload& InOwner, FNativeLogicalTailLease&& InCompletion);
	FManagedCompletionAccess(FManagedOperationPayload& InOperation, FNativeLogicalTailLease&& InCompletion);
	void LaunchWorkerOnGT();
	void WorkerReturnedOnGT();
	FNativeOperationRef Ref;
	FNativeLogicalTailLease Completion;
	FManagedCompletionPayload* Owner = nullptr;    // GTK only, bound once before any decode/delivery worker launch
	FManagedOperationPayload* Operation = nullptr; // immutable native raw identity, valid only under actual Completion authority
	std::atomic<FManagedCompletionPayload*> BoundFrame{nullptr};
	std::atomic<bool> bCancelPosted{false};
	std::atomic<bool> bCancelTaskEnded{false};
	bool bCancelInvoked = false; // GTK only; task end acquire precedes fallback read
};
} // namespace VaRest::Http
