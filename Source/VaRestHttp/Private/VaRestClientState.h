// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/ScopeRWLock.h"
#include "VaRestClient.h"
#include "VaRestLimiter.h"
#include "VaRestRedactor.h"
#include "VaRestStage.h"

namespace VaRest::Http
{
	/** One immutable configuration snapshot; FClientState swaps a new one in on every edit (spec 3.1) */
	struct FClientConfig
	{
		FString BaseUrl;
		FString DisplayName;
		FVaRestHeaders DefaultHeaders;
		EVaRestJsonNaming Naming = EVaRestJsonNaming::CamelCase;
		EVaRestLogLevel LogLevel = EVaRestLogLevel::Basic;
		int32 MaxConcurrent = 16;
		float Timeout = 0.f;
		float ActivityTimeout = 0.f;
		int64 MaxJsonBytes = 32 * 1024 * 1024;
		/** The project's (UVaRestHttpSettings::RedactKeys) and this client's additions */
		TArray<FString> RedactKeys;
		TArray<TPair<FVaRestStageRef, EVaRestStageSlot>> CustomStages;
		TSharedPtr<IVaRestTransport, ESPMode::ThreadSafe> Transport;
		TSharedPtr<IVaRestClock, ESPMode::ThreadSafe> Clock;

		// Derived by FClientState::Publish
		TSharedPtr<const FVaRestStageChain, ESPMode::ThreadSafe> Chain;
		TSharedPtr<const FVaRestRedactor, ESPMode::ThreadSafe> Redactor;
		/** A custom stage may re-issue: the transport copies bodies (decision 12) */
		bool bMayReissue = false;
	};

	class FClientState : public TSharedFromThis<FClientState, ESPMode::ThreadSafe>
	{
	public:
		FClientState(uint64 InClientId, FClientConfig&& InConfig);
		~FClientState();

		/** Builds the derived configuration and registers the client. Game thread. */
		static TSharedRef<FClientState, ESPMode::ThreadSafe> Create(FClientConfig&& InConfig);

		uint64 GetClientId() const { return ClientId; }
		/** Any thread */
		TSharedRef<const FClientConfig, ESPMode::ThreadSafe> GetConfig() const;
		/** Game thread: copy, edit, rebuild the chain and the redactor, swap */
		void EditConfig(TFunctionRef<void(FClientConfig&)> Edit);

		/** Game thread: a sent exchange enters the pipeline */
		void Enter(const FVaRestExchangeRef& Exchange);
		/** Game thread: the transport's result, brought over from the thread that completed it */
		void OnAttemptDone(const FVaRestExchangeRef& Exchange, FVaRestTransportResult&& Result, FVaRestOutcomeCallback&& OnOutcome);
		/** Game thread */
		int32 NumInFlight() const { return InFlight.Num(); }
		/** Game thread */
		FLimiter& GetLimiter() { return Limiter; }

		/** Every live client (game thread): the trace snapshot, engine shutdown */
		static void ForEachLive(TFunctionRef<void(FClientState&)> Visit);

		/**
		 * Game thread. The first reason wins, except that a silent one (OwnerGone, Shutdown) overrides User: callbacks
		 * must not run into an owner that is gone or at engine exit, whatever canceled first (review C-2).
		 */
		void Cancel(const FVaRestExchangeRef& Exchange, EVaRestCancelReason Reason);
		/**
		 * Game thread: every in-flight exchange with InTag (all for NAME_None) -- those Enter has seen. One sent from another
		 * thread whose Enter task has not run is not in InFlight yet; its handle cancels it (task 11 review, minor 3).
		 */
		void CancelAll(FName InTag, EVaRestCancelReason Reason);
		/** FCoreDelegates::OnEnginePreExit: every live client's requests, without callbacks; later sends complete silently (review M-14) */
		static void CancelForShutdown();
		/** Any thread */
		static bool IsShuttingDown();
#if WITH_DEV_AUTOMATION_TESTS
		/** Tests: the editor goes on after a simulated shutdown */
		static void ResetShutdownInternal();
#endif

	private:
		bool Resolve(FVaRestExchange& Exchange, FString& OutError) const;
		void OnChainDone(const FVaRestExchangeRef& Exchange, FVaRestAttemptOutcome&& Outcome);
		void Complete(const FVaRestExchangeRef& Exchange, FFinish&& Finish);
		FFinish MakeFinish(const FVaRestExchange& Exchange) const;
		void Publish(const TSharedRef<FClientConfig, ESPMode::ThreadSafe>& Next);

		const uint64 ClientId;
		mutable FRWLock ConfigLock;
		/**
		 * Never null between the constructor and the destructor. A TSharedPtr, not a TSharedRef, so that ~FClientState can
		 * move it into a game-thread task: a TSharedRef "move" copies (SharedPointer.h:354-361), which would leave the
		 * member's reference to die on the dying thread (review N-3).
		 */
		TSharedPtr<const FClientConfig, ESPMode::ThreadSafe> Config;
		/** Spec 3.2: in-flight exchanges are owned here and keep this state alive (each holds its State) */
		TMap<uint64, FVaRestExchangeRef> InFlight;
		/** Spec 4.6 */
		FLimiter Limiter;
	};

	/** The chain's built-in stages (VaRestPipeline.cpp) */
	FVaRestStageRef MakeAttemptBoundaryStage();
	FVaRestStageRef MakeLimiterStage();
	FVaRestStageRef MakeTransportStage();
}
