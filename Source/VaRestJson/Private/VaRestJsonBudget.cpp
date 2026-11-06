// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonBudget.h"

namespace VaRest::Json
{
FJsonBudget::FJsonBudget(FJsonBudgetLimits InLimits, bool bTrackUnlimitedUsage)
	: Limits(InLimits)
{
	bEnabled = bTrackUnlimitedUsage || Limits.OutputBytes != 0 || Limits.Values != 0 || Limits.WorkUnits != 0;
	if (Limits.OutputBytes < 0 || Limits.Values < 0 || Limits.WorkUnits < 0)
		Refuse(EJsonBudgetFailure::InvalidLimits);
}

bool FJsonBudget::Refuse(EJsonBudgetFailure Reason)
{
	if (Failure == EJsonBudgetFailure::None)
		Failure = Reason;
	return false;
}

bool FJsonBudget::Charge(int64 OutputBytes, int64 Values, int64 WorkUnits)
{
	if (HasFailed())
		return false;
	if (OutputBytes < 0 || Values < 0 || WorkUnits < 0)
		return Refuse(EJsonBudgetFailure::InvalidCharge);
	if (!bEnabled)
		return true;
	if (OutputBytes > MAX_int64 - Usage.OutputBytes || Values > MAX_int64 - Usage.Values || WorkUnits > MAX_int64 - Usage.WorkUnits)
		return Refuse(EJsonBudgetFailure::CountOverflow);
	if (Limits.OutputBytes > 0 && OutputBytes > Limits.OutputBytes - Usage.OutputBytes)
		return Refuse(EJsonBudgetFailure::OutputBytes);
	if (Limits.Values > 0 && Values > Limits.Values - Usage.Values)
		return Refuse(EJsonBudgetFailure::Values);
	if (Limits.WorkUnits > 0 && WorkUnits > Limits.WorkUnits - Usage.WorkUnits)
		return Refuse(EJsonBudgetFailure::WorkUnits);
	Usage.OutputBytes += OutputBytes;
	Usage.Values += Values;
	Usage.WorkUnits += WorkUnits;
	return true;
}

bool FJsonBudget::ChargeProduct(int64 Count, int64 ElementBytes, int64 Values, int64 WorkUnits)
{
	if (HasFailed())
		return false;
	if (Count < 0 || ElementBytes < 0)
		return Refuse(EJsonBudgetFailure::InvalidCharge);
	if (Count != 0 && ElementBytes > MAX_int64 / Count)
		return Refuse(EJsonBudgetFailure::CountOverflow);
	return Charge(Count * ElementBytes, Values, WorkUnits);
}

const TCHAR* FJsonBudget::GetFailureMessage() const
{
	switch (Failure)
	{
	case EJsonBudgetFailure::None: return TEXT("");
	case EJsonBudgetFailure::InvalidLimits: return TEXT("JSON logical limits must be nonnegative");
	case EJsonBudgetFailure::InvalidCharge: return TEXT("JSON logical charge cannot be negative");
	case EJsonBudgetFailure::CountOverflow: return TEXT("JSON logical accounting overflow");
	case EJsonBudgetFailure::OutputBytes: return TEXT("JSON requested/logical output byte limit exceeded");
	case EJsonBudgetFailure::Values: return TEXT("JSON value/key visit limit exceeded");
	case EJsonBudgetFailure::WorkUnits: return TEXT("JSON logical work limit exceeded");
	}
	return TEXT("JSON logical budget failure");
}
} // namespace VaRest::Json
