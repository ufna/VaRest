// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestQueueFence.h"
#include "VaRestNativeLifetime.h"

namespace VaRest::Http
{
FNativeQueueFenceToken::FNativeQueueFenceToken(TSharedRef<FNativeLifetimeControl, ESPMode::ThreadSafe> InControl,
	FNativeQueueFenceState& InState, uint64 InGeneration, uint64 InClientGeneration)
	: Control(MoveTemp(InControl))
	, State(&InState)
	, Generation(InGeneration)
	, ClientGeneration(InClientGeneration)
{
	check(IsInGameThread() && State->Outstanding.load(std::memory_order_relaxed) == 0);
	State->Outstanding.store(1, std::memory_order_release); // before publishing ANY callback; generation is already initialized
}
FNativeQueueFenceToken::~FNativeQueueFenceToken()
{
	SettleOnce(EQueueFenceResult::ShutdownDiscarded);
}
FNativeQueueFenceToken::FNativeQueueFenceToken(FNativeQueueFenceToken&& Other) noexcept
	: Control(MoveTemp(Other.Control))
	, NamespaceCounter(MoveTemp(Other.NamespaceCounter))
	, State(Other.State)
	, Generation(Other.Generation)
	, ClientGeneration(Other.ClientGeneration)
{
	Other.State = nullptr;
}
FNativeQueueFenceToken& FNativeQueueFenceToken::operator=(FNativeQueueFenceToken&& Other) noexcept
{
	if (this != &Other)
	{
		SettleOnce(EQueueFenceResult::ShutdownDiscarded);
		Control = MoveTemp(Other.Control);
		NamespaceCounter = MoveTemp(Other.NamespaceCounter);
		State = Other.State;
		Generation = Other.Generation;
		ClientGeneration = Other.ClientGeneration;
		Other.State = nullptr;
	}
	return *this;
}
bool FNativeQueueFenceToken::AttachNamespaceCounter(TSharedRef<std::atomic<uint32>, ESPMode::ThreadSafe> Counter)
{
	check(IsInGameThread() && State && !NamespaceCounter);
	uint32 Seen = Counter->load(std::memory_order_acquire);
	while (Seen < 512)
		if (Counter->compare_exchange_weak(Seen, Seen + 1, std::memory_order_acq_rel, std::memory_order_acquire))
		{
			NamespaceCounter = MoveTemp(Counter);
			return true;
		}
	return false;
}
void FNativeQueueFenceToken::SettleOnce(EQueueFenceResult Result)
{
	if (!State)
		return;
	check(Result != EQueueFenceResult::Pending);
	const auto Kept = MoveTemp(Control); // keep the exact Control allocation through this ENTIRE method
	const auto Counter = MoveTemp(NamespaceCounter);
	FNativeQueueFenceState* Native = State;
	State = nullptr;
	check(Native->Generation.load(std::memory_order_acquire) == Generation &&
		  Native->ClientGeneration.load(std::memory_order_acquire) == ClientGeneration);
	EQueueFenceResult Expected = EQueueFenceResult::Pending;
	Native->Result.compare_exchange_strong(Expected, Result, std::memory_order_release, std::memory_order_relaxed);
	if (Counter)
	{
		const uint32 Before = Counter->fetch_sub(1, std::memory_order_release);
		check(Before != 0); // no namespace IO/use follows this publication; the remaining step is native row release only
	}
	const uint32 Before = Native->Outstanding.fetch_sub(1, std::memory_order_release);
	check(Before == 1); // LAST native row access. GT acquire of zero permits row cleanup/reuse; no later pointer dereference.
}
} // namespace VaRest::Http
