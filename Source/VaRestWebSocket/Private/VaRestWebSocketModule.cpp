// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/CoreDelegates.h"
#include "Misc/Fork.h"
#include "Modules/ModuleManager.h"
#include "VaRestLwsTransport.h"
#include "VaRestWebSocketLog.h"

DEFINE_LOG_CATEGORY(LogVaRestSocket);
LLM_DEFINE_TAG(VaRest_WebSocket);

/**
 * The startup policy (plan decision 8) and the engine-exit and fork hooks. LoadingPhase Default (spec 1.1, VaRest.uplugin):
 * the startup hook needs only that the engine's WebSockets, SSL and HTTP modules can be loaded, which holds from PreDefault
 * on: LoadStartupModules runs every phase (LaunchEngineLoop.cpp:4669-4695) after LoadStartupCoreModules (:3785, :3896). See the
 * Task 7 report (Task 1 review M-1)
 */
class FVaRestWebSocketModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// Before any module shuts down (CoreDelegates.h:261-262); an automation run's Quit force-exits, so there neither this
		// nor ShutdownModule runs (CLAUDE.md, VaRestHttpModule.cpp:24-26)
		PreExitHandle = FCoreDelegates::OnEnginePreExit.AddStatic(&VaRest::WebSocket::Lws::ShutdownDefault);
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
		VaRest::WebSocket::Lws::ShutdownDefault();
	}

private:
	FDelegateHandle PreExitHandle;
	FDelegateHandle ForkHandle;
};

IMPLEMENT_MODULE(FVaRestWebSocketModule, VaRestWebSocket);
