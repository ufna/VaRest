// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestClientState.h"
#include "VaRestPersistenceGuard.h"
#include "VaRestIdentity.h"
#include "VaRestRequestSpec.h"

#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"
#include "Tasks/Task.h"
#include "VaRestAuthInternal.h"
#include "VaRestHttpTesting.h"
#include "VaRestRetryInternal.h"
#include "VaRestIdentity.h"
#include "Misc/Paths.h"
#include "VaRestTracePoints.h"

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

	/** Case-sensitive: FString's operator== ignores case, and a secret or key differing only in case is another one */
	bool ContainsExactly(const TArray<FString>& Texts, const FString& Text)
	{
		return Texts.ContainsByPredicate([&Text](const FString& Known) { return Known.Equals(Text, ESearchCase::CaseSensitive); });
	}

	/**
	 * Review I-1: Now's redactor with what Known knew and Now's does not -- its known values, and its keys (InKnownKeys:
	 * FVaRestRedactRules does not list its own), which only grow today (WithRedactedKeys appends) but are joined all the
	 * same. Now's own redactor when Known adds nothing. OutKeys, when given, gets the union's keys.
	 */
	TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> MakeUnionRedactor(const FVaRestRedactor& Known, TConstArrayView<FString> InKnownKeys,
		const FClientConfig& Now, TArray<FString>* OutKeys = nullptr)
	{
		const TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> Current = Now.Redactor.ToSharedRef();
		TArray<FString> Values = Current->GetSecrets();
		FVaRestRedactRules Rules = Current->GetRules();
		if (OutKeys)
		{
			*OutKeys = Now.RedactedKeys;
		}
		bool bWider = false;
		for (const FString& Value : Known.GetSecrets())
		{
			if (!ContainsExactly(Values, Value))
			{
				Values.Add(Value);
				bWider = true;
			}
		}
		for (const FString& Key : InKnownKeys)
		{
			if (!ContainsExactly(Now.RedactedKeys, Key))
			{
				Rules.AddKey(Key);
				bWider = true;
				if (OutKeys)
				{
					OutKeys->Add(Key);
				}
			}
		}
		if (!bWider)
		{
			return Current;
		}
		return MakeShared<FVaRestRedactor, ESPMode::ThreadSafe>(MoveTemp(Rules), MoveTemp(Values));
	}

	/** What Ex's redactor has known so far: its attempts' join (Task 12 review M-7) once there is one, else the Send() snapshot's */
	const FVaRestRedactor& KnownRedactor(const FExchangeState& Ex)
	{
		return Ex.AppliedRedactor.IsValid() ? *Ex.AppliedRedactor : *Ex.Config->Redactor;
	}

	TConstArrayView<FString> KnownKeys(const FExchangeState& Ex)
	{
		return Ex.AppliedRedactor.IsValid() ? TConstArrayView<FString>(Ex.AppliedKeys) : TConstArrayView<FString>(Ex.Config->RedactedKeys);
	}

	/** IN-8's name comes from WithName, or, without one, from the base URL: a new base renames an unnamed client (review M5) */
	bool TraceNameChanged(const FClientConfig& Before, const FClientConfig& After)
	{
		return !After.DisplayName.Equals(Before.DisplayName, ESearchCase::CaseSensitive)
			|| (After.DisplayName.IsEmpty() && !After.BaseUrl.Equals(Before.BaseUrl, ESearchCase::CaseSensitive));
	}
}

VaRest::Http::FClientState::FClientState(uint64 InClientId, FClientConfig&& InConfig)
	: ClientId(InClientId)
	, Config(MakeShared<FClientConfig, ESPMode::ThreadSafe>(MoveTemp(InConfig)))
	, Random(static_cast<int32>(static_cast<uint32>(InClientId * 2654435761ull) ^ FPlatformTime::Cycles()))
{
}

VaRest::Http::FClientState::~FClientState()
{
	// IN-8. Off the game thread only when a game's own thread dropped the last FVaRestClient (the core hands its
	// references to the game thread, review I-1); the event still goes out on the game thread. First, while Config is
	// still here: the backstop below moves it away.
	TracePoints::Client(ClientId, *Config, false);
	if (!IsInGameThread())
	{
#if !UE_BUILD_SHIPPING
		++Testing::Counters().ClientStatesDestroyedOffGameThread;
#endif
		// Review I-1 backstop: the configuration (custom stages, transport, clock) and the session (its listeners, its
		// refresh waiters: game code and its captures, concurrency review M-9) die on the game thread. Moved, not copied
		// (review N-3), so this thread keeps no reference; the task body dies on the game thread right after it runs
		// (TaskPrivate.h:871-877).
		UE::Tasks::Launch(TEXT("VaRest.ReleaseConfig"), [Id = ClientId, Doomed = MoveTemp(Config), DoomedSession = MoveTemp(Session)] { Doomed->Connectivity->UnregisterProbe(Id); }, UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
	}
	else
	{
		Config->Connectivity->UnregisterProbe(ClientId);
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
	{
		ClientStatePrivate::FLiveRegistry& Registry = ClientStatePrivate::Live();
		FScopeLock Lock(&Registry.Lock);
		Registry.States.Add(Created);
	}
	// IN-8, once the state is live (a sink added from this event's OnEvent finds it in its snapshot) and outside the lock:
	// a sink's code runs here
	Created->AttachConnectivity();
	TracePoints::Client(Created->GetClientId(), *Created->GetConfig(), true);
	return Created;
}

TSharedRef<const VaRest::Http::FClientConfig, ESPMode::ThreadSafe> VaRest::Http::FClientState::GetConfig() const
{
	FReadScopeLock ReadLock(ConfigLock);
	return Config.ToSharedRef();
}

TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> VaRest::Http::RedactorFor(const FExchangeState& Ex)
{
	check(IsInGameThread());
	const TSharedRef<const FClientConfig, ESPMode::ThreadSafe> Now = Ex.State->GetConfig();
	const TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> Current = Now->Redactor.ToSharedRef();
	if (!Ex.Config.IsValid() || &ClientStatePrivate::KnownRedactor(Ex) == &Current.Get())
	{
		// The usual case, nothing changed while the request ran: one redactor, one pass
		return Current;
	}
	if (Ex.UnionBuiltFrom.Get() != &Current.Get())
	{
		// Snapshot, applied and current (Task 12 review M-7): KnownRedactor is the first two joined already
		Ex.UnionRedactor = ClientStatePrivate::MakeUnionRedactor(ClientStatePrivate::KnownRedactor(Ex), ClientStatePrivate::KnownKeys(Ex), *Now);
		Ex.UnionBuiltFrom = Current;
	}
	return Ex.UnionRedactor.ToSharedRef();
}

void VaRest::Http::RememberAppliedRedactor(FExchangeState& Ex)
{
	check(IsInGameThread());
	const TSharedRef<const FClientConfig, ESPMode::ThreadSafe> Now = Ex.State->GetConfig();
	// Keep persistence knowledge independently of the log redactor's minimum/history and early return.
	auto Guard = Ex.PersistenceGuard.IsValid()
		? MakeShared<FPersistenceGuard, ESPMode::ThreadSafe>(*Ex.PersistenceGuard)
		: MakeShared<FPersistenceGuard, ESPMode::ThreadSafe>();
	if (Now->PersistenceGuard.IsValid()) Guard->UnionWith(*Now->PersistenceGuard);
	Ex.PersistenceGuard = MoveTemp(Guard);
	const FVaRestRedactor& Known = ClientStatePrivate::KnownRedactor(Ex);
	if (&Known == Now->Redactor.Get())
	{
		// The usual case: the client's redactor is the one this exchange already knows -- nothing set or forgotten since
		// Send() or the last attempt
		return;
	}
	// The client changed while the request waited (a login while it queued, a refresh before a replay): what this attempt
	// carries is in Now's redactor, and the request's later lines must keep it after the client forgets it. MakeUnionRedactor
	// returns Now's own redactor when it knows all Known did, so RedactorFor's one comparison holds again until the next change.
	TArray<FString> Keys;
	const TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> Joined = ClientStatePrivate::MakeUnionRedactor(Known, ClientStatePrivate::KnownKeys(Ex), *Now, &Keys);
	Ex.AppliedRedactor = Joined;
	Ex.AppliedKeys = MoveTemp(Keys);
	// RedactorFor's cached union was built from the old Known
	Ex.UnionRedactor.Reset();
	Ex.UnionBuiltFrom.Reset();
}

void VaRest::Http::FClientState::EditConfig(TFunctionRef<void(FClientConfig&)> Edit)
{
	check(IsInGameThread());
	const TSharedRef<const FClientConfig, ESPMode::ThreadSafe> Previous = GetConfig();
	TSharedRef<FClientConfig, ESPMode::ThreadSafe> Next = MakeShared<FClientConfig, ESPMode::ThreadSafe>(*Previous);
	Edit(*Next);
	Publish(Next);
	// Trace-only work, so behind the mask (task 15 review M8): a sink attached later learns the name from its snapshot
	if (FVaRestTrace::Detail() != EVaRestTraceDetail::None && ClientStatePrivate::TraceNameChanged(*Previous, *Next))
	{
		// Decision 23: IN-8 has Created and Destroyed only; a rename is Created again for the same id (a store updates the row)
		TracePoints::Client(ClientId, *Next, true);
	}
}

void VaRest::Http::FClientState::Publish(const TSharedRef<FClientConfig, ESPMode::ThreadSafe>& Next)
{
	// Spec 4's chain. Custom Outer stages first: they see every logical request once (decision 13); M4's built-in Outer
	// stages (offline queue, cache, retry, the auth gate) go between them and the attempt boundary. The limiter opens each
	// attempt (ruling I-3), so what follows it runs when the request leaves the queue; custom PerAttempt stages see the
	// final headers; the log stage sits next to the transport.
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
	// Cache answers before Retry; a request already inside Retry retains that complete retry cycle.
	NewChain->Add(MakeCacheStage());
	NewChain->Add(MakeRetryStage());
	// Spec 4.2: after retry (a replay is not a retry, plan decision 2), before the boundary (a replay is a new attempt) and
	// the limiter (a parked request holds no slot)
	NewChain->Add(MakeAuthGateStage());
	NewChain->Add(MakeAttemptBoundaryStage());
	// Ruling I-3 (spec 4.6 as amended): the first stage of every attempt. A custom PerAttempt stage's signature, the log
	// lines and an auth's credentials are written when the request leaves the queue, so none of them goes stale while it
	// waits; a queued retry still waits its turn.
	NewChain->Add(MakeLimiterStage());
	// Spec 4.2: credentials per attempt, when the attempt leaves the queue (ruling I-3), before the custom PerAttempt stages,
	// which then sign the final headers
	NewChain->Add(MakeAuthApplyStage());
	for (const TPair<FVaRestStageRef, EVaRestStageSlot>& Entry : Next->CustomStages)
	{
		if (Entry.Value == EVaRestStageSlot::PerAttempt)
		{
			NewChain->Add(Entry.Key);
			bReissue |= Entry.Key->MayReissue();
		}
	}
	NewChain->Add(MakeLogStage()); // after custom PerAttempt stages: it logs the final headers
	NewChain->Add(MakeTransportStage());
	Next->Chain = NewChain;
	Next->bMayReissue = bReissue;

	FVaRestRedactRules Rules;
	for (const FString& Key : Next->RedactedKeys)
	{
		Rules.AddKey(Key);
	}
	Next->Redactor = MakeShared<FVaRestRedactor, ESPMode::ThreadSafe>(MoveTemp(Rules), Secrets.GetActive(Next->Clock->Now()));
	auto Guard = MakeShared<FPersistenceGuard, ESPMode::ThreadSafe>();
	for (const FString& Key : Next->RedactedKeys) Guard->AddKey(Key);
	for (const FString& Secret : Secrets.GetCurrentForPersistence()) Guard->AddSecret(Secret);
	Next->PersistenceGuard = MoveTemp(Guard);

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
			// M4a final review A5 (review A M-4, spec 4.8): the task took the tags of the game's WithMaxConcurrent() call
			// (TaskPrivate.h:192), and the attempts its grants run are VaRest's
			LLM_SCOPE_BYTAG(VaRest);
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
	// Review M-1: under the game's own prefix, so the game cannot replace a session or auth value, and neither
	// ClearSession nor an auth can forget the game's
	Secrets.Set(GameSecretSlot(Slot), Value, Now);
	EditConfig([](FClientConfig&) {});
	ScheduleSecretExpiry();
}

void VaRest::Http::FClientState::SetSecrets(TConstArrayView<TPair<FName, FString>> Values)
{
	SetSecrets(Values, [](FClientConfig&) {});
}

void VaRest::Http::FClientState::SetSecrets(TConstArrayView<TPair<FName, FString>> Values, TFunctionRef<void(FClientConfig&)> AlsoEdit)
{
	check(IsInGameThread());
	const double Now = GetConfig()->Clock->Now();
	Secrets.Prune(Now);
	for (const TPair<FName, FString>& Value : Values)
	{
		Secrets.Set(Value.Key, Value.Value, Now);
	}
	EditConfig(AlsoEdit);
	ScheduleSecretExpiry();
}

void VaRest::Http::FClientState::ClearSecrets()
{
	check(IsInGameThread());
	// Review I-1: the game's slots only. The session's (and an auth's) values stay known while they are in use, and the
	// session's rotated ones keep their 10 minutes (Inspector D21): their expiry is scheduled again, not cleared.
	Secrets.ClearSlots([](FName Slot) { return IsGameSecretSlot(Slot); });
	EditConfig([](FClientConfig&) {});
	ScheduleSecretExpiry();
}

void VaRest::Http::FClientState::SetAuth(const TSharedPtr<IVaRestAuth, ESPMode::ThreadSafe>& InAuth)
{
	check(IsInGameThread());
	TArray<FString> Keys;
	TArray<TPair<FName, FString>> Given;
	if (InAuth.IsValid())
	{
		InAuth->GetRedactedKeys(Keys);
		InAuth->GetSecretValues(Given);
	}
	TArray<TPair<FName, FString>> Values;
	TArray<FName> NewSlots;
	for (const TPair<FName, FString>& Value : Given)
	{
		// Task 14 carry: a custom auth names its slots freely; stored under the auth's prefix, "VaRest.Session.AccessToken"
		// from one is not the session's token slot, so it cannot rotate the live token out or be forgotten by ClearSession
		const FName Slot = AuthSecretSlot(Value.Key);
		// Review M-3: two values under one slot are Set in turn, so the first is retired (forgotten after 10 minutes) while the
		// auth may still send it. Reported, not refused: the last value stays current, as IVaRestAuth::GetSecretValues says.
		ensureMsgf(!NewSlots.Contains(Slot), TEXT("VaRest: the auth %s returns the secret slot %s twice; only the last value stays current"),
			*InAuth->GetAuthName().ToString(), *Slot.ToString());
		Values.Emplace(Slot, Value.Value);
		NewSlots.AddUnique(Slot);
	}
	for (const FName Old : AuthSlots)
	{
		if (!NewSlots.Contains(Old))
		{
			// Task 14 carry: retired, as ImportSession retires a header it drops (VaRestSession.cpp) -- an empty value moves the
			// old one to the previous values (FVaRestSecretSet::Set), scrubbed for 10 minutes, then forgotten (Inspector D21).
			// Kept current, an auth replaced long ago would stay a known secret forever.
			Values.Emplace(Old, FString());
		}
	}
	AuthSlots = MoveTemp(NewSlots);
	// IN-13 layers 2 and 3 in the auth's own republish: its key names are checked once where they enter (task 14 review M4)
	SetSecrets(Values, [&InAuth, &Keys](FClientConfig& Edited)
	{
		Edited.Auth = InAuth;
		AppendRedactKeys(Edited.RedactedKeys, Keys);
	});
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

void VaRest::Http::FClientState::AttachConnectivity()
{
    const auto Reachability = GetConfig()->Connectivity;
    const TWeakPtr<FClientState, ESPMode::ThreadSafe> Weak(AsShared());
    Reachability->RegisterProbe(ClientId, [Weak]
    {
        const auto Pinned = Weak.Pin();
        if (!Pinned.IsValid() || FClientState::IsShuttingDown()) return false;
        const auto Current = Pinned->GetConfig();
        if (Current->HealthUrl.IsEmpty() || !SameOrigin(Current->BaseUrl, Current->HealthUrl)) return false;
        FRequestSpecAccess::SendProbe(Pinned.ToSharedRef(), Current->HealthUrl,
            [Connectivity = Current->Connectivity] { Connectivity->ProbeFinished(); });
        return true;
    });
}

TSharedRef<VaRest::Http::FDiskCache, ESPMode::ThreadSafe> VaRest::Http::FClientState::GetDiskCacheFor(const FClientConfig& Cfg, int64 Capacity)
{
    check(IsInGameThread());
    FString Origin;
    CanonicalOrigin(Cfg.BaseUrl, Origin);
    DiskCache = FDiskCache::Open(FPaths::Combine(Cfg.CacheRoot,
        HashFields(TEXT("VaRest/cache-origin/v1"), TArray<FString>{Origin})), Capacity);
    return DiskCache.ToSharedRef();
}
void VaRest::Http::FClientState::ClearCacheState()
{
    check(IsInGameThread());
    ++CacheEpoch;
    MemoryCache.Clear();
    const auto Current = GetConfig();
    FString Origin;
    if (Current->StorageConfigError.IsEmpty() && CanonicalOrigin(Current->BaseUrl, Origin))
        GetDiskCacheFor(*Current, Current->Cache.DiskBytes)->Clear();
}
