// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestNativeOperationStore.h"
#include "VaRestStage.h"
#include <array>

namespace VaRest::Http
{
class FManagedOperationPayload;
class FManagedNativeOutcomeAccess;

/**
 * Actual stage callbacks stay in this operation-owned GT storage. An adapter contains only its native access owner,
 * never the original callback/config/exchange captures. Every occupied position has one genuine logical tail INCLUDED
 * in the operation's64, until that adapter and any active invocation end. An uncalled drop settles its authority only;
 * it does not invent a stage outcome or normal completion. Stage callbacks retain their existing GT invocation contract.
 */
class FManagedOutcomeSlots final
{
public:
	~FManagedOutcomeSlots();
	FManagedOutcomeSlots(const FManagedOutcomeSlots&) = delete;
	FManagedOutcomeSlots& operator=(const FManagedOutcomeSlots&) = delete;
	/** GT. Reserve before exposing an adapter; failure leaves Original untouched for the caller's own error path. */
	FVaRestOutcomeCallback WrapOnGT(FVaRestOutcomeCallback& Original);
	/** Same64 positions/tails for actual GT timer/task captures; this does not authorize a worker invocation. */
	TUniqueFunction<void()> WrapTaskOnGT(TUniqueFunction<void()>& Original);

private:
	friend class FManagedOperationPayload;
	friend class FManagedNativeOutcomeAccess;
	explicit FManagedOutcomeSlots(FNativeOperationRef InRef);
	struct FSlot
	{
		uint64 Generation = 1;
		bool bOccupied = false;
		bool bRetired = false;
		bool bInvoked = false;
		bool bExecuting = false;
		std::atomic<bool> bAccessEnded{false};
		FVaRestOutcomeCallback Original;
		TUniqueFunction<void()> Task;
	};
	TSharedPtr<FManagedNativeOutcomeAccess, ESPMode::ThreadSafe> ReserveOnGT(FVaRestOutcomeCallback* Outcome, TUniqueFunction<void()>* Task);
	FSlot& HeldSlot(uint8 Index, uint64 Generation);
	void InvokeOnGT(uint8 Index, uint64 Generation, FVaRestAttemptOutcome&& Outcome);
	void InvokeTaskOnGT(uint8 Index, uint64 Generation);
	/** Owned GT sweep/local preparation only. A retained adapter remains a real unsettled obligation even at Stop. */
	void ServiceReadyOnGT();
	FNativeOperationRef Ref;
	std::array<FSlot, FManagedOperationSettlement::MaxLogicalTails> Slots;
	bool bServicing = false;
};
} // namespace VaRest::Http
