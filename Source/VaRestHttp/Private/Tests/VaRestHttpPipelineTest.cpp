// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Tasks/Task.h"
#include "VaRestClient.h"
#include "VaRestClientState.h"
#include "VaRestHttpTesting.h"

namespace VaRest::Http::PipelineTestPrivate
{
	using FLog = TSharedRef<TArray<FString>, ESPMode::ThreadSafe>;

	/** Logs what it sees; PerAttempt instances can sign; an Outer one can re-issue or answer by itself */
	class FRecordingStage final : public IVaRestStage
	{
	public:
		enum class EMode : uint8
		{
			Pass,
			Sign,
			Reissue,
			Answer,
			/** Reports the outcome twice (a buggy stage, review C-3) */
			Twice
		};

		FRecordingStage(FString InLabel, FLog InLog, EMode InMode = EMode::Pass)
			: Label(MoveTemp(InLabel))
			, Log(MoveTemp(InLog))
			, Mode(InMode)
		{
		}

		virtual FName GetStageName() const override { return FName(*Label); }
		virtual bool MayReissue() const override { return Mode == EMode::Reissue; }

		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			Log->Add(FString::Printf(TEXT("%s>%d"), *Label, Exchange->GetAttempt()));
			if (Mode == EMode::Answer)
			{
				FVaRestAttemptOutcome Cached;
				Cached.Status = 200;
				Cached.ContentType = TEXT("application/json");
				Cached.Headers.Set(TEXT("Content-Type"), TEXT("application/json"));
				Cached.Body = { '{', '"', 'c', '"', ':', '1', '}' };
				OnOutcome(MoveTemp(Cached)); // synchronously, inside Send(): the core must defer the callbacks
				return;
			}
			if (Mode == EMode::Sign)
			{
				Exchange->GetAttemptHeaders().Set(TEXT("X-Signed"), FString::Printf(TEXT("attempt-%d"), Exchange->GetAttempt()));
			}
			FLog Recorder = Log;
			FString Name = Label;
			if (Mode == EMode::Twice)
			{
				Next.Proceed(Exchange, [Done = MoveTemp(OnOutcome)](FVaRestAttemptOutcome&& Outcome) mutable
				{
					FVaRestAttemptOutcome Again = Outcome;
					Done(MoveTemp(Outcome));
					Done(MoveTemp(Again));
				});
				return;
			}
			if (Mode == EMode::Reissue)
			{
				// What M4's retry does: a second trip through the stages after this one, once the first outcome is back
				Next.Proceed(Exchange, [Recorder, Name, Exchange, Next, Done = MoveTemp(OnOutcome)](FVaRestAttemptOutcome&& First) mutable
				{
					Recorder->Add(FString::Printf(TEXT("%s<%d"), *Name, First.Status));
					Next.Proceed(Exchange, MoveTemp(Done));
				});
				return;
			}
			Next.Proceed(Exchange, [Recorder, Name, Done = MoveTemp(OnOutcome)](FVaRestAttemptOutcome&& Outcome) mutable
			{
				Recorder->Add(FString::Printf(TEXT("%s<%d"), *Name, Outcome.Status));
				Done(MoveTemp(Outcome));
			});
		}

	private:
		FString Label;
		FLog Log;
		EMode Mode;
	};

	FVaRestFakeResponse Json(int32 Status, const char* Text)
	{
		return FVaRestFakeResponse::Json(Status, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text)));
	}

	/** One configuration edit that sets X-A and X-B to Gen together: a request that sees X-A != X-B saw half an edit */
	void EditGeneration(const FVaRestClient& Client, int32 Gen)
	{
		VaRest::Http::FClientAccess::State(Client)->EditConfig([Gen](FClientConfig& Edited)
		{
			Edited.DefaultHeaders.Set(TEXT("X-A"), LexToString(Gen));
			Edited.DefaultHeaders.Set(TEXT("X-B"), LexToString(Gen));
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPipelineGetJsonTest, "VaRest.Http.Pipeline.GetJson",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPipelineGetJsonTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::PipelineTestPrivate;
	FTestRig Rig;
	Rig.Fake->Enqueue(Json(200, R"({"name":"neo","level":42})"));
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/users/{id}")).Path(TEXT("id"), 7).Query(TEXT("lang"), TEXT("en")).Header(TEXT("X-Trace"), TEXT("t1")), Outcome).Send();
	TestTrue(TEXT("the handle has the request id"), Handle.GetId() != 0);
	TestTrue(TEXT("completes"), Rig.Wait(Outcome));
	TestEqual(TEXT("one OnSuccess"), Outcome->SuccessCalls.load(), 1);
	TestEqual(TEXT("no OnError"), Outcome->ErrorCalls.load(), 0);
	TestEqual(TEXT("one OnComplete"), Outcome->CompleteCalls.load(), 1);
	TestTrue(TEXT("every callback on the game thread (spec 1)"), Outcome->bAllOnGameThread.load());
	TestTrue(TEXT("the decoded JSON"), Outcome->Value.IsSet() && Outcome->Value->Get(TEXT("level")).AsInt() == 42);
	const FVaRestTransportRequest& Sent = Rig.Fake->GetCall(0).Request;
	TestEqualSensitive(TEXT("verb"), Sent.Verb, FString(TEXT("GET")));
	TestEqualSensitive(TEXT("base URL + expanded path + query"), Sent.Url, FString(TEXT("https://api.test/users/7?lang=en")));
	TestEqualSensitive(TEXT("the request's header"), Sent.Headers.Get(TEXT("X-Trace")), FString(TEXT("t1")));
	TestEqual(TEXT("one attempt"), Outcome->Response.Attempts, 1);
	TestEqual(TEXT("status"), Outcome->Response.Status, 200);
	TestEqual(TEXT("request id in the response"), Outcome->Response.RequestId, Handle.GetId());
	TestEqual(TEXT("a successful decode releases the raw bytes (spec 3.2)"), Outcome->Response.Body.Num(), 0);
	TestFalse(TEXT("the handle is done"), Handle.IsActive());
	TestEqual(TEXT("nothing in flight"), Rig.Client.GetInFlightCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPipelineResultTypesTest, "VaRest.Http.Pipeline.ResultTypes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPipelineResultTypesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200, TEXT("ignored")));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Bytes(200, { 0, 1, 2, 255 }));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200, TEXT("café")));
	FVaRestFakeResponse Raw = FVaRestFakeResponse::Text(201, TEXT("raw"), TEXT("text/x-raw"));
	Raw.Headers.Add(TEXT("Set-Cookie"), TEXT("a=1"));
	Rig.Fake->Enqueue(Raw);

	auto None = MakeOutcome<void>();
	auto Bytes = MakeOutcome<TArray<uint8>>();
	auto Text = MakeOutcome<FString>();
	auto Response = MakeOutcome<FVaRestResponse>();
	Observe(Rig.Client.Delete<void>(TEXT("/a")), None).Send();
	Observe(Rig.Client.Get<TArray<uint8>>(TEXT("/b")), Bytes).Send();
	Observe(Rig.Client.Get<FString>(TEXT("/c")), Text).Send();
	Observe(Rig.Client.Get<FVaRestResponse>(TEXT("/d")), Response).Send();
	TestTrue(TEXT("all complete"), PumpUntil([&] { return None->IsDone() && Bytes->IsDone() && Text->IsDone() && Response->IsDone(); }));

	TestTrue(TEXT("void: success, the body released"), None->SuccessCalls == 1 && None->Response.Body.Num() == 0);
	TestTrue(TEXT("bytes: the body itself"), Bytes->Value.IsSet() && *Bytes->Value == TArray<uint8>({ 0, 1, 2, 255 }));
	TestEqualSensitive(TEXT("FString: UTF-8 decoded"), Text->Value.Get(FString()), FString(TEXT("café")));
	TestTrue(TEXT("FVaRestResponse: the raw response, body kept"), Response->Value.IsSet() && Response->Value->Status == 201
		&& Response->Value->Body.Num() == 3 && Response->Value->Headers.Get(TEXT("set-cookie")) == TEXT("a=1"));
	TestEqualSensitive(TEXT("verbs"), Rig.Fake->GetCall(0).Request.Verb, FString(TEXT("DELETE")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPipelineErrorsTest, "VaRest.Http.Pipeline.Errors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPipelineErrorsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::PipelineTestPrivate;
	FTestRig Rig;
	Rig.Fake->Enqueue(Json(404, R"({"error":"missing"})"));
	Rig.Fake->Enqueue(Json(503, R"({})"));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Text(500, TEXT("<h1>oops</h1>"), TEXT("text/html")));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Failed(EVaRestTransportFailure::Connection, TEXT("ConnectionError")));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Failed(EVaRestTransportFailure::Timeout, TEXT("TimedOut")));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Failed(EVaRestTransportFailure::Other, TEXT("BodyRejected")));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Failed(EVaRestTransportFailure::Other, TEXT("ResponseFileWriteFailed")));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Failed(EVaRestTransportFailure::Other, TEXT("TransportDropped")));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Failed(EVaRestTransportFailure::Other, TEXT("UploadReadFailed")));
	// Task 7's details: a BodyFile gone at Start, a header the transport refused, a body past MaxBodyBytes
	Rig.Fake->Enqueue(FVaRestFakeResponse::Failed(EVaRestTransportFailure::Other, TEXT("BodyFileMissing")));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Failed(EVaRestTransportFailure::Other, TEXT("InvalidHeader")));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Failed(EVaRestTransportFailure::Other, TEXT("ResponseTooLarge")));
	TArray<TSharedRef<TTestOutcome<FVaRestJson>, ESPMode::ThreadSafe>> Outcomes;
	for (int32 i = 0; i < 12; ++i)
	{
		Outcomes.Add(MakeOutcome<FVaRestJson>());
		Observe(Rig.Client.Get(FString::Printf(TEXT("/e%d?token=secret-%d"), i, i)).Tag(TEXT("errs")), Outcomes.Last()).Send();
	}
	TestTrue(TEXT("all complete"), PumpUntil([&Outcomes] { for (const auto& O : Outcomes) { if (!O->IsDone()) { return false; } } return true; }));

	const FVaRestError& NotFound = Outcomes[0]->Error;
	TestTrue(TEXT("404: Kind Http, the status"), NotFound.Kind == EVaRestErrorKind::Http && NotFound.HttpStatus == 404);
	TestEqualSensitive(TEXT("... the JSON error body parsed (spec 3.3)"), NotFound.Body[TEXT("error")].AsString(), FString(TEXT("missing")));
	TestFalse(TEXT("... not retryable"), NotFound.bRetryable);
	TestTrue(TEXT("... a message naming the status"), NotFound.Message.Contains(TEXT("404")));
	TestEqualSensitive(TEXT("... the URL, redacted"), NotFound.Url, FString(TEXT("https://api.test/e0?token=***")));
	TestTrue(TEXT("... tag and attempts"), NotFound.Tag == FName(TEXT("errs")) && NotFound.Attempts == 1);
	TestTrue(TEXT("... the raw body kept on error (spec 3.2)"), Outcomes[0]->Response.Body.Num() > 0);
	TestEqual(TEXT("OnError once"), Outcomes[0]->ErrorCalls.load(), 1);
	TestEqual(TEXT("OnSuccess never"), Outcomes[0]->SuccessCalls.load(), 0);

	TestTrue(TEXT("503 is retryable (spec 4.1)"), Outcomes[1]->Error.bRetryable);
	const FVaRestError& Html = Outcomes[2]->Error;
	TestTrue(TEXT("a non-JSON error body: no Body, RawBody holds the text"), !Html.Body.IsValid() && Html.RawBody.Contains(TEXT("oops")));
	TestTrue(TEXT("no connection: Kind Connection, retryable"), Outcomes[3]->Error.Kind == EVaRestErrorKind::Connection && Outcomes[3]->Error.bRetryable && Outcomes[3]->Error.HttpStatus == 0);
	TestTrue(TEXT("a total timeout: Kind Timeout"), Outcomes[4]->Error.Kind == EVaRestErrorKind::Timeout && Outcomes[4]->Error.bRetryable);
	// Review M14: a local failure is not a network one -- M4's retry must not re-send onto a full disk
	TestTrue(TEXT("a body the engine refused: Kind Config, not retryable, with the detail"), Outcomes[5]->Error.Kind == EVaRestErrorKind::Config
		&& !Outcomes[5]->Error.bRetryable && Outcomes[5]->Error.Message.Contains(TEXT("BodyRejected")));
	TestTrue(TEXT("a file that cannot be written: Kind Decode, not retryable"), Outcomes[6]->Error.Kind == EVaRestErrorKind::Decode && !Outcomes[6]->Error.bRetryable);
	TestTrue(TEXT("a completion the transport dropped: Connection, not retryable (review I-3)"), Outcomes[7]->Error.Kind == EVaRestErrorKind::Connection
		&& !Outcomes[7]->Error.bRetryable && Outcomes[7]->Error.Message.Contains(TEXT("TransportDropped")));
	TestTrue(TEXT("an upload that could not be read: Kind Config, not retryable (review N-4)"), Outcomes[8]->Error.Kind == EVaRestErrorKind::Config
		&& !Outcomes[8]->Error.bRetryable && Outcomes[8]->Error.Message.Contains(TEXT("UploadReadFailed")));
	// Task 7 carry: the request itself is wrong -- sending it again fails again
	TestTrue(TEXT("a BodyFile missing at Start: Kind Config, not retryable, with the detail"), Outcomes[9]->Error.Kind == EVaRestErrorKind::Config
		&& !Outcomes[9]->Error.bRetryable && Outcomes[9]->Error.Message.Contains(TEXT("BodyFileMissing")));
	TestTrue(TEXT("a header the transport refused: Kind Config, not retryable, with the detail"), Outcomes[10]->Error.Kind == EVaRestErrorKind::Config
		&& !Outcomes[10]->Error.bRetryable && Outcomes[10]->Error.Message.Contains(TEXT("InvalidHeader")));
	// Decision 20: the transport stopped a JSON body at the cap -- untrusted input, bounded memory, the same body next time
	TestTrue(TEXT("a body past MaxBodyBytes: Kind Decode, not retryable"), Outcomes[11]->Error.Kind == EVaRestErrorKind::Decode
		&& !Outcomes[11]->Error.bRetryable && Outcomes[11]->Error.Message.Contains(TEXT("MaxJsonBytes")));
	TestEqual(TEXT("every one of them reached the transport once"), Rig.Fake->NumCalls(), 12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPipelineNotInsideSendTest, "VaRest.Http.Pipeline.NeverInsideSend",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPipelineNotInsideSendTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::PipelineTestPrivate;
	FTestRig Rig;
	Rig.Fake->bCompleteOnWorker = false; // the transport answers inside Start, synchronously
	TSharedRef<bool, ESPMode::ThreadSafe> bInsideSend = MakeShared<bool, ESPMode::ThreadSafe>(false);
	TSharedRef<int32, ESPMode::ThreadSafe> CalledInside = MakeShared<int32, ESPMode::ThreadSafe>(0);
	TSharedRef<int32, ESPMode::ThreadSafe> Called = MakeShared<int32, ESPMode::ThreadSafe>(0);
	auto Probe = [bInsideSend, CalledInside, Called](const auto&)
	{
		++*Called;
		*CalledInside += *bInsideSend ? 1 : 0;
	};

	*bInsideSend = true;
	Rig.Client.Get(TEXT("/inline")).OnComplete(Probe).Send();
	Rig.Client.Get(TEXT("/users/{id}")).OnComplete(Probe).Send(); // a Config error, known inside Send
	*bInsideSend = false;

	FLog Log = MakeShared<TArray<FString>, ESPMode::ThreadSafe>();
	FTestRig Cached;
	Cached.Client.Use(MakeShared<FRecordingStage, ESPMode::ThreadSafe>(TEXT("cache"), Log, FRecordingStage::EMode::Answer), EVaRestStageSlot::Outer);
	*bInsideSend = true;
	Cached.Client.Get(TEXT("/cached")).OnComplete(Probe).Send(); // a stage answers inside Send
	*bInsideSend = false;

	TestTrue(TEXT("all three complete"), PumpUntil([Called] { return *Called == 3; }));
	TestEqual(TEXT("decision 3: no callback runs inside Send()"), *CalledInside, 0);
	TestEqual(TEXT("the answering stage kept the transport idle"), Cached.Fake->NumCalls(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPipelineFromWorkerTest, "VaRest.Http.Pipeline.SendFromWorker",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPipelineFromWorkerTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	auto Outcome = MakeOutcome<FVaRestJson>();
	TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> SentFrom = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0); // 1 worker, 2 game thread
	TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> bActiveOnWorker = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	FVaRestClient Client = Rig.Client;
	// Not Wait(): a waited task may be retracted and run on the waiting thread (TaskPrivate.cpp:75-100)
	UE::Tasks::Launch(TEXT("VaRest.Test.SendFromWorker"), [Client, Outcome, SentFrom, bActiveOnWorker]
	{
		const FVaRestRequestHandle Handle = Observe(Client.Post(TEXT("/from-worker")).Body(TEXT("hi")), Outcome).Send();
		*bActiveOnWorker = Handle.IsActive();
		*SentFrom = IsInGameThread() ? 2 : 1;
	});
	// Review I-6: drain no game-thread task until the worker is done, or the game thread could finish the whole request
	// between its Send() and IsActive(). The worker never needs the game thread, so this cannot deadlock.
	const double SpinUntil = FPlatformTime::Seconds() + 10.0;
	while (SentFrom->load() == 0 && FPlatformTime::Seconds() < SpinUntil)
	{
		FPlatformProcess::Yield();
	}
	TestTrue(TEXT("the worker sent"), SentFrom->load() != 0);
	TestEqual(TEXT("... from a worker thread"), SentFrom->load(), 1);
	TestTrue(TEXT("sent from a worker, the request is active before it reaches the game thread"), bActiveOnWorker->load());
	TestTrue(TEXT("completes"), Rig.Wait(Outcome));
	TestTrue(TEXT("it was marshalled to the game thread: callbacks there (spec 1)"), Outcome->bAllOnGameThread.load());
	TestEqual(TEXT("the transport was called"), Rig.Fake->NumCalls(), 1);
	TestTrue(TEXT("with the body"), Rig.Fake->ReadBody(0) == TArray<uint8>({ 'h', 'i' }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPipelineStagesTest, "VaRest.Http.Pipeline.Stages",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPipelineStagesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::PipelineTestPrivate;
	FTestRig Rig;
	FLog Log = MakeShared<TArray<FString>, ESPMode::ThreadSafe>();
	Rig.Client.WithHeader(TEXT("X-Client"), TEXT("c")).WithHeader(TEXT("x-mode"), TEXT("client"));
	Rig.Client.Use(MakeShared<FRecordingStage, ESPMode::ThreadSafe>(TEXT("outer"), Log), EVaRestStageSlot::Outer);
	Rig.Client.Use(MakeShared<FRecordingStage, ESPMode::ThreadSafe>(TEXT("sign"), Log, FRecordingStage::EMode::Sign), EVaRestStageSlot::PerAttempt);
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/s")).Header(TEXT("X-Mode"), TEXT("request")), Outcome).Send();
	TestTrue(TEXT("completes"), Rig.Wait(Outcome));
	// Outer before the attempt boundary (attempt 0), PerAttempt after it (attempt 1), outcomes back in reverse
	TestEqualSensitive(TEXT("stage order (spec 4, decision 13)"), FString::Join(*Log, TEXT(",")), FString(TEXT("outer>0,sign>1,sign<200,outer<200")));
	const FVaRestHeaders& Sent = Rig.Fake->GetCall(0).Request.Headers;
	TestEqualSensitive(TEXT("a PerAttempt stage sets final headers (request signing)"), Sent.Get(TEXT("X-Signed")), FString(TEXT("attempt-1")));
	TestEqualSensitive(TEXT("client default headers are sent"), Sent.Get(TEXT("X-Client")), FString(TEXT("c")));
	TestEqualSensitive(TEXT("the request's header wins over the client's, whatever the case"), Sent.Get(TEXT("X-MODE")), FString(TEXT("request")));
	TestEqual(TEXT("... and replaces it"), Sent.GetPairs().FilterByPredicate([](const FVaRestHeaders::FPair& P) { return P.Key.Equals(TEXT("x-mode"), ESearchCase::IgnoreCase); }).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPipelineReissueTest, "VaRest.Http.Pipeline.Reissue",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPipelineReissueTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::PipelineTestPrivate;
	FTestRig Rig;
	FLog Log = MakeShared<TArray<FString>, ESPMode::ThreadSafe>();
	Rig.Client.Use(MakeShared<FRecordingStage, ESPMode::ThreadSafe>(TEXT("retry"), Log, FRecordingStage::EMode::Reissue), EVaRestStageSlot::Outer);
	Rig.Fake->Enqueue(Json(503, "{}"));
	Rig.Fake->Enqueue(Json(200, R"({"ok":true})"));
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Post(TEXT("/r")).Body(TArray<uint8>{ 'a', 'b', 'c' }), Outcome).Send();
	TestTrue(TEXT("completes"), Rig.Wait(Outcome));
	TestEqual(TEXT("two transport calls"), Rig.Fake->NumCalls(), 2);
	TestTrue(TEXT("attempt numbers count up across the boundary"), Rig.Fake->GetCall(0).Request.Attempt == 1 && Rig.Fake->GetCall(1).Request.Attempt == 2);
	TestTrue(TEXT("a stage that may re-issue keeps the body for the second attempt (decision 12)"),
		Rig.Fake->ReadBody(0) == TArray<uint8>({ 'a', 'b', 'c' }) && Rig.Fake->ReadBody(1) == TArray<uint8>({ 'a', 'b', 'c' }));
	TestTrue(TEXT("the second outcome is the result"), Outcome->SuccessCalls == 1 && Outcome->Response.Attempts == 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPipelineDroppedTest, "VaRest.Http.Pipeline.ClientDroppedInFlight",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPipelineDroppedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::PipelineTestPrivate;
	FTestRig Rig;
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	const int32 ExchangesOffBefore = Counters().ExchangesDestroyedOffGameThread.load();
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/slow")), Outcome).Send(); // on the game thread, and no handle pinned anywhere
	PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; });
	TWeakPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> WeakState = VaRest::Http::FClientAccess::State(Rig.Client);
	Rig.Client = FVaRestClient(); // the last handle
	TestTrue(TEXT("spec 3.2: the in-flight exchange keeps the client state alive"), WeakState.IsValid());
	const int32 OffBefore = Counters().ClientStatesDestroyedOffGameThread.load();
	// The completion comes from a worker (bCompleteOnWorker) and the JSON is decoded on another: both move their
	// reference to the exchange into the game-thread continuation instead of dropping it where they run (review I-1).
	// Two layers are counted. The exchange: a continuation that copied its reference instead of moving it would let the
	// exchange die on a worker whenever that copy happened to be the last -- a timing-dependent catch, so a regression
	// shows often, not always. The client state: a regression of the exchange's backstop as well.
	Rig.Fake->Complete(0, Json(200, R"({"late":true})"));
	TestTrue(TEXT("the request still completes"), Rig.Wait(Outcome));
	TestTrue(TEXT("... and calls back"), Outcome->SuccessCalls == 1);
	PumpUntil([&WeakState] { return !WeakState.IsValid(); }, 1.0);
	TestFalse(TEXT("then the state dies"), WeakState.IsValid());
	TestEqual(TEXT("... on the game thread (spec 1; review I-1)"), Counters().ClientStatesDestroyedOffGameThread.load() - OffBefore, 0);
	TestEqual(TEXT("the exchange died on the game thread too: no continuation kept a reference (review I-1)"),
		Counters().ExchangesDestroyedOffGameThread.load() - ExchangesOffBefore, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPipelineManyTest, "VaRest.Http.Pipeline.ManyConcurrent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPipelineManyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	FTestRig Rig; // completions arrive from worker tasks, all at once
	TArray<TSharedRef<TTestOutcome<FVaRestJson>, ESPMode::ThreadSafe>> Outcomes;
	for (int32 i = 0; i < 100; ++i)
	{
		Outcomes.Add(MakeOutcome<FVaRestJson>());
		Observe(Rig.Client.Get(FString::Printf(TEXT("/m/%d"), i)), Outcomes.Last()).Send();
	}
	TestTrue(TEXT("all complete"), PumpUntil([&Outcomes] { for (const auto& O : Outcomes) { if (!O->IsDone()) { return false; } } return true; }, 10.0));
	PumpUntil([] { return false; }, 0.05);
	int32 Exact = 0;
	bool bGameThread = true;
	for (const auto& O : Outcomes)
	{
		Exact += (O->CompleteCalls == 1 && O->SuccessCalls == 1 && O->ErrorCalls == 0) ? 1 : 0;
		bGameThread &= O->bAllOnGameThread.load();
	}
	TestEqual(TEXT("exactly one OnSuccess and one OnComplete each"), Exact, 100);
	TestTrue(TEXT("all on the game thread"), bGameThread);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPipelineConfigRaceTest, "VaRest.Http.Pipeline.ConfigEditedWhileSending",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPipelineConfigRaceTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	// One edit changes two headers together: a request that ever sees X-A != X-B saw half an edit
	auto EditBoth = [&Rig](int32 Gen)
	{
		VaRest::Http::FClientAccess::State(Rig.Client)->EditConfig([Gen](VaRest::Http::FClientConfig& Edited)
		{
			Edited.DefaultHeaders.Set(TEXT("X-A"), LexToString(Gen));
			Edited.DefaultHeaders.Set(TEXT("X-B"), LexToString(Gen));
		});
	};
	EditBoth(0);
	const int32 ExchangesOffBefore = Counters().ExchangesDestroyedOffGameThread.load();
	FVaRestClient Client = Rig.Client;
	TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Finished = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	for (int32 t = 0; t < 4; ++t)
	{
		UE::Tasks::Launch(TEXT("VaRest.Test.Sender"), [Client, Finished]
		{
			for (int32 i = 0; i < 50; ++i)
			{
				// Spec 3.1: the snapshot is taken here, at Send() on a worker, while the game thread replaces it
				Client.Get(TEXT("/worker")).Send();
			}
			++*Finished;
		});
	}
	for (int32 Gen = 1; Gen <= 200; ++Gen)
	{
		EditBoth(Gen);
		Rig.Client.Get(FString::Printf(TEXT("/gt/%d"), Gen)).Send(); // sent right after edit Gen, on the game thread
		if (Gen % 10 == 0)
		{
			PumpUntil([] { return false; }, 0.001); // let the workers' requests enter between edits
		}
	}
	TestTrue(TEXT("the senders finish"), PumpUntil([Finished] { return Finished->load() == 4; }, 10.0));
	TestTrue(TEXT("all 400 reach the transport"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 400 && Rig.Client.GetInFlightCount() == 0; }, 10.0));
	bool bWhole = true;
	bool bOrdered = true;
	bool bAtSend = true;
	int32 LastGameThreadGen = -1;
	TSet<int32> Seen;
	for (int32 i = 0; i < Rig.Fake->NumCalls(); ++i)
	{
		const FVaRestTransportRequest& Sent = Rig.Fake->GetCall(i).Request;
		int32 A = -1;
		int32 B = -2;
		bWhole &= LexTryParseString(A, *Sent.Headers.Get(TEXT("X-A"))) && LexTryParseString(B, *Sent.Headers.Get(TEXT("X-B"))) && A == B;
		Seen.Add(A);
		int32 SentAt = -1;
		if (Sent.Url.Contains(TEXT("/gt/")) && LexTryParseString(SentAt, *Sent.Url.RightChop(Sent.Url.Find(TEXT("/gt/")) + 4)))
		{
			bOrdered &= A >= LastGameThreadGen;
			bAtSend &= A == SentAt;
			LastGameThreadGen = A;
		}
	}
	TestTrue(TEXT("every request carries one whole configuration (X-A == X-B)"), bWhole);
	// A game-thread Send() enters at once, so these two hold for a snapshot at Send and at Enter alike: the worker's
	// snapshot at Send() is VaRest.Http.Pipeline.SnapshotAtSend's (task 8 review I1). Worker requests are left out of the
	// order check: with a correct snapshot at Send, one made at generation 5 may still enter after /gt/7.
	TestTrue(TEXT("a game-thread request carries the configuration of the edit just before its Send()"), bAtSend);
	TestTrue(TEXT("... so game-thread generations never go back in transport order"), bOrdered);
	TestTrue(TEXT("snapshots were taken at different generations"), Seen.Num() > 1);
	// Task 8 review minor 2: a worker's Send() moves its reference into the Enter task, so no exchange dies on a sender
	// thread even when the game thread runs the whole request before Send() returns there (timing-dependent: a
	// regression shows only when that happens)
	TestEqual(TEXT("no exchange died on a sending worker (review I-1)"), Counters().ExchangesDestroyedOffGameThread.load() - ExchangesOffBefore, 0);
	// The data-race half (a worker reading while the game thread swaps) is a crash/TSAN smoke test: it fails only by crashing
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPipelineSnapshotAtSendTest, "VaRest.Http.Pipeline.SnapshotAtSend",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPipelineSnapshotAtSendTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::PipelineTestPrivate;
	// Spec 3.1, verbatim: "A request snapshots the configuration at Send()" (decision 27; reviews I-7, I6; task 8 review
	// I1). Deterministic: a worker sends while generation 1 is current; the game thread drains no task until that Send()
	// returned (review I-6), so the request's Enter task cannot have run; it then edits to generation 2, and only then
	// lets the request enter. A snapshot taken at Enter would carry 2.
	FTestRig Rig;
	EditGeneration(Rig.Client, 1);
	FVaRestClient Client = Rig.Client;
	auto FromWorker = MakeOutcome<FVaRestJson>();
	TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> bSent = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	// Not Wait(): a waited task may be retracted and run on the waiting thread (TaskPrivate.cpp:75-100)
	UE::Tasks::Launch(TEXT("VaRest.Test.SnapshotSender"), [Client, FromWorker, bSent]
	{
		Observe(Client.Get(TEXT("/snap")), FromWorker).Send();
		*bSent = true;
	});
	// The worker never needs the game thread to send, so this cannot deadlock
	const double SpinUntil = FPlatformTime::Seconds() + 10.0;
	while (!bSent->load() && FPlatformTime::Seconds() < SpinUntil)
	{
		FPlatformProcess::Yield();
	}
	TestTrue(TEXT("the worker sent"), bSent->load());
	TestEqual(TEXT("... and its request has not entered the pipeline: the Enter task waits for the game thread"), Rig.Fake->NumCalls(), 0);
	EditGeneration(Rig.Client, 2);
	auto AfterEdit = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/after")), AfterEdit).Send(); // the control: the edit took effect before the worker's request entered
	TestTrue(TEXT("both complete"), PumpUntil([FromWorker, AfterEdit] { return FromWorker->IsDone() && AfterEdit->IsDone(); }));
	TestTrue(TEXT("... nothing left in flight"), PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; }));
	FString SnapA;
	FString SnapB;
	FString AfterA;
	int32 Found = 0;
	for (int32 i = 0; i < Rig.Fake->NumCalls(); ++i)
	{
		const FVaRestTransportRequest& Sent = Rig.Fake->GetCall(i).Request;
		if (Sent.Url.EndsWith(TEXT("/snap"), ESearchCase::CaseSensitive))
		{
			SnapA = Sent.Headers.Get(TEXT("X-A"));
			SnapB = Sent.Headers.Get(TEXT("X-B"));
			++Found;
		}
		else if (Sent.Url.EndsWith(TEXT("/after"), ESearchCase::CaseSensitive))
		{
			AfterA = Sent.Headers.Get(TEXT("X-A"));
			++Found;
		}
	}
	if (!TestEqual(TEXT("both reached the transport, once each"), Found, 2))
	{
		return false;
	}
	TestEqualSensitive(TEXT("spec 3.1: the worker's request carries the configuration of its Send() (generation 1), not of its later Enter (2)"),
		SnapA, FString(TEXT("1")));
	TestEqualSensitive(TEXT("... all of it"), SnapB, FString(TEXT("1")));
	TestEqualSensitive(TEXT("a request sent after the edit carries generation 2"), AfterA, FString(TEXT("2")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPipelineConfigErrorTest, "VaRest.Http.Pipeline.ConfigErrors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPipelineConfigErrorTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	FTestRig NoBase(TEXTVIEW(""));
	auto Relative = MakeOutcome<FVaRestJson>();
	Observe(NoBase.Client.Get(TEXT("users/7")), Relative).Send();
	FTestRig Rig;
	auto Missing = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/users/{id}")), Missing).Send();
	TestTrue(TEXT("both complete"), PumpUntil([&] { return Relative->IsDone() && Missing->IsDone(); }));
	TestTrue(TEXT("a relative URL without a base is Kind Config"), Relative->Error.Kind == EVaRestErrorKind::Config && Relative->Error.Message.Contains(TEXT("absolute")));
	TestTrue(TEXT("a missing path parameter is Kind Config, named"), Missing->Error.Kind == EVaRestErrorKind::Config && Missing->Error.Message.Contains(TEXT("{id}")));
	TestEqual(TEXT("the transport never saw them"), NoBase.Fake->NumCalls() + Rig.Fake->NumCalls(), 0);
	TestFalse(TEXT("Config errors are not retryable"), Missing->Error.bRetryable);

	// Review M12: only the path is a template; a brace in the query is the caller's text
	auto Search = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/items/{id}?fields={a,b}")).Path(TEXT("id"), 7), Search).Send();
	TestTrue(TEXT("completes"), Rig.Wait(Search));
	TestTrue(TEXT("a brace in the query is no path parameter"), Search->SuccessCalls == 1
		&& Rig.Fake->GetCall(0).Request.Url.Equals(TEXT("https://api.test/items/7?fields={a,b}"), ESearchCase::CaseSensitive));

	// Decision 33 (review M22): a base URL may carry a query, not a fragment
	AddExpectedErrorPlain(TEXT("has a fragment"), EAutomationExpectedErrorFlags::Contains, 1);
	FTestRig Fragment(TEXTVIEW("https://api.test/v1#x"));
	auto Refused = MakeOutcome<FVaRestJson>();
	Observe(Fragment.Client.Get(TEXT("/a")), Refused).Send();
	TestTrue(TEXT("completes"), Fragment.Wait(Refused));
	TestTrue(TEXT("a base URL with a fragment: Kind Config, never sent"), Refused->Error.Kind == EVaRestErrorKind::Config && Fragment.Fake->NumCalls() == 0);
	FTestRig Keyed(TEXTVIEW("https://api.test/v1?key=k"));
	auto WithKey = MakeOutcome<FVaRestJson>();
	Observe(Keyed.Client.Get(TEXT("/pets?limit=2")).Query(TEXT("page"), 3), WithKey).Send();
	TestTrue(TEXT("completes"), Keyed.Wait(WithKey));
	TestEqualSensitive(TEXT("the base query first, then the request's own pairs"), Keyed.Fake->GetCall(0).Request.Url,
		FString(TEXT("https://api.test/v1/pets?key=k&limit=2&page=3")));

	// Task 3 carry (review I2): a non-ASCII host needs punycode, which is out of scope; curl would send it as '?'
	// (TCHAR_TO_ANSI, CurlHttp.cpp:926), so it is refused before any transport call. The path may be non-ASCII: JoinUrl encodes it.
	FTestRig Idn(TEXTVIEW("https://bücher.test"));
	auto NonAscii = MakeOutcome<FVaRestJson>();
	Observe(Idn.Client.Get(TEXT("/a")), NonAscii).Send();
	auto AbsoluteNonAscii = MakeOutcome<FVaRestJson>();
	Observe(Idn.Client.Get(TEXT("https://éx.test/a")), AbsoluteNonAscii).Send();
	auto PathNonAscii = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/café")), PathNonAscii).Send();
	TestTrue(TEXT("complete"), PumpUntil([NonAscii, AbsoluteNonAscii, PathNonAscii] { return NonAscii->IsDone() && AbsoluteNonAscii->IsDone() && PathNonAscii->IsDone(); }));
	TestTrue(TEXT("a non-ASCII host in the base URL: Kind Config, named"), NonAscii->Error.Kind == EVaRestErrorKind::Config
		&& NonAscii->Error.Message.Contains(TEXT("host")) && !NonAscii->Error.bRetryable);
	TestTrue(TEXT("a non-ASCII host in an absolute request URL: Kind Config"), AbsoluteNonAscii->Error.Kind == EVaRestErrorKind::Config);
	TestEqual(TEXT("... never sent"), Idn.Fake->NumCalls(), 0);
	TestTrue(TEXT("a non-ASCII path is no host problem: sent"), PathNonAscii->SuccessCalls == 1);
	if (!TestEqual(TEXT("... the second call of this rig"), Rig.Fake->NumCalls(), 2))
	{
		return false;
	}
	TestEqualSensitive(TEXT("... percent-encoded (RFC 3987 3.1)"), Rig.Fake->GetCall(1).Request.Url, FString(TEXT("https://api.test/caf%C3%A9")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPipelineControlInAuthorityTest, "VaRest.Http.Pipeline.ControlInAuthority",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPipelineControlInAuthorityTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	// Task 14 fix round 2: JoinUrl percent-encodes CTLs and spaces only after the authority (VaRestUrl.cpp:146), so one inside
	// it -- an absolute URL a server handed back, or a base URL -- is refused as Config before any transport call, as a
	// non-ASCII host is, instead of failing in curl without a reason
	FTestRig Rig;
	auto Injected = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("https://api.test\r\nX-Evil: 1/x")), Injected).Send();
	FTestRig Spaced(TEXTVIEW("https://api test"));
	auto BaseSpace = MakeOutcome<FVaRestJson>();
	Observe(Spaced.Client.Get(TEXT("/a")), BaseSpace).Send();
	auto PathControl = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a\tb")), PathControl).Send();
	TestTrue(TEXT("complete"), PumpUntil([Injected, BaseSpace, PathControl] { return Injected->IsDone() && BaseSpace->IsDone() && PathControl->IsDone(); }));
	TestTrue(TEXT("CR LF in an absolute URL's authority: Kind Config, named"), Injected->Error.Kind == EVaRestErrorKind::Config
		&& Injected->Error.Message.Contains(TEXT("authority")) && !Injected->Error.bRetryable);
	TestTrue(TEXT("a space in the base URL's authority: Kind Config"), BaseSpace->Error.Kind == EVaRestErrorKind::Config);
	TestEqual(TEXT("... neither was sent"), Spaced.Fake->NumCalls(), 0);
	TestTrue(TEXT("a control in the path is no authority problem: sent"), PathControl->SuccessCalls == 1);
	if (!TestEqual(TEXT("... the only call of this rig"), Rig.Fake->NumCalls(), 1))
	{
		return false;
	}
	TestEqualSensitive(TEXT("... percent-encoded"), Rig.Fake->GetCall(0).Request.Url, FString(TEXT("https://api.test/a%09b")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPipelineDroppedCompletionTest, "VaRest.Http.Pipeline.DroppedCompletion",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPipelineDroppedCompletionTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	// Review I-3: the engine destroys completions uncalled at HTTP module shutdown and with NullHttp
	FTestRig Rig;
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/lost")), Outcome).Send();
	TestTrue(TEXT("dispatched"), PumpUntil([&Rig] { return Rig.Fake->NumInFlight() == 1; }));
	Rig.Fake->Drop(0);
	TestTrue(TEXT("the request still finishes"), Rig.Wait(Outcome));
	TestTrue(TEXT("... with one OnError and one OnComplete"), Outcome->ErrorCalls == 1 && Outcome->CompleteCalls == 1 && Outcome->SuccessCalls == 0);
	TestTrue(TEXT("... as a dropped transport call"), Outcome->Error.Kind == EVaRestErrorKind::Connection && Outcome->Error.Message.Contains(TEXT("TransportDropped")));
	TestEqual(TEXT("nothing left in flight"), Rig.Client.GetInFlightCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPipelineOutcomeTwiceTest, "VaRest.Http.Pipeline.OutcomeTwice",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPipelineOutcomeTwiceTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::PipelineTestPrivate;
	// Review C-3: a stage that reports its outcome twice -- the second report is ignored, nothing crashes
	FTestRig Rig;
	FLog Log = MakeShared<TArray<FString>, ESPMode::ThreadSafe>();
	Rig.Client.Use(MakeShared<FRecordingStage, ESPMode::ThreadSafe>(TEXT("twice"), Log, FRecordingStage::EMode::Twice), EVaRestStageSlot::Outer);
	Rig.Fake->Enqueue(Json(200, R"({"ok":true})"));
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/twice")), Outcome).Send();
	TestTrue(TEXT("completes"), Rig.Wait(Outcome));
	PumpUntil([] { return false; }, 0.05);
	TestTrue(TEXT("one OnSuccess, one OnComplete"), Outcome->SuccessCalls == 1 && Outcome->CompleteCalls == 1 && Outcome->ErrorCalls == 0);
	return true;
}

#endif
