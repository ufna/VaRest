// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace VaRest::Json
{
struct FJsonBudgetLimits
{
	int64 OutputBytes = 0;
	int64 Values = 0;
	int64 WorkUnits = 0;
};

struct FJsonBudgetUsage
{
	int64 OutputBytes = 0;
	int64 Values = 0;
	int64 WorkUnits = 0;
};

enum class EJsonBudgetFailure : uint8
{
	None,
	InvalidLimits,
	InvalidCharge,
	CountOverflow,
	OutputBytes,
	Values,
	WorkUnits
};

/**
 * One parse/conversion's monotonic logical account. Charge BEFORE the controlled allocation or traversal step.
 * A refused compound charge commits no dimension; failure is sticky for the rest of the operation.
 * Bytes are cumulative requested/logical bytes, not allocator slack, heap/RSS or a physical peak guarantee.
 * All-zero limits disable accounting for ordinary code-first calls. This is not a pending-operation reservation.
 */
class FJsonBudget
{
public:
	explicit FJsonBudget(FJsonBudgetLimits InLimits, bool bTrackUnlimitedUsage = false);
	bool Charge(int64 OutputBytes, int64 Values, int64 WorkUnits);
	bool ChargeProduct(int64 Count, int64 ElementBytes, int64 Values, int64 WorkUnits);

	bool IsEnabled() const { return bEnabled; }
	bool HasFailed() const { return Failure != EJsonBudgetFailure::None; }
	EJsonBudgetFailure GetFailure() const { return Failure; }
	const FJsonBudgetUsage& GetUsage() const { return Usage; }
	const TCHAR* GetFailureMessage() const;

private:
	bool Refuse(EJsonBudgetFailure Reason);
	FJsonBudgetLimits Limits;
	FJsonBudgetUsage Usage;
	EJsonBudgetFailure Failure = EJsonBudgetFailure::None;
	bool bEnabled = false;
};
} // namespace VaRest::Json
