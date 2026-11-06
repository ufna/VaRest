// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Containers/Ticker.h"
#include "VaRestNativeLifetime.h"

namespace VaRest::Http
{
/**
 * GT/module-owned lifetime of the single native Control and its one weak ticker. Workers never own or look up
 * this service. The module owns its hooks and the bounded operation/input/queue-drain advancement.
 */
class FNativeLifetimeService final
{
public:
	/** Module uses the core ticker; native fixtures can own an isolated real FTSTicker without a global drain. */
	static TUniquePtr<FNativeLifetimeService> CreateOnGT(FTSTicker& Ticker = FTSTicker::GetCoreTicker());
	~FNativeLifetimeService();
	FNativeLifetimeService(const FNativeLifetimeService&) = delete;
	FNativeLifetimeService& operator=(const FNativeLifetimeService&) = delete;
	/** Private managed factory only; never a worker's late-settlement path. Null after BeginStopping. */
	TSharedPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> ControlForNewClientOnGT() const;
	/** No cancellation/trace/user callout: close every permanent gate before owned pre-exit cancellation. */
	void BeginStoppingOnGT();
	/** Graceful pre-exit only, while the HTTP module is alive; every gate was closed before these cancel callouts. */
	void DetachQueuesForPreExitOnGT();
	void FlushQuiescentQueuesForPreExitOnGT();
	/** Caller first applies exit/user suppression and detaches GT-only adapters. Does not call transport Cancel. */
	void StopOnGT();

private:
	FNativeLifetimeService() = default;
	TSharedPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> Control;
	FTSTicker::FDelegateHandle TickHandle;
	bool bStopping = false;
	bool bPreExitQueuesDetached = false;
};

/** Private GT factory acquisition only. Does not load a module; captured worker settlement never calls this. */
TSharedPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> NativeControlForNewClientOnGT();
} // namespace VaRest::Http
