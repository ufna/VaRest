// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestClient.h"
#include "VaRestEnvelope.h"

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
	return true;
}

#endif
