// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestSession.h"
#include "VaRestStage.h"

namespace VaRest::Http
{
struct FCacheEntry;
struct FQueuePrepareResult;
struct FQueueCommitResult;
struct FIoResult;
/** Actual GTK stage/timer/task callers only; ordinary clients retain the original callback unchanged. */
bool PrepareManagedGTCallback(const FVaRestExchangeRef& Exchange, TUniqueFunction<void()>& Callback);
/** A request-gate waiter uses the SAME operation-owned callback slots/tails; its deferred adapter owns native data only. */
bool PrepareManagedRefreshWaiter(const FVaRestExchangeRef& Exchange, FRefreshWaiter& Callback);
bool PrepareManagedCacheRead(const FVaRestExchangeRef& Exchange,
	TUniqueFunction<void(TSharedPtr<const FCacheEntry, ESPMode::ThreadSafe>)>& Callback);
bool PrepareManagedQueuePrepare(const FVaRestExchangeRef& Exchange, TUniqueFunction<void(FQueuePrepareResult)>& Callback);
bool PrepareManagedQueueCommit(const FVaRestExchangeRef& Exchange, TUniqueFunction<void(FQueueCommitResult)>& Callback);
bool PrepareManagedQueueCleanup(const FVaRestExchangeRef& Exchange, TUniqueFunction<void(bool)>& Callback);
bool PrepareManagedPersistenceResult(const FVaRestExchangeRef& Exchange, TUniqueFunction<void(FIoResult&&)>& Callback);
bool IsManagedStopped(const FVaRestExchangeRef& Exchange);
bool IsManagedClosed(const FVaRestExchangeRef& Exchange);
FVaRestAttemptOutcome MakeManagedCapacityOutcome(FName Detail);
} // namespace VaRest::Http
