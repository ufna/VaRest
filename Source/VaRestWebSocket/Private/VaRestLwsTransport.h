// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestLwsService.h"
#include "VaRestSocketTransport.h"

namespace VaRest::WebSocket::Lws
{
	/**
	 * IVaRestSocketTransport on one FService: VaRest's own client on the engine-bundled libwebsockets (spec 2, ruling R1).
	 * Game thread, GetCounters' pointee excepted (atomics, any thread)
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
		/** Game thread: every connection ends (FService::Shutdown), the thread stops, the context goes; later connects end with Shutdown */
		void Shutdown();
		/** Game thread: a forking server's child starts again, with a context and a thread of its own (LwsWebSocketsManager.cpp:260-279) */
		void ResetAfterFork();
		/** Game thread: the service's counters (atomics, readable anywhere while the service lives); null before the first start */
		const FServiceCounters* GetCounters() const;

	private:
		FServiceOptions Options;
		TSharedPtr<FService, ESPMode::ThreadSafe> Service;
		bool bShutDown = false;
		uint64 NextId = 1;
	};

	/** Game thread: the process's transport (plan decision 8) */
	FTransport& GetDefault();
	/** Game thread, StartupModule: plan decision 8's start in a shared LWS copy */
	void OnModuleStartup();
	/** Game thread: OnEnginePreExit and ShutdownModule; nothing when the default transport was never made */
	void ShutdownDefault();
	/** Game thread: OnChildEndFramePostFork */
	void ResetDefaultAfterFork();
}
