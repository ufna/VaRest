// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Math/RandomStream.h"
#include "Misc/ScopeRWLock.h"
#include "VaRestAuth.h"
#include "VaRestClient.h"
#include "VaRestEnvelope.h"
#include "VaRestExchangeState.h"
#include "VaRestLimiter.h"
#include "VaRestRedactor.h"
#include "VaRestRetry.h"
#include "VaRestSession.h"
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
		/** Spec 4.1, WithRetry: none until set (plan decision 1); a request's Retry()/NoRetry() replaces it */
		FVaRestRetryPolicy Retry = FVaRestRetryPolicy::Disabled();
		/** Spec 3.4, WithEnvelope: null = none; a request's Envelope()/NoEnvelope() replaces it. Shared, immutable: decode tasks read it. */
		TSharedPtr<const FVaRestEnvelope, ESPMode::ThreadSafe> Envelope;
		/** The project's (UVaRestHttpSettings::RedactedKeys) and this client's additions (WithRedactedKeys) */
		TArray<FString> RedactedKeys;
		/** Spec 4.2, WithAuth: null = none (session headers only). Shared, immutable to the core: AuthApply reads it per attempt. */
		TSharedPtr<IVaRestAuth, ESPMode::ThreadSafe> Auth;
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
		/**
		 * Any thread: the redactor of the configuration published now. It forgets values (ClearSecretValues, a rotated
		 * value's 10 minutes, the cap of FVaRestSecretSet::MaxPrevious), so an exchange's lines use RedactorFor (review I-1).
		 */
		TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> GetRedactor() const { return GetConfig()->Redactor.ToSharedRef(); }
		/** Game thread: copy, edit, rebuild the chain and the redactor, swap */
		void EditConfig(TFunctionRef<void(FClientConfig&)> Edit);

		/**
		 * Game thread: the stream retry jitter draws from (plan decision 26). Seeded from the client id and the CPU cycle
		 * counter, so clients made in the same frame differ; WithRandomSeed makes a test exact.
		 */
		FRandomStream& GetRandom()
		{
			// GetFraction mutates the stream's seed, and nothing else guards it: WithRandomSeed asserts the same
			check(IsInGameThread());
			return Random;
		}

		/** Game thread: a sent exchange enters the pipeline */
		void Enter(const FVaRestExchangeRef& Exchange);
		/** Game thread: the transport's result, brought over from the thread that completed it */
		void OnAttemptDone(const FVaRestExchangeRef& Exchange, FVaRestTransportResult&& Result, FVaRestOutcomeCallback&& OnOutcome);
		/** Game thread */
		int32 NumInFlight() const { return InFlight.Num(); }
		/** Game thread */
		FLimiter& GetLimiter() { return Limiter; }

		/**
		 * Game thread: the game's layer-3 values (SetSecretValue; Inspector 2.2), each stored as GameSecretSlot(Slot) (review
		 * M-1); publishes a new redactor
		 */
		void SetSecret(FName Slot, const FString& Value);
		/**
		 * Game thread: VaRest's own slots (the session's, an auth's), taken as named; several, one republish (each SetSecret
		 * republishes). AlsoEdit joins that republish: a session header's name becomes a key in the same one.
		 */
		void SetSecrets(TConstArrayView<TPair<FName, FString>> Values);
		void SetSecrets(TConstArrayView<TPair<FName, FString>> Values, TFunctionRef<void(FClientConfig&)> AlsoEdit);
		/** Game thread, ClearSecretValues: forgets the game's slots only, the rotated ones too (review I-1) */
		void ClearSecrets();
		/**
		 * Game thread: WithAuth (InAuth valid) and WithoutAuth (null). One republish: the auth, its layer-2 keys and its
		 * layer-3 values, each under AuthSecretSlot (Task 14 carry: a custom auth's name never reaches a session or game
		 * slot). The previous auth's slots the new one does not set are retired (Task 14 carry, as ImportSession retires a
		 * dropped header): known for 10 minutes, then forgotten (Inspector D21).
		 */
		void SetAuth(const TSharedPtr<IVaRestAuth, ESPMode::ThreadSafe>& InAuth);
		/** Game thread: WithClock. Pending secret expiries move to the new clock's time, and their timer with them (review M6). */
		void SetClock(const TSharedRef<IVaRestClock, ESPMode::ThreadSafe>& NewClock);

		// ---- Session (spec 4.2, plan decisions 5 and 6; VaRestSession.cpp). Game thread.
		const FVaRestSessionData& GetSessionData() const { return Session.Data; }
		FVaRestSessionChangedEvent& OnSessionChanged() { return Session.Changed; }
		FVaRestSessionExpiredEvent& OnSessionExpired() { return Session.Expired; }
		/**
		 * Concurrency review C-1, I-2: the one way into a session change. Keeps this state alive while Change and the events
		 * it queued run -- a listener may drop the last FVaRestClient -- then delivers the queued events in order, each to
		 * every listener before the next. Inside a listener it only queues; the outer call delivers. The functions below
		 * check that they run inside one.
		 */
		void RunSessionChange(TFunctionRef<void(FClientState&)> Change);
		/**
		 * Data and secrets only, no event; RefreshToken null keeps the current one; ExpiresInSeconds 0 = unknown.
		 * LifetimeSeconds: the token's whole lifetime when the caller knows it (a login or refresh response), else 0 -- an
		 * imported session's time left is not its lifetime (re-review N-10). True when anything changed.
		 */
		bool UpdateTokens(const FString& AccessToken, const FString* RefreshToken, double ExpiresInSeconds, double LifetimeSeconds);
		/** Queues an IN-9 marker and OnSessionChanged(Change) for RunSessionChange to deliver */
		void AnnounceSession(EVaRestSessionChange Change, uint64 CauseRequestId, const TCHAR* Text);
		/** SetBearerToken, SetSession, ImportSession: Replaced, or Refreshed with bSameIdentity (plan decision 5); no event when nothing changed */
		void SetTokensByGame(const FString& AccessToken, const FString* RefreshToken, double ExpiresInSeconds, double LifetimeSeconds, bool bSameIdentity);
		void SetSessionHeader(FStringView Name, FStringView Value);
		void RemoveSessionHeader(FStringView Name);
		/** Logout: tokens, headers and the session's secret slots; OnSessionChanged(Cleared). Nothing happens to an empty session. */
		void ClearSession();
		/**
		 * A rejected refresh (Task 16) or NotifySessionExpired: the tokens go, the headers stay; OnSessionExpired queued.
		 * Once: a session with no token left queues nothing (concurrency review I-2).
		 */
		void ExpireSession(uint64 CauseRequestId, FStringView Why);
		/** Spec 4.2: the session as JSON (Task 13). Game thread; not a change. */
		FString ExportSession() const;
		/**
		 * Inside a change only: false, and nothing changes, for anything but an ExportSession() result of at most
		 * MaxSessionJsonChars. Tokens that are both empty are a logout: ClearSession(), true.
		 */
		bool ImportSession(FStringView InJson, bool bSameIdentity);

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
		/**
		 * Game thread: Ex's answer was accepted (Complete, a deferred Complete, OnChainDone's decode), so nothing more may
		 * start for it. A stage's wait ends through its interrupt hook (plan decision 7; concurrency review M-3, re-review
		 * N-6), and an attempt still in the limiter's queue -- a retry re-issued just before the answer -- leaves it, so a
		 * grant cannot dispatch it (Task 8 review minor 3). Each answers Canceled up the chain; the caller has already made
		 * the exchange ignore it (bFinished, bCompletionQueued or Phase).
		 */
		void EndWaits(FExchangeState& Ex);
		FFinish MakeFinish(const FExchangeState& Exchange) const;
		void Publish(const TSharedRef<FClientConfig, ESPMode::ThreadSafe>& Next);
		/** Publishes again when the next rotated-out value expires */
		void ScheduleSecretExpiry();
		/** IN-13 layer 3: the session's tokens and header values as its secret slots, one republish (VaRestSession.cpp) */
		void PublishSessionSecrets();
		/** The same, with AlsoEdit in that republish */
		void PublishSessionSecrets(TFunctionRef<void(FClientConfig&)> AlsoEdit);
		/** Every session function but the getters runs inside RunSessionChange, on the game thread (concurrency review C-1) */
		void CheckInSessionChange() const;

		const uint64 ClientId;
		mutable FRWLock ConfigLock;
		/**
		 * Never null between the constructor and the destructor. A TSharedPtr, not a TSharedRef, so that ~FClientState can
		 * move it into a game-thread task: a TSharedRef "move" copies (SharedPointer.h:354-361), which would leave the
		 * member's reference to die on the dying thread (review N-3).
		 */
		TSharedPtr<const FClientConfig, ESPMode::ThreadSafe> Config;
		FRandomStream Random;
		/** Spec 3.2: in-flight exchanges are owned here and keep this state alive (each holds its State) */
		TMap<uint64, FVaRestExchangeRef> InFlight;
		/** Spec 4.6 */
		FLimiter Limiter;
		/** Game thread; Publish copies its active values into each redactor */
		FVaRestSecretSet Secrets;
		FVaRestTimerHandle SecretExpiryTimer;
		/** Game thread: the slots the current auth's values are in (SetAuth), so the next SetAuth knows what to retire */
		TArray<FName> AuthSlots;
		/**
		 * Spec 4.2: the live session (plan decision 6). Game thread. Its listeners and (Task 16) refresh waiters are game code,
		 * so ~FClientState off the game thread sends it there with the configuration (concurrency review M-9).
		 */
		FSessionState Session;
	};

	/**
	 * The one friend of FVaRestClient: a client's state for VaRestHttp's own code and its tests, never for a module outside
	 * it (final review I1)
	 */
	struct FClientAccess
	{
		static TSharedPtr<FClientState, ESPMode::ThreadSafe> State(const FVaRestClient& Client) { return Client.GetStateInternal(); }
		/** A handle on a state, for the refresh context and the coordinator (Tasks 15-16) */
		static FVaRestClient Wrap(TSharedPtr<FClientState, ESPMode::ThreadSafe> InState) { return FVaRestClient(MoveTemp(InState)); }
	};

	/**
	 * Game thread: the redactor of Ex's logs, traces and FVaRestError::Url (plan decision 17, carry (b); review I-1) --
	 * what Ex's Send() snapshot knew, what its attempts applied (Ex.AppliedRedactor, Task 12 review M-7) and what the
	 * client knows now, all three: a secret set while the request is out (an auth refresh) is scrubbed from its later
	 * lines, and a token the request carried stays scrubbed after the client forgets it (a logout, a rotated value's 10
	 * minutes, the cap of MaxPrevious) -- the one it had at Send() and one AuthApply wrote later alike. The current
	 * redactor itself when it is the one Ex knows (nothing changed while the request ran) or knows all that one did; else
	 * a union, built once per change and cached on Ex.
	 * Cost: after ClearSession (ClearSecretValues today) an in-flight request's later lines still scrub the old token --
	 * the exchange keeps it in memory until it dies, as the snapshot did before decision 17.
	 */
	TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> RedactorFor(const FExchangeState& Ex);
	/**
	 * Game thread, AuthApply (Task 12 review M-7): when the client's redactor now is not the one Ex knows (Config's, or
	 * AppliedRedactor once set), joins it into Ex.AppliedRedactor, so what this attempt carries stays scrubbed for Ex's
	 * life. One pointer comparison when nothing changed since Send() or the last attempt.
	 */
	void RememberAppliedRedactor(FExchangeState& Ex);

	/** The chain's built-in stages (VaRestPipeline.cpp) */
	FVaRestStageRef MakeAttemptBoundaryStage();
	FVaRestStageRef MakeLimiterStage();
	FVaRestStageRef MakeTransportStage();
	/** Spec 4.5, per attempt (VaRestLogStage.cpp) */
	FVaRestStageRef MakeLogStage();
	/** The Basic line (a Warning) of a request refused before the chain, with Kind=Config: a builder error or Resolve's */
	void LogConfigError(const FExchangeState& Ex, FStringView Message);
	/** Checks and de-duplicates redaction keys where they enter (task 14 review M4); VaRestClient.cpp */
	void AppendRedactKeys(TArray<FString>& Into, TConstArrayView<FString> Keys);
	/** The Basic line of an attempt that ended in the limiter's queue (ruling I-3: it never reaches the log stage) */
	void LogEndedInQueue(const FExchangeState& Ex, const FVaRestAttemptOutcome& Outcome);
	/** The cvar VaRest.LogLevel when it is 0..3, else the client's level */
	EVaRestLogLevel EffectiveLogLevel(EVaRestLogLevel ClientLevel);
	/** Spec 4.5: "Logged bodies are truncated to 4 KiB" */
	inline constexpr int32 LogBodyCap = 4096;
}
