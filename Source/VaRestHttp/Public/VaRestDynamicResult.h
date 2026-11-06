// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestApiTypes.h"
#include "VaRestDynamicBudget.h"
#include "VaRestJson.h"
#include <atomic>

#if WITH_DEV_AUTOMATION_TESTS
struct FVaRestDynamicReleaseProbe;
#endif

namespace VaRest::Http
{
struct FDynamicResultData;
struct FDynamicResultAccess;
}

/** Plain validation metadata; no reflected descriptor/pin or address is published with a retained result. */
enum class EVaRestDynamicAvailability : uint8
{
	None,
	Available,
	Unavailable,
	TypeChanged,
	BudgetRefused
};

struct VARESTHTTP_API FVaRestDynamicTypeInfo
{
	EVaRestTypeKind Kind = EVaRestTypeKind::None;
	EVaRestTypeContainer Container = EVaRestTypeContainer::None;
	bool bJsonValue = false;
	FName Name;
	EVaRestDynamicAvailability Availability = EVaRestDynamicAvailability::None;
};

/**
 * Copyable immutable native result shell. No UObject/type/property pointer, reflected carrier, config/clock,
 * budget lease or GT release token is retained here. Copies/final destruction are valid on any thread.
 * JSON handles retain the existing shared-document contract: clone before independent/concurrent mutation.
 * Typed Blueprint transfer is a separate private synchronous GT preparation operation, never a shell getter.
 * Dynamic OnComplete may retain this plain metadata shell even on error; TVaRestResult::Error stays authoritative.
 */
class VARESTHTTP_API FVaRestDynamicResult
{
public:
	FVaRestDynamicResult() = default;
	FVaRestDynamicTypeInfo GetType() const;
	FVaRestDynamicTypeInfo GetErrorType() const;
	FVaRestJson GetJson() const;
	FVaRestJson GetErrorJson() const;
	/** Raw String mode. JSON-value String/Bytes mode exposes plain JSON through GetJson and private typed transfer. */
	FString GetText() const;
	/** Borrowed immutable view, valid while this shell (or another copy owning the same payload) is retained. */
	TConstArrayView<uint8> GetBytes() const;
	/** Whether this shell retains a JSON/text/byte payload; response diagnostics remain in TVaRestResult's Error/Response. */
	bool HasBody() const;
#if WITH_DEV_AUTOMATION_TESTS
	int32 GetNativeOwnerCountForTesting() const;
	bool AttachReleaseProbeForTesting(TSharedRef<FVaRestDynamicReleaseProbe,ESPMode::ThreadSafe> Probe) const;
#endif

private:
	friend struct VaRest::Http::FDynamicResultAccess;
	explicit FVaRestDynamicResult(TSharedRef<const VaRest::Http::FDynamicResultData, ESPMode::ThreadSafe> InData);
	TSharedPtr<const VaRest::Http::FDynamicResultData, ESPMode::ThreadSafe> Data;
};

#if WITH_DEV_AUTOMATION_TESTS
/** Native-only exact-payload destructor observation. No UObject, callback, clock or GTK service is retained. */
struct FVaRestDynamicReleaseProbe
{
	std::atomic<bool> bDestroyed{false},bDestroyedOnGameThread{false};
};
#endif

/**
 * GT activation inputs, consumed/validated before dynamic work admission. These reflected pointers NEVER enter
 * the published result shell or an immutable parser worker payload. Ordinary Request<T> remains unchanged.
 */
struct VARESTHTTP_API FVaRestDynamicResultConfig
{
	FVaRestTypeRef ResponseType = FVaRestTypeRef::Json();
	FVaRestTypeRef ErrorType;
	const UObject* RuleOwner = nullptr;
	TSharedPtr<FVaRestDynamicBudget, ESPMode::ThreadSafe> Budget;
	/** Internal already-parsed protocol layout; only a scalar ordinary struct can receive positional arguments. */
	bool bPositionalResponse = false;
};
