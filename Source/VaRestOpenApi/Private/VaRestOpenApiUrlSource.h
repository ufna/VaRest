// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "VaRestOpenApiSource.h"
#include "VaRestClient.h"
#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopedSlowTask.h"
#include "Misc/CoreMisc.h"
#include <atomic>

namespace VaRest::OpenApi
{
/** IO requests contain only copied native strings/bytes. The native client/settings stay on GT. */
class FUrlSourceSession final : public TSharedFromThis<FUrlSourceSession,ESPMode::ThreadSafe>
{
public:
	bool Read(const FVaRestOpenApiReadRequest& Request,FVaRestOpenApiReadResult& Out);
	void CancelOnGT();
	bool IsCancelled() const { return Cancelled.load(std::memory_order_acquire); }
private:
	struct FFetch;
	struct FPending { FVaRestRequestHandle Handle;TSharedPtr<FFetch,ESPMode::ThreadSafe> Fetch; };
	void StartOnGT(FVaRestOpenApiReadRequest Request,TSharedRef<FFetch,ESPMode::ThreadSafe> Fetch);
	std::atomic<bool> Cancelled{false};
	TArray<FPending> Pending; // GT writes; only weak native handles and native promises, never a client shell or UObject
};
TSharedRef<FUrlSourceSession,ESPMode::ThreadSafe> NewUrlSourceSession();
FLocalSourceAnalysis AnalyzeUrlSource(const FString& Uri,const TSharedRef<FUrlSourceSession,ESPMode::ThreadSafe>& Session);
void CancelUrlSourcesOnGT();

template <typename T> T WaitUrlSource(TFuture<T>& Future,FScopedSlowTask& Progress,const TSharedRef<FUrlSourceSession,ESPMode::ThreadSafe>& Session,bool& Cancelled)
{
	check(IsInGameThread());
	while (!Future.IsReady())
	{
		// Only the source IO boundary marshals work to GT; this is not an engine-frame or global ticker simulation.
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		Progress.EnterProgressFrame(0);Cancelled=Cancelled || Progress.ShouldCancel() || IsEngineExitRequested();if (Cancelled && !Session->IsCancelled()) Session->CancelOnGT();
		FPlatformProcess::SleepNoStats(.005f);
	}
	Cancelled=Cancelled || Progress.ShouldCancel() || Session->IsCancelled();return Future.Get();
}
}
