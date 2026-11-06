// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestNativeOperationStore.h"

class FVaRestExchange;

namespace VaRest::Http
{
struct FClientConfig;
class FManagedDispatchSlots;
class FManagedOutcomeSlots;
class FManagedCompletionPayload;
class FManagedCompletionAccess;

/**
 * Actual GT-affine owners of a managed operation, installed while its cell is Claimed, before current-config/Exchange
 * construction. This is a private ownership foundation; public Send, dispatch/decode callers and delivery routing are separate.
 * The cell's existing ReleaseAnchor uniquely owns this payload. Parked reservations must be serviced BEFORE the final
 * reclamation predicate, otherwise their own authority would prevent the anchor that releases them from ever running.
 */
class FManagedOperationPayload final
{
public:
	~FManagedOperationPayload();
	FManagedOperationPayload(const FManagedOperationPayload&) = delete;
	FManagedOperationPayload& operator=(const FManagedOperationPayload&) = delete;

	/** Write directly into anchored members while Claimed, never into an unanchored last-owner sender local. */
	TSharedPtr<const FClientConfig, ESPMode::ThreadSafe> SendConfig;
	TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> Exchange;
	/** GT under a real held operation authority; actual dispatch payload storage remains in this anchor. */
	FManagedDispatchSlots& DispatchesOnGT();
	/** GT under a real held operation authority; stage callbacks stay in this same uniquely owned anchor. */
	FManagedOutcomeSlots& OutcomesOnGT();
	/** One typed completion frame, under the SAME actual held operation authority. */
	FManagedCompletionPayload& CompletionOnGT();
	/** Identity only, never cell-access authority. Claim owner uses it before release publication initializes Exchange. */
	const FNativeOperationRef& Reference() const { return Ref; }

	/** Different incoming objects only. Initial Enter keeps its independent Handoff through the complete active stack. */
	void ParkCompletionOnGT(FNativeLogicalTailLease&& CompletionLease, FNativeLogicalTailLease&& DeliveryWorker);
	/** First selector only; different empty outputs. Aliasing refuses without selecting/moving parked or caller authority. */
	bool TakeCompletionOnGT(FNativeLogicalTailLease& OutCompletion, FNativeLogicalTailLease& OutDeliveryWorker);

private:
	friend class FNativeOperationStore;
	friend class FNativeOperationReservation;
	friend class FManagedCompletionAccess;
	FManagedOperationPayload(FNativeOperationRef InRef, FNativeInputBorrow&& InInput);
	bool MatchesOperation(const FNativeOperationRef& Other) const;
	/** Called by the existing GT sweep before checking authority-zero/work-release, with outer reentrancy protection. */
	void ServiceReadyOnGT(bool bStopped);
	/** ReleaseAnchor only, after every exact-generation authority has ended. Whole input is finished/reset last. */
	void DestroyOnGT(bool bDiscarded);
	FNativeOperationRef Ref;
	FNativeInputBorrow Input;
	TUniquePtr<FManagedDispatchSlots> Dispatches;
	TUniquePtr<FManagedOutcomeSlots> Outcomes;
	TUniquePtr<FManagedCompletionPayload> Completion;
	FNativeLogicalTailLease ParkedCompletion;
	TSharedPtr<FManagedCompletionAccess, ESPMode::ThreadSafe> ParkedNativeCompletion;
	FNativeLogicalTailLease ParkedDeliveryWorker;
	bool bCompletionSelected = false;
	bool bDestroyed = false;
};
} // namespace VaRest::Http
