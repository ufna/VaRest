// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestInspectorPhases.h"

#include "Math/UnrealMathUtility.h"

#if VAREST_WITH_INSPECTOR
namespace VaRest::Inspector::Private
{
FVaRestInspectorPhase Between(double Begin, double End)
{
	FVaRestInspectorPhase Result;
	if (FMath::IsFinite(Begin) && FMath::IsFinite(End) && Begin >= 0.0 && End >= Begin)
	{
		Result.Seconds = End - Begin;
		Result.bKnown = true;
	}
	return Result;
}

void DeriveDispatchPhases(FVaRestInspectorDispatchView& Dispatch)
{
	Dispatch.Auth = {};
	Dispatch.Queued = {};
	Dispatch.Prepare = {};
	Dispatch.Send = {};
	Dispatch.Wait = {};
	Dispatch.Receive = {};
	Dispatch.Wall = {};
	Dispatch.Gap = {};
	Dispatch.bCombinedSendWait = false;
	if (Dispatch.bHasDispatched && Dispatch.Dispatched.Fixed)
	{
		const auto& Scalars = Dispatch.Dispatched.Fixed->Scalars;
		Dispatch.Auth = Between(0, Scalars.AuthWait);
		Dispatch.Queued = Between(0, Scalars.LimiterWait);
		Dispatch.Gap = Between(0, Scalars.GapSeconds);
		const auto BeforeTransport = Between(Dispatch.AttemptStart, Dispatch.DispatchAt);
		if (BeforeTransport.bKnown && Dispatch.Auth.bKnown && Dispatch.Queued.bKnown)
			Dispatch.Prepare = Between(0, BeforeTransport.Seconds - Dispatch.Auth.Seconds - Dispatch.Queued.Seconds);
	}
	if (!Dispatch.bHasEnded)
		return;
	Dispatch.Wait = Between(Dispatch.UploadDoneAt, Dispatch.StatusAt);
	Dispatch.Receive = Between(Dispatch.StatusAt, Dispatch.TransportDoneAt);
	if (!Dispatch.bHasDispatched)
		return;
	Dispatch.Wall = Between(Dispatch.AttemptStart, Dispatch.TransportDoneAt);
	Dispatch.Send = Between(Dispatch.DispatchAt, Dispatch.UploadDoneAt);
	if ((!FMath::IsFinite(Dispatch.UploadDoneAt) || Dispatch.UploadDoneAt < 0) &&
		FMath::IsFinite(Dispatch.StatusAt) && Dispatch.StatusAt >= 0)
	{
		Dispatch.Wait = Between(Dispatch.DispatchAt, Dispatch.StatusAt);
		Dispatch.bCombinedSendWait = Dispatch.Wait.bKnown;
	}
}

void DeriveCompletionPhases(FVaRestInspectorRecordView& Record)
{
	Record.Total = {};
	Record.Decode = {};
	Record.Deliver = {};
	if (!Record.bTerminalKnown)
		return;
	if (Record.bStartedAtKnown)
		Record.Total = Between(Record.StartedAt, Record.CompletedAt);
	Record.Decode = Between(Record.DecodeStartAt, Record.DecodeEndAt);
	Record.Deliver = Between(Record.DecodeEndAt, Record.CompletedAt);
}
} // namespace VaRest::Inspector::Private
#endif
