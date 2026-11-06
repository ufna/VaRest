// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include <atomic>

namespace VaRest::Http
{
struct FQueueMemberKey
{
	uint64 ClientId = 0;
	uint64 Epoch = 0;
	bool operator==(const FQueueMemberKey& Other) const { return ClientId == Other.ClientId && Epoch == Other.Epoch; }
	friend uint32 GetTypeHash(const FQueueMemberKey& Key) { return HashCombineFast(::GetTypeHash(Key.ClientId), ::GetTypeHash(Key.Epoch)); }
};

/** Native-only authority. Closing and acquiring share one word, including a sender paused inside acquisition. */
class FQueueCaptureGate final
{
public:
	bool TryAcquire()
	{
		uint64 Seen = Word.load(std::memory_order_acquire);
		while (!(Seen & Closed) && Seen != Closed - 1)
			if (Word.compare_exchange_weak(Seen, Seen + 1, std::memory_order_acq_rel, std::memory_order_acquire))
				return true;
		return false;
	}
	void Release()
	{
		const uint64 Before = Word.fetch_sub(1, std::memory_order_acq_rel);
		check((Before & ~Closed) != 0);
	}
	void Close() { Word.fetch_or(Closed, std::memory_order_acq_rel); }
	bool IsClosed() const { return (Word.load(std::memory_order_acquire)&Closed)!=0; }
	uint64 Count() const { return Word.load(std::memory_order_acquire) & ~Closed; }

private:
	static constexpr uint64 Closed = uint64(1) << 63;
	std::atomic<uint64> Word{0};
};

class FQueueBinding;

/** One actual Send capture, never an external immutable config/builder reference. No affine payload is owned here. */
class FQueueCaptureLease final
{
public:
	FQueueCaptureLease() = default;
	~FQueueCaptureLease() { Reset(); }
	FQueueCaptureLease(FQueueCaptureLease&& Other) noexcept
		: Binding(MoveTemp(Other.Binding))
	{
	}
	FQueueCaptureLease& operator=(FQueueCaptureLease&& Other) noexcept
	{
		if (this != &Other)
		{
			Reset();
			Binding = MoveTemp(Other.Binding);
		}
		return *this;
	}
	FQueueCaptureLease(const FQueueCaptureLease&) = delete;
	FQueueCaptureLease& operator=(const FQueueCaptureLease&) = delete;
	bool IsHeld() const { return Binding.IsValid(); }
	const TSharedPtr<FQueueBinding, ESPMode::ThreadSafe>& GetBinding() const { return Binding; }
	void Reset();

private:
	friend class FQueueBinding;
	TSharedPtr<FQueueBinding, ESPMode::ThreadSafe> Binding;
};

/** Exact registration identity. It retains neither client/config/queue nor a UObject or callback. */
class FQueueBinding final : public TSharedFromThis<FQueueBinding, ESPMode::ThreadSafe>
{
public:
	FQueueBinding(FQueueMemberKey InKey, TSharedRef<FQueueCaptureGate, ESPMode::ThreadSafe> InCoordinator)
		: Key(InKey)
		, Coordinator(MoveTemp(InCoordinator))
	{
	}
	const FQueueMemberKey& GetKey() const { return Key; }
	/** Called under the SAME client config read lock as the Send snapshot. No callout or IO. */
	bool TryCapture(FQueueCaptureLease& Out)
	{
		if (Out.IsHeld() || !Coordinator->TryAcquire())
			return false;
		// The coordinator count is installed FIRST. A paused per-binding acquisition cannot disappear from fencing.
		if (!Captures.TryAcquire())
		{
			Coordinator->Release();
			return false;
		}
		Out.Binding = AsShared();
		return true;
	}
	/** Config replacement closes capture admission under that client's config WRITE lock. Existing leases survive. */
	void CloseCaptureAdmission() { Captures.Close(); }
	/** Additional managed revocation; ordinary legacy replacement does not revoke complete Send snapshots. */
	void RevokeOnGT()
	{
		check(IsInGameThread());
		bRevoked.store(true, std::memory_order_release);
		CloseCaptureAdmission();
	}
	bool AllowsEnqueue() const { return !bRevoked.load(std::memory_order_acquire); }
	uint64 NumCaptures() const { return Captures.Count(); }
	bool CaptureAdmissionIsClosed() const { return Captures.IsClosed(); }

private:
	friend class FQueueCaptureLease;
	FQueueMemberKey Key;
	TSharedRef<FQueueCaptureGate, ESPMode::ThreadSafe> Coordinator;
	FQueueCaptureGate Captures;
	std::atomic<bool> bRevoked{false};
};

inline void FQueueCaptureLease::Reset()
{
	if (Binding)
	{
		// The per-member count ends before the aggregate: a zero coordinator sample includes every final member release.
		Binding->Captures.Release();
		Binding->Coordinator->Release();
		Binding.Reset();
	}
}
} // namespace VaRest::Http
