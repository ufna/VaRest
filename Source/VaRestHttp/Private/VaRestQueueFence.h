// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include <atomic>

namespace VaRest::Http
{
class FNativeLifetimeControl;
class FNativeClientSlots;

enum class EQueueFenceResult : uint8
{
	Pending,
	Flushed,
	Failed,
	NotAdmitted,
	ShutdownDiscarded
};

/** Inline native part of one of the TWO preallocated drain rows. No queue/client/clock/UObject is read by a token. */
struct FNativeQueueFenceState
{
	std::atomic<uint64> Generation{0};
	std::atomic<uint64> ClientGeneration{0};
	std::atomic<uint32> Outstanding{0};
	std::atomic<EQueueFenceResult> Result{EQueueFenceResult::Pending};
};

/** Actual accepted barrier callback authority. Destruction records discard; never posts work or touches a GT payload. */
class FNativeQueueFenceToken final
{
public:
	FNativeQueueFenceToken() = default;
	~FNativeQueueFenceToken();
	FNativeQueueFenceToken(FNativeQueueFenceToken&& Other) noexcept;
	FNativeQueueFenceToken& operator=(FNativeQueueFenceToken&& Other) noexcept;
	FNativeQueueFenceToken(const FNativeQueueFenceToken&) = delete;
	FNativeQueueFenceToken& operator=(const FNativeQueueFenceToken&) = delete;
	bool IsValid() const { return State != nullptr; }
	bool AttachNamespaceCounter(TSharedRef<std::atomic<uint32>, ESPMode::ThreadSafe> Counter);
	void SettleOnce(EQueueFenceResult Result);

private:
	friend class FNativeClientSlots;
	FNativeQueueFenceToken(TSharedRef<FNativeLifetimeControl, ESPMode::ThreadSafe> InControl,
		FNativeQueueFenceState& InState, uint64 InGeneration, uint64 InClientGeneration);
	TSharedPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> Control;
	TSharedPtr<std::atomic<uint32>, ESPMode::ThreadSafe> NamespaceCounter;
	FNativeQueueFenceState* State = nullptr;
	uint64 Generation = 0, ClientGeneration = 0;
};
} // namespace VaRest::Http
