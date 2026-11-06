// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FVaRestDynamicBudget;

namespace VaRest::Http
{
class FDynamicFrameControl;

/** Native-only authority; actual callback/pins stay in module-owned GT storage. Last drop is atomic-only. */
class FDynamicFrameTicket final : public TSharedFromThis<FDynamicFrameTicket, ESPMode::ThreadSafe>
{
public:
	~FDynamicFrameTicket();
	/** Actual GT return establishes readiness; strictly older GFrameCounter is required for invocation. */
	bool ReadyOnGT();
	/** Native-only final worker handoff. GT observes release, stamps its current frame, then waits another frame. */
	void NotifyReadyFromWorker();
	bool IsStopped() const;
	void CancelOnGT();

private:
	friend class FDynamicFrameControl;
	FDynamicFrameTicket(TSharedRef<FDynamicFrameControl, ESPMode::ThreadSafe> InControl, uint16 InIndex, uint64 InGeneration);
	TSharedRef<FDynamicFrameControl, ESPMode::ThreadSafe> Control;
	uint16 Index;
	uint64 Generation;
};

/** Task6's fixed256 module frame slots; reserve BEFORE worker launch/retention, never an overflow callback queue. */
class FFrameDispatcher final
{
public:
	static TUniquePtr<FFrameDispatcher> CreateOnGT(uint32 AdmissionLimit = 256);
	~FFrameDispatcher();
	/** Failure preserves Callback. This slot remains occupied until callback disposal AND last native ticket use. */
	TSharedPtr<FDynamicFrameTicket, ESPMode::ThreadSafe> TryReserveOnGT(TUniqueFunction<void()>& Callback);
	void StopOnGT();

private:
	TSharedPtr<FDynamicFrameControl, ESPMode::ThreadSafe> Control;
	FDelegateHandle EndFrameHandle;
};
/** Initial GT admission only. Late tickets never look up the module or a destroyed service. */
TSharedPtr<FDynamicFrameTicket, ESPMode::ThreadSafe> ReserveDynamicFrameOnGT(TUniqueFunction<void()>& Callback);
TSharedPtr<FVaRestDynamicBudget, ESPMode::ThreadSafe> DefaultDynamicBudgetOnGT();
}
