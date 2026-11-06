// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestTrace.h"

namespace VaRest::Http
{
class FNativeLifetimeControl;
/**
 * Native scope metadata retained by its host/control and client configurations. The registry owns only weak refs.
 * Close records lifecycle AFTER its caller's quiescence fence; admission/retirement itself belongs to Task 5.
 * Destruction is plain data cleanup: no UObject dereference, trace emission or game-thread service lookup.
 */
class FNativeScopeLifetime final
{
public:
	static TSharedPtr<FNativeScopeLifetime, ESPMode::ThreadSafe> Open(FVaRestTraceScope Info, double Time);
	static void ContributeSnapshot(FVaRestTraceSnapshot& Snapshot);
	uint64 GetId() const { return Info.ScopeId; } // immutable after Open, any thread
	bool IsOpen() const
	{
		check(IsInGameThread());
		return Info.bOpened;
	}
	bool SetLabel(FStringView Label);
	void Close(double Time); // GT, idempotent; caller already settled all native work
	/** Hub closes all client gates first; native slot ownership keeps metadata alive until actual cleanup. */
	void RequestRetirementOnGT();
	bool TryAddClientOnGT(TSharedRef<FNativeLifetimeControl, ESPMode::ThreadSafe> Control);
	void ReleaseClientOnGT(bool bAllowCloseTrace);
	void AddSnapshotPinOnGT();
	void ReleaseSnapshotPinOnGT();
	uint32 NumOwnedClientsOnGT() const
	{
		check(IsInGameThread());
		return OwnedClients;
	}
	bool AcceptsClientsOnGT() const
	{
		check(IsInGameThread());
		return Info.bOpened && !bRetirementRequested;
	}

private:
	explicit FNativeScopeLifetime(FVaRestTraceScope InInfo)
		: Info(MoveTemp(InInfo))
	{
	}
	FVaRestTraceScope Info;
	uint32 OwnedClients = 0;
	uint64 SnapshotPins = 0; // synchronous trace attachment stack only; no background owner or completed history
	bool bRetirementRequested = false;
	bool bHasControl = false;
	TWeakPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> NativeControl; // immutable identity once bound, plain native metadata
};
} // namespace VaRest::Http
