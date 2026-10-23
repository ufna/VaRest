// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestPersistence.h"

#include "Containers/Ticker.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "Tasks/Pipe.h"
#include "VaRestDecode.h"

namespace VaRest::Http
{
	struct FIoContinuation
	{
		FIoDone Done;
		bool bControlDuringStop = false;
		int64 Reservation = 0;
	};
	struct FPersistenceRuntime
	{
		UE::Tasks::FPipe Pipe{TEXT("VaRest.Persistence")};
		bool bAccepting = false;
		bool bPumping = false;
		uint64 NextTicket = 0;
		int64 PendingBytes = 0; // GT only; includes result slots awaiting GT consumption
		TMap<uint64, FIoContinuation> Callbacks;
		FCriticalSection CompletedLock;
		TArray<TPair<uint64, FIoResult>> Completed;
		TArray<TUniqueFunction<bool()>> Deferred;
		int32 DeferredSlots = 0; // includes callbacks temporarily held by PumpOn
		uint64 NextCompletionTicket = 0;
		TMap<uint64, TUniqueFunction<void()>> OwnedCompletions; // 8192 separate permits, includes unready waits
		FTSTicker::FDelegateHandle Ticker;
		FIoCounters Counters;
	};
#if WITH_DEV_AUTOMATION_TESTS
	FPersistenceRuntime* TestRuntime = nullptr; // GT lookup only; worker always captures its admitted Runtime
#endif
	FPersistenceRuntime& PersistenceRuntime()
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (IsInGameThread() && TestRuntime)
			return *TestRuntime;
#endif
		static FPersistenceRuntime* Instance = new FPersistenceRuntime();
		return *Instance;
	}
	FIoCounters& PersistenceCounters()
	{
		return PersistenceRuntime().Counters;
	}
	void PumpOn(FPersistenceRuntime& Runtime)
	{
		check(IsInGameThread());
		if (Runtime.bPumping)
			return;
		TGuardValue<bool> Pumping(Runtime.bPumping, true);
		TArray<TPair<uint64, FIoResult>> Completed;
		{
			FScopeLock Lock(&Runtime.CompletedLock);
			Completed = MoveTemp(Runtime.Completed);
		}
		auto Deferred = MoveTemp(Runtime.Deferred); // callbacks posted while pumping wait until next pump
		TArray<uint64> ReadyCompletions;
		if (Runtime.bAccepting)
			for (const auto& Pair : Runtime.OwnedCompletions)
				if (Pair.Value)
					ReadyCompletions.Add(Pair.Key);
		ReadyCompletions.Sort(); // admission order; newly ready entries wait until next pump
		for (auto& Slot : Completed)
		{
			auto* Found = Runtime.Callbacks.Find(Slot.Key);
			check(Found);
			FIoContinuation Continuation = MoveTemp(*Found);
			Runtime.Callbacks.Remove(Slot.Key);
			if ((Runtime.bAccepting || Continuation.bControlDuringStop) && Continuation.Done)
				Continuation.Done(MoveTemp(Slot.Value));
			Slot.Value = FIoResult();
			Continuation.Done.Reset(); // discard DTO boxes on GT before releasing their reservation
			Runtime.PendingBytes -= Continuation.Reservation;
			++Runtime.Counters.Delivered;
		}
		for (uint64 Ticket : ReadyCompletions)
		{
			auto* Found = Runtime.OwnedCompletions.Find(Ticket);
			if (!Found || !Runtime.bAccepting)
				continue;
			auto Callback = MoveTemp(*Found);
			Runtime.OwnedCompletions.Remove(Ticket);
			if (Callback)
				Callback(); // no general GT queue; normal Complete releases idempotently
		}
		for (auto& Callback : Deferred)
		{
			if (Runtime.bAccepting && !Callback())
				Runtime.Deferred.Add(MoveTemp(Callback));
			else
			{
				Callback.Reset(); // release captures before returning their permit
				--Runtime.DeferredSlots;
			}
		}
	}
	bool PostPersistencePoll(TUniqueFunction<bool()>&& Callback)
	{
		check(IsInGameThread());
		auto& Runtime = PersistenceRuntime();
		if (!Runtime.bAccepting || Runtime.DeferredSlots >= 8192)
			return false;
		++Runtime.DeferredSlots;
		Runtime.Deferred.Add(MoveTemp(Callback));
		return true;
	}
	bool PostPersistenceContinuation(TUniqueFunction<void()>&& Callback)
	{
		return PostPersistencePoll(
		    [Callback = MoveTemp(Callback)]() mutable
		    {
			    Callback();
			    return true;
		    });
	}
	uint64 AcquirePersistenceCompletion()
	{
		check(IsInGameThread());
		auto& Runtime = PersistenceRuntime();
		if (!Runtime.bAccepting || Runtime.OwnedCompletions.Num() >= 8192 || Runtime.NextCompletionTicket == MAX_uint64)
			return 0;
		const uint64 Ticket = ++Runtime.NextCompletionTicket;
		Runtime.OwnedCompletions.Add(Ticket, TUniqueFunction<void()>());
		return Ticket;
	}
	void SetPersistenceCompletion(uint64 Ticket, TUniqueFunction<void()>&& Callback)
	{
		check(IsInGameThread());
		auto& Runtime = PersistenceRuntime();
		auto* Slot = Runtime.OwnedCompletions.Find(Ticket);
		check(Slot && Runtime.bAccepting); // acquired before wait; no fallible second admission
		*Slot = MoveTemp(Callback);        // later Cancel may replace the accepted outcome, still asynchronous
	}
	void ReleasePersistenceCompletion(uint64 Ticket)
	{
		check(IsInGameThread());
		if (Ticket)
			PersistenceRuntime().OwnedCompletions.Remove(Ticket);
	}
	void StartOn(FPersistenceRuntime& Runtime)
	{
		check(IsInGameThread());
		if (Runtime.bAccepting)
			return;
		check(!Runtime.Pipe.HasWork() && Runtime.Callbacks.IsEmpty());
		Runtime.bAccepting = true;
		Runtime.Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		    [&Runtime](float)
		    {
			    PumpOn(Runtime);
			    return true;
		    }));
	}
	void StopOn(FPersistenceRuntime& Runtime)
	{
		check(IsInGameThread());
		if (!Runtime.bAccepting)
			return;
		check(!Runtime.bPumping);
		Runtime.bAccepting = false;
		FTSTicker::GetCoreTicker().RemoveTicker(Runtime.Ticker);
		Runtime.Ticker.Reset();
		Runtime.DeferredSlots -= Runtime.Deferred.Num();
		Runtime.Deferred.Reset();
		Runtime.Pipe.WaitUntilEmpty();    // workers never depend on GT acknowledgement
		PumpOn(Runtime);                  // only executor-owned slots; no TaskGraph/ticker/general message pump
		Runtime.OwnedCompletions.Empty(); // canceled exchanges settled first; discard any pending closure, never user delivery
		check(Runtime.Callbacks.IsEmpty() && Runtime.PendingBytes == 0);
		check(Runtime.Counters.Delivered.load() == Runtime.Counters.Launched.load());
	}
} // namespace VaRest::Http

namespace VaRest::Http
{
	bool LaunchOn(FPersistenceRuntime& Runtime, FIoWork&& Work, FIoDone&& Done, int64 ReservedBytes, bool bControlDuringStop
#if WITH_DEV_AUTOMATION_TESTS
	              ,
	              TSharedPtr<Testing::FPersistenceBarrier, ESPMode::ThreadSafe> BeforePublish = nullptr
#endif
	);
} // namespace VaRest::Http
bool VaRest::Http::LaunchOn(FPersistenceRuntime& Runtime, FIoWork&& Work, FIoDone&& Done, int64 ReservedBytes, bool bControlDuringStop
#if WITH_DEV_AUTOMATION_TESTS
                            ,
                            TSharedPtr<Testing::FPersistenceBarrier, ESPMode::ThreadSafe> BeforePublish
#endif
)
{
	check(IsInGameThread());
	if (!Runtime.bAccepting || ReservedBytes < 0 || ReservedBytes > 128 * 1024 * 1024 || Runtime.PendingBytes > 128 * 1024 * 1024 - ReservedBytes ||
	    Runtime.Callbacks.Num() >= 8192 || Runtime.NextTicket == MAX_uint64)
		return false;
	Runtime.PendingBytes += ReservedBytes;
	const uint64 Ticket = ++Runtime.NextTicket;
	Runtime.Callbacks.Add(Ticket, FIoContinuation{MoveTemp(Done), bControlDuringStop, ReservedBytes});
	++Runtime.Counters.Launched;
	Runtime.Pipe.Launch(TEXT("VaRest.Persistence"),
	                    [&Runtime, Ticket, Job = MoveTemp(Work)
#if WITH_DEV_AUTOMATION_TESTS
	                                           ,
	                     BeforePublish = MoveTemp(BeforePublish)
#endif
	]() mutable
	                    {
		                    FIoResult Result = Job();
		                    Job.Reset();
#if WITH_DEV_AUTOMATION_TESTS
		                    Testing::WaitPersistenceBarrier(BeforePublish);
#endif
		                    auto& Current = Runtime;
		                    {
			                    FScopeLock Lock(&Current.CompletedLock);
			                    Current.Completed.Emplace(Ticket, MoveTemp(Result));
		                    }
		                    ++Current.Counters.HandedBack;
	                    });
	return true;
}

bool VaRest::Http::LaunchPersistence(FIoWork&& Work, FIoDone&& Done, int64 ReservedBytes, bool bControlDuringStop)
{
	return LaunchOn(PersistenceRuntime(), MoveTemp(Work), MoveTemp(Done), ReservedBytes, bControlDuringStop);
}
void VaRest::Http::PumpPersistence()
{
	PumpOn(PersistenceRuntime());
}
bool VaRest::Http::PersistenceAccepting()
{
	check(IsInGameThread());
	return PersistenceRuntime().bAccepting;
}
bool VaRest::Http::PersistenceIdle()
{
	check(IsInGameThread());
	auto& Runtime = PersistenceRuntime();
	FScopeLock Lock(&Runtime.CompletedLock);
	return !Runtime.bPumping && Runtime.DeferredSlots == 0 && Runtime.Callbacks.IsEmpty() && Runtime.Completed.IsEmpty() && Runtime.Deferred.IsEmpty() &&
	       Runtime.OwnedCompletions.IsEmpty() && Runtime.Counters.HandedBack.load() == Runtime.Counters.Launched.load() && Runtime.PendingBytes == 0 &&
	       Runtime.Counters.Launched.load() == Runtime.Counters.Delivered.load();
}
void VaRest::Http::StartPersistence()
{
	StartOn(PersistenceRuntime());
}
void VaRest::Http::StopPersistence()
{
	StopOn(PersistenceRuntime());
}

#if WITH_DEV_AUTOMATION_TESTS
int64 VaRest::Http::Testing::PersistencePendingBytesForTest()
{
	check(IsInGameThread());
	return PersistenceRuntime().PendingBytes;
}
VaRest::Http::Testing::FScopedPersistenceRuntime::FScopedPersistenceRuntime()
{
	check(IsInGameThread());
	check(!TestRuntime);
	check(PersistenceIdle());
	auto* Created = new FPersistenceRuntime();
	Owned = Created;
	TestRuntime = Created;
	StartOn(*Created);
}
void VaRest::Http::Testing::FScopedPersistenceRuntime::Stop()
{
	check(IsInGameThread());
	StopOn(*static_cast<FPersistenceRuntime*>(Owned));
}
VaRest::Http::Testing::FScopedPersistenceRuntime::~FScopedPersistenceRuntime()
{
	Stop();
	check(TestRuntime == Owned);
	TestRuntime = nullptr;
	delete static_cast<FPersistenceRuntime*>(Owned);
}
bool VaRest::Http::Testing::LaunchWithPublicationBarrier(FIoWork&& Work, FIoDone&& Done, const TSharedPtr<FPersistenceBarrier, ESPMode::ThreadSafe>& Barrier)
{
	return LaunchOn(PersistenceRuntime(), MoveTemp(Work), MoveTemp(Done), 0, false, Barrier);
}
namespace VaRest::Http::Testing
{
	FStopProbe RunIsolatedStopProbe()
	{
		FStopProbe Result;
		FPersistenceRuntime Runtime;
		StartOn(Runtime);
		UE::Tasks::Launch(
		    TEXT("VaRest.UnrelatedStopWitness"), [Seen = Result.Unrelated] { Seen->store(true); }, UE::Tasks::ETaskPriority::Normal,
		    UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
		LaunchOn(
		    Runtime,
		    []
		    {
			    FIoResult Done;
			    Done.bOk = true;
			    return Done;
		    },
		    [&Result](FIoResult&&) { Result.bOrdinaryCalled = true; }, 1, false);
		LaunchOn(
		    Runtime,
		    []
		    {
			    FIoResult Done;
			    Done.bOk = true;
			    return Done;
		    },
		    [&Result, &Runtime](FIoResult&&)
		    {
			    Result.bCommitCalled = true;
			    Result.bAcceptedAfterStop = LaunchOn(Runtime, [] { return FIoResult(); }, [](FIoResult&&) {}, 0, false);
		    },
		    1, true);
		Runtime.Deferred.Add(
		    [&Result]
		    {
			    Result.bDeferredCalled = true;
			    return true;
		    });
		++Runtime.DeferredSlots;
		Runtime.OwnedCompletions.Add(++Runtime.NextCompletionTicket, [&Result] { Result.bOwnedCalled = true; });
		StopOn(Runtime);
		Result.bUnrelatedCalledAtStop = Result.Unrelated->load();
		Result.bClean = Runtime.Callbacks.IsEmpty() && Runtime.Completed.IsEmpty() && Runtime.Deferred.IsEmpty() && Runtime.OwnedCompletions.IsEmpty() &&
		                Runtime.DeferredSlots == 0 && Runtime.PendingBytes == 0 && Runtime.Counters.Delivered.load() == 2;
		return Result;
	}
} // namespace VaRest::Http::Testing
#endif
#if WITH_DEV_AUTOMATION_TESTS
namespace VaRest::Http::Testing
{
	TMap<FString, TWeakPtr<FPersistenceHooks, ESPMode::ThreadSafe>>& HookRegistry()
	{
		static auto* Registry = new TMap<FString, TWeakPtr<FPersistenceHooks, ESPMode::ThreadSafe>>();
		return *Registry;
	}
	FScopedPersistenceHooks::FScopedPersistenceHooks(FString InRoot) : Root(MoveTemp(InRoot)), Hooks(MakeShared<FPersistenceHooks, ESPMode::ThreadSafe>())
	{
		check(IsInGameThread());
		HookRegistry().Add(Root, Hooks);
	}
	FScopedPersistenceHooks::~FScopedPersistenceHooks()
	{
		check(IsInGameThread());
		for (const auto& Barrier : {Hooks->BeforeRead, Hooks->BeforeQueueLoad, Hooks->BeforeCacheWrite, Hooks->AfterPrepare, Hooks->BeforeRename})
			if (Barrier.IsValid())
				Barrier->Released = true;
		HookRegistry().Remove(Root);
	}
	TSharedPtr<const FPersistenceHooks, ESPMode::ThreadSafe> PersistenceHooksFor(const FString& Directory)
	{
		check(IsInGameThread());
		for (const auto& Pair : HookRegistry())
			if (Directory == Pair.Key || FPaths::IsUnderDirectory(Directory, Pair.Key))
			{
				if (const auto Pinned = Pair.Value.Pin())
					return MakeShared<FPersistenceHooks, ESPMode::ThreadSafe>(*Pinned);
			}
		return nullptr;
	}
	void WaitPersistenceBarrier(const TSharedPtr<FPersistenceBarrier, ESPMode::ThreadSafe>& Barrier)
	{
		if (!Barrier.IsValid())
			return;
		check(!IsInGameThread());
		Barrier->Entered = true;
		const double Deadline = FPlatformTime::Seconds() + 30.0;
		while (!Barrier->Released.load())
		{
			if (FPlatformTime::Seconds() >= Deadline)
			{
				Barrier->TimedOut = true;
				return;
			}
			FPlatformProcess::SleepNoStats(0.001f);
		}
	}
} // namespace VaRest::Http::Testing
#endif
namespace VaRest::Http::PersistencePrivate
{
	struct FFileAdapter
	{
		TFunction<bool(IFileHandle&, uint8*, int64)> Read = [](IFileHandle& File, uint8* Into, int64 Size) { return File.Read(Into, Size); };
		TFunction<bool(IFileHandle&, const uint8*, int64)> Write = [](IFileHandle& File, const uint8* From, int64 Size) { return File.Write(From, Size); };
		TFunction<bool(IFileHandle&)> Flush = [](IFileHandle& File) { return File.Flush(true); };
		TFunction<bool(IPlatformFile&, const FString&, const FString&)> Move = [](IPlatformFile& Platform, const FString& To, const FString& From)
		{ return Platform.MoveFile(*To, *From); };
	};
	FIoResult ReadFile(const FString& Path, int64 MaxBytes, const FFileAdapter& Ops);
	FIoResult WriteFile(const FString& Path, TConstArrayView<uint8> Bytes, const FFileAdapter& Ops);
} // namespace VaRest::Http::PersistencePrivate
namespace VaRest::Http::PersistencePrivate
{
	void CheckWorker()
	{
		if (IsInGameThread())
			++PersistenceCounters().OperationsOnGameThread;
		ensureMsgf(!IsInGameThread(), TEXT("Persistence disk operation on game thread"));
		check(!IsInGameThread());
	}
	FIoResult Failure(const TCHAR* Detail)
	{
		FIoResult Result;
		Result.Detail = Detail;
		return Result;
	}
} // namespace VaRest::Http::PersistencePrivate
VaRest::Http::FIoResult VaRest::Http::PersistencePrivate::ReadFile(const FString& Path, int64 MaxBytes, const FFileAdapter& Ops)
{
	PersistencePrivate::CheckWorker();
	IPlatformFile& Platform = FPlatformFileManager::Get().GetPlatformFile();
	if (Platform.IsSymlink(*Path) == ESymlinkResult::Symlink)
		return PersistencePrivate::Failure(TEXT("Symlink"));
	TUniquePtr<IFileHandle> File(Platform.OpenRead(*Path));
	if (!File.IsValid())
		return PersistencePrivate::Failure(TEXT("Missing"));
	const int64 Size = File->Size();
	if (Size < 0 || Size > MaxBytes || Size > MAX_int32)
		return PersistencePrivate::Failure(TEXT("Size"));
	FIoResult Result;
	Result.Bytes.SetNumUninitialized(static_cast<int32>(Size));
	if (Size > 0 && !Ops.Read(*File, Result.Bytes.GetData(), Size))
		return PersistencePrivate::Failure(TEXT("Read"));
	Result.bOk = true;
	return Result;
}
VaRest::Http::FIoResult VaRest::Http::PersistencePrivate::WriteFile(const FString& Path, TConstArrayView<uint8> Bytes, const FFileAdapter& Ops)
{
	PersistencePrivate::CheckWorker();
	IPlatformFile& Platform = FPlatformFileManager::Get().GetPlatformFile();
	if (Platform.IsSymlink(*FPaths::GetPath(Path)) == ESymlinkResult::Symlink || Platform.IsSymlink(*Path) == ESymlinkResult::Symlink)
		return PersistencePrivate::Failure(TEXT("Symlink"));
	if (Platform.FileExists(*Path) || Platform.DirectoryExists(*Path))
		return PersistencePrivate::Failure(TEXT("Exists"));
	if (!Platform.CreateDirectoryTree(*FPaths::GetPath(Path)))
		return PersistencePrivate::Failure(TEXT("Directory"));
	const FString Temporary = MakeTempFileName(Path, 0);
	ON_SCOPE_EXIT
	{
		Platform.DeleteFile(*Temporary);
	};
	TUniquePtr<IFileHandle> File(Platform.OpenWrite(*Temporary, false, false));
	if (!File.IsValid())
		return PersistencePrivate::Failure(TEXT("Open"));
	if (Bytes.Num() > 0 && !Ops.Write(*File, Bytes.GetData(), Bytes.Num()))
		return PersistencePrivate::Failure(TEXT("Write"));
	if (!Ops.Flush(*File))
		return PersistencePrivate::Failure(TEXT("Flush"));
	File.Reset();
	if (!Ops.Move(Platform, Path, Temporary))
		return PersistencePrivate::Failure(TEXT("Rename"));
	FIoResult Result;
	Result.bOk = true;
	return Result;
}
VaRest::Http::FIoResult VaRest::Http::DeleteFile(const FString& Path)
{
	PersistencePrivate::CheckWorker();
	IPlatformFile& Platform = FPlatformFileManager::Get().GetPlatformFile();
	FIoResult Result;
	Result.bOk = !Platform.FileExists(*Path) || Platform.DeleteFile(*Path);
	if (!Result.bOk)
		Result.Detail = TEXT("Delete");
	return Result;
}
VaRest::Http::FIoResult VaRest::Http::ListFiles(const FString& Directory)
{
	PersistencePrivate::CheckWorker();
	IPlatformFile& Platform = FPlatformFileManager::Get().GetPlatformFile();
	FIoResult Result;
	if (Platform.IsSymlink(*Directory) == ESymlinkResult::Symlink)
		return PersistencePrivate::Failure(TEXT("Symlink"));
	if (!Platform.DirectoryExists(*Directory))
	{
		Result.bOk = true;
		return Result;
	}
	int64 NameChars = 0, NameBytes = 0;
	Result.bOk =
	    Platform.IterateDirectory(*Directory,
		                          [&Result, &Platform, &NameChars, &NameBytes](const TCHAR* Path, bool bDirectory)
		                          {
		                              if (Result.Names.Num() >= 8192)
		                              {
			                              Result.Detail = TEXT("DirectoryLimit");
			                              return false;
		                              }
		                              if (!bDirectory && Platform.IsSymlink(Path) != ESymlinkResult::Symlink)
		                              {
			                              FString Name = FPaths::GetCleanFilename(Path);
			                              if (Name.Len() > 256 || NameChars + Name.Len() > 524288 || NameBytes + Name.GetAllocatedSize() > 2 * 1024 * 1024)
			                              {
				                              Result.Detail = TEXT("DirectoryLimit");
				                              return false;
			                              }
			                              NameChars += Name.Len();
			                              NameBytes += Name.GetAllocatedSize();
			                              Result.Names.Add(MoveTemp(Name));
		                              }
		                              return true;
	                              });
	return Result;
}

VaRest::Http::FIoResult VaRest::Http::ReadBoundedFile(const FString& Path, int64 MaxBytes)
{
	return PersistencePrivate::ReadFile(Path, MaxBytes, PersistencePrivate::FFileAdapter());
}
VaRest::Http::FIoResult VaRest::Http::WriteNewFile(const FString& Path, TConstArrayView<uint8> Bytes)
{
	return PersistencePrivate::WriteFile(Path, Bytes, PersistencePrivate::FFileAdapter());
}
#if WITH_DEV_AUTOMATION_TESTS
VaRest::Http::FIoResult VaRest::Http::Testing::ReadWithFault(const FString& Path, int64 MaxBytes, EFileFault Fault)
{
	PersistencePrivate::FFileAdapter Ops;
	if (Fault == EFileFault::ShortRead)
		Ops.Read = [](IFileHandle&, uint8*, int64) { return false; };
	return PersistencePrivate::ReadFile(Path, MaxBytes, Ops);
}
VaRest::Http::FIoResult VaRest::Http::Testing::WriteWithFault(const FString& Path, TConstArrayView<uint8> Bytes, EFileFault Fault)
{
	PersistencePrivate::FFileAdapter Ops;
	if (Fault == EFileFault::Write)
		Ops.Write = [](IFileHandle&, const uint8*, int64) { return false; };
	if (Fault == EFileFault::Flush)
		Ops.Flush = [](IFileHandle&) { return false; };
	if (Fault == EFileFault::Rename)
		Ops.Move = [](IPlatformFile&, const FString&, const FString&) { return false; };
	return PersistencePrivate::WriteFile(Path, Bytes, Ops);
}
#endif
