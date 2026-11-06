// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace VaRest::Http
{
class FDynamicFrameTicket;
struct FDynamicCompletionPayload;
class FDynamicTypedState;

/**
 * Native-only producer for one already-admitted dynamic frame. The actual finish/kernel is stored exclusively
 * in the module's GT-owned frame payload. This object can be retained/dropped by a native worker after Stop.
 */
class FDynamicCompletion final : public TSharedFromThis<FDynamicCompletion, ESPMode::ThreadSafe>
{
public:
	static TSharedPtr<FDynamicCompletion, ESPMode::ThreadSafe> ReserveOnGT();
	/** Select once, before terminal bookkeeping. Failure preserves Kernel; success never invokes it inline. */
	bool DeferOnGT(TUniqueFunction<void()>& Kernel);
	bool BindWorkerOnGT(TUniqueFunction<void()>& Kernel);
	void NotifyReadyFromWorker();
	bool IsStopped() const;
	void CancelOnGT();
	/** Only the GT-owned frame payload strongly owns reflected state. Workers retain this native producer only. */
	bool SetTypesOnGT(TSharedRef<FDynamicTypedState,ESPMode::ThreadSafe> Types);
	TSharedPtr<FDynamicTypedState,ESPMode::ThreadSafe> TypesOnGT() const;
	/** One admitted action anchor/cleanup. Its original callable lives and dies only in the reserved GT payload. */
	bool SetActionLifetimeOnGT(TUniqueFunction<void()>&& Cleanup);

private:
	FDynamicCompletion(TSharedRef<FDynamicFrameTicket, ESPMode::ThreadSafe> InTicket,
		TWeakPtr<FDynamicCompletionPayload, ESPMode::ThreadSafe> InPayload);
	TSharedRef<FDynamicFrameTicket, ESPMode::ThreadSafe> Ticket;
	TWeakPtr<FDynamicCompletionPayload, ESPMode::ThreadSafe> Payload;
};
}
