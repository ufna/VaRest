// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "VaRestInspectorExport.h"
#if WITH_DEV_AUTOMATION_TESTS
#include <atomic>
#endif

class IVaRestInspector;
namespace VaRest::Inspector::Export
{
	void StartOnGT();
	void StopOnGT();
	FVaRestInspectorExportTask BeginOnGT(IVaRestInspector&, FString Path, const FVaRestInspectorExportOptions&);
#if WITH_DEV_AUTOMATION_TESTS
	// Owned native state only. Cleanup must release before joining export tasks.
	struct FWorkerGate
	{
		std::atomic<int32> Entered{0};
		std::atomic<bool> Released{false};
	};
	void SetWorkerGateOnGT(TSharedPtr<FWorkerGate, ESPMode::ThreadSafe>);
#endif
}
