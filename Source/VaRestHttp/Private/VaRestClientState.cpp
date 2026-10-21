// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestClientState.h"

#include "Misc/ScopeLock.h"
#include "Tasks/Task.h"
#include "VaRestHttpTesting.h"

namespace VaRest::Http::ClientStatePrivate
{
	/** Locked: a client state can die on any thread (a game's own worker may drop the last FVaRestClient) */
	struct FLiveRegistry
	{
		FCriticalSection Lock;
		TArray<TWeakPtr<FClientState, ESPMode::ThreadSafe>> States;
	};

	/**
	 * Leaked on purpose (review I-4): a global FVaRestClient in a game module is destroyed during static destruction,
	 * possibly after this file's statics; its ~FClientState must not lock a destroyed mutex or edit a freed array.
	 */
	FLiveRegistry& Live()
	{
		static FLiveRegistry* Registry = new FLiveRegistry();
		return *Registry;
	}
}

VaRest::Http::FClientState::FClientState(uint64 InClientId, FClientConfig&& InConfig)
	: ClientId(InClientId)
	, Config(MakeShared<FClientConfig, ESPMode::ThreadSafe>(MoveTemp(InConfig)))
{
}

VaRest::Http::FClientState::~FClientState()
{
	if (!IsInGameThread())
	{
#if !UE_BUILD_SHIPPING
		++Testing::Counters().ClientStatesDestroyedOffGameThread;
#endif
		// Review I-1 backstop: the configuration (custom stages, transport, clock) dies on the game thread. Moved, not
		// copied (review N-3), so this thread keeps no reference; the task body dies on the game thread right after it
		// runs (TaskPrivate.h:871-877).
		UE::Tasks::Launch(TEXT("VaRest.ReleaseConfig"), [Doomed = MoveTemp(Config)] {}, UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
	}
	ClientStatePrivate::FLiveRegistry& Registry = ClientStatePrivate::Live();
	FScopeLock Lock(&Registry.Lock);
	Registry.States.RemoveAll([this](const TWeakPtr<FClientState, ESPMode::ThreadSafe>& Weak) { return !Weak.IsValid() || Weak.HasSameObject(this); });
}

TSharedRef<VaRest::Http::FClientState, ESPMode::ThreadSafe> VaRest::Http::FClientState::Create(FClientConfig&& InConfig)
{
	check(IsInGameThread());
	TSharedRef<FClientState, ESPMode::ThreadSafe> Created = MakeShared<FClientState, ESPMode::ThreadSafe>(FVaRestTrace::NewClientId(), MoveTemp(InConfig));
	Created->EditConfig([](FClientConfig&) {});
	ClientStatePrivate::FLiveRegistry& Registry = ClientStatePrivate::Live();
	FScopeLock Lock(&Registry.Lock);
	Registry.States.Add(Created);
	return Created;
}

TSharedRef<const VaRest::Http::FClientConfig, ESPMode::ThreadSafe> VaRest::Http::FClientState::GetConfig() const
{
	FReadScopeLock ReadLock(ConfigLock);
	return Config.ToSharedRef();
}

void VaRest::Http::FClientState::EditConfig(TFunctionRef<void(FClientConfig&)> Edit)
{
	check(IsInGameThread());
	TSharedRef<FClientConfig, ESPMode::ThreadSafe> Next = MakeShared<FClientConfig, ESPMode::ThreadSafe>(*GetConfig());
	Edit(*Next);
	Publish(Next);
}

void VaRest::Http::FClientState::Publish(const TSharedRef<FClientConfig, ESPMode::ThreadSafe>& Next)
{
	// Spec 4's chain. Custom Outer stages first: they see every logical request once (decision 13); M4's built-in Outer
	// stages (offline queue, cache, retry) go between them and the attempt boundary. Custom PerAttempt stages see the
	// final headers; the built-ins next to the transport (log stage, Task 14; limiter, Task 12) come last.
	TSharedRef<FVaRestStageChain, ESPMode::ThreadSafe> NewChain = MakeShared<FVaRestStageChain, ESPMode::ThreadSafe>();
	bool bReissue = false;
	for (const TPair<FVaRestStageRef, EVaRestStageSlot>& Entry : Next->CustomStages)
	{
		if (Entry.Value == EVaRestStageSlot::Outer)
		{
			NewChain->Add(Entry.Key);
			bReissue |= Entry.Key->MayReissue();
		}
	}
	NewChain->Add(MakeAttemptBoundaryStage());
	for (const TPair<FVaRestStageRef, EVaRestStageSlot>& Entry : Next->CustomStages)
	{
		if (Entry.Value == EVaRestStageSlot::PerAttempt)
		{
			NewChain->Add(Entry.Key);
			bReissue |= Entry.Key->MayReissue();
		}
	}
	NewChain->Add(MakeLogStage()); // after custom PerAttempt stages: it logs the final headers
	NewChain->Add(MakeLimiterStage()); // spec 4.6: "next to the transport, so queued retries wait their turn too"
	NewChain->Add(MakeTransportStage());
	Next->Chain = NewChain;
	Next->bMayReissue = bReissue;

	FVaRestRedactRules Rules;
	for (const FString& Key : Next->RedactKeys)
	{
		Rules.AddKey(Key);
	}
	Next->Redactor = MakeShared<FVaRestRedactor, ESPMode::ThreadSafe>(MoveTemp(Rules), Secrets.GetActive(Next->Clock->Now()));

	// The replaced snapshot is released after the lock is dropped: with it can go the last reference to a transport, a
	// clock or a stage, whose destructor may read a configuration, and FRWLock is not recursive
	TSharedPtr<const FClientConfig, ESPMode::ThreadSafe> Replaced;
	{
		FWriteScopeLock WriteLock(ConfigLock);
		Replaced = MoveTemp(Config);
		Config = Next;
	}
	const int32 Wanted = FMath::Max(1, Next->MaxConcurrent);
	if (Limiter.GetCapacity() != Wanted)
	{
		// Review M-2: only on a change, and the grants from a later game-thread task -- a grant can run a request to its
		// end at once, and Publish runs inside WithHeader(), SetSecretValue() and the like (decision 3)
		Limiter.SetCapacity(Wanted);
		UE::Tasks::Launch(TEXT("VaRest.LimiterPump"), [Weak = TWeakPtr<FClientState, ESPMode::ThreadSafe>(AsShared())]
		{
			if (const TSharedPtr<FClientState, ESPMode::ThreadSafe> Pinned = Weak.Pin())
			{
				Pinned->Limiter.Pump();
			}
		}, UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
	}
}

void VaRest::Http::FClientState::ForEachLive(TFunctionRef<void(FClientState&)> Visit)
{
	check(IsInGameThread());
	TArray<TSharedRef<FClientState, ESPMode::ThreadSafe>> Alive;
	{
		ClientStatePrivate::FLiveRegistry& Registry = ClientStatePrivate::Live();
		FScopeLock Lock(&Registry.Lock);
		for (const TWeakPtr<FClientState, ESPMode::ThreadSafe>& Weak : Registry.States)
		{
			if (TSharedPtr<FClientState, ESPMode::ThreadSafe> Pinned = Weak.Pin())
			{
				Alive.Add(Pinned.ToSharedRef());
			}
		}
	}
	for (const TSharedRef<FClientState, ESPMode::ThreadSafe>& Client : Alive)
	{
		Visit(*Client);
	}
	// Review M-9: a pin taken here can be a state's last reference. Released here, inside FVaRestTrace::AddSink's
	// snapshot, the state's Client{Destroyed} would go out before the new sink is added, and the sink would keep a ghost
	// client. Released from a later game-thread task, it reaches the sink after OnAttached.
	UE::Tasks::Launch(TEXT("VaRest.ReleaseSnapshotPins"), [Kept = MoveTemp(Alive)] {}, UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
}

void VaRest::Http::FClientState::SetSecret(FName Slot, const FString& Value)
{
	check(IsInGameThread());
	const double Now = GetConfig()->Clock->Now();
	Secrets.Prune(Now); // review C1: expired values never count toward the next expiry
	Secrets.Set(Slot, Value, Now);
	EditConfig([](FClientConfig&) {});
	ScheduleSecretExpiry();
}

void VaRest::Http::FClientState::ClearSecrets()
{
	check(IsInGameThread());
	Secrets.ClearAll();
	GetConfig()->Clock->ClearTimer(SecretExpiryTimer);
	EditConfig([](FClientConfig&) {});
}

void VaRest::Http::FClientState::SetClock(const TSharedRef<IVaRestClock, ESPMode::ThreadSafe>& NewClock)
{
	check(IsInGameThread());
	// Task 14 review M6: an expiry is a time on the clock it was set with. Left as it is, the old clock's timer would prune at
	// its deadline read on the new clock, and GetActive would compare old times with new ones: a rotated value could stop
	// being scrubbed before its 10 minutes. Each keeps the time it had left.
	const TSharedRef<const FClientConfig, ESPMode::ThreadSafe> Current = GetConfig();
	Current->Clock->ClearTimer(SecretExpiryTimer);
	Secrets.Rebase(NewClock->Now() - Current->Clock->Now());
	EditConfig([&NewClock](FClientConfig& Edited) { Edited.Clock = NewClock; });
	ScheduleSecretExpiry();
}

void VaRest::Http::FClientState::ScheduleSecretExpiry()
{
	const TSharedRef<const FClientConfig, ESPMode::ThreadSafe> Current = GetConfig();
	Current->Clock->ClearTimer(SecretExpiryTimer);
	// Review C1: prune first, and only a value still pending counts -- an expired one would re-arm a timer due at once,
	// forever (a hang under the virtual clock, a republished configuration every frame under the real one)
	const double Now = Current->Clock->Now();
	Secrets.Prune(Now);
	const double ExpiresAt = Secrets.NextExpiry(Now);
	if (ExpiresAt < 0.0)
	{
		return;
	}
	const TWeakPtr<FClientState, ESPMode::ThreadSafe> Weak = AsShared();
	SecretExpiryTimer = Current->Clock->SetTimer(ExpiresAt - Now, [Weak, ExpiresAt]
	{
		if (const TSharedPtr<FClientState, ESPMode::ThreadSafe> Pinned = Weak.Pin())
		{
			// Reviews N-2, N-I3: the clock can fire a hair before ExpiresAt (the virtual clock rounds the delay to whole
			// nanoseconds, so Now() may be one ulp below a double sum). Prune at the deadline this timer was armed for, or the
			// value survives, NextExpiry returns it again and a timer due at once re-arms forever.
			Pinned->Secrets.Prune(FMath::Max(Pinned->GetConfig()->Clock->Now(), ExpiresAt));
			Pinned->EditConfig([](FClientConfig&) {}); // a redactor without the expired value
			Pinned->ScheduleSecretExpiry();
		}
	});
}
