// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "VaRestClient.h"
#include "VaRestEnvelope.h"
#include "VaRestHttpTesting.h"
#include "VaRestResultHandler.h"

namespace VaRest::Http::EnvelopeTestPrivate
{
	FVaRestJson Parse(const TCHAR* Text)
	{
		TValueOrError<FVaRestJson, FVaRestJsonError> Parsed = FVaRestJson::Parse(FStringView(Text));
		return Parsed.HasValue() ? Parsed.StealValue() : FVaRestJson();
	}

	/** {"ok": true, "data": ...} with {"code", "message"} on failure: the test server's /envelope routes */
	FVaRestEnvelope OkData()
	{
		return FVaRestEnvelope::DataAt(TEXT("data")).SuccessWhen(TEXT("ok"), { TEXT("true") }).ErrorCodeAt(TEXT("code")).ErrorMessageAt(TEXT("message"));
	}

	FVaRestFakeResponse Json(int32 Status, const char* Text)
	{
		return FVaRestFakeResponse::Json(Status, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text)));
	}

	/** Cancels what the rig has in flight and pumps until it is done: the drain of a test that stops early (spec review M-5) */
	void Abandon(VaRest::Http::Testing::FTestRig& Rig)
	{
		Rig.Client.CancelAll();
		VaRest::Http::Testing::PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpEnvelopeEvaluateTest, "VaRest.Http.Envelope.Evaluate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpEnvelopeEvaluateTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::EnvelopeTestPrivate;
	const FVaRestEnvelope Envelope = OkData();

	const FVaRestEnvelopeResult Ok = Envelope.Evaluate(Parse(TEXT(R"({"ok":true,"data":{"id":7}})")));
	TestTrue(TEXT("a success: the data at DataPath"), Ok.bSuccess && Ok.bDataFound && Ok.Data.Get(TEXT("id")).AsInt() == 7);

	const FVaRestEnvelopeResult No = Envelope.Evaluate(Parse(TEXT(R"({"ok":false,"code":"E42","message":"the envelope says no"})")));
	TestTrue(TEXT("a failed predicate"), !No.bSuccess && !No.Data.IsValid());
	TestEqualSensitive(TEXT("... its code"), No.Code, FString(TEXT("E42")));
	TestEqualSensitive(TEXT("... its message"), No.Message, FString(TEXT("the envelope says no")));
	TestEqualSensitive(TEXT("... what failed"), No.FailedPath + TEXT("=") + No.Actual + TEXT(" not ") + No.Expected, FString(TEXT("ok=false not true")));

	const FVaRestError Error = No.ToError(Parse(TEXT(R"({"ok":false})")), 200);
	TestTrue(TEXT("ToError: Kind Server, the 2xx status, not retryable, the body kept"), Error.Kind == EVaRestErrorKind::Server && Error.HttpStatus == 200
		&& !Error.bRetryable && Error.Body.IsValid() && Error.Code == TEXT("E42"));
	const FVaRestEnvelopeResult Bare = FVaRestEnvelope::DataAt(TEXT("data")).SuccessWhen(TEXT("ok"), { TEXT("true") }).Evaluate(Parse(TEXT(R"({"ok":false})")));
	TestTrue(TEXT("ToError without a message path: the fallback names the predicate"),
		Bare.ToError(FVaRestJson(), 200).Message.Contains(TEXT("ok is false, not true")));

	// dawg-style: a numeric code; compared as text, so 200 and "200" both pass
	const FVaRestEnvelope Dawg = FVaRestEnvelope::DataAt(TEXT("Data")).SuccessWhen(TEXT("Code"), { TEXT("200") });
	TestTrue(TEXT("Code 200"), Dawg.Evaluate(Parse(TEXT(R"({"Code":200,"Data":1})"))).bSuccess);
	TestTrue(TEXT("Code \"200\""), Dawg.Evaluate(Parse(TEXT(R"({"Code":"200","Data":1})"))).bSuccess);
	TestFalse(TEXT("Code 500"), Dawg.Evaluate(Parse(TEXT(R"({"Code":500})"))).bSuccess);

	// Ruling Q3, one row per rendering rule (spec review C-2): string -> its text; bool -> true/false; an integral number ->
	// its decimal, no fraction, no exponent; any other number -> shortest round-trip text; null, object, array never match;
	// case-sensitive
	const FVaRestEnvelope Flag = FVaRestEnvelope().SuccessWhen(TEXT("ok"), { TEXT("true") });
	TestTrue(TEXT("Q3: bool true renders \"true\""), Flag.Evaluate(Parse(TEXT(R"({"ok":true})"))).bSuccess);
	TestTrue(TEXT("Q3: the string \"true\" is its text"), Flag.Evaluate(Parse(TEXT(R"({"ok":"true"})"))).bSuccess);
	TestFalse(TEXT("Q3: case-sensitive -- \"True\" is not \"true\""), Flag.Evaluate(Parse(TEXT(R"({"ok":"True"})"))).bSuccess);
	TestTrue(TEXT("Q3: 200.0 is integral: \"200\""), Dawg.Evaluate(Parse(TEXT(R"({"Code":200.0})"))).bSuccess);
	TestTrue(TEXT("Q3: 2e2 is integral: \"200\""), Dawg.Evaluate(Parse(TEXT(R"({"Code":2e2})"))).bSuccess);
	TestTrue(TEXT("Q3: a non-integral number is its shortest text"),
		FVaRestEnvelope().SuccessWhen(TEXT("v"), { TEXT("1.5") }).Evaluate(Parse(TEXT(R"({"v":1.5})"))).bSuccess);
	TestFalse(TEXT("Q3: null never matches, not even \"null\""),
		FVaRestEnvelope().SuccessWhen(TEXT("ok"), { TEXT("null") }).Evaluate(Parse(TEXT(R"({"ok":null})"))).bSuccess);
	TestFalse(TEXT("Q3: an object never matches, not even \"{}\""),
		FVaRestEnvelope().SuccessWhen(TEXT("ok"), { TEXT("{}") }).Evaluate(Parse(TEXT(R"({"ok":{}})"))).bSuccess);
	TestFalse(TEXT("Q3: an array never matches, not even \"[]\""),
		FVaRestEnvelope().SuccessWhen(TEXT("ok"), { TEXT("[]") }).Evaluate(Parse(TEXT(R"({"ok":[]})"))).bSuccess);
	TestEqualSensitive(TEXT("the shown value is the body's own (IN-5 Actual)"),
		Dawg.Evaluate(Parse(TEXT(R"({"Code":5e2})"))).Actual, FString(TEXT("500.0")));

	// Review Focus: paths that miss
	const FVaRestEnvelopeResult NoData = Envelope.Evaluate(Parse(TEXT(R"({"ok":true})")));
	TestTrue(TEXT("no data: still a success, the data missing"), NoData.bSuccess && !NoData.bDataFound && !NoData.Data.IsValid());
	const FVaRestEnvelopeResult NoOk = Envelope.Evaluate(Parse(TEXT(R"({"data":1})")));
	TestTrue(TEXT("no predicate value: a failure, the value '(missing)'"), !NoOk.bSuccess && NoOk.Actual == TEXT("(missing)"));
	TestFalse(TEXT("an array body: the predicate misses"), Envelope.Evaluate(Parse(TEXT("[1,2]"))).bSuccess);
	TestFalse(TEXT("ok = 0"), Envelope.Evaluate(Parse(TEXT(R"({"ok":0})"))).bSuccess);
	TestFalse(TEXT("ok = \"false\""), Envelope.Evaluate(Parse(TEXT(R"({"ok":"false"})"))).bSuccess);
	TestFalse(TEXT("an invalid handle"), Envelope.Evaluate(FVaRestJson()).bSuccess);
	const FVaRestEnvelopeResult Plain = FVaRestEnvelope().Evaluate(Parse(TEXT("[1,2]")));
	TestTrue(TEXT("an empty envelope: the whole body"), Plain.bSuccess && Plain.Data.Num() == 2);

	// Path syntax is FVaRestJson::Path's: dotted with brackets, or a JSON pointer
	TestEqual(TEXT("dotted with brackets"), FVaRestEnvelope::DataAt(TEXT("result.items[1]")).Evaluate(Parse(TEXT(R"({"result":{"items":[5,6]}})"))).Data.AsInt(), 6);
	TestEqual(TEXT("a JSON pointer"), FVaRestEnvelope::DataAt(TEXT("/a.b/c")).Evaluate(Parse(TEXT(R"({"a.b":{"c":3}})"))).Data.AsInt(), 3);

	FString Code;
	FString Message;
	FVaRestEnvelope::DataAt(TEXT("")).ErrorCodeAt(TEXT("error.code")).ErrorMessageAt(TEXT("error.message")).ReadErrorFields(
		Parse(TEXT(R"({"error":{"code":42,"message":"gone"}})")), Code, Message);
	TestEqualSensitive(TEXT("ReadErrorFields: a numeric code as text"), Code, FString(TEXT("42")));
	TestEqualSensitive(TEXT("... the message"), Message, FString(TEXT("gone")));
	FVaRestEnvelope::DataAt(TEXT("")).ErrorCodeAt(TEXT("code")).ReadErrorFields(Parse(TEXT(R"({"code":42.0})")), Code, Message);
	TestEqualSensitive(TEXT("... an integral double code reads as Q3 renders it"), Code, FString(TEXT("42")));

	// Task 10 review minor 1: an integral double outside [-2^63, 2^64) is one of Q3's "other numbers" -- the writer's
	// shortest round-trip text -- never %.0f's exact binary expansion (1e300 as "1000000000000000052504...")
	const FVaRestJson Huge = Parse(TEXT(R"({"Code":1e300})"));
	TestEqualSensitive(TEXT("Q3: 1e300 is past 64 bits: the writer's text"), FVaRestEnvelope::MatchText(Huge.Get(TEXT("Code"))).Get(FString()), FString(TEXT("1e300")));
	TestTrue(TEXT("Q3: ... so {\"1e300\"} accepts it"), FVaRestEnvelope().SuccessWhen(TEXT("Code"), { TEXT("1e300") }).Evaluate(Huge).bSuccess);
	TestFalse(TEXT("Q3: ... and the %.0f expansion does not"),
		FVaRestEnvelope().SuccessWhen(TEXT("Code"), { FString::Printf(TEXT("%.0f"), 1e300) }).Evaluate(Huge).bSuccess);
	TestEqualSensitive(TEXT("Q3: 2^63 as a double is in uint64's range: its decimal"),
		FVaRestEnvelope::MatchText(Parse(TEXT(R"({"v":9.223372036854775808e18})")).Get(TEXT("v"))).Get(FString()), FString(TEXT("9223372036854775808")));
	TestEqualSensitive(TEXT("Q3: 2^64 as a double is past it: the writer's text"),
		FVaRestEnvelope::MatchText(Parse(TEXT(R"({"v":1.8446744073709551616e19})")).Get(TEXT("v"))).Get(FString()), FString(TEXT("18446744073709552000.0")));

	// Task 10 review minor 2: a message path holding an object or an array falls back to condensed JSON, capped like every
	// shown value; a string message is the server's user-facing text and stays whole
	const FString LongText = FString::ChrN(300, TEXT('m'));
	const FVaRestEnvelope MessageAt = FVaRestEnvelope().ErrorMessageAt(TEXT("message"));
	MessageAt.ReadErrorFields(Parse(*FString::Printf(TEXT(R"({"message":{"detail":"%s"}})"), *LongText)), Code, Message);
	TestTrue(TEXT("ReadErrorFields: an object message is capped"), Message.Len() == 256 + 3 && Message.StartsWith(TEXT("{\"detail\":\"mmm")) && Message.EndsWith(TEXT("...")));
	MessageAt.ReadErrorFields(Parse(*FString::Printf(TEXT(R"({"message":["%s"]})"), *LongText)), Code, Message);
	TestTrue(TEXT("... so is an array message"), Message.Len() == 256 + 3 && Message.StartsWith(TEXT("[\"mmm")) && Message.EndsWith(TEXT("...")));
	MessageAt.ReadErrorFields(Parse(*FString::Printf(TEXT(R"({"message":"%s"})"), *LongText)), Code, Message);
	TestEqualSensitive(TEXT("... a string message stays whole"), Message, LongText);

	// Task 10 review minor 4: an empty allow-list allows nothing, so every response fails, and Expected says so
	const FVaRestEnvelopeResult NoneAllowed = FVaRestEnvelope().SuccessWhen(TEXT("ok"), {}).Evaluate(Parse(TEXT(R"({"ok":true})")));
	TestFalse(TEXT("an empty SuccessValues fails even ok:true"), NoneAllowed.bSuccess);
	TestEqualSensitive(TEXT("... Expected reads (none)"), NoneAllowed.Expected, FString(TEXT("(none)")));
	TestTrue(TEXT("... and so does the fallback message"), NoneAllowed.ToError(FVaRestJson(), 200).Message.EndsWith(TEXT("(ok is true, not (none))")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpEnvelopePipelineTest, "VaRest.Http.Envelope.Pipeline",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpEnvelopePipelineTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::EnvelopeTestPrivate;
	FTestRig Rig;
	Rig.Client.WithEnvelope(OkData().ErrorCodeAt(TEXT("error.code")).ErrorMessageAt(TEXT("error.message")).ParseErrorBodies());
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request)
	{
		const FString& Url = Request.Url;
		if (Url.EndsWith(TEXT("/no")) || Url.EndsWith(TEXT("/void-no")))
		{
			return Json(200, R"({"ok":false,"error":{"code":"E42","message":"the envelope says no"}})");
		}
		if (Url.EndsWith(TEXT("/gone")))
		{
			return Json(404, R"({"error":{"code":"NF","message":"gone"}})");
		}
		if (Url.EndsWith(TEXT("/text")))
		{
			return FVaRestFakeResponse::Text(200, TEXT("plain"));
		}
		if (Url.EndsWith(TEXT("/empty")))
		{
			FVaRestFakeResponse NoContent;
			NoContent.Status = 204;
			return NoContent;
		}
		return Json(200, R"({"ok":true,"data":{"id":7}})");
	});
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	auto Ok = MakeOutcome<FVaRestJson>();
	auto No = MakeOutcome<FVaRestJson>();
	auto Gone = MakeOutcome<FVaRestJson>();
	auto Raw = MakeOutcome<FVaRestJson>();
	auto Narrow = MakeOutcome<FVaRestJson>();
	auto VoidNo = MakeOutcome<void>();
	auto VoidOk = MakeOutcome<void>();
	auto VoidText = MakeOutcome<void>();
	auto Text = MakeOutcome<FString>();
	auto Empty = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle NoHandle = Observe(Rig.Client.Get(TEXT("/no")), No).Send();
	Observe(Rig.Client.Get(TEXT("/ok")), Ok).Send();
	Observe(Rig.Client.Get(TEXT("/gone")), Gone).Send();
	Observe(Rig.Client.Get(TEXT("/raw")).NoEnvelope(), Raw).Send();
	Observe(Rig.Client.Get(TEXT("/narrow")).Envelope(FVaRestEnvelope::DataAt(TEXT("data.id"))), Narrow).Send();
	const int32 EnvelopeParsesBefore = Counters().EnvelopeParsesOnGameThread.load();
	Observe(Rig.Client.Post<void>(TEXT("/void-no")), VoidNo).Send();
	Observe(Rig.Client.Post<void>(TEXT("/void-ok")), VoidOk).Send();
	Observe(Rig.Client.Post<void>(TEXT("/text")), VoidText).Send();
	Observe(Rig.Client.Get<FString>(TEXT("/ok")), Text).Send();
	Observe(Rig.Client.Get(TEXT("/empty")), Empty).Send();
	if (!TestTrue(TEXT("all complete"), PumpUntil([&] { return Ok->IsDone() && No->IsDone() && Gone->IsDone() && Raw->IsDone() && Narrow->IsDone() && VoidNo->IsDone()
		&& VoidOk->IsDone() && VoidText->IsDone() && Text->IsDone() && Empty->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("unwrapped: the data object"), Ok->SuccessCalls.load() == 1 && Ok->Value.IsSet() && Ok->Value->Get(TEXT("id")).AsInt() == 7);
	TestTrue(TEXT("a failed predicate: Kind Server, status 200, not retryable"), No->Error.Kind == EVaRestErrorKind::Server && No->Error.HttpStatus == 200
		&& !No->Error.bRetryable);
	TestTrue(TEXT("... its code and message from the envelope paths"), No->Error.Code == TEXT("E42") && No->Error.Message == TEXT("the envelope says no"));
	TestTrue(TEXT("... the body kept as the error body"), No->Error.Body.IsValid() && !No->Error.Body.Get(TEXT("ok")).AsBool(true));
	TestTrue(TEXT("bParseErrorBodies: a 404's code and message"), Gone->Error.Kind == EVaRestErrorKind::Http && Gone->Error.HttpStatus == 404
		&& Gone->Error.Code == TEXT("NF") && Gone->Error.Message == TEXT("gone"));
	TestTrue(TEXT("NoEnvelope(): the whole body"), Raw->Value.IsSet() && Raw->Value->Get(TEXT("ok")).AsBool());
	TestTrue(TEXT("a request's own envelope"), Narrow->Value.IsSet() && Narrow->Value->AsInt() == 7);
	TestTrue(TEXT("void with a predicate: a failure is Kind Server"), VoidNo->Error.Kind == EVaRestErrorKind::Server && VoidNo->Error.Code == TEXT("E42"));
	// Spec 3.2: raw bytes "are kept on error for diagnostics"; void parsed a copy, so they are all still there
	TestTrue(TEXT("... its raw bytes kept, as on every error"), VoidNo->Response.Body.Num() > 0 && VoidNo->Error.Body.IsValid());
	TestTrue(TEXT("... a success is a success"), VoidOk->SuccessCalls.load() == 1 && VoidOk->Response.Body.Num() == 0);
	TestTrue(TEXT("... a body that is not JSON is nothing to judge"), VoidText->SuccessCalls.load() == 1);
	// Concurrency review I-6: counted where the parse happens, so a game-thread parse cannot hide behind RunDecode's counter
	TestEqual(TEXT("... and its parse ran off the game thread"), Counters().EnvelopeParsesOnGameThread.load() - EnvelopeParsesBefore, 0);
	TestTrue(TEXT("an FString result is the body as it came"), Text->Value.IsSet() && Text->Value->Contains(TEXT("\"ok\":true")));
	TestTrue(TEXT("204: a success with no value to unwrap"), Empty->SuccessCalls.load() == 1 && Empty->Value.IsSet() && !Empty->Value->IsValid());

	// IN-5: Completed carries the envelope's outcome
	const TArray<FVaRestTraceEvent> Events = Scoped.Recorder->For(NoHandle.GetId());
	const FVaRestTraceEvent* Completed = Events.FindByPredicate([](const FVaRestTraceEvent& Event) { return Event.Kind == EVaRestTraceKind::Completed; });
	if (!TestNotNull(TEXT("Completed"), Completed))
	{
		return false;
	}
	const FVaRestTraceEnvelope& Seen = Completed->Payload.Get<FVaRestTraceCompleted>().Envelope;
	TestTrue(TEXT("IN-5 Envelope: applied, data path, the failed predicate, code and message"), Seen.bApplied && Seen.DataPath == TEXT("data")
		&& Seen.PredicatePath == TEXT("ok") && Seen.Expected == TEXT("true") && Seen.Actual == TEXT("false") && Seen.Code == TEXT("E42")
		&& Seen.Message == TEXT("the envelope says no"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpEnvelopeMissingTest, "VaRest.Http.Envelope.MissingPaths",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpEnvelopeMissingTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::EnvelopeTestPrivate;
	// Review Focus: a data path that misses is a missing value for FVaRestJson (spec 2.2), never a crash or a Server error
	FTestRig Rig;
	Rig.Client.WithEnvelope(FVaRestEnvelope::DataAt(TEXT("data")));
	Rig.Fake->Enqueue(Json(200, R"({"ok":true})"));
	Rig.Fake->Enqueue(Json(200, "[1,2]"));
	auto NoData = MakeOutcome<FVaRestJson>();
	auto Array = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a")), NoData).Send();
	Observe(Rig.Client.Get(TEXT("/b")), Array).Send();
	if (!TestTrue(TEXT("both complete"), PumpUntil([NoData, Array] { return NoData->IsDone() && Array->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("no data: a success with an invalid handle"), NoData->SuccessCalls.load() == 1 && NoData->Value.IsSet() && !NoData->Value->IsValid());
	TestTrue(TEXT("an array body: the same"), Array->SuccessCalls.load() == 1 && Array->Value.IsSet() && !Array->Value->IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpEnvelopeUncheckedTest, "VaRest.Http.Envelope.VoidPredicateUnchecked",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpEnvelopeUncheckedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::EnvelopeTestPrivate;
	// Spec review M-13: a void result whose JSON body cannot be parsed (broken, or over MaxJsonBytes) still succeeds --
	// void decodes nothing -- but says at Basic that its envelope's predicate never ran
	TGuardConsoleVariable<int32> Level(TEXT("VaRest.LogLevel"), -1);
	FTestRig Rig;
	Rig.Client.WithEnvelope(OkData()).WithLogLevel(EVaRestLogLevel::Basic).WithMaxJsonBytes(64);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request)
	{
		if (Request.Url.EndsWith(TEXT("/broken")))
		{
			return Json(200, R"({"ok":)");
		}
		return Json(200, R"({"ok":false,"data":"0123456789012345678901234567890123456789012345678901234567890123456789"})");
	});
	FLogCapture Capture;
	AddExpectedMessagePlain(TEXT("VaRest: the envelope's predicate could not be checked"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);
	auto Broken = MakeOutcome<void>();
	auto Large = MakeOutcome<void>();
	const FVaRestRequestHandle BrokenHandle = Observe(Rig.Client.Post<void>(TEXT("/broken")), Broken).Send();
	const FVaRestRequestHandle LargeHandle = Observe(Rig.Client.Post<void>(TEXT("/large")), Large).Send();
	if (!TestTrue(TEXT("both complete"), PumpUntil([Broken, Large] { return Broken->IsDone() && Large->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("void decodes nothing: both succeed (the large one's ok:false was never read)"), Broken->SuccessCalls.load() == 1 && Large->SuccessCalls.load() == 1);
	TestTrue(TEXT("... and each says its predicate never ran, with its id (IN-16)"),
		Capture.Contains(FString::Printf(TEXT("#%llu VaRest: the envelope's predicate could not be checked"), BrokenHandle.GetId()))
		&& Capture.Contains(FString::Printf(TEXT("#%llu VaRest: the envelope's predicate could not be checked"), LargeHandle.GetId())));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpEnvelopeUntracedTest, "VaRest.Http.Envelope.TraceFieldsOnlyWithSink",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpEnvelopeUntracedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::EnvelopeTestPrivate;
	// Spec review M-14 (IN-11: nothing is built for the trace without a sink): the envelope fills FFinish::EnvelopeTrace
	// only when the mask was set at the finish (bTimeDecode); the request's own result is the same either way
	for (const bool bTraced : { false, true })
	{
		VaRest::Http::FFinish Finish;
		Finish.Envelope = MakeShared<const FVaRestEnvelope, ESPMode::ThreadSafe>(OkData());
		Finish.Response.Status = 200;
		Finish.Response.ContentType = TEXT("application/json");
		Finish.bTimeDecode = bTraced;
		const FUtf8StringView Body(UTF8TEXT(R"({"ok":false,"code":"E1","message":"no"})"));
		Finish.Body.Append(reinterpret_cast<const uint8*>(Body.GetData()), Body.Len());
		FVaRestJson Value;
		const bool bDecoded = VaRest::Http::DecodeJson(Finish, Value);
		const FString Label = bTraced ? TEXT("with a sink") : TEXT("without one");
		TestTrue(Label + TEXT(": a failed predicate is Kind Server with its code"), !bDecoded && Finish.Error.Kind == EVaRestErrorKind::Server && Finish.Error.Code == TEXT("E1"));
		TestEqual(Label + TEXT(": the trace's copy is built only with a sink"), Finish.EnvelopeTrace.bApplied, bTraced);
		TestEqual(Label + TEXT(": ... its strings too"), Finish.EnvelopeTrace.Code.IsEmpty(), !bTraced);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpEnvelopeRetriedTest, "VaRest.Http.Envelope.RetriedOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpEnvelopeRetriedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::EnvelopeTestPrivate;
	// Spec 3.4 / 4.1: the retry decision is made on each attempt's status, before any decode; only the final attempt is
	// decoded, so the envelope runs once, on it, and the Kind=Server it makes there is never retried
	FTestRig Rig;
	FVaRestRetryPolicy Policy = FVaRestRetryPolicy::Exponential(3);
	Policy.Backoff.Jitter = EVaRestJitter::None;
	Rig.Client.WithRetry(Policy).WithEnvelope(OkData().ParseErrorBodies());
	Rig.Fake->Enqueue(Json(503, R"({"ok":false,"code":"BUSY","message":"later"})"));
	Rig.Fake->Enqueue(Json(200, R"({"ok":false,"code":"E42","message":"the envelope says no"})"));
	Rig.Fake->Enqueue(Json(200, R"({"ok":true,"data":1})"));
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	// Process-wide counters: an earlier test's late tasks land before the baseline (review M9)
	Rig.Drain();
	const int32 DecodesBefore = Counters().DecodeTasksLaunched.load();
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/r")), Outcome).Send();
	if (!TestTrue(TEXT("the first attempt, then a backoff"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1 && Rig.Clock->NumPendingTimers() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(0.5);
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestEqual(TEXT("two attempts: the 503 retried, the envelope's failure not"), Rig.Fake->NumCalls(), 2);
	TestTrue(TEXT("... Kind Server from the final attempt, not retryable"), Outcome->Error.Kind == EVaRestErrorKind::Server && Outcome->Error.Code == TEXT("E42")
		&& !Outcome->Error.bRetryable && Outcome->Response.Attempts == 2);
	TestEqual(TEXT("... no retry waits"), Rig.Clock->NumPendingTimers(), 0);
	TestEqual(TEXT("one decode for the logical request"), Counters().DecodeTasksLaunched.load() - DecodesBefore, 1);
	const TArray<FVaRestTraceEvent> Events = Scoped.Recorder->For(Handle.GetId());
	const FVaRestTraceEvent* Completed = Events.FindByPredicate([](const FVaRestTraceEvent& Event) { return Event.Kind == EVaRestTraceKind::Completed; });
	if (!TestNotNull(TEXT("Completed"), Completed))
	{
		return false;
	}
	TestEqualSensitive(TEXT("IN-5: the final attempt's envelope outcome"), Completed->Payload.Get<FVaRestTraceCompleted>().Envelope.Code, FString(TEXT("E42")));
	return true;
}

#endif
