// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "Modules/ModuleInterface.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include "VaRestInspectorRecords.h"
#include "VaRestInspectorExport.h"

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
	// GT controls are deferred during observers/visitors; initial-off first use is lazy and checked.
	virtual void SetCapture(bool bEnabled) = 0;
	virtual bool IsCapturing() const = 0;
	virtual void Clear() = 0;
	/** Admission can fail during nested observers when the bounded scope-control queue is full. */
	virtual bool ClearScope(uint64 ScopeId) = 0;
	/** Editor PreBeginPIE: retire closed PIE scopes only; process/editor/open scopes survive. */
	virtual void BeginPIESession(bool bPreserveLog) = 0;
	virtual uint64 GetRevision() const = 0;
	/** GTK scalar diagnostics; does not allocate a capture snapshot. */
	virtual FVaRestInspectorDiagnostics GetDiagnostics() const = 0;
	// Stable delegate identity, including before the first capture service exists.
	virtual FVaRestInspectorChanged& OnChanged() = 0;
	virtual bool TrySnapshot(
		TSharedPtr<const FVaRestInspectorSnapshot, ESPMode::ThreadSafe>& Out) const = 0;
	virtual bool ForEachRecord(
		TFunctionRef<void(const FVaRestInspectorRecordView&)> Visitor) const = 0;
	/** GTK-only borrowed links, read separately from record visitors. */
	virtual bool ForEachLink(TFunctionRef<void(const FVaRestInspectorLinkView&)> Visitor) const = 0;
	/** Copies a bounded DTO on GT; only native data and the destination path reach the file worker. */
	virtual FVaRestInspectorExportTask ExportHar(FString Path, const FVaRestInspectorExportOptions& Options = {}) = 0;
};
