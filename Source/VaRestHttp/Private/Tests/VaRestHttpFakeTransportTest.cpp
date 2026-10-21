// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "VaRestFakeTransport.h"
#include "VaRestHttpTesting.h"
#include "VaRestTrace.h"

namespace VaRest::Http::VaRestHttpFakeTransportTestPrivate
{
	/** Written on a worker, polled on the game thread: bDone is atomic and set last (review M17) */
	struct FDone
	{
		std::atomic<bool> bDone{ false };
		bool bOnGameThread = true;
		FVaRestTransportResult Result;
	};
	using FDoneRef = TSharedRef<FDone, ESPMode::ThreadSafe>;

	FVaRestTransportComplete Into(const FDoneRef& Done)
	{
		return [Done](FVaRestTransportResult&& Result)
		{
			Done->bOnGameThread = IsInGameThread();
			Done->Result = MoveTemp(Result);
			Done->bDone = true;
		};
	}

	FVaRestTransportRequest Get(FStringView Url)
	{
		FVaRestTransportRequest Request;
		Request.Verb = TEXT("GET");
		Request.Url = FString(Url);
		return Request;
	}

	struct FRig
	{
		TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> Clock = MakeShared<FVaRestVirtualClock, ESPMode::ThreadSafe>();
		TSharedRef<FVaRestFakeTransport, ESPMode::ThreadSafe> Fake = MakeShared<FVaRestFakeTransport, ESPMode::ThreadSafe>(Clock);
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpFakeRespondsTest, "VaRest.Http.Fake.Responds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpFakeRespondsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::VaRestHttpFakeTransportTestPrivate;
	FRig Rig;
	Rig.Fake->Enqueue(FVaRestFakeResponse::Json(201, FUtf8StringView(UTF8TEXT(R"({"id":700000000})"))));
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request) { return FVaRestFakeResponse::Text(404, Request.Url); });

	FDoneRef First = MakeShared<FDone, ESPMode::ThreadSafe>();
	FVaRestTransportRequest Request = Get(TEXT("https://api.test/a"));
	Request.Headers.Set(TEXT("X-Test"), TEXT("1"));
	Rig.Fake->Start(MoveTemp(Request), Into(First));
	TestTrue(TEXT("completes"), VaRest::Http::Testing::PumpUntil([First] { return First->bDone.load(); }));
	TestFalse(TEXT("off the game thread, as the HTTP thread completes"), First->bOnGameThread);
	TestEqual(TEXT("the enqueued response first"), First->Result.Status, 201);
	TestEqualSensitive(TEXT("content type from the headers"), First->Result.ContentType, FString(TEXT("application/json")));
	// Review I-2: a body whose size is already an allocation step, so the slack can only come from the fake's own Reserve
	TArray<uint8> Unslacked;
	Unslacked.Append(First->Result.Body.GetData(), First->Result.Body.Num());
	if (!TestEqual(TEXT("precondition: appending 16 bytes to an empty array allocates exactly 16"), Unslacked.Max(), Unslacked.Num()))
	{
		return false;
	}
	TestTrue(TEXT("the body has one byte of slack for ParseOwned (spec 2.1)"), First->Result.Body.Max() > First->Result.Body.Num());
	TestTrue(TEXT("... and the fake remembers its buffer"), Rig.Fake->GetCall(0).BodyData == First->Result.Body.GetData());
	TestEqualSensitive(TEXT("the effective URL defaults to the request URL"), First->Result.EffectiveUrl, FString(TEXT("https://api.test/a")));
	TestEqualSensitive(TEXT("the call is recorded"), Rig.Fake->GetCall(0).Request.Headers.Get(TEXT("x-test")), FString(TEXT("1")));

	FDoneRef Second = MakeShared<FDone, ESPMode::ThreadSafe>();
	Rig.Fake->Start(Get(TEXT("https://api.test/b")), Into(Second));
	VaRest::Http::Testing::PumpUntil([Second] { return Second->bDone.load(); });
	TestEqual(TEXT("then the responder"), Second->Result.Status, 404);
	TestEqual(TEXT("two calls"), Rig.Fake->NumCalls(), 2);

	Rig.Fake->bCompleteOnWorker = false;
	FDoneRef Inline = MakeShared<FDone, ESPMode::ThreadSafe>();
	Rig.Fake->Start(Get(TEXT("https://api.test/c")), Into(Inline));
	TestTrue(TEXT("bCompleteOnWorker = false completes inside Start, on the calling thread"), Inline->bDone && Inline->bOnGameThread);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpFakeTimingTest, "VaRest.Http.Fake.VirtualTime",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpFakeTimingTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::VaRestHttpFakeTransportTestPrivate;
	FRig Rig;
	Rig.Fake->bCompleteOnWorker = false;

	FVaRestFakeResponse Later = FVaRestFakeResponse::Json(200, FUtf8StringView(UTF8TEXT("{}")));
	Later.Delay = 2.0;
	Rig.Fake->Enqueue(Later);
	FDoneRef Delayed = MakeShared<FDone, ESPMode::ThreadSafe>();
	Rig.Fake->Start(Get(TEXT("https://api.test/delay")), Into(Delayed));
	Rig.Clock->Advance(1.9);
	TestFalse(TEXT("not before its delay"), Delayed->bDone);
	Rig.Clock->Advance(0.1);
	TestTrue(TEXT("at its delay"), Delayed->bDone && Delayed->Result.Status == 200);

	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	FDoneRef Manual = MakeShared<FDone, ESPMode::ThreadSafe>();
	Rig.Fake->Start(Get(TEXT("https://api.test/manual")), Into(Manual));
	Rig.Clock->Advance(100.0);
	TestFalse(TEXT("a manual call waits for Complete"), Manual->bDone);
	TestEqual(TEXT("one in flight"), Rig.Fake->NumInFlight(), 1);
	Rig.Fake->Complete(1, FVaRestFakeResponse::Text(503, TEXT("busy")));
	TestTrue(TEXT("Complete"), Manual->bDone && Manual->Result.Status == 503);

	// The engine: a total timeout is TimedOut, an activity timeout a connection error (HttpRequestCommon.cpp:299-305)
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	FDoneRef Total = MakeShared<FDone, ESPMode::ThreadSafe>();
	FVaRestTransportRequest WithTimeout = Get(TEXT("https://api.test/slow"));
	WithTimeout.Timeout = 1.f;
	Rig.Fake->Start(MoveTemp(WithTimeout), Into(Total));
	Rig.Clock->Advance(1.0);
	TestTrue(TEXT("the total timeout"), Total->bDone && Total->Result.Failure == EVaRestTransportFailure::Timeout);
	Rig.Fake->Complete(2, FVaRestFakeResponse::Text(200, TEXT("late")));
	TestEqual(TEXT("a late Complete is ignored"), Total->Result.Status, 0);

	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	FDoneRef Idle = MakeShared<FDone, ESPMode::ThreadSafe>();
	FVaRestTransportRequest WithActivity = Get(TEXT("https://api.test/drip"));
	WithActivity.ActivityTimeout = 2.f;
	Rig.Fake->Start(MoveTemp(WithActivity), Into(Idle));
	Rig.Clock->Advance(1.5);
	Rig.Fake->ReportProgress(3, 0, 100);
	Rig.Clock->Advance(1.5);
	TestFalse(TEXT("progress resets the activity timeout"), Idle->bDone);
	TestEqual(TEXT("progress lands in the probe"), Rig.Fake->GetCall(3).Request.Probe->BytesReceived.load(), static_cast<int64>(100));
	Rig.Clock->Advance(0.6);
	TestTrue(TEXT("no activity for the activity timeout: a connection failure"), Idle->bDone && Idle->Result.Failure == EVaRestTransportFailure::Connection
		&& Idle->Result.FailureDetail == FName(TEXT("ActivityTimeout")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpFakeCancelTest, "VaRest.Http.Fake.CancelAndFiles",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpFakeCancelTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::VaRestHttpFakeTransportTestPrivate;
	FRig Rig;
	Rig.Fake->bCompleteOnWorker = false;

	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	FDoneRef Canceled = MakeShared<FDone, ESPMode::ThreadSafe>();
	TSharedRef<IVaRestTransportCall, ESPMode::ThreadSafe> Call = Rig.Fake->Start(Get(TEXT("https://api.test/x")), Into(Canceled));
	Call->Cancel();
	TestTrue(TEXT("Cancel completes with Canceled"), Canceled->bDone && Canceled->Result.Failure == EVaRestTransportFailure::Canceled);
	TestEqual(TEXT("counted"), Rig.Fake->NumCanceled(), 1);
	Call->Cancel();
	TestEqual(TEXT("a second Cancel is a no-op"), Rig.Fake->NumCanceled(), 1);

	const FString File = FPaths::ProjectSavedDir() / TEXT("VaRestTests") / TEXT("fake-download.part");
	IFileManager::Get().Delete(*File);
	Rig.Fake->Enqueue(FVaRestFakeResponse::Bytes(200, TArray<uint8>{ 1, 2, 3, 4 }));
	FVaRestTransportRequest Download = Get(TEXT("https://api.test/file"));
	Download.ResponseFile = File;
	FDoneRef Saved = MakeShared<FDone, ESPMode::ThreadSafe>();
	Rig.Fake->Start(MoveTemp(Download), Into(Saved));
	TArray<uint8> OnDisk;
	TestTrue(TEXT("a ResponseFile request writes the body there"), Saved->Result.bBodyInFile && FFileHelper::LoadFileToArray(OnDisk, *File) && OnDisk.Num() == 4);
	TestEqual(TEXT("... and not into the result"), Saved->Result.Body.Num(), 0);
	IFileManager::Get().Delete(*File);

	FVaRestTransportRequest Upload = Get(TEXT("https://api.test/up"));
	Upload.Verb = TEXT("POST");
	FVaRestBodySegment Head;
	Head.Bytes = { 'a', 'b' };
	FVaRestBodySegment Tail;
	Tail.Bytes = { 'c' };
	Upload.BodySegments = { Head, Tail };
	FDoneRef Up = MakeShared<FDone, ESPMode::ThreadSafe>();
	Rig.Fake->Start(MoveTemp(Upload), Into(Up));
	TestTrue(TEXT("ReadBody reads segments back"), Rig.Fake->ReadBody(2) == TArray<uint8>({ 'a', 'b', 'c' }));

	const uint64 Binds = FVaRestTrace::Counters().DelegateBinds;
	FVaRestTransportRequest Watched = Get(TEXT("https://api.test/w"));
	Watched.bReportProgress = true;
	Watched.bReportStatusTime = true;
	FDoneRef W = MakeShared<FDone, ESPMode::ThreadSafe>();
	Rig.Fake->Start(MoveTemp(Watched), Into(W));
	TestEqual(TEXT("progress and status binds are counted like the HTTP transport's (IN-11)"), FVaRestTrace::Counters().DelegateBinds.load(), Binds + 2);
	TestTrue(TEXT("status time is stamped"), Rig.Fake->GetCall(3).Request.Probe->StatusAt.load() >= 0.0 && W->Result.TransportDoneAt >= 0.0);

	// Reviews M-6, M13: past MaxBodyBytes the sink stops the transfer and keeps nothing
	Rig.Fake->Enqueue(FVaRestFakeResponse::Json(200, FUtf8StringView(UTF8TEXT(R"({"padding":"0123456789"})"))));
	FVaRestTransportRequest Capped = Get(TEXT("https://api.test/big"));
	Capped.MaxBodyBytes = 8;
	FDoneRef Big = MakeShared<FDone, ESPMode::ThreadSafe>();
	Rig.Fake->Start(MoveTemp(Capped), Into(Big));
	TestTrue(TEXT("over MaxBodyBytes: ResponseTooLarge, no body"), Big->bDone && Big->Result.Failure == EVaRestTransportFailure::Other
		&& Big->Result.FailureDetail == FName(TEXT("ResponseTooLarge")) && Big->Result.Body.Num() == 0 && Big->Result.Status == 200);

	// Review I-3: a dropped completion is destroyed uncalled
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	FDoneRef Lost = MakeShared<FDone, ESPMode::ThreadSafe>();
	Rig.Fake->Start(Get(TEXT("https://api.test/lost")), Into(Lost));
	Rig.Fake->Drop(5);
	TestFalse(TEXT("Drop never calls the completion"), Lost->bDone.load());
	TestEqual(TEXT("... and destroys it (the test holds the only reference left)"), Lost.GetSharedReferenceCount(), 1);
	TestEqual(TEXT("... and the call is no longer in flight"), Rig.Fake->NumInFlight(), 0);
	return true;
}

#endif
