// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestInspector.h"
#include "VaRestInspectorExportService.h"
#include "VaRestInspectorExportAction.h"
#include "VaRestInspectorConsole.h"

#include "CoreGlobals.h"
#include "GenericPlatform/GenericPlatformMemory.h"
#include "HAL/MallocDoubleFreeFinder.h"
#include "HAL/MallocFrameProfiler.h"
#include "HAL/MallocStomp2.h"
#include "HAL/MemoryBase.h"
#include "HAL/PlatformMemory.h"
#include "Misc/CString.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "VaRestInspectorPolicy.h"
#include "VaRestInspectorService.h"
#include "VaRestInspectorSettings.h"
#include "Misc/App.h"
#include "UObject/UObjectGlobals.h"

namespace VaRest::Inspector::Private
{
namespace
{
bool bShuttingDown = false;
bool bBootstrapInspected = false;
FSupportedAllocator BootstrapSupport;

FSupportedAllocator InspectAllocator()
{
	check(IsInGameThread());
	FSupportedAllocator Out;
#if defined(PLATFORM_LINUX) && PLATFORM_LINUX && defined(FORCE_ANSI_ALLOCATOR) && !FORCE_ANSI_ALLOCATOR && !defined(__clang_analyzer__) && defined(IS_MONOLITHIC) && !IS_MONOLITHIC && defined(UE_BUILD_DEVELOPMENT) && UE_BUILD_DEVELOPMENT && defined(WITH_EDITOR) && WITH_EDITOR
	if (sizeof(void*) != 8 || __STDCPP_DEFAULT_NEW_ALIGNMENT__ > 16)
		return Out;
	if (FParse::Param(FCommandLine::Get(), TEXT("mallocsavereplay")) ||
		FParse::Param(FCommandLine::Get(), TEXT("mallocframeprofiler")) ||
		FParse::Param(FCommandLine::Get(), TEXT("purgatorymallocproxy")) ||
		GMallocDoubleFreeFinder || GMallocDoubleFreeFinderEnabled ||
		GMallocFrameProfiler || GMallocFrameProfilerEnabled)
		return Out;
#if WITH_MALLOC_STOMP2
	if (GMallocStomp2 || GMallocStomp2Enabled)
		return Out;
#endif
	FMalloc* Allocator = UE::Private::GMalloc;
	if (!Allocator)
		return Out;
	const auto Selected = FPlatformMemory::AllocatorToUse;
	const TCHAR* Name = Allocator->GetDescriptiveName();
	const bool bBinned2 = Selected == FGenericPlatformMemory::Binned2 && FCString::Strcmp(Name, TEXT("Binned2")) == 0;
	const bool bMimalloc = Selected == FGenericPlatformMemory::Mimalloc && FCString::Strcmp(Name, TEXT("Mimalloc")) == 0;
	if (!bBinned2 && !bMimalloc)
		return Out;
	// PROVISIONAL facts, conditional on the controller's exact audited
	// source/build/Core-binary manifest and the no-arbitrary-replacement contract.
	// The compile constraints and live family are necessary, not sufficient:
	// neither a matching name nor these bits attest a binary or traverse a chain.
	// Before Task3 calibration/positive use, the controller MUST qualify the
	// produced Inspector compile/link/replacement-new/binary and this process's
	// actual argv/allocator/proxy chain against that SAME manifest. An unknown or
	// changed build/chain has no accepted capability. Public FMalloc cannot
	// detect arbitrary same-name runtime replacement (e.g. Memory.UsePurgatory).
	Out.Family = bBinned2 ? FSupportedAllocator::EFamily::Binned2 : FSupportedAllocator::EFamily::Mimalloc200;
	Out.bVerifiedModuleNew = true;
	Out.bVerifiedNoInflatingProxy = true;
	// Calibration belongs to Task3's charged, pre-reserved allocation path.
	// SupportsController is always false for this Task2 bootstrap value.
#endif
	return Out;
}

void BeginShutdown()
{
	check(IsInGameThread());
	bShuttingDown = true;
	BootstrapSupport = {};
	bBootstrapInspected = false;
}
} // namespace

FSupportedAllocator InspectCurrentAllocatorForFactory()
{ return !bShuttingDown ? InspectAllocator() : FSupportedAllocator(); }

FSupportedAllocator GetSupportedAllocator()
{
	check(IsInGameThread());
	return !bShuttingDown && bBootstrapInspected ? BootstrapSupport : FSupportedAllocator();
}
} // namespace VaRest::Inspector::Private

class FVaRestInspectorModule final : public IVaRestInspector
{
public:
 void StartupModule() override
 {
  check(IsInGameThread());
  using namespace VaRest::Inspector::Private;
  bShuttingDown=false;
  VaRest::Inspector::Export::StartOnGT();
  VaRest::Inspector::Console::StartOnGT();
  ExportPreExit=FCoreDelegates::OnPreExit.AddRaw(this,&FVaRestInspectorModule::StopExports);
  BootstrapSupport=InspectAllocator(); bBootstrapInspected=true; // Pure facts, always uncalibrated, including meaningful initial-off.
#if VAREST_WITH_INSPECTOR
  Policy=CurrentPolicy();
  const auto* Settings=GetDefault<UVaRestInspectorSettings>();
  const bool Enabled=FService::InitialCapturePolicy(GIsEditor,FApp::IsUnattended(),Settings->bCaptureInEditor,Settings->bCaptureInPackaged,FCommandLine::Get());
#if WITH_DEV_AUTOMATION_TESTS
  if (!Enabled)
  {
   FTypeLayoutRow Rows[64]; const uint32 Count=FService::DescribeProductionTypes(Rows,UE_ARRAY_COUNT(Rows));
   for (uint32 N=0;N<FMath::Min(Count,uint32(UE_ARRAY_COUNT(Rows)));++N)
    UE_LOG(LogTemp,Display,TEXT("VaRestInspectorType %s Size=%llu Align=%u Request=%llu"),Rows[N].Name,uint64(Rows[N].Size),Rows[N].Alignment,uint64(Rows[N].Request));
  }
#endif
  if (Enabled) SetCapture(true);
#endif
 }
 void ShutdownModule() override
 {
  check(IsInGameThread()); FCoreDelegates::OnPreExit.Remove(ExportPreExit); StopExports(); VaRest::Inspector::Private::BeginShutdown();
#if VAREST_WITH_INSPECTOR
  auto Keep=MoveTemp(Service);
  if (Keep) { Keep->DetachFacade(); Keep->Shutdown(); }
  Keep.Reset();
#endif
  Changed.Clear();
 }
 bool SupportsDynamicReloading() override { return false; }
 void SetCapture(bool Enabled) override
 {
  check(IsInGameThread()); if (VaRest::Inspector::Private::bShuttingDown) return;
#if VAREST_WITH_INSPECTOR
  Policy=CurrentPolicy();
  auto Keep=Service;
  if (Enabled && !Keep)
  {
   if (!VaRest::Inspector::Private::FService::TryCreate(Policy,Keep)) return;
   Keep->AttachFacade(Changed,&VaRest::Inspector::Private::bShuttingDown); Service=Keep;
  }
  if (Keep) { if (Enabled) Keep->Start(Policy); else { Keep->RequestPolicy(Policy); Keep->Pause(); } }
#endif
 }
 bool IsCapturing() const override
 {
  check(IsInGameThread());
#if VAREST_WITH_INSPECTOR
  return !VaRest::Inspector::Private::bShuttingDown && Service && Service->IsCapturing();
#else
  return false;
#endif
 }
 void Clear() override
 {
  check(IsInGameThread());
#if VAREST_WITH_INSPECTOR
  auto Keep=Service; if (Keep && !VaRest::Inspector::Private::bShuttingDown)
  { Policy=CurrentPolicy(); Keep->RequestPolicy(Policy); Keep->Clear(); }
#endif
 }
 bool ClearScope(uint64 Id) override
 {
  check(IsInGameThread());
#if VAREST_WITH_INSPECTOR
  auto Keep=Service; return !VaRest::Inspector::Private::bShuttingDown && (!Keep || Keep->ClearScope(Id));
#else
  return false;
#endif
 }
 void BeginPIESession(bool Preserve) override
 {
  check(IsInGameThread());
#if VAREST_WITH_INSPECTOR
  auto Keep=Service; if (Keep && !VaRest::Inspector::Private::bShuttingDown) Keep->BeginPIESession(Preserve);
#endif
 }
 uint64 GetRevision() const override
 {
  check(IsInGameThread());
#if VAREST_WITH_INSPECTOR
  auto Keep=Service; return Keep?Keep->Revision():0;
#else
  return 0;
#endif
 }
 FVaRestInspectorDiagnostics GetDiagnostics() const override
 {
  check(IsInGameThread());
#if VAREST_WITH_INSPECTOR
  auto Keep=Service; return Keep && !VaRest::Inspector::Private::bShuttingDown ? Keep->GetDiagnostics() : FVaRestInspectorDiagnostics();
#else
  return {};
#endif
 }
 FVaRestInspectorChanged& OnChanged() override { check(IsInGameThread()); return Changed; }
 bool TrySnapshot(TSharedPtr<const FVaRestInspectorSnapshot,ESPMode::ThreadSafe>& Out) const override
 {
  check(IsInGameThread()); if (VaRest::Inspector::Private::bShuttingDown) return false;
#if VAREST_WITH_INSPECTOR
  auto Keep=Service; return Keep && Keep->TrySnapshot(Out);
#else
  Out=MakeShared<FVaRestInspectorSnapshot,ESPMode::ThreadSafe>(); return true;
#endif
 }
 bool ForEachRecord(TFunctionRef<void(const FVaRestInspectorRecordView&)> Visitor) const override
 {
  check(IsInGameThread()); if (VaRest::Inspector::Private::bShuttingDown) return false;
#if VAREST_WITH_INSPECTOR
  auto Keep=Service; return !Keep || Keep->ForEachRecord(Visitor);
#else
  return true;
#endif
 }
 bool ForEachLink(TFunctionRef<void(const FVaRestInspectorLinkView&)> Visitor) const override
 {
  check(IsInGameThread()); if (VaRest::Inspector::Private::bShuttingDown) return false;
#if VAREST_WITH_INSPECTOR
  auto Keep=Service; return !Keep || Keep->ForEachLink(Visitor);
#else
  return true;
#endif
 }
 FVaRestInspectorExportTask ExportHar(FString Path,const FVaRestInspectorExportOptions& Options) override
 { return VaRest::Inspector::Export::BeginOnGT(*this,MoveTemp(Path),Options); }
private:
 void StopExports() { VaRest::Inspector::Console::StopOnGT(); UVaRestInspectorExportAction::ShutdownAllOnGT(); VaRest::Inspector::Export::StopOnGT(); }
 FDelegateHandle ExportPreExit;
 FVaRestInspectorChanged Changed;
#if VAREST_WITH_INSPECTOR
 static VaRest::Inspector::Private::FCapturePolicy CurrentPolicy()
 {
  const auto* Settings=GetDefault<UVaRestInspectorSettings>(); VaRest::Inspector::Private::FCapturePolicy P;
  P.Limits=VaRest::Inspector::Private::NormalizeLimits(GIsEditor?Settings->Editor:Settings->Packaged); P.bHeaders=Settings->bCaptureHeaders; P.bBodies=Settings->bCaptureBodies; return P;
 }
 VaRest::Inspector::Private::FCapturePolicy Policy;
 TSharedPtr<VaRest::Inspector::Private::FService,ESPMode::ThreadSafe> Service;
#endif
};

IVaRestInspector& IVaRestInspector::Get()
{
	check(IsInGameThread());
	check(!VaRest::Inspector::Private::bShuttingDown);
	return FModuleManager::LoadModuleChecked<IVaRestInspector>(TEXT("VaRestInspector"));
}
IVaRestInspector* IVaRestInspector::TryGet()
{
	check(IsInGameThread());
	return VaRest::Inspector::Private::bShuttingDown ? nullptr : FModuleManager::GetModulePtr<IVaRestInspector>(TEXT("VaRestInspector"));
}

IMPLEMENT_MODULE(FVaRestInspectorModule, VaRestInspector)
