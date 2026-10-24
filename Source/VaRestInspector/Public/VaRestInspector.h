// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "Modules/ModuleInterface.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include "VaRestInspectorRecords.h"

#ifndef VAREST_WITH_INSPECTOR
#define VAREST_WITH_INSPECTOR (!UE_BUILD_SHIPPING)
#endif

class FVaRestInspectorSnapshot;
struct FVaRestInspectorRecordView;
DECLARE_MULTICAST_DELEGATE_OneParam(FVaRestInspectorChanged, uint64);

class VARESTINSPECTOR_API IVaRestInspector : public IModuleInterface
{
public:
	static IVaRestInspector& Get();
	static IVaRestInspector* TryGet();
	static constexpr bool IsCompiledIn() { return VAREST_WITH_INSPECTOR != 0; }
	virtual void SetCapture(bool bEnabled) = 0;
	virtual bool IsCapturing() const = 0;
	virtual void Clear() = 0;
	virtual uint64 GetRevision() const = 0;
	virtual FVaRestInspectorChanged& OnChanged() = 0;
	virtual bool TrySnapshot(
		TSharedPtr<const FVaRestInspectorSnapshot, ESPMode::ThreadSafe>& Out) const = 0;
	virtual bool ForEachRecord(
		TFunctionRef<void(const FVaRestInspectorRecordView&)> Visitor) const = 0;
};
