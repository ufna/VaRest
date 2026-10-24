// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include <atomic>

namespace VaRest::Http
{
	struct FIoResult
	{
		bool bOk = false;
        bool bCleanupOk = true; // WriteNewFile: no newly owned temporary remains; pre-IO refusal is clean
		FString Detail;
		TArray<uint8> Bytes;
		TArray<FString> Names;
	};
	struct FIoCounters
	{
		std::atomic<uint64> Launched{0};
		std::atomic<uint64> HandedBack{0};
		std::atomic<uint64> Delivered{0};
		std::atomic<uint64> OperationsOnGameThread{0};
	};
	using FIoWork = TUniqueFunction<FIoResult()>;
	using FIoDone = TUniqueFunction<void(FIoResult&&)>;
	bool LaunchPersistence(FIoWork&& Work, FIoDone&& Done, int64 ReservedBytes = 0, bool bControlDuringStop = false); // GT admission only
	void StartPersistence();
	void PumpPersistence();      // GT only, consumes this executor's slots
	bool PersistenceIdle();      // GT only, includes callbacks and deferred continuations
	bool PersistenceAccepting(); // GT only
	bool PostPersistenceContinuation(TUniqueFunction<void()>&& Callback);
	bool PostPersistencePoll(TUniqueFunction<bool()>&& Callback);
	uint64 AcquirePersistenceCompletion(); // GT, bounded permit acquired before a persistence wait
	void SetPersistenceCompletion(uint64 Ticket, TUniqueFunction<void()>&& Callback);
	void ReleasePersistenceCompletion(uint64 Ticket); // GT, idempotent, destroys pending completion on GT
	void StopPersistence();                           // worker drain + silent internal commit accounting; suppresses user handbacks at pre-exit
	FIoCounters& PersistenceCounters();
	enum class EPathKind : uint8 { Missing, Regular, Directory, Symlink, Other, Error };
	struct FPathStatus
	{
		EPathKind Kind = EPathKind::Error;
		int64 Size = 0;
		FDateTime Modified;
	};
	/** Worker-only physical writable-file status; Missing is positive absence, never unknown IO failure. */
	FPathStatus InspectPersistencePath(const FString& Path);
	/** Worker-only writable native path; preserves mobile platform mapping before absolute normalization. */
	FString PersistencePathForIo(const FString& Path);
	/** Worker-only known-link check; uses the same physical status boundary. */
	bool IsKnownSymlink(const FString& Path);
	FIoResult ReadBoundedFile(const FString& Path, int64 MaxBytes);
	FIoResult WriteNewFile(const FString& Path, TConstArrayView<uint8> Bytes);
	FIoResult DeleteFile(const FString& Path);
	FIoResult ListFiles(const FString& Directory);

	template <typename ResultType>
	bool LaunchPersistenceResult(TUniqueFunction<ResultType()>&& Work, TUniqueFunction<void(ResultType&&)>&& Done, int64 ReservedBytes = 0)
	{
		const auto Box = MakeShared<TOptional<ResultType>, ESPMode::ThreadSafe>();
		return LaunchPersistence(
		    [Box, Job = MoveTemp(Work)]() mutable
		    {
			    Box->Emplace(Job());
			    FIoResult Status;
			    Status.bOk = true;
			    return Status;
		    },
		    [Box, Callback = MoveTemp(Done)](FIoResult&& Status) mutable
		    {
			    check(Status.bOk && Box->IsSet());
			    Callback(MoveTemp(Box->GetValue()));
		    },
		    ReservedBytes);
	}

#if WITH_DEV_AUTOMATION_TESTS
	namespace Testing
	{
		struct FPersistenceBarrier
		{
			std::atomic<bool> Entered{false}, Released{false}, TimedOut{false};
		};
		struct FPersistenceHooks
		{
			TSharedPtr<FPersistenceBarrier, ESPMode::ThreadSafe> BeforeRead, BeforeQueueLoad, BeforeCacheWrite, AfterPrepare, BeforeRename, BeforePrepareCleanup;
			bool bFailCommit = false, bFailCacheControl = false, bFailPrepare = false; // configured before admission, immutable on worker
		};
		int64 PersistencePendingBytesForTest();
		TSharedPtr<const FPersistenceHooks, ESPMode::ThreadSafe> PersistenceHooksFor(const FString& Directory);
		void WaitPersistenceBarrier(const TSharedPtr<FPersistenceBarrier, ESPMode::ThreadSafe>& Barrier);
		class FScopedPersistenceHooks
		{
			public:
			explicit FScopedPersistenceHooks(FString InRoot);
			~FScopedPersistenceHooks();
			FScopedPersistenceHooks(const FScopedPersistenceHooks&) = delete;
			FScopedPersistenceHooks& operator=(const FScopedPersistenceHooks&) = delete;
			FString Root;
			TSharedRef<FPersistenceHooks, ESPMode::ThreadSafe> Hooks;
		};
		class FScopedPersistenceRuntime
		{
			public:
			FScopedPersistenceRuntime();
			~FScopedPersistenceRuntime();
			FScopedPersistenceRuntime(const FScopedPersistenceRuntime&) = delete;
			FScopedPersistenceRuntime& operator=(const FScopedPersistenceRuntime&) = delete;
			void Stop();

			private:
			void* Owned = nullptr;
		};
		// Captured per job: no production-global barrier or counter mutation.
		bool LaunchWithPublicationBarrier(FIoWork&& Work, FIoDone&& Done, const TSharedPtr<FPersistenceBarrier, ESPMode::ThreadSafe>& Barrier);
		struct FStopProbe
		{
			bool bOrdinaryCalled = false, bCommitCalled = false, bUnrelatedCalledAtStop = false, bAcceptedAfterStop = false, bClean = false;
			bool bDeferredCalled = false, bOwnedCalled = false;
			TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> Unrelated = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
		};
		FStopProbe RunIsolatedStopProbe();
		enum class EFileFault : uint8
		{
			ShortRead,
			Write,
			Flush,
			Rename
		};
		FIoResult ReadWithFault(const FString& Path, int64 MaxBytes, EFileFault Fault);
		FIoResult WriteWithFault(const FString& Path, TConstArrayView<uint8> Bytes, EFileFault Fault,
            TSharedPtr<FPersistenceBarrier, ESPMode::ThreadSafe> BeforeCleanup = nullptr);
	} // namespace Testing
#endif
} // namespace VaRest::Http
