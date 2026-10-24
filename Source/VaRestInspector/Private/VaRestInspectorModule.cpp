// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestInspector.h"

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

FSupportedAllocator GetSupportedAllocator()
{
	check(IsInGameThread());
	return !bShuttingDown && bBootstrapInspected ? BootstrapSupport : FSupportedAllocator();
}
} // namespace VaRest::Inspector::Private

class FVaRestInspectorModule final : public IVaRestInspector
{
public:
	virtual void StartupModule() override
	{
		check(IsInGameThread());
		VaRest::Inspector::Private::BootstrapSupport = VaRest::Inspector::Private::InspectAllocator();
		VaRest::Inspector::Private::bBootstrapInspected = true;
		PreExitHandle = FCoreDelegates::OnPreExit.AddStatic(&VaRest::Inspector::Private::BeginShutdown);
	}
	virtual void ShutdownModule() override
	{
		check(IsInGameThread());
		VaRest::Inspector::Private::BeginShutdown();
		FCoreDelegates::OnPreExit.Remove(PreExitHandle);
		PreExitHandle.Reset();
		Changed.Clear();
	}
	virtual bool SupportsDynamicReloading() override { return false; }
	virtual void SetCapture(bool bEnabled) override { check(IsInGameThread()); }
	virtual bool IsCapturing() const override
	{
		check(IsInGameThread());
		return false;
	}
	virtual void Clear() override { check(IsInGameThread()); }
	virtual uint64 GetRevision() const override
	{
		check(IsInGameThread());
		return 0;
	}
	virtual FVaRestInspectorChanged& OnChanged() override
	{
		check(IsInGameThread());
		return Changed;
	}
	virtual bool TrySnapshot(TSharedPtr<const FVaRestInspectorSnapshot, ESPMode::ThreadSafe>& Out) const override
	{
		check(IsInGameThread());
		if (VaRest::Inspector::Private::bShuttingDown)
			return false;
#if VAREST_WITH_INSPECTOR
		(void)Out;
		return false;
#else
		Out = MakeShared<FVaRestInspectorSnapshot, ESPMode::ThreadSafe>();
		return true;
#endif
	}
	virtual bool ForEachRecord(TFunctionRef<void(const FVaRestInspectorRecordView&)>) const override
	{
		check(IsInGameThread());
		return !VaRest::Inspector::Private::bShuttingDown;
	}

private:
	FVaRestInspectorChanged Changed;
	FDelegateHandle PreExitHandle;
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
