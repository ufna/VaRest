// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestNativeOwnerPump.h"

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)

#include "Connector/VaRestNumericConnectorNative.h"
#include "Templates/UniquePtr.h"
#include <atomic>

class FRunnableThread;

namespace VaRest::WebSocket::Lws
{
	class FService;
	class FNativeOwnerServiceAdapter;

	// Installed before create, kept by the actual creating graph through real join.
	// The child reads no parent journal or Thread pointer. These atomics publish
	// only local initialization/diagnostics, never cold admission or join authority.
	class [[gnu::visibility("hidden")]] FNativeOwnerServiceChildSnapshot final
	{
		friend class FNativeOwnerServiceAdapter;
		friend class FService;

	public:
		~FNativeOwnerServiceChildSnapshot() = default;
		VrStatus RegisterFromActualInit() const;
		void RunCleanupFromActualRun() const;
		void RequestCleanupStopFromActualStop() const;
		int32 GetC1EntryDiagnostic() const;
		int32 GetC1CleanupDiagnostic() const;
		// Advisory local initialization only; every actual native API call still
		// revalidates real role/lease/pump/StopEpoch and the original deadline.
		bool IsLocalNativeWorkPermitted() const;
		VrStatus GetRegistrationDiagnostic() const;

	private:
		FNativeOwnerServiceChildSnapshot(const FNativeOwnerServiceValues&, VrSession,
			NumericConnectorC1::FNative1*, const NumericConnectorC1::FResourceBinding1*,
            lws_context*, const VrLwsCustody1*, lws_vhost*, lws_vhost*);
		FNativeOwnerServiceChildSnapshot(const FNativeOwnerServiceChildSnapshot&) = delete;
		FNativeOwnerServiceChildSnapshot& operator=(const FNativeOwnerServiceChildSnapshot&) = delete;
		void CloseLocalPermit();
		const FNativeOwnerServiceValues Values;
		const VrSession Session;
		NumericConnectorC1::FNative1* const Native;
		const NumericConnectorC1::FResourceBinding1* const Resources;
        lws_context* const Context;
        const VrLwsCustody1* const View;
        lws_vhost* const Vhosts[2];
        mutable std::atomic<int32> ServiceDiagnostic {VR_STATUS_NO_RESULT};
        mutable std::atomic<uint32> SourceFullReturnDiagnostic {0};
		mutable std::atomic<uint32> LocalPermit {1};
		mutable std::atomic<int32> RegistrationDiagnostic {VR_STATUS_PENDING};
		mutable std::atomic<int32> C1EntryDiagnostic {static_cast<int32>(NumericConnectorC1::EStatus1::PENDING)};
		mutable std::atomic<int32> C1CleanupDiagnostic {static_cast<int32>(NumericConnectorC1::EStatus1::PENDING)};
	};

	class [[gnu::visibility("hidden")]] FNativeOwnerServiceAdapter final : public TSharedFromThis<FNativeOwnerServiceAdapter, ESPMode::ThreadSafe>
	{
		friend class FService;

	public:
		// PRECONDITION: the actual creating owner retained this whole graph/host
		// until CanReleaseCreatingAdapter. This destructor does not acknowledge,
		// forget, retry or clean native responsibility. A violation is not recovery.
		~FNativeOwnerServiceAdapter();

	private:
		enum class ECreatingOutcome : uint8
		{
			None, StartReserved, NeverCreatedPending, NeverCreatedRecorded,
			CreatedJoinUnobserved, JoinedPending, JoinedRecorded, Uncertain
		};

		// Every later call also requires continuous actual producer/O/live-lease
		// and WS host-code protection; shared objects and these bits are not pins.
		// ZERO production callers. Actual qualified mapping/O, Supplier/control and
		// host code/return lifetime are prerequisites, not certified by these values.
		static TSharedPtr<FNativeOwnerServiceAdapter, ESPMode::ThreadSafe> CreateForService(const FNativeOwnerServiceValues&);
		explicit FNativeOwnerServiceAdapter(const FNativeOwnerServiceValues&);
		FNativeOwnerServiceAdapter(const FNativeOwnerServiceAdapter&) = delete;
		FNativeOwnerServiceAdapter& operator=(const FNativeOwnerServiceAdapter&) = delete;
        // PRECONDITION before this attach: genuine Host lexical frame/retention
        // interval is ALREADY committed. PumpRegistered1 only records that fact.
		VrStatus AttachBeforeCreatingResources();
        FVaRestNativeOwnerHostStep1 PumpOnControlThread();
        FVaRestNativeOwnerHostStep1 PreserveFrameDisposition1(FVaRestNativeOwnerHostStep1) const;
		VrStatus InitializeStartupPhaseBeforeResources();
		VrLwsPhaseBudget1* GetCreatingPhaseOnControlThread();
		void EnterFinalPhaseAfterKnownOutcome();
		const FNativeOwnerServiceChildSnapshot* PrepareRealThreadCreate(NumericConnectorC1::FNative1*,
			const NumericConnectorC1::FResourceBinding1*, lws_context* = nullptr, const VrLwsCustody1* = nullptr, lws_vhost* = nullptr, lws_vhost* = nullptr);
		VrStatus ObserveRealThreadCreated(FRunnableThread&);
		VrStatus AbortStartupBeforeThreadCreate();
		// Called only at the actual source-proven failed real-create/no-child boundary.
		VrStatus ObserveKnownNoChildAfterCreateFailure();
		void ObserveUncertainCreateOutcome();
		VrStatus StopBeforeCreatingJoin();
		// Returns the same recorded real wrapper once. Actual create/Kill/delete
		// remain I3. No later BUSY bookkeeping status can return it for another join.
		FRunnableThread* PrepareFirstCreatingJoin();
		// The sole creating owner ALREADY consumed its first Kill(true) result at
		// the real I3 boundary. No Boolean or generic SetOutcome can assert a fact.
		VrStatus ObserveKnownFirstSuccessfulJoin();
		void ObserveUncertainFirstJoin();
		VrStatus RetryPendingOutcomeOnControlThread();
		// Genuine actual creating close/callback completion is a CALLSITE premise;
		// no resource counter is edited. PENDING can retire local calls while the
		// native durable joined/deferred lease remains charged, not claim detach OK.
		VrStatus AfterKnownCreatingResourcesClosed();
		bool CanReleaseCreatingAdapter() const;

		VrStatus CommitKnownNoChild();
		VrStatus CommitKnownJoin();
		VrStatus StopPump();
		void RetireLocalCallingValues();
		bool HasOutstandingCreatingResponsibility() const;

		const FNativeOwnerServiceValues Values;
		VrSession Session {};
		ECreatingOutcome Outcome = ECreatingOutcome::None;
		TSharedPtr<FActiveOwnerPump, ESPMode::ThreadSafe> Pump;
		TUniquePtr<FNativeOwnerServiceChildSnapshot> Child;
		FRunnableThread* CreatedThread = nullptr; // Borrowed, parent GT only; no delete here.
		bool CreatePrepared = false;
		bool JoinPrepared = false;
		bool StopRequested = false;
		bool Attaching = false;
		bool CommittingOutcome = false;
		bool CreatingResourcesClosed = false;
		bool JoinFactDurable = false;
		bool NativeDetached = false;
		bool LocalCallsRetired = false;
		VrStatus LastOutcomeStatus = VR_STATUS_PENDING;
		VrStatus LastDetachStatus = VR_STATUS_PENDING;
		// GT-only owned gas, survives pending steps. NEVER read by the child.
		// C1<=51 + Init worst4 are held WITHIN each cap, not actual-call claims.
		VrLwsPhaseBudget1 CreatingPhase {};
	};
}

#endif
