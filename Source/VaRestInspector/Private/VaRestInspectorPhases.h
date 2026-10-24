// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestInspector.h"
#include "VaRestInspectorRecords.h"

#if VAREST_WITH_INSPECTOR
namespace VaRest::Inspector::Private
{
FVaRestInspectorPhase Between(double Begin, double End);
void DeriveDispatchPhases(FVaRestInspectorDispatchView& Dispatch);
void DeriveCompletionPhases(FVaRestInspectorRecordView& Record);
} // namespace VaRest::Inspector::Private
#endif
