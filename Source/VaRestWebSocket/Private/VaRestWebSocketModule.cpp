// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "Containers/Ticker.h"
#include "VaRestDnsNative.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Fork.h"
#include "Modules/ModuleManager.h"
#include "VaRestLwsTransport.h"
#include "VaRestNativeOwnerHost1.h"
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
#include "NativeOwner/VaRestNativeOwnerPump.h"
#endif
#include "VaRestSocketState.h"
#include "VaRestWebSocketLog.h"

DEFINE_LOG_CATEGORY(LogVaRestSocket);
LLM_DEFINE_TAG(VaRest_WebSocket);

/**
 * The startup policy (plan decision 8) and the engine-exit and fork hooks. LoadingPhase Default (spec 1.1, VaRest.uplugin):
 * the startup hook needs only that the engine's WebSockets, SSL and HTTP modules can be loaded, which holds from PreDefault
 * on: LoadStartupModules runs every phase (LaunchEngineLoop.cpp:4669-4695) after LoadStartupCoreModules (:3785, :3896). See the
 * Task 7 report (Task 1 review M-1)
 */
class FVaRestWebSocketModule final : public IVaRestNativeOwnerHost1
{
public:
    virtual FVaRestNativeOwnerHostStep1 StartNativeOwnerSession1() override
    {
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
        const VrStatus Role = VaRest::WebSocket::Lws::RequireNativeOwnerControlThread(TEXT("Host StartNativeOwnerSession1"));
        if (Role != VR_STATUS_OK) return {16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Refused),Role};
        if (!VaRest::WebSocket::Lws::NativeOwnerRealThreadModeAllowed())
            return {16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Refused),VR_STATUS_FORK_UNESTABLISHED};
        // BOTH real guards precede even the existing lazy default slot.
        return VaRest::WebSocket::Lws::GetDefault().StartNativeOwnerSession1();
#else
        return {16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Refused),7};
#endif
    }

    virtual FVaRestNativeOwnerHostStep1 FinishNativeOwnerSession1() override
    {
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
        const VrStatus Role = VaRest::WebSocket::Lws::RequireNativeOwnerControlThread(TEXT("Host FinishNativeOwnerSession1"));
        if (Role != VR_STATUS_OK) return {16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Refused),Role};
        if (!VaRest::WebSocket::Lws::NativeOwnerRealThreadModeAllowed())
            return {16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Refused),VR_STATUS_FORK_UNESTABLISHED};
        // BOTH real guards precede even the existing lazy default slot.
        return VaRest::WebSocket::Lws::GetDefault().FinishNativeOwnerSession1();
#else
        return {16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Refused),7};
#endif
    }

    virtual FVaRestNativeOwnerHostStep1 AdvanceNativeOwnerFinish1() override
    {
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
        const VrStatus Role = VaRest::WebSocket::Lws::RequireNativeOwnerControlThread(TEXT("Host AdvanceNativeOwnerFinish1"));
        if (Role != VR_STATUS_OK) return {16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Refused),Role};
        if (!VaRest::WebSocket::Lws::NativeOwnerRealThreadModeAllowed())
            return {16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Refused),VR_STATUS_FORK_UNESTABLISHED};
        // BOTH real guards precede even the existing lazy default slot.
        return VaRest::WebSocket::Lws::GetDefault().AdvanceNativeOwnerFinish1();
#else
        return {16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Refused),7};
#endif
    }

    virtual FVaRestNativeOwnerHostStep1 PumpNativeOwnerSession1() override
    {
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
        const VrStatus Role = VaRest::WebSocket::Lws::RequireNativeOwnerControlThread(TEXT("Host PumpNativeOwnerSession1"));
        if (Role != VR_STATUS_OK) return {16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Refused),Role};
        if (!VaRest::WebSocket::Lws::NativeOwnerRealThreadModeAllowed())
            return {16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Refused),VR_STATUS_FORK_UNESTABLISHED};
        // Actual pre-Start Host frame/retention commitment is an external Source-H
        // premise. Neither this call nor the default slot creates that authority.
        return VaRest::WebSocket::Lws::GetDefault().PumpNativeOwnerSession1();
#else
        return {16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Refused),7};
#endif
    }

	virtual void StartupModule() override
	{
#if VAREST_HAS_PRIVATE_DNS
		(void)VaRest::WebSocket::Dns::InitializeOnGT(VaRest::WebSocket::Lws::PrivateDnsThreadsAllowedOnGT());
		DnsTicker = FTSTicker::GetCoreTicker().AddTicker(TEXT("VaRestPrivateDnsRetirement"), 0.05f, [](float)
		{
			if (IsInGameThread())
			{
				// A child may become multithread-ready only after the fork callback's frame.
				if (FForkProcessHelper::IsForkRequested())
					(void)VaRest::WebSocket::Dns::InitializeOnGT(VaRest::WebSocket::Lws::PrivateDnsThreadsAllowedOnGT());
				(void)VaRest::WebSocket::Dns::PollRetirement();
			}
			return true;
		});
#endif

		// Before any module shuts down (CoreDelegates.h:261-262); an automation run's Quit force-exits, so there neither this
		// nor ShutdownModule runs (CLAUDE.md, VaRestHttpModule.cpp:24-26)
		VaRest::WebSocket::StartSocketsOnGT();
		PreExitHandle = FCoreDelegates::OnEnginePreExit.AddLambda([]
		{
			VaRest::WebSocket::StopSocketsOnGT();
			VaRest::WebSocket::Lws::ShutdownDefault();
#if VAREST_HAS_PRIVATE_DNS
			VaRest::WebSocket::Dns::StopForModule();
#endif
		});
		if (FForkProcessHelper::IsForkRequested())
		{
			// A forking server's child makes its own context and thread (LwsWebSocketsManager.cpp:260-279; plan decision 22)
			ForkHandle = FCoreDelegates::OnChildEndFramePostFork.AddStatic(&VaRest::WebSocket::Lws::ResetDefaultAfterFork);
		}
		VaRest::WebSocket::Lws::OnModuleStartup();
	}

	virtual void ShutdownModule() override
	{
		FCoreDelegates::OnChildEndFramePostFork.Remove(ForkHandle);
		FCoreDelegates::OnEnginePreExit.Remove(PreExitHandle);
		VaRest::WebSocket::StopSocketsOnGT();
		VaRest::WebSocket::Lws::ShutdownDefault();
#if VAREST_HAS_PRIVATE_DNS
		FTSTicker::RemoveTicker(DnsTicker);
		VaRest::WebSocket::Dns::StopForModule(); // Tryjoin only; unresolved POD/image reference deliberately remain process-owned.
#endif
	}
	virtual bool SupportsDynamicReloading() override { return false; }

private:
#if VAREST_HAS_PRIVATE_DNS
	FTSTicker::FDelegateHandle DnsTicker;
#endif
	FDelegateHandle PreExitHandle;
	FDelegateHandle ForkHandle;
};

IMPLEMENT_MODULE(FVaRestWebSocketModule, VaRestWebSocket);
