// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/ScopeRWLock.h"
#include "VaRestClient.h"
#include "VaRestExchangeState.h"
#include "VaRestLimiter.h"
#include "VaRestRedactor.h"
#include "VaRestStage.h"

namespace VaRest::Http
{
	/** One immutable configuration snapshot; FClientState swaps a new one in on every edit (spec 3.1) */
	struct FClientConfig
	{
		FString BaseUrl;
		/** WithName's text; empty: the client is named by its current BaseUrl (IN-8), which the trace URL-redacts */
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

		/** Game thread: layer 3's known values (Inspector 2.2); publishes a new redactor */
		void SetSecret(FName Slot, const FString& Value);
		void ClearSecrets();
		/** Game thread: WithClock. Pending secret expiries move to the new clock's time, and their timer with them (review M6). */
		void SetClock(const TSharedRef<IVaRestClock, ESPMode::ThreadSafe>& NewClock);

		/** Every live client (game thread): the trace snapshot, engine shutdown */
		static void ForEachLive(TFunctionRef<void(FClientState&)> Visit);
		/** Game thread: the trace snapshot (IN-12) */
		void ForEachInFlight(TFunctionRef<void(FExchangeState&)> Visit) const
		{
			for (const TPair<uint64, FVaRestExchangeRef>& Pair : InFlight)
			{
				Visit(Internals(*Pair.Value));
			}
		}

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
		bool Resolve(FExchangeState& Exchange, FString& OutError) const;
		void OnChainDone(const FVaRestExchangeRef& Exchange, FVaRestAttemptOutcome&& Outcome);
		void Complete(const FVaRestExchangeRef& Exchange, FFinish&& Finish);
		FFinish MakeFinish(const FExchangeState& Exchange) const;
		void Publish(const TSharedRef<FClientConfig, ESPMode::ThreadSafe>& Next);
		/** Publishes again when the next rotated-out value expires */
		void ScheduleSecretExpiry();

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
		/** Game thread; Publish copies its active values into each redactor */
		FVaRestSecretSet Secrets;
		FVaRestTimerHandle SecretExpiryTimer;
	};

	/**
	 * The one friend of FVaRestClient: a client's state for VaRestHttp's own code and its tests, never for a module outside
	 * it (final review I1)
	 */
	struct FClientAccess
	{
		static TSharedPtr<FClientState, ESPMode::ThreadSafe> State(const FVaRestClient& Client) { return Client.GetStateInternal(); }
	};

	/** The chain's built-in stages (VaRestPipeline.cpp) */
	FVaRestStageRef MakeAttemptBoundaryStage();
	FVaRestStageRef MakeLimiterStage();
	FVaRestStageRef MakeTransportStage();
	/** Spec 4.5, per attempt (VaRestLogStage.cpp) */
	FVaRestStageRef MakeLogStage();
	/** The Basic line (a Warning) of a request refused before the chain, with Kind=Config: a builder error or Resolve's */
	void LogConfigError(const FExchangeState& Ex, FStringView Message);
	/** The cvar VaRest.LogLevel when it is 0..3, else the client's level */
	EVaRestLogLevel EffectiveLogLevel(EVaRestLogLevel ClientLevel);
	/** Spec 4.5: "Logged bodies are truncated to 4 KiB" */
	inline constexpr int32 LogBodyCap = 4096;
}
