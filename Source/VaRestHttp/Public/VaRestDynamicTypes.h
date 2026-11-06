// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UScriptStruct;

/**
 * Explicit native lifecycle contract for the new dynamic GT converter only. The registering module attests that
 * initialization/defaults, copy, destruction, hash/equality and all nonreflected dependencies are GT-safe and
 * cannot mutate reflection or pump editor work. This is a declaration requiring project qualification, not an
 * IsNative/zero-constructor inference. It never enables reflected worker conversion or changes Request<T>.
 */
struct VARESTHTTP_API FVaRestDynamicNativeTypes
{
	/** Explicit nonreflected struct/enum dependencies join the same qualified, pinned transitive closure. */
	static bool RegisterGameThreadType(const UScriptStruct* Type, FName Contract,
		TConstArrayView<const UObject*> NativeTypeDependencies = {});
	static void UnregisterGameThreadType(const UScriptStruct* Type);
};

namespace VaRest::Http::DynamicTypes
{
/** Runtime/editor boundary. Invalidate BEFORE destroying old reflection; settle only after the mutation ends. */
VARESTHTTP_API void InvalidateOnGT();
VARESTHTTP_API void ScheduleSettleOnGT();
/** Missing editor/transactor hooks disable reflected materialization. Raw JSON/text/bytes remain available. */
VARESTHTTP_API void SetEditorHooksAvailableOnGT(bool bAvailable, TFunction<bool()>&& CurrentBindings = {});
}
