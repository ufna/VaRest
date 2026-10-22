// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Templates/Function.h"

#include <atomic>

namespace UE::Tasks
{
	class FPipe;
}

namespace VaRest::Http
{
	/**
	 * A redaction pipe behind a launch gate (plan decision 16): Launch from any thread, Start, Stop and Wait on the game
	 * thread. Callers never hold the pipe, so none can race its delete (review M-10). The module owns one (the functions
	 * below); a test owns its own, so it can force interleavings through the hook without touching the module's (review I-2).
	 * Trivially destructible and constant-initialised, so safe as a static (review I-4): a gate dropped while started leaks
	 * its pipe, as an `Automation RunTests ...; Quit` session does with the module's.
	 */
	class FRedactionGate
	{
	public:
		/** False, and Body dropped, before Start, after Stop, or while it stops */
		bool Launch(const TCHAR* DebugName, TUniqueFunction<void()>&& Body);
		void Start();
		/**
		 * Refuses new launches, waits for launches already inside Launch, then for the queued work, then deletes the pipe --
		 * FPipe's destructor checks it is idle (Pipe.h:40-43). A gate already stopped stays so.
		 */
		void Stop();
		/** Waits until the pipe is idle; false when there is none */
		bool Wait();

#if WITH_DEV_AUTOMATION_TESTS
		/** Where the test hook runs: in Launch, on the launching thread, and in Stop, on the game thread */
		enum class EHookPoint : uint8
		{
			/** Launch read the flag clear and has not counted itself in */
			AfterUncountedRead,
			/** Launch counted itself in and holds a published pipe, before FPipe::Launch */
			HoldingPipe,
			/** Stop raised the flag and reads a launch still inside: once per wait */
			StopWaitingForCount,
			/** Stop has seen no launch inside and has not taken the pipe out */
			StopAfterDrain,
		};
		using FHook = void (*)(void* Context, EHookPoint Point);
		/** Tests, game thread, before any thread launches on this gate and after the last one has returned */
		void SetTestHook(FHook InHook, void* InContext)
		{
			Hook = InHook;
			HookContext = InContext;
		}
#endif

	private:
#if WITH_DEV_AUTOMATION_TESTS
		void Reach(EHookPoint Point) const
		{
			if (Hook)
			{
				Hook(HookContext, Point);
			}
		}
		FHook Hook = nullptr;
		void* HookContext = nullptr;
#endif

		/**
		 * Sequentially consistent on purpose (Dekker): a launcher raises Launching and then reads bStopping again, the
		 * stopper raises bStopping and then reads Launching, so at least one sees the other -- the launch is refused, or
		 * the stopper waits for it. That second read in Launch carries the safety: without it a launcher that read the
		 * flag clear just before a stop counts itself in after the stopper's drain and launches on a pipe being deleted.
		 * The first, uncounted read only spares the stopper a wait under steady launching.
		 */
		std::atomic<UE::Tasks::FPipe*> Pipe{ nullptr };
		std::atomic<int32> Launching{ 0 };
		std::atomic<bool> bStopping{ true };
	};

	/**
	 * Core's redaction pipe (Inspector 7): body excerpts for logs and traces are redacted there, in order, never on the game
	 * thread. Any thread (plan decision 16): false, and Body dropped, before StartRedactionPipe, after StopRedactionPipe, or
	 * while it stops.
	 */
	bool LaunchRedaction(const TCHAR* DebugName, TUniqueFunction<void()>&& Body);
	/** Game thread (StartupModule) */
	void StartRedactionPipe();
	/** Game thread (ShutdownModule): FRedactionGate::Stop */
	void StopRedactionPipe();
	/** Tests, game thread (where StopRedactionPipe deletes it): waits until the pipe is idle; false when there is none */
	bool WaitForRedactionPipe();
}
