// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestManagedCallbacks.h"
#include "VaRestClientState.h"
#include "VaRestManagedOperation.h"
#include "VaRestManagedOutcome.h"
#include "VaRestMemoryCache.h"
#include "VaRestQueueStore.h"
#include "VaRestPersistence.h"

namespace VaRest::Http
{
bool IsManagedStopped(const FVaRestExchangeRef& Exchange)
{
	const auto& Ref = Internals(*Exchange).Managed;
	return Ref.IsValid() && Ref.Control->IsStopped();
}
bool IsManagedClosed(const FVaRestExchangeRef& Exchange)
{
	check(IsInGameThread());
	const auto& Ex = Internals(*Exchange);
	return Ex.Managed.IsValid() && (!Ex.State || Ex.State->GetManagedLifetime().Gate->IsClosed());
}
bool PrepareManagedGTCallback(const FVaRestExchangeRef& Exchange, TUniqueFunction<void()>& Callback)
{
	check(IsInGameThread());
	const auto& Ref = Internals(*Exchange).Managed;
	if (!Ref.IsValid())
		return true;
	const auto Control = Ref.Control;
	if (Control->IsStopped() || IsManagedClosed(Exchange))
		return false;
	// One active GTK preparation and one retained callback are separate obligations included in the actual shared64.
	auto Stack = Ref.Store->TryAddLogicalTailOnGT(Ref);
	if (!Stack.IsHeld())
		return false;
	auto* Payload = Stack.ManagedPayloadOnGT();
	check(Payload);
	auto Adapter = Payload->OutcomesOnGT().WrapTaskOnGT(Callback);
	if (!Adapter || Control->IsStopped() || IsManagedClosed(Exchange))
		return false;
	Callback = MoveTemp(Adapter);
	return true;
}
FVaRestAttemptOutcome MakeManagedCapacityOutcome(FName Detail)
{
	FVaRestAttemptOutcome Refused;
	Refused.Failure = EVaRestTransportFailure::Other;
	Refused.FailureDetail = Detail;
	return Refused;
}
bool PrepareManagedRefreshWaiter(const FVaRestExchangeRef& Exchange, FRefreshWaiter& Callback)
{
	check(IsInGameThread());
	if (!Internals(*Exchange).Managed.IsValid())
		return true;
	// This cell contains plain native result data, never the affine original callback/run/config.
	const auto Result = MakeShared<FRefreshEnded, ESPMode::ThreadSafe>();
	TUniqueFunction<void()> Invoke = [Original = MoveTemp(Callback), Result]() mutable { Original(*Result); };
	if (!PrepareManagedGTCallback(Exchange, Invoke))
		return false; // failed original destruction stays on this GT call stack
	Callback = [Native = MoveTemp(Invoke), Result](const FRefreshEnded& End) mutable {
		check(IsInGameThread());
		*Result = End;
		Native();
	};
	return true;
}
bool PrepareManagedCacheRead(const FVaRestExchangeRef& Exchange,
	TUniqueFunction<void(TSharedPtr<const FCacheEntry, ESPMode::ThreadSafe>)>& Callback)
{
	check(IsInGameThread());
	if (!Internals(*Exchange).Managed.IsValid())
		return true;
	// Cache entries are plain immutable native headers/bytes/metadata. Only the original callback/run is GT-affine.
	const auto Result = MakeShared<TSharedPtr<const FCacheEntry, ESPMode::ThreadSafe>, ESPMode::ThreadSafe>();
	TUniqueFunction<void()> Invoke = [Original = MoveTemp(Callback), Result]() mutable { Original(MoveTemp(*Result)); };
	if (!PrepareManagedGTCallback(Exchange, Invoke))
		return false;
	Callback = [Native = MoveTemp(Invoke), Result](TSharedPtr<const FCacheEntry, ESPMode::ThreadSafe> Entry) mutable { check(IsInGameThread());*Result=MoveTemp(Entry);Native(); };
	return true;
}
bool PrepareManagedQueuePrepare(const FVaRestExchangeRef& Exchange, TUniqueFunction<void(FQueuePrepareResult)>& Callback)
{
	if (!Internals(*Exchange).Managed.IsValid()) return true;
	const auto Result=MakeShared<FQueuePrepareResult, ESPMode::ThreadSafe>();
	TUniqueFunction<void()> Invoke=[Original=MoveTemp(Callback),Result]() mutable { Original(*Result); };
	if (!PrepareManagedGTCallback(Exchange,Invoke)) return false;
	Callback=[Native=MoveTemp(Invoke),Result](FQueuePrepareResult Value) mutable { check(IsInGameThread());*Result=Value;Native(); };
	return true;
}
bool PrepareManagedQueueCommit(const FVaRestExchangeRef& Exchange, TUniqueFunction<void(FQueueCommitResult)>& Callback)
{
	if (!Internals(*Exchange).Managed.IsValid()) return true;
	const auto Result=MakeShared<FQueueCommitResult, ESPMode::ThreadSafe>();
	TUniqueFunction<void()> Invoke=[Original=MoveTemp(Callback),Result]() mutable { Original(*Result); };
	if (!PrepareManagedGTCallback(Exchange,Invoke)) return false;
	Callback=[Native=MoveTemp(Invoke),Result](FQueueCommitResult Value) mutable { check(IsInGameThread());*Result=Value;Native(); };
	return true;
}
bool PrepareManagedQueueCleanup(const FVaRestExchangeRef& Exchange, TUniqueFunction<void(bool)>& Callback)
{
	if (!Internals(*Exchange).Managed.IsValid()) return true;
	const auto Result=MakeShared<bool, ESPMode::ThreadSafe>(false);
	TUniqueFunction<void()> Invoke=[Original=MoveTemp(Callback),Result]() mutable { Original(*Result); };
	if (!PrepareManagedGTCallback(Exchange,Invoke)) return false;
	Callback=[Native=MoveTemp(Invoke),Result](bool Value) mutable { check(IsInGameThread());*Result=Value;Native(); };
	return true;
}
bool PrepareManagedPersistenceResult(const FVaRestExchangeRef& Exchange, TUniqueFunction<void(FIoResult&&)>& Callback)
{
	if (!Internals(*Exchange).Managed.IsValid()) return true;
	const auto Result=MakeShared<FIoResult, ESPMode::ThreadSafe>();
	TUniqueFunction<void()> Invoke=[Original=MoveTemp(Callback),Result]() mutable { Original(MoveTemp(*Result)); };
	if (!PrepareManagedGTCallback(Exchange,Invoke)) return false;
	Callback=[Native=MoveTemp(Invoke),Result](FIoResult&& Value) mutable { check(IsInGameThread());*Result=MoveTemp(Value);Native(); };
	return true;
}
} // namespace VaRest::Http
