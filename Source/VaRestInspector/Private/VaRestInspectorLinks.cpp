// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestInspectorLinks.h"
#include "VaRestInspectorStore.h"

#if VAREST_WITH_INSPECTOR
namespace VaRest::Inspector::Private
{
bool InferObservedLink(EVaRestTraceKind Kind, const View::FPacketScalars& Scalars,
	FStringView QueueId, FLinkSeed& Out)
{
	Out = {};
	if (Scalars.RequestId == 0)
		return false;
	if ((Kind == EVaRestTraceKind::AttemptDispatched || Kind == EVaRestTraceKind::Completed) && Scalars.AuthRefreshId != 0)
	{
		Out = {Scalars.RequestId, Scalars.AuthRefreshId, {}, true};
		return true;
	}
	if (Kind == EVaRestTraceKind::Started && (Scalars.OriginalRequestId != 0 || !QueueId.IsEmpty()))
	{
		Out = {Scalars.RequestId, Scalars.OriginalRequestId, QueueId, false};
		return true;
	}
	return false;
}
} // namespace VaRest::Inspector::Private
#endif
