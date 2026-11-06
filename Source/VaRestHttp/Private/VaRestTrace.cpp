// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestTrace.h"
#include "VaRestNativeScope.h"
#include "VaRestClientState.h"
#include "VaRestTraceSnapshot.h"

#include "Misc/ScopeRWLock.h"

std::atomic<uint32> FVaRestTrace::DetailMask{ 0 };

namespace VaRest::Http::TracePrivate
{
	struct FAttachmentPins;
	FAttachmentPins* ActiveAttachment = nullptr; // GT-only synchronous/reentrant stack, never a service or task owner
	struct FAttachmentPins
	{
		FAttachmentPins* Previous = ActiveAttachment;
		TArray<TSharedRef<FClientState, ESPMode::ThreadSafe>> Clients;
		TArray<TSharedRef<FNativeScopeLifetime, ESPMode::ThreadSafe>> Scopes;
		TArray<FVaRestTraceEvent> DeferredEvents;
		FAttachmentPins() { ActiveAttachment = this; }
		~FAttachmentPins()
		{
			check(IsInGameThread() && ActiveAttachment == this);
			ActiveAttachment = Previous; // a destructor/Closed sink may attach another sink reentrantly
			for (auto& Event : DeferredEvents)
				FVaRestTrace::EmitAfterAttachmentOnGT(Event.Kind, Event.Time, MoveTemp(Event.Payload), Event.Source);
			DeferredEvents.Empty();
			Clients.Empty(); // Destroyed reaches the newly attached sink before a possible last Scope Closed
			for (const auto& Scope : Scopes)
				Scope->ReleaseSnapshotPinOnGT();
		}
	};
	void HoldSnapshotScopeOnGT(TSharedRef<FNativeScopeLifetime, ESPMode::ThreadSafe> Scope)
	{
		check(IsInGameThread());
		if (!ActiveAttachment || ActiveAttachment->Scopes.Contains(Scope))
			return;
		Scope->AddSnapshotPinOnGT();
		ActiveAttachment->Scopes.Add(MoveTemp(Scope));
	}
	void HoldManagedSnapshotClientOnGT(TSharedRef<FClientState, ESPMode::ThreadSafe> Client)
	{
		check(IsInGameThread() && Client->GetManagedLifetime().IsValid());
		if (ActiveAttachment)
			ActiveAttachment->Clients.Add(MoveTemp(Client));
	}
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
	std::atomic<uint64> NextScope{ 0 };
	std::atomic<uint64> NextSocket{ 0 }, NextSocketFrame{ 0 };

	/** Keep every public kind paired with its concrete native payload. */
	bool PayloadMatchesKind(EVaRestTraceKind Kind, const FVaRestTracePayload& Payload)
	{
		switch (Kind)
		{
		case EVaRestTraceKind::Started: return Payload.IsType<FVaRestTraceStarted>();
		case EVaRestTraceKind::AttemptDispatched: return Payload.IsType<FVaRestTraceAttemptDispatched>();
		case EVaRestTraceKind::AttemptEnded: return Payload.IsType<FVaRestTraceAttemptEnded>();
		case EVaRestTraceKind::Note: return Payload.IsType<FVaRestTraceNote>();
		case EVaRestTraceKind::Completed: return Payload.IsType<FVaRestTraceCompleted>();
		case EVaRestTraceKind::Excerpt: return Payload.IsType<FVaRestTraceExcerptEvent>();
		case EVaRestTraceKind::Scope: return Payload.IsType<FVaRestTraceScope>();
		case EVaRestTraceKind::Client: return Payload.IsType<FVaRestTraceClient>();
		case EVaRestTraceKind::Marker: return Payload.IsType<FVaRestTraceMarker>();
		case EVaRestTraceKind::Socket: return Payload.IsType<FVaRestTraceSocket>();
		case EVaRestTraceKind::SocketAttempt: return Payload.IsType<FVaRestTraceSocketAttempt>();
		case EVaRestTraceKind::SocketFrame: return Payload.IsType<FVaRestTraceSocketFrame>();
		case EVaRestTraceKind::SocketNote: return Payload.IsType<FVaRestTraceSocketNote>();
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
				// Task 5 review minor 2: a sink's 0 means the default, so it is mapped before the max -- a 1234 sink beside a
				// default one must not shrink the default one's budget to 1234. Only a sink that asks for excerpts (Bodies) has
				// a say: a Metadata sink's default would otherwise lift a Bodies sink's smaller budget. (Every sink receives the
				// excerpts all the same: Emit delivers every event to every sink, VaRestTrace.cpp:213-216.)
				if (EnumHasAnyFlags(One.Flags, EVaRestTraceDetail::Bodies))
				{
					const int64 Budget = One.MaxPendingExcerptBytes > 0 ? One.MaxPendingExcerptBytes : FVaRestTraceDetailSettings::DefaultMaxPendingExcerptBytes;
					Merged.MaxPendingExcerptBytes = FMath::Max(Merged.MaxPendingExcerptBytes, Budget);
				}
			}
		}
		return Merged;
	}
}

FString FVaRestTraceExcerpt::ToText() const
{
	if (Kind == EVaRestExcerptKind::Binary && bBinaryCaptured)
	{
		FString Hex;
		if (Bytes) for (uint8 Byte : *Bytes) Hex.Appendf(TEXT("%02x"), Byte);
		return Hex;
	}
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
	// Managed pins span snapshot construction, OnAttached and registry publication synchronously. Their actual GT
	// destruction then reaches this sink; no uncounted named-GT cleanup task can survive managed retirement/Stop.
	VaRest::Http::TracePrivate::FAttachmentPins ManagedPins;
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

bool FVaRestTrace::IsAttachingOnGT() { check(IsInGameThread()); return VaRest::Http::TracePrivate::ActiveAttachment != nullptr; }
void FVaRestTrace::EmitAfterAttachmentOnGT(EVaRestTraceKind Kind, double Time, FVaRestTracePayload&& Payload, EVaRestTraceSource Source)
{
	check(IsInGameThread());
	if (auto* Attachment = VaRest::Http::TracePrivate::ActiveAttachment)
	{
		FVaRestTraceEvent Event; Event.Kind = Kind; Event.Time = Time; Event.Source = Source; Event.Payload = MoveTemp(Payload);
		Attachment->DeferredEvents.Add(MoveTemp(Event)); return;
	}
	if (Detail() != EVaRestTraceDetail::None) Emit(Kind, Time, MoveTemp(Payload), Source);
}

void FVaRestTrace::Emit(EVaRestTraceKind Kind, double Time, FVaRestTracePayload&& Payload, EVaRestTraceSource Source)
{
	// Spec 9: nothing is emitted from the HTTP thread; every kind but Excerpt (a background task) is game-thread only.
	checkSlow(IsInGameThread() || Kind == EVaRestTraceKind::Excerpt);
	// A mismatch would otherwise surface as a TVariant::Get failure inside some sink.
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
	VaRest::Http::FNativeScopeLifetime::ContributeSnapshot(Snapshot);
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

uint64 FVaRestTrace::NewScopeId()
{
	auto& Counter = VaRest::Http::TracePrivate::NextScope;
	uint64 Previous = Counter.load(std::memory_order_relaxed);
	while (Previous != MAX_uint64)
		if (Counter.compare_exchange_weak(Previous, Previous + 1, std::memory_order_relaxed))
			return Previous + 1;
	return 0;
}

uint64 FVaRestTrace::NewSocketId()
{
	auto& Counter = VaRest::Http::TracePrivate::NextSocket;
	uint64 Previous = Counter.load(std::memory_order_relaxed);
	while (Previous != MAX_uint64)
		if (Counter.compare_exchange_weak(Previous, Previous + 1, std::memory_order_relaxed)) return Previous + 1;
	return 0;
}
uint64 FVaRestTrace::NewSocketFrameId()
{
	auto& Counter = VaRest::Http::TracePrivate::NextSocketFrame;
	uint64 Previous = Counter.load(std::memory_order_relaxed);
	while (Previous != MAX_uint64)
		if (Counter.compare_exchange_weak(Previous, Previous + 1, std::memory_order_relaxed)) return Previous + 1;
	return 0;
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
