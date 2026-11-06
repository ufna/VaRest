// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestOfflineInternal.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "VaRestClientState.h"
#include "VaRestHttpLog.h"
#include "VaRestRedactionPipe.h"
#include "VaRestPersistence.h"
#include "VaRestDiskCache.h"
#include "VaRestTrace.h"
#include "VaRestTracePoints.h"
#include "VaRestHttpSettings.h"
#include "VaRestNativeLifetimeService.h"
#include "VaRestFrameDispatcher.h"
#include "VaRestDynamicBudget.h"
#include "VaRestDynamicClosure.h"

#if WITH_EDITOR
#include "GameDelegates.h"
#endif

DEFINE_LOG_CATEGORY(LogVaRestHttp);

class FVaRestHttpModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		VaRest::Http::StartRedactionPipe();
		VaRest::Http::StartPersistence();
		NativeLifetime = VaRest::Http::FNativeLifetimeService::CreateOnGT();
		DynamicFrames = VaRest::Http::FFrameDispatcher::CreateOnGT(GetDefault<UVaRestHttpSettings>()->DynamicGlobalMaxPending);
		DynamicTypes = VaRest::Http::FDynamicTypeRuntime::CreateOnGT();
		FString DynamicError;
		DynamicBudget=FVaRestDynamicBudget::Create(FVaRestDynamicLimits::FromSettingsOnGT(*GetDefault<UVaRestHttpSettings>()),DynamicError);
		// Ordering: OnEnginePreExit fires before any module shuts down (CoreDelegates.h:261-262; GEngine->PreExit() at
		// LaunchEngineLoop.cpp:5065 precedes UnloadModulesAtShutdown at :5182). Modules then shut down last-loaded first
		// (ModuleManager.cpp:1458), so the HTTP module, loaded lazily after this one, can be gone before our ShutdownModule
		// runs: too late to cancel its requests there. In a packaged game the broadcast comes after the worlds' EndPlay(Quit),
		// GameInstance::Shutdown and CleanupWorld (GameEngine.cpp:1373, :1377, :1380, then Super::PreExit() at :1396): a
		// request that completes during that teardown still gets its callbacks. An automation run's Quit force-exits
		// (AutomationCommandline.cpp:504; on Linux a forced RequestExit is _exit, UnixPlatformMisc.cpp:350), so neither
		// this hook nor ShutdownModule runs there.
		PreExitHandle = FCoreDelegates::OnEnginePreExit.AddLambda([this]
		{
			bDynamicStopping=true;
			if (NativeLifetime) NativeLifetime->BeginStoppingOnGT(); // close managed admission before any cancellation callout
			VaRest::Http::FClientState::SuppressForShutdown();
			if (DynamicTypes) DynamicTypes->StopOnGT(); // destroy every old-layout carrier/pin on GT before worker quarantine
			if (NativeLifetime) NativeLifetime->DetachQueuesForPreExitOnGT();
			VaRest::Http::FClientState::CancelForShutdown();
			if (DynamicFrames) DynamicFrames->StopOnGT();
			if (NativeLifetime) NativeLifetime->FlushQuiescentQueuesForPreExitOnGT();
            VaRest::Http::FOfflineQueue::StopAll();
			VaRest::Http::StopPersistence();
			VaRest::Http::FDiskCache::StopControls();
			if (NativeLifetime) NativeLifetime->StopOnGT();
		});
		// IN-12: a sink that attaches late learns the live clients and in-flight requests. OnBuildSnapshot and the live-client
		// registry it walks are leaked singletons (review I-4), so neither runs anything at static destruction.
		SnapshotHandle = FVaRestTrace::OnBuildSnapshot().AddStatic(&VaRest::Http::TracePoints::ContributeSnapshot);
#if WITH_EDITOR
		ModifyCookHandle = FGameDelegates::Get().GetModifyCookDelegate().AddLambda([](TConstArrayView<const ITargetPlatform*>, TArray<FName>& ToCook, TArray<FName>&)
		{
			for (const auto& Entry : GetDefault<UVaRestHttpSettings>()->Apis)
			{
				const FString Package = Entry.ToSoftObjectPath().GetLongPackageName();
				if (!Package.IsEmpty()) ToCook.AddUnique(FName(*Package));
			}
		});
#endif
	}

	virtual void ShutdownModule() override
	{
		bDynamicStopping=true;
		if (NativeLifetime) NativeLifetime->BeginStoppingOnGT();
		// No pre-exit assumption here: suppression is safe even when the engine HTTP module is already gone.
		VaRest::Http::FClientState::SuppressForShutdown();
		if (DynamicTypes) DynamicTypes->StopOnGT();
		if (DynamicFrames) DynamicFrames->StopOnGT();
		if (NativeLifetime) NativeLifetime->StopOnGT();
#if WITH_EDITOR
		FGameDelegates::Get().GetModifyCookDelegate().Remove(ModifyCookHandle);
#endif
		FVaRestTrace::OnBuildSnapshot().Remove(SnapshotHandle);
		FCoreDelegates::OnEnginePreExit.Remove(PreExitHandle);
		NativeLifetime.Reset();
		DynamicFrames.Reset();
		DynamicTypes.Reset();
		DynamicBudget.Reset(); // native-only accounts can outlive the module through already-admitted work
		VaRest::Http::StopPersistence();
		VaRest::Http::FDiskCache::StopControls();
		VaRest::Http::StopRedactionPipe();
	}

	virtual bool SupportsDynamicReloading() override { return false; }

	TSharedPtr<VaRest::Http::FNativeLifetimeControl, ESPMode::ThreadSafe> ControlForNewClientOnGT() const
	{
		check(IsInGameThread());
		return NativeLifetime ? NativeLifetime->ControlForNewClientOnGT() : nullptr;
	}
	TSharedPtr<VaRest::Http::FDynamicFrameTicket, ESPMode::ThreadSafe> ReserveDynamicFrameOnGT(TUniqueFunction<void()>& Callback) const
	{
		check(IsInGameThread());
		return DynamicFrames && !bDynamicStopping && !VaRest::Http::FClientState::IsShuttingDown() ? DynamicFrames->TryReserveOnGT(Callback) : nullptr;
	}
	TSharedPtr<FVaRestDynamicBudget,ESPMode::ThreadSafe> BudgetForNewRequestOnGT() const
	{
		check(IsInGameThread());
		return bDynamicStopping || VaRest::Http::FClientState::IsShuttingDown() ? nullptr : DynamicBudget;
	}

private:
	TUniquePtr<VaRest::Http::FNativeLifetimeService> NativeLifetime;
	TUniquePtr<VaRest::Http::FFrameDispatcher> DynamicFrames;
	TUniquePtr<VaRest::Http::FDynamicTypeRuntime> DynamicTypes;
	TSharedPtr<FVaRestDynamicBudget,ESPMode::ThreadSafe> DynamicBudget;
	bool bDynamicStopping=false;
	FDelegateHandle PreExitHandle;
	FDelegateHandle SnapshotHandle;
#if WITH_EDITOR
	FDelegateHandle ModifyCookHandle;
#endif
};

TSharedPtr<VaRest::Http::FNativeLifetimeControl, ESPMode::ThreadSafe> VaRest::Http::NativeControlForNewClientOnGT()
{
	check(IsInGameThread());
	const auto* Module = FModuleManager::GetModulePtr<FVaRestHttpModule>(TEXT("VaRestHttp"));
	return Module ? Module->ControlForNewClientOnGT() : nullptr;
}

IMPLEMENT_MODULE(FVaRestHttpModule, VaRestHttp);

TSharedPtr<VaRest::Http::FDynamicFrameTicket, ESPMode::ThreadSafe> VaRest::Http::ReserveDynamicFrameOnGT(TUniqueFunction<void()>& Callback)
{
	check(IsInGameThread());
	const auto* Module = FModuleManager::GetModulePtr<FVaRestHttpModule>(TEXT("VaRestHttp"));
	return Module ? Module->ReserveDynamicFrameOnGT(Callback) : nullptr;
}
TSharedPtr<FVaRestDynamicBudget,ESPMode::ThreadSafe> VaRest::Http::DefaultDynamicBudgetOnGT()
{
	check(IsInGameThread());
	const auto* Module=FModuleManager::GetModulePtr<FVaRestHttpModule>(TEXT("VaRestHttp"));
	return Module ? Module->BudgetForNewRequestOnGT() : nullptr;
}
