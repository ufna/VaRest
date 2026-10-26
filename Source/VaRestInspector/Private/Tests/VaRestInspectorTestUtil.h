// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "Misc/AutomationTest.h"
#include "VaRestHttpTesting.h"
#include "VaRestInspectorService.h"

#if WITH_DEV_AUTOMATION_TESTS && VAREST_WITH_INSPECTOR
namespace VaRest::Inspector::Private::CaptureTesting
{
FVaRestFakeResponse Json(int32 Status, const char* Text = "{}");
// Called inside a visitor: no borrowed string, packet or row escapes this function.
void CheckCompleted(FAutomationTestBase&, const FVaRestInspectorRecordView&, const FVaRestTraceEvent&);
class FCaptureFixture
{
public:
 explicit FCaptureFixture(FAutomationTestBase&, bool bPersistence=false, bool bAutoStart=true, bool bRecord=true);
 ~FCaptureFixture();
 bool Ready() const { return Service.IsValid() && Clock.IsValid(); }
 FVaRestRequestHandle Track(FVaRestRequestHandle H) { Handles.Add(H); return H; }
 bool Start();
 void AddRecorder();
 void DrainCapture(); // Test body only; never called by a ticker, observer or GT task.
 bool Settle();
 bool Row(uint64, TFunctionRef<void(const FVaRestInspectorRecordView&)>);
 bool Check(uint64, bool bPartial=false);
 TSharedPtr<const FVaRestInspectorSnapshot, ESPMode::ThreadSafe> Snapshot();
 void Keep(TSharedPtr<const FVaRestInspectorSnapshot, ESPMode::ThreadSafe> S) { Retained.Add(MoveTemp(S)); }
 void ReleaseSnapshots() { Retained.Reset(); }
 // Barriers/stage holds/extra sinks release before cancellation, including early returns.
 void OnCleanup(TFunction<void()>&& F) { Releases.Add(MoveTemp(F)); }
 FVaRestTrace::FScopedSinkIsolation Isolation; // First constructed, last destroyed.
 FAutomationTestBase& Test;
 VaRest::Http::Testing::FTestRig Rig;
 TUniquePtr<VaRest::Http::Testing::FScopedPersistenceTest> Disk;
 TSharedPtr<VaRest::Http::Testing::FTraceRecorder, ESPMode::ThreadSafe> Recorder;
 FCapturePolicy Policy;
 FServiceFactoryWitness Witness;
 TSharedPtr<FService, ESPMode::ThreadSafe> Service;
 TSharedPtr<FDeterministicReceiptClock, ESPMode::ThreadSafe> Clock;
private:
 bool bRecorderAdded=false;
 TArray<FVaRestRequestHandle> Handles;
 TArray<TSharedPtr<const FVaRestInspectorSnapshot, ESPMode::ThreadSafe>> Retained;
 TArray<TFunction<void()>> Releases;
};
}
#endif
