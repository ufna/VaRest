// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectGlobals.h"
#include "VaRestInspector.h"
#include "VaRestInspectorPolicy.h"
#include "VaRestInspectorSettings.h"
#include "VaRestTrace.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorModuleTest, "VaRest.Inspector.Module",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestInspectorModuleTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Inspector is loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("VaRestInspector")));
	IVaRestInspector* Inspector = IVaRestInspector::TryGet();
	if (!TestNotNull(TEXT("TryGet finds the loaded Inspector"), Inspector))
		return false;
	TestTrue(TEXT("Get returns the same module"), &IVaRestInspector::Get() == Inspector);
	TestFalse(TEXT("dynamic reloading is disabled"), Inspector->SupportsDynamicReloading());
	TestEqual(TEXT("compiled-in flag matches the macro"), IVaRestInspector::IsCompiledIn(), VAREST_WITH_INSPECTOR != 0);

	const UVaRestInspectorSettings* Settings = GetDefault<UVaRestInspectorSettings>();
	if (!TestNotNull(TEXT("settings exist"), Settings))
		return false;
	TestEqual(TEXT("settings category"), Settings->GetCategoryName(), FName(TEXT("Plugins")));
	TestEqual(TEXT("settings section"), Settings->GetSectionName(), FName(TEXT("VaRestInspector")));
	TestTrue(TEXT("editor capture default"), Settings->bCaptureInEditor);
	TestFalse(TEXT("packaged capture default"), Settings->bCaptureInPackaged);
	TestTrue(TEXT("headers default"), Settings->bCaptureHeaders);
	TestTrue(TEXT("bodies default"), Settings->bCaptureBodies);
	TestTrue(TEXT("engine IO frames hidden by default (future I3/I4)"), Settings->bHideEngineIoFrames);
	TestEqual(TEXT("editor budget"), Settings->Editor.BudgetMiB, 64);
	TestEqual(TEXT("editor records"), Settings->Editor.MaxRecords, 10000);
	TestEqual(TEXT("editor body cap"), Settings->Editor.MaxBodyBytes, 65536);
	TestEqual(TEXT("editor binary cap"), Settings->Editor.MaxBinaryExcerptBytes, 0);
	TestEqual(TEXT("editor frame cap"), Settings->Editor.MaxFrameExcerptBytes, 4096);
	TestEqual(TEXT("editor frames"), Settings->Editor.MaxFramesPerSocket, 5000);
	TestEqual(TEXT("editor pending events"), Settings->Editor.MaxPendingEvents, 65536);
	TestEqual(TEXT("editor pending MiB"), Settings->Editor.MaxPendingMiB, 16);
	TestEqual(TEXT("packaged budget"), Settings->Packaged.BudgetMiB, 8);
	TestEqual(TEXT("packaged MaxRecords"), Settings->Packaged.MaxRecords, 2000);
	TestEqual(TEXT("packaged body cap"), Settings->Packaged.MaxBodyBytes, 16384);
	TestEqual(TEXT("packaged binary cap"), Settings->Packaged.MaxBinaryExcerptBytes, 0);
	TestEqual(TEXT("packaged frame cap"), Settings->Packaged.MaxFrameExcerptBytes, 1024);
	TestEqual(TEXT("packaged frames"), Settings->Packaged.MaxFramesPerSocket, 1000);
	TestEqual(TEXT("packaged pending events"), Settings->Packaged.MaxPendingEvents, 16384);
	TestEqual(TEXT("packaged pending MiB"), Settings->Packaged.MaxPendingMiB, 4);

	const auto Limits = VaRest::Inspector::Private::NormalizeLimits(Settings->Packaged);
	TestEqual(TEXT("normalized packaged byte budget"), Limits.StoreBytes, 8ll * 1024 * 1024);
	TestEqual(TEXT("normalization preserves records"), Limits.MaxRecords, Settings->Packaged.MaxRecords);
	FVaRestInspectorLimits Invalid;
	Invalid.BudgetMiB = MIN_int32;
	Invalid.MaxRecords = MAX_int32;
	Invalid.MaxBodyBytes = -1;
	Invalid.MaxBinaryExcerptBytes = MAX_int32;
	Invalid.MaxFrameExcerptBytes = MAX_int32;
	Invalid.MaxFramesPerSocket = 0;
	Invalid.MaxPendingEvents = 0;
	Invalid.MaxPendingMiB = MAX_int32;
	const auto Bounded = VaRest::Inspector::Private::NormalizeLimits(Invalid);
	TestEqual(TEXT("budget minimum"), Bounded.StoreBytes, 1024ll * 1024);
	TestEqual(TEXT("record ceiling"), Bounded.MaxRecords, 65536);
	TestEqual(TEXT("negative body cap"), Bounded.BodyBytes, 0);
	TestEqual(TEXT("binary ceiling"), Bounded.BinaryBytes, 16 * 1024 * 1024);
	TestEqual(TEXT("frame ceiling"), Bounded.FrameBytes, 16 * 1024 * 1024);
	TestEqual(TEXT("frame count minimum"), Bounded.MaxFramesPerSocket, 1);
	TestEqual(TEXT("pending count minimum"), Bounded.PendingEvents, 1);
	TestEqual(TEXT("pending byte ceiling"), Bounded.PendingBytes, 256ll * 1024 * 1024);
	TestEqual(TEXT("normalization leaves input unchanged"), Invalid.BudgetMiB, MIN_int32);
	Invalid.MaxPendingMiB = 0;
	TestEqual(TEXT("pending byte minimum"), VaRest::Inspector::Private::NormalizeLimits(Invalid).PendingBytes, 64ll * 1024);
	VaRest::Inspector::Private::FCapturePolicy Policy;
	Policy.Limits = Limits;
	const auto Detail = Policy.Detail();
	TestTrue(TEXT("metadata requested"), EnumHasAnyFlags(Detail.Flags, EVaRestTraceDetail::Metadata));
	TestEqual(TEXT("I1 requests no socket frames"), Detail.FrameCap, 0);
	const auto Support = VaRest::Inspector::Private::GetSupportedAllocator();
	TestFalse(TEXT("Task2 has no allocator calibration"), Support.bCalibrationPassed);
	TestFalse(TEXT("uncalibrated capability refuses controllers"), Support.SupportsController(64, 8));

	const auto Before = FVaRestTrace::GetSinks();
	const int32 SinkCount = Before ? Before->Num() : 0;
	TestFalse(TEXT("empty shell is inactive"), Inspector->IsCapturing());
	Inspector->SetCapture(true);
	Inspector->Clear();
	Inspector->SetCapture(false);
	TestFalse(TEXT("empty controls remain inactive"), Inspector->IsCapturing());
	TestEqual(TEXT("empty facade revision"), Inspector->GetRevision(), uint64(0));
	TestFalse(TEXT("empty changed delegate"), Inspector->OnChanged().IsBound());
	int32 Records = 0;
	TestTrue(TEXT("empty facade enumeration succeeds"), Inspector->ForEachRecord([&Records](const FVaRestInspectorRecordView&) { ++Records; }));
	TestEqual(TEXT("no facade records"), Records, 0);
	TSharedPtr<const FVaRestInspectorSnapshot, ESPMode::ThreadSafe> Snapshot;
#if VAREST_WITH_INSPECTOR
	Snapshot = MakeShared<FVaRestInspectorSnapshot, ESPMode::ThreadSafe>();
	const auto* Sentinel = Snapshot.Get();
	TestFalse(TEXT("compiled-in inert facade refuses snapshot"), Inspector->TrySnapshot(Snapshot));
	TestTrue(TEXT("inert refusal preserves nonnull output"), Snapshot.Get() == Sentinel);
	Snapshot.Reset();
	TestFalse(TEXT("inert refusal also preserves null output"), Inspector->TrySnapshot(Snapshot));
	TestNull(TEXT("null output remains null"), Snapshot.Get());
#else
	if (!TestTrue(TEXT("empty snapshot succeeds"), Inspector->TrySnapshot(Snapshot)) ||
		!TestTrue(TEXT("snapshot exists"), Snapshot.IsValid()))
		return false;
	TestEqual(TEXT("snapshot revision"), Snapshot->GetRevision(), uint64(0));
	Snapshot->ForEachRecord([&Records](const FVaRestInspectorRecordView&) { ++Records; });
	int32 Stats = 0;
	Snapshot->ForEachStats([&Stats](const FVaRestInspectorHttpStats&) { ++Stats; });
	TestEqual(TEXT("no snapshot records"), Records, 0);
	TestEqual(TEXT("no snapshot stats"), Stats, 0);
	TestEqual(TEXT("no links"), Snapshot->GetLinks().Num(), 0);
	const auto& Diagnostics = Snapshot->GetDiagnostics();
	TestTrue(TEXT("snapshot diagnostics are stable"), &Diagnostics == &Snapshot->GetDiagnostics());
	TestEqual(TEXT("no dropped events"), Diagnostics.DroppedEvents, uint64(0));
	TestEqual(TEXT("no dropped bytes"), Diagnostics.DroppedBytes, uint64(0));
	TestEqual(TEXT("no refused rows"), Diagnostics.RefusedRows, uint64(0));
	TestEqual(TEXT("no omitted metadata"), Diagnostics.MetadataOmitted, uint64(0));
	TestEqual(TEXT("no allocator refusals before capture"), Diagnostics.UnsupportedAllocator, uint64(0));
	TestEqual(TEXT("no interrupted rows"), Diagnostics.InterruptedRows, uint64(0));
	TestEqual(TEXT("no store allocation"), Diagnostics.StoreBytes, int64(0));
	TestEqual(TEXT("no store peak"), Diagnostics.StorePeakBytes, int64(0));
	TestEqual(TEXT("no pending peak"), Diagnostics.PendingPeakBytes, int64(0));
	TestEqual(TEXT("no effective limit before capture"), Diagnostics.EffectiveStoreLimit, int64(0));
	TestEqual(TEXT("no requested limit before capture"), Diagnostics.RequestedStoreLimit, int64(0));
	TestEqual(TEXT("no pending allocation"), Diagnostics.PendingBytes, int64(0));
	TestEqual(TEXT("no snapshot backing controllers"), Diagnostics.SnapshotControllers, 0);
	TestFalse(TEXT("diagnostics inactive"), Diagnostics.bCapturing);
	TestFalse(TEXT("no pending limits"), Diagnostics.bPendingLimits);
#endif
	const auto After = FVaRestTrace::GetSinks();
	TestEqual(TEXT("controls and snapshot do not register a sink"), After ? After->Num() : 0, SinkCount);
	TestNull(TEXT("capture console object is absent"), IConsoleManager::Get().FindConsoleObject(TEXT("VaRest.Inspector.Capture")));
	return true;
}

#endif
