// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestFakeTransport.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/FileHelper.h"
#include "Tasks/Pipe.h"
#include "Tasks/Task.h"
#include "VaRestTrace.h"

class FVaRestFakeTransport::FCallHandle final : public IVaRestTransportCall
{
public:
	FCallHandle(TWeakPtr<FVaRestFakeTransport, ESPMode::ThreadSafe> InTransport, int32 InIndex)
		: Transport(MoveTemp(InTransport))
		, Index(InIndex)
	{
	}

	virtual void Cancel() override
	{
		if (TSharedPtr<FVaRestFakeTransport, ESPMode::ThreadSafe> Pinned = Transport.Pin())
		{
			Pinned->CancelCall(Index);
		}
	}

private:
	TWeakPtr<FVaRestFakeTransport, ESPMode::ThreadSafe> Transport;
	int32 Index;
};

FVaRestFakeResponse FVaRestFakeResponse::Json(int32 InStatus, FUtf8StringView InJson)
{
	FVaRestFakeResponse Out;
	Out.Status = InStatus;
	Out.Headers.Set(TEXT("Content-Type"), TEXT("application/json"));
	Out.Body.Append(reinterpret_cast<const uint8*>(InJson.GetData()), InJson.Len());
	return Out;
}

FVaRestFakeResponse FVaRestFakeResponse::Text(int32 InStatus, FStringView InText, FStringView InContentType)
{
	FVaRestFakeResponse Out;
	Out.Status = InStatus;
	if (!InContentType.IsEmpty())
	{
		Out.Headers.Set(TEXT("Content-Type"), InContentType);
	}
	FTCHARToUTF8 Utf8(InText.GetData(), InText.Len());
	Out.Body.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	return Out;
}

FVaRestFakeResponse FVaRestFakeResponse::Bytes(int32 InStatus, TArray<uint8> InBody, FStringView InContentType)
{
	FVaRestFakeResponse Out;
	Out.Status = InStatus;
	if (!InContentType.IsEmpty())
	{
		Out.Headers.Set(TEXT("Content-Type"), InContentType);
	}
	Out.Body = MoveTemp(InBody);
	return Out;
}

FVaRestFakeResponse FVaRestFakeResponse::Failed(EVaRestTransportFailure InFailure, FName InDetail)
{
	FVaRestFakeResponse Out;
	Out.Status = 0;
	Out.Failure = InFailure;
	Out.FailureDetail = InDetail;
	return Out;
}

FVaRestFakeResponse FVaRestFakeResponse::Manual()
{
	FVaRestFakeResponse Out;
	Out.Delay = -1.0;
	return Out;
}

FVaRestFakeTransport::FVaRestFakeTransport(TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> InClock)
	: Clock(MoveTemp(InClock))
{
}

void FVaRestFakeTransport::SetResponder(FResponder InResponder)
{
	Responder = MoveTemp(InResponder);
}

void FVaRestFakeTransport::Enqueue(FVaRestFakeResponse Response)
{
	Queue.Add(MoveTemp(Response));
}

int32 FVaRestFakeTransport::NumInFlight() const
{
	int32 Count = 0;
	for (const TSharedRef<FEntry>& Entry : Entries)
	{
		Count += Entry->Call.bCompleted ? 0 : 1;
	}
	return Count;
}

int32 FVaRestFakeTransport::NumCanceled() const
{
	int32 Count = 0;
	for (const TSharedRef<FEntry>& Entry : Entries)
	{
		Count += Entry->Call.bCanceled ? 1 : 0;
	}
	return Count;
}

TArray<uint8> FVaRestFakeTransport::ReadBody(int32 Index) const
{
	const FVaRestTransportRequest& Request = Entries[Index]->Call.Request;
	TArray<uint8> Out;
	if (!Request.BodyFile.IsEmpty())
	{
		FFileHelper::LoadFileToArray(Out, *Request.BodyFile);
	}
	else if (Request.BodySegments.Num() > 0)
	{
		FVaRestSegmentStream Stream(Request.BodySegments, Request.RequestId);
		Out.SetNumUninitialized(Stream.TotalSize());
		Stream.Serialize(Out.GetData(), Out.Num());
	}
	else
	{
		Out = Request.Body;
	}
	return Out;
}

TSharedRef<IVaRestTransportCall, ESPMode::ThreadSafe> FVaRestFakeTransport::Start(FVaRestTransportRequest&& Request, FVaRestTransportComplete&& OnComplete)
{
	check(IsInGameThread());
	TSharedRef<FEntry> Entry = MakeShared<FEntry>();
	const int32 Index = Entries.Add(Entry);
	Entry->Call.StartedAt = Clock->Now();
	Entry->LastActivityAt = Entry->Call.StartedAt;
	Entry->OnComplete = MoveTemp(OnComplete);
	Entry->Call.Request = MoveTemp(Request);
	FVaRestTransportRequest& Req = Entry->Call.Request;

	// The HTTP transport binds one engine delegate per flag (IN-11); count the same way
	FVaRestTrace::Counters().DelegateBinds += (Req.bReportProgress ? 1 : 0) + (Req.bReportStatusTime ? 1 : 0);
	if (Req.bReportProgress || Req.bReportStatusTime)
	{
		Req.Probe->UploadDoneAt = Clock->Now(); // the fake uploads instantly
	}

	FVaRestFakeResponse Response;
	if (Queue.Num() > 0)
	{
		Response = MoveTemp(Queue[0]);
		Queue.RemoveAt(0);
	}
	else if (Responder)
	{
		Response = Responder(Req);
	}
	else
	{
		Response = FVaRestFakeResponse::Json(200, FUtf8StringView(UTF8TEXT("{}")));
	}

	TWeakPtr<FVaRestFakeTransport, ESPMode::ThreadSafe> Weak = AsShared();
	const bool bWaits = Response.Delay < 0.0;
	if (Req.Timeout > 0.f && (bWaits || Response.Delay > Req.Timeout))
	{
		Entry->TimeoutTimer = Clock->SetTimer(Req.Timeout, [Weak, Index]
		{
			if (TSharedPtr<FVaRestFakeTransport, ESPMode::ThreadSafe> Pinned = Weak.Pin())
			{
				Pinned->Finish(Index, FVaRestFakeResponse::Failed(EVaRestTransportFailure::Timeout, TEXT("TimedOut")));
			}
		});
	}
	if (Req.ActivityTimeout > 0.f && (bWaits || Response.Delay > Req.ActivityTimeout))
	{
		ArmActivity(Index);
	}
	if (Response.Delay == 0.0)
	{
		Finish(Index, MoveTemp(Response));
	}
	else if (!bWaits)
	{
		const double Delay = Response.Delay;
		Entry->DelayTimer = Clock->SetTimer(Delay, [Weak, Index, Scripted = MoveTemp(Response)]() mutable
		{
			if (TSharedPtr<FVaRestFakeTransport, ESPMode::ThreadSafe> Pinned = Weak.Pin())
			{
				Pinned->Finish(Index, MoveTemp(Scripted));
			}
		});
	}
	return MakeShared<FCallHandle, ESPMode::ThreadSafe>(Weak, Index);
}

void FVaRestFakeTransport::Complete(int32 Index, FVaRestFakeResponse Response)
{
	check(IsInGameThread());
	Finish(Index, MoveTemp(Response));
}

void FVaRestFakeTransport::ReportProgress(int32 Index, int64 BytesSent, int64 BytesReceived, int64 DownloadTotal)
{
	check(IsInGameThread());
	FEntry& Entry = *Entries[Index];
	Entry.Call.Request.Probe->BytesSent = BytesSent;
	Entry.Call.Request.Probe->BytesReceived = BytesReceived;
	Entry.Call.Request.Probe->DownloadTotal = DownloadTotal;
	Entry.LastActivityAt = Clock->Now();
	if (Entry.Call.Request.ActivityTimeout > 0.f && !Entry.Call.bCompleted)
	{
		ArmActivity(Index);
	}
}

void FVaRestFakeTransport::Drop(int32 Index)
{
	check(IsInGameThread());
	FEntry& Entry = *Entries[Index];
	if (Entry.Call.bCompleted)
	{
		return;
	}
	Entry.Call.bCompleted = true;
	Clock->ClearTimer(Entry.DelayTimer);
	Clock->ClearTimer(Entry.TimeoutTimer);
	Clock->ClearTimer(Entry.ActivityTimer);
	Entry.OnComplete.Reset(); // destroyed uncalled
}

void FVaRestFakeTransport::ArmActivity(int32 Index)
{
	FEntry& Entry = *Entries[Index];
	Clock->ClearTimer(Entry.ActivityTimer);
	const double Due = Entry.LastActivityAt + Entry.Call.Request.ActivityTimeout - Clock->Now();
	TWeakPtr<FVaRestFakeTransport, ESPMode::ThreadSafe> Weak = AsShared();
	Entry.ActivityTimer = Clock->SetTimer(Due, [Weak, Index]
	{
		if (TSharedPtr<FVaRestFakeTransport, ESPMode::ThreadSafe> Pinned = Weak.Pin())
		{
			// HttpRequestCommon.cpp:303-305: the engine reports an activity timeout as a connection error
			Pinned->Finish(Index, FVaRestFakeResponse::Failed(EVaRestTransportFailure::Connection, TEXT("ActivityTimeout")));
		}
	});
}

void FVaRestFakeTransport::CancelCall(int32 Index)
{
	check(IsInGameThread()); // IVaRestTransportCall::Cancel is game thread (review M-8)
	FEntry& Entry = *Entries[Index];
	if (Entry.Call.bCompleted)
	{
		return;
	}
	Entry.Call.bCanceled = true;
	Finish(Index, FVaRestFakeResponse::Failed(EVaRestTransportFailure::Canceled, TEXT("Cancelled")));
}

void FVaRestFakeTransport::Finish(int32 Index, FVaRestFakeResponse&& Response)
{
	FEntry& Entry = *Entries[Index];
	if (Entry.Call.bCompleted)
	{
		return;
	}
	Entry.Call.bCompleted = true;
	Clock->ClearTimer(Entry.DelayTimer);
	Clock->ClearTimer(Entry.TimeoutTimer);
	Clock->ClearTimer(Entry.ActivityTimer);

	const FVaRestTransportRequest& Req = Entry.Call.Request;
	FVaRestTransportResult Result;
	Result.Failure = Response.Failure;
	Result.FailureDetail = Response.FailureDetail;
	Result.BytesUp = Req.Probe->UploadTotal;
	if (Response.Failure == EVaRestTransportFailure::None)
	{
		Result.Status = Response.Status;
		Result.Headers = Response.Headers;
		Result.ContentType = Response.Headers.Get(TEXT("Content-Type"));
		Result.EffectiveUrl = Response.EffectiveUrl.IsEmpty() ? Req.Url : Response.EffectiveUrl;
		if (!Req.ResponseFile.IsEmpty())
		{
			// As the HTTP transport's file sink: the file exists only once a byte arrived
			if (Response.Body.Num() > 0)
			{
				if (!FFileHelper::SaveArrayToFile(Response.Body, *Req.ResponseFile))
				{
					// As the HTTP transport's file sink (review M-7)
					Result.Failure = EVaRestTransportFailure::Other;
					Result.FailureDetail = TEXT("ResponseFileWriteFailed");
				}
			}
			Result.bBodyInFile = Result.Failure == EVaRestTransportFailure::None;
			Result.BytesDown = Response.Body.Num();
		}
		else if (Req.MaxBodyBytes > 0 && Response.Body.Num() > Req.MaxBodyBytes)
		{
			// As the HTTP transport's sink: the transfer stops past the JSON cap, nothing is kept (reviews M-6, M13)
			Result.Failure = EVaRestTransportFailure::Other;
			Result.FailureDetail = TEXT("ResponseTooLarge");
			Result.BytesDown = Req.MaxBodyBytes + 1;
		}
		else
		{
			// As the HTTP transport's memory sink: one byte of slack for ParseOwned's terminator (spec 2.1)
			Result.Body.Reserve(Response.Body.Num() + 1);
			Result.Body.Append(Response.Body);
			Entry.Call.BodyData = Result.Body.GetData();
			Entry.Call.BodyNum = Result.Body.Num();
			Result.BytesDown = Result.Body.Num();
		}
		// The response's Content-Length is known once it arrived (probe contract)
		int64 UnknownTotal = -1;
		Req.Probe->DownloadTotal.compare_exchange_strong(UnknownTotal, Response.Body.Num());
		if (Req.bReportStatusTime)
		{
			// The fake's own clock, not Req.Clock: its delays and timeouts run on this one, and the core hands it the same instance
			Req.Probe->StatusAt = Clock->Now();
		}
	}
	if (Result.Failure == EVaRestTransportFailure::None)
	{
		// A completed body is every byte that arrived: the engine's count only grows (CurlHttp.cpp:663), and its finish
		// reports it once more (CurlHttp.cpp:1274, :1221-1237), so a script that reported more than the body ends at the body
		Req.Probe->BytesReceived = Result.BytesDown;
		// Task 13 review minor 2 (P13.2): that finishing report carries the sent count too (CurlHttp.cpp:1225, :1236), and a
		// completed request sent its whole body: never lower than a ReportProgress already put there
		int64 Sent = Req.Probe->BytesSent.load();
		const int64 Whole = Req.Probe->UploadTotal.load();
		while (Sent < Whole && !Req.Probe->BytesSent.compare_exchange_weak(Sent, Whole))
		{
		}
	}
	else
	{
		// Never lower what ReportProgress already reported: a timeout or cancel after partial progress keeps it (review M-7)
		int64 Reported = Req.Probe->BytesReceived.load();
		while (Reported < Result.BytesDown && !Req.Probe->BytesReceived.compare_exchange_weak(Reported, Result.BytesDown))
		{
		}
	}
	if (Req.bReportStatusTime)
	{
		Result.TransportDoneAt = Clock->Now();
	}

	FVaRestTransportComplete Callback = MoveTemp(Entry.OnComplete);
	if (bCompleteOnWorker)
	{
		// One leaked pipe: completions run FIFO, one at a time, off the game thread, as the engine's single HTTP thread
		// runs them (review M-5). Leaked because ~FPipe checks it is idle (Pipe.h:40-43) and tests may end with work queued.
		static UE::Tasks::FPipe* const CompletionPipe = new UE::Tasks::FPipe(TEXT("VaRest.FakeTransport"));
		++CompletionsQueued;
		CompletionPipe->Launch(TEXT("VaRest.FakeTransport.Complete"), [Deliver = MoveTemp(Callback), Moved = MoveTemp(Result), Ran = CompletionsRun]() mutable
		{
			Deliver(MoveTemp(Moved));
			// Last, after the callback queued its game-thread task (FTestRig::Drain)
			++*Ran;
		});
	}
	else
	{
		Callback(MoveTemp(Result));
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
