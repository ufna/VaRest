// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "VaRestLwsService.h"
#include "VaRestNativeOwnerHost1.h"
#include "VaRestSocketTransport.h"

namespace VaRest::WebSocket::Lws
{
	/**
	 * IVaRestSocketTransport on one FService: VaRest's own client on the engine-bundled libwebsockets (spec 2, ruling R1).
	 * Game thread, but GetCounters' pointee (atomics, any thread) and IsShuttingDown (the game thread or the service thread)
	 */
	class FTransport final : public IVaRestSocketTransport
	{
	public:
		explicit FTransport(FServiceOptions InOptions);
		/** Game thread: shuts the service down when Shutdown did not */
		virtual ~FTransport() override;

		/** Game thread (IVaRestSocketTransport::Connect) */
		virtual TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Connect(FVaRestSocketConnectRequest&& Request, TFunction<void()> OnEvents = nullptr) override;

		/** Game thread: the context and the thread, made once (plan decision 8); false when that failed, or after Shutdown */
        bool EnsureStarted();
        // Nonvirtual private-module forwards; IVaRestSocketTransport is unchanged.
        FVaRestNativeOwnerHostStep1 StartNativeOwnerSession1();
        FVaRestNativeOwnerHostStep1 FinishNativeOwnerSession1();
        FVaRestNativeOwnerHostStep1 AdvanceNativeOwnerFinish1();
        // Existing session only; no lazy Service, Start, join or delegate dispatch.
        FVaRestNativeOwnerHostStep1 PumpNativeOwnerSession1();
		/** Game thread: every connection ends (FService::Shutdown), the thread stops, the context goes; later connects end with Shutdown */
		void Shutdown();
		/** Game thread: a forking server's child starts again, with a context and a thread of its own (LwsWebSocketsManager.cpp:260-279) */
		void ResetAfterFork();
		/** Game thread: the service's counters (atomics, readable anywhere while the service lives); null before the first start */
		const FServiceCounters* GetCounters() const;
		/**
		 * The game thread or the service thread: FService::IsShuttingDown (an atomic); false before the first start. Service
		 * is a plain TSharedPtr that EnsureStarted assigns on the game thread before the service thread starts, and only
		 * ResetAfterFork changes it later, so no other thread may read it (Task 8 review M-4)
		 */
		bool IsShuttingDown() const;

    private:
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
        // Sole I3-C Host-control caller; actual H/O/service-owner closure remains required.
        // No ready/capability Boolean or default-selection switch is accepted.
        [[gnu::visibility("hidden")]] VrStatus EnsureNativeStartedOnCurrentControl();
#if defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
        VrStatus NativeHostStatus1(const FService&, VrStatus) const;
        bool NativeHostFinished1(const FService&) const;
        bool NativeHostStartCommitted1(const FService&) const;
        bool NativeHostAdvanceAllowed1(const FService&) const;
        FVaRestNativeOwnerHostStep1 NativeHostFinishResult1(const FService&, VrStatus) const;
#endif
#endif
        FServiceOptions Options;
		TSharedPtr<FService, ESPMode::ThreadSafe> Service;
		bool bShutDown = false;
		uint64 NextId = 1;
	};

	/**
	 * Where the process's transport lives (plan decision 8): made at its first use, and shut down by the engine-exit hook
	 * whether or not anything used it. GetDefaultTransport reads the process's one; a test makes its own, so it never touches
	 * the transport other tests share (Task 7 review M-3)
	 */
	class FDefaultTransportSlot
	{
	public:
		explicit FDefaultTransportSlot(FServiceOptions InOptions);
		FDefaultTransportSlot(const FDefaultTransportSlot&) = delete;
		FDefaultTransportSlot& operator=(const FDefaultTransportSlot&) = delete;

		/** Any thread: the transport, made at the first call (constructing one makes no context and no thread) */
		TSharedRef<FTransport, ESPMode::ThreadSafe> Get();
		/**
		 * Game thread: the engine-exit hook's work. The transport is made first when nothing used it yet, so a first use
		 * after exit ends with Shutdown instead of starting a context and a thread during exit (Task 7 review M-3)
		 */
		void ShutdownForExit();

	private:
		const FServiceOptions Options;
		FCriticalSection Lock;
		TSharedPtr<FTransport, ESPMode::ThreadSafe> Made;
	};

	/** Game thread: the process's transport (plan decision 8) */
	FTransport& GetDefault();
	/** Game thread, StartupModule: plan decision 8's start in a shared LWS copy */
	void OnModuleStartup();
	/** Game thread: OnEnginePreExit and ShutdownModule (FDefaultTransportSlot::ShutdownForExit on the process's slot) */
	void ShutdownDefault();
	/** Game thread: OnChildEndFramePostFork */
	void ResetDefaultAfterFork();
}
