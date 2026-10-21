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
	std::atomic<uint64> NextRequest{ 1 };
	std::atomic<uint64> NextClient{ 1 };

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
	check(IsInGameThread());
	TSharedPtr<const FSinkList, ESPMode::ThreadSafe> Current = GetSinks();
	if (!Current.IsValid())
	{
		return;
	}
	TSharedRef<FSinkList, ESPMode::ThreadSafe> Next = MakeShared<FSinkList, ESPMode::ThreadSafe>(*Current);
	Next->RemoveAll([&Sink](const FSinkRef& Existing) { return Existing == Sink; });
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
	return VaRest::Http::TracePrivate::NextRequest.fetch_add(1, std::memory_order_relaxed);
}

uint64 FVaRestTrace::NewClientId()
{
	return VaRest::Http::TracePrivate::NextClient.fetch_add(1, std::memory_order_relaxed);
}

FVaRestTraceCounters& FVaRestTrace::Counters()
{
	// Leaked on purpose (review I-4): atomics, but read by trace points that may run late
	static FVaRestTraceCounters* Instance = new FVaRestTraceCounters();
	return *Instance;
}

FVaRestScopedSinkIsolation::FVaRestScopedSinkIsolation()
{
	using namespace VaRest::Http::TracePrivate;
	check(IsInGameThread());
	{
		FWriteScopeLock WriteLock(Registry().Lock);
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
		Restored = Registry().Saved.Pop();
	}
	FVaRestTrace::Swap(MoveTemp(Restored));
}
