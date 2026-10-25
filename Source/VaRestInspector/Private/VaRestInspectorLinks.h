// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Containers/StringView.h"
#include "VaRestInspectorPacketView.h"

#if VAREST_WITH_INSPECTOR
namespace VaRest::Inspector::Private
{
struct FLinkSeed;
// Allocation-free inference from an actual observation. No row or metadata
// lookup may manufacture a target for an unknown original request.
bool InferObservedLink(EVaRestTraceKind Kind, const View::FPacketScalars& Scalars,
	FStringView QueueId, FLinkSeed& Out);
} // namespace VaRest::Inspector::Private
#endif
