// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestTrace.h"

#include "Misc/ScopeRWLock.h"

std::atomic<uint32> FVaRestTrace::DetailMask{ 0 };

namespace VaRest::Http::TracePrivate
{
	/**
	 * The sink list, the merged caps and the isolation stack. The list itself is immutable once published (IN-12). Leaked
	 * on purpose (review I-4): nothing in VaRestHttp may run at static destruction, where a client state dying late (a
	 * global FVaRestClient) or a sink still registered (ShutdownModule never runs in an automation session) would lock a
	 * destroyed lock or read a freed list.
	 */
	struct FRegistry
	{
		FRWLock Lock;
		TSharedPtr<const FVaRestTrace::FSinkList, ESPMode::ThreadSafe> Sinks;
		FVaRestTraceDetailSettings Settings;
		/** Lists saved by FVaRestScopedSinkIsolation, innermost last */
		TArray<TSharedPtr<const FVaRestTrace::FSinkList, ESPMode::ThreadSafe>> Saved;
	};

	FRegistry& Registry()
	{
		static FRegistry* Instance = new FRegistry();
		return *Instance;
	}

	// Trivially destructible: safe as statics
	std::atomic<uint64> NextSeq{ 1 };
	// Counters start at 0 and ids are fetch_add + 1, so 0 can never be handed out (IN-10)
	std::atomic<uint64> NextRequest{ 0 };
	std::atomic<uint64> NextClient{ 0 };

	/** Kinds without a payload type yet (Note, Excerpt, Marker, the socket kinds) are not checked */
	bool PayloadMatchesKind(EVaRestTraceKind Kind, const FVaRestTracePayload& Payload)
	{
		switch (Kind)
		{
		case EVaRestTraceKind::Started: return Payload.IsType<FVaRestTraceStarted>();
		case EVaRestTraceKind::AttemptDispatched: return Payload.IsType<FVaRestTraceAttemptDispatched>();
		case EVaRestTraceKind::AttemptEnded: return Payload.IsType<FVaRestTraceAttemptEnded>();
		case EVaRestTraceKind::Completed: return Payload.IsType<FVaRestTraceCompleted>();
		case EVaRestTraceKind::Scope: return Payload.IsType<FVaRestTraceScope>();
		case EVaRestTraceKind::Client: return Payload.IsType<FVaRestTraceClient>();
		default: return true;
		}
	}

	/** Outside any lock: GetDetail is the sink's code (review M-8) */
	FVaRestTraceDetailSettings Merge(const FVaRestTrace::FSinkList* List)
	{
		FVaRestTraceDetailSettings Merged;
		if (List)
		{
			for (const FVaRestTrace::FSinkRef& Sink : *List)
			{
				const FVaRestTraceDetailSettings One = Sink->GetDetail();
				Merged.Flags |= One.Flags;
				Merged.BodyCap = FMath::Max(Merged.BodyCap, One.BodyCap);
				Merged.BinaryCap = FMath::Max(Merged.BinaryCap, One.BinaryCap);
				Merged.FrameCap = FMath::Max(Merged.FrameCap, One.FrameCap);
			}
		}
		return Merged;
	}
}

FString FVaRestTraceExcerpt::ToText() const
{
	return Bytes.IsValid() ? FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes->GetData()), Bytes->Num()) : FString();
}

void FVaRestTrace::Swap(TSharedPtr<const FSinkList, ESPMode::ThreadSafe> Next)
{
	using namespace VaRest::Http::TracePrivate;
	check(IsInGameThread());
	const FVaRestTraceDetailSettings Merged = Merge(Next.Get());
	TSharedPtr<const FSinkList, ESPMode::ThreadSafe> Old;
	{
		FWriteScopeLock WriteLock(Registry().Lock);
		Old = MoveTemp(Registry().Sinks);
		Registry().Sinks = MoveTemp(Next);
		Registry().Settings = Merged;
		DetailMask.store(static_cast<uint32>(Merged.Flags), std::memory_order_relaxed);
	}
	// Old goes here, after the lock is released
}

FVaRestTraceDetailSettings FVaRestTrace::GetDetailSettings()
{
	using namespace VaRest::Http::TracePrivate;
	FReadScopeLock ReadLock(Registry().Lock);
	return Registry().Settings;
}

void FVaRestTrace::AddSink(const FSinkRef& Sink)
{
	check(IsInGameThread());
	// Re-adding must not deliver a second snapshot: a store would rebuild or duplicate its rows (review M-8)
	{
		const TSharedPtr<const FSinkList, ESPMode::ThreadSafe> Existing = GetSinks();
		if (Existing.IsValid() && Existing->Contains(Sink))
		{
			return;
		}
	}
	// IN-12: a late sink (Record after Pause, a module loading after the PreDefault core) learns what already exists.
	// A client the snapshot pinned can die only after this function returns: FClientState::ForEachLive releases its pins
	// from a later game-thread task, so that client's Client{Destroyed} reaches this sink after OnAttached (review M-9).
	Sink->OnAttached(BuildSnapshot());
	TSharedPtr<const FSinkList, ESPMode::ThreadSafe> Current = GetSinks();
	TSharedRef<FSinkList, ESPMode::ThreadSafe> Next = MakeShared<FSinkList, ESPMode::ThreadSafe>(Current.IsValid() ? *Current : FSinkList());
	Next->AddUnique(Sink);
	Swap(Next);
}

void FVaRestTrace::RemoveSink(const FSinkRef& Sink)
{
	using namespace VaRest::Http::TracePrivate;
	check(IsInGameThread());
	const auto Without = [&Sink](const FSinkList& List)
	{
		TSharedRef<FSinkList, ESPMode::ThreadSafe> Stripped = MakeShared<FSinkList, ESPMode::ThreadSafe>(List);
		Stripped->RemoveAll([&Sink](const FSinkRef& Existing) { return Existing == Sink; });
		return Stripped;
	};

#if WITH_DEV_AUTOMATION_TESTS
	// Strip the sink from the lists an isolation saved too, or restoring would resurrect it after a Pause. Only the game
	// thread touches Saved (and this is the game thread), so reading it needs no lock; the swap is under the write lock
	// and the replaced lists are released after it (review M-8).
	{
		TArray<TSharedPtr<const FSinkList, ESPMode::ThreadSafe>> Rebuilt = Registry().Saved;
		bool bChanged = false;
		for (TSharedPtr<const FSinkList, ESPMode::ThreadSafe>& Saved : Rebuilt)
		{
			if (Saved.IsValid() && Saved->Contains(Sink))
			{
				Saved = Without(*Saved);
				bChanged = true;
			}
		}
		if (bChanged)
		{
			TArray<TSharedPtr<const FSinkList, ESPMode::ThreadSafe>> Replaced;
			{
				FWriteScopeLock WriteLock(Registry().Lock);
				Replaced = MoveTemp(Registry().Saved);
				Registry().Saved = MoveTemp(Rebuilt);
			}
		}
	}
#endif

	TSharedPtr<const FSinkList, ESPMode::ThreadSafe> Current = GetSinks();
	if (!Current.IsValid())
	{
		return;
	}
	TSharedRef<FSinkList, ESPMode::ThreadSafe> Next = Without(*Current);
	Current.Reset();
	Swap(Next);
}

void FVaRestTrace::RefreshDetail()
{
	check(IsInGameThread());
	Swap(GetSinks());
}

TSharedPtr<const FVaRestTrace::FSinkList, ESPMode::ThreadSafe> FVaRestTrace::GetSinks()
{
	using namespace VaRest::Http::TracePrivate;
	FReadScopeLock ReadLock(Registry().Lock);
	return Registry().Sinks;
}

void FVaRestTrace::Emit(EVaRestTraceKind Kind, double Time, FVaRestTracePayload&& Payload, EVaRestTraceSource Source)
{
	// Spec 9: nothing is emitted from the HTTP thread; every kind but Excerpt (a background task) is game-thread only.
	checkSlow(IsInGameThread() || Kind == EVaRestTraceKind::Excerpt);
	// A mismatch would otherwise surface as a TVariant::Get failure inside some sink. Kinds without a payload type yet
	// (Note, Excerpt, Marker, the socket kinds) are not checked.
	checkSlow(VaRest::Http::TracePrivate::PayloadMatchesKind(Kind, Payload));
	const TSharedPtr<const FSinkList, ESPMode::ThreadSafe> List = GetSinks();
	if (!List.IsValid() || List->Num() == 0)
	{
		return;
	}
	FVaRestTraceEvent Event;
	Event.Kind = Kind;
	Event.Source = Source;
	Event.Seq = VaRest::Http::TracePrivate::NextSeq.fetch_add(1, std::memory_order_relaxed);
	Event.Time = Time;
	Event.Payload = MoveTemp(Payload);
	++Counters().EventsBuilt;
	for (const FSinkRef& Sink : *List)
	{
		Sink->OnEvent(Event);
	}
}

TMulticastDelegate<void(FVaRestTraceSnapshot&)>& FVaRestTrace::OnBuildSnapshot()
{
	// Leaked on purpose (review I-4)
	static TMulticastDelegate<void(FVaRestTraceSnapshot&)>* Delegate = new TMulticastDelegate<void(FVaRestTraceSnapshot&)>();
	return *Delegate;
}

FVaRestTraceSnapshot FVaRestTrace::BuildSnapshot()
{
	check(IsInGameThread());
	FVaRestTraceSnapshot Snapshot;
	FVaRestTraceScope& Process = Snapshot.Scopes.AddDefaulted_GetRef();
	Process.ScopeId = ProcessScopeId;
	Process.Label = TEXT("Process");
	OnBuildSnapshot().Broadcast(Snapshot);
	return Snapshot;
}

uint64 FVaRestTrace::NewRequestId()
{
	const uint64 Id = VaRest::Http::TracePrivate::NextRequest.fetch_add(1, std::memory_order_relaxed) + 1;
	check(Id != 0);
	return Id;
}

uint64 FVaRestTrace::NewClientId()
{
	const uint64 Id = VaRest::Http::TracePrivate::NextClient.fetch_add(1, std::memory_order_relaxed) + 1;
	check(Id != 0);
	return Id;
}

FVaRestTraceCounters& FVaRestTrace::Counters()
{
	// Leaked on purpose (review I-4): atomics, but read by trace points that may run late
	static FVaRestTraceCounters* Instance = new FVaRestTraceCounters();
	return *Instance;
}

#if WITH_DEV_AUTOMATION_TESTS
FVaRestScopedSinkIsolation::FVaRestScopedSinkIsolation()
{
	using namespace VaRest::Http::TracePrivate;
	check(IsInGameThread());
	{
		FWriteScopeLock WriteLock(Registry().Lock);
		Depth = Registry().Saved.Num();
		Registry().Saved.Push(Registry().Sinks);
	}
	FVaRestTrace::Swap(nullptr);
}

FVaRestScopedSinkIsolation::~FVaRestScopedSinkIsolation()
{
	using namespace VaRest::Http::TracePrivate;
	check(IsInGameThread());
	TSharedPtr<const FVaRestTrace::FSinkList, ESPMode::ThreadSafe> Restored;
	{
		FWriteScopeLock WriteLock(Registry().Lock);
		// Non-LIFO destruction would restore someone else's list
		check(Registry().Saved.Num() == Depth + 1);
		Restored = Registry().Saved.Pop();
	}
	FVaRestTrace::Swap(MoveTemp(Restored));
}
#endif
