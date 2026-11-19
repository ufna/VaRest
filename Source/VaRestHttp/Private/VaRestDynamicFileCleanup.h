// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Tasks/Task.h"
#include <atomic>
namespace VaRest::Http
{
class FDynamicCompletion;
namespace DynamicRequestPrivate { struct FNativeDynamicWorker; }
#if WITH_DEV_AUTOMATION_TESTS
struct FDynamicFileCleanupObservation
{
	std::atomic<bool> Ran{false},Ended{false},DataDestroyed{false},OnGT{true};
	std::atomic<uint32> Thread{0};
};
#endif
/** Native only. Does not own the release-token or any GTK/affine callable. */
struct FDynamicFileCleanupData final
{
	FDynamicFileCleanupData(FString InPath,uint64 InRequest,uint32 InSerial,TSharedRef<FDynamicCompletion,ESPMode::ThreadSafe> InCompletion);
	~FDynamicFileCleanupData();
	FDynamicFileCleanupData(const FDynamicFileCleanupData&)=delete;
	FDynamicFileCleanupData& operator=(const FDynamicFileCleanupData&)=delete;
#if WITH_DEV_AUTOMATION_TESTS
	// Declared FIRST, therefore marks destruction AFTER all native data/ticket members ended.
	struct FFinalObservation
	{
		TSharedRef<FDynamicFileCleanupObservation,ESPMode::ThreadSafe> State=MakeShared<FDynamicFileCleanupObservation,ESPMode::ThreadSafe>();
		~FFinalObservation() { State->DataDestroyed.store(true,std::memory_order_release); }
	} FinalObservation;
#endif
	const FString Path;
	const uint64 Request;
	const uint32 Serial;
	TSharedRef<FDynamicCompletion,ESPMode::ThreadSafe> Completion;
	UE::Tasks::FTaskEvent Released;
	TSharedPtr<DynamicRequestPrivate::FNativeDynamicWorker,ESPMode::ThreadSafe> Worker;
};
/** Last actual file-use capture only signals native work selected before physical Start. */
class FDynamicFileUse final
{
public:
	explicit FDynamicFileUse(TSharedRef<FDynamicFileCleanupData,ESPMode::ThreadSafe> InData):Data(MoveTemp(InData)) {}
	~FDynamicFileUse();
	FDynamicFileUse(const FDynamicFileUse&)=delete;
	FDynamicFileUse& operator=(const FDynamicFileUse&)=delete;
	const TSharedRef<FDynamicFileCleanupData,ESPMode::ThreadSafe> Data;
};
}
