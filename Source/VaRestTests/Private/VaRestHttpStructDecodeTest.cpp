// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestClient.h"
#include "VaRestEnvelope.h"
#include "VaRestHttpTestTypes.h"
#include "VaRestHttpTesting.h"

namespace VaRestTestsStructDecodePrivate
{
	FVaRestFakeResponse Json(int32 Status, const char* Text)
	{
		return FVaRestFakeResponse::Json(Status, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text)));
	}

	const char* const Profile = R"({"id":9007199254740993,"name":"neo","items":[{"name":"a","level":1},{"name":"b","level":"2"}]})";
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpStructDecodeTest, "VaRest.Http.Decode.Struct",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpStructDecodeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsStructDecodePrivate;
	FTestRig Rig;
	Rig.Fake->Enqueue(Json(200, Profile));
	auto Outcome = MakeOutcome<FVaRestHttpTestProfile>();
	// Review M9: process-wide counters -- let an earlier test's late tasks land before the baseline
	FlushRedactionPipe();
	PumpUntil([] { return false; }, 0.05);
	const int32 OffBefore = Counters().DecodesOffGameThread;
	const int32 OnBefore = Counters().DecodesOnGameThread;
	Observe(Rig.Client.Get<FVaRestHttpTestProfile>(TEXT("/me")), Outcome).Send();
	TestTrue(TEXT("completes"), Rig.Wait(Outcome));
	TestTrue(TEXT("decoded"), Outcome->SuccessCalls == 1 && Outcome->Value.IsSet());
	// Not GetValue() after a non-fatal check: an unset value would assert and end the whole run
	const FVaRestHttpTestProfile Unset;
	const FVaRestHttpTestProfile& P = Outcome->Value.Get(Unset);
	TestEqual(TEXT("int64 past 2^53 stays exact"), P.Id, 9007199254740993ll);
	TestEqualSensitive(TEXT("names match tolerantly (spec 2.4)"), P.Name, FString(TEXT("neo")));
	TestTrue(TEXT("arrays of structs"), P.Items.Num() == 2 && P.Items[1].Level == 2);
	TestEqual(TEXT("a field the JSON lacks keeps its default"), P.Score, 7);
	TestTrue(TEXT("a lenient coercion is a warning with its JSON path (spec 2.4)"),
		Outcome->Response.DecodeWarnings.ContainsByPredicate([](const FVaRestJsonError& W) { return W.Path == TEXT("items[1].level"); }));
	// Spec 1: "Parsing, decoding ... run on UE::Tasks background tasks"
	TestEqual(TEXT("the decode ran off the game thread"), Counters().DecodesOffGameThread - OffBefore, 1);
	TestEqual(TEXT("... and not on it"), Counters().DecodesOnGameThread - OnBefore, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpStructStrictTest, "VaRest.Http.Decode.StructErrors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpStructStrictTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsStructDecodePrivate;
	FTestRig Rig;
	Rig.Fake->Enqueue(Json(200, Profile));
	Rig.Fake->Enqueue(Json(200, "[1,2]"));
	auto Strict = MakeOutcome<FVaRestHttpTestProfile>();
	auto NotObject = MakeOutcome<FVaRestHttpTestProfile>();
	Observe(Rig.Client.Get<FVaRestHttpTestProfile>(TEXT("/me")).StrictDecode(), Strict).Send();
	Observe(Rig.Client.Get<FVaRestHttpTestProfile>(TEXT("/list")), NotObject).Send();
	TestTrue(TEXT("complete"), PumpUntil([&] { return Strict->IsDone() && NotObject->IsDone(); }));
	TestTrue(TEXT("strict: a mismatch is a Decode error"), Strict->Error.Kind == EVaRestErrorKind::Decode);
	TestEqualSensitive(TEXT("... with the value's JSON path"), Strict->Error.Path, FString(TEXT("items[1].level")));
	TestTrue(TEXT("... and the parsed body"), Strict->Error.Body.IsObject());
	TestFalse(TEXT("... and no value"), Strict->Value.IsSet());
	TestTrue(TEXT("an array for a struct is a Decode error"), NotObject->Error.Kind == EVaRestErrorKind::Decode);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpEmptyBodiesTest, "VaRest.Http.Decode.EmptyBodies",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpEmptyBodiesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsStructDecodePrivate;
	FTestRig Rig;
	Rig.Fake->Enqueue(Json(200, ""));
	Rig.Fake->Enqueue(Json(200, ""));
	// Review M3: a 204 with a stray body, so this case rests on the 204 rule (spec 3.2), not on an empty body
	Rig.Fake->Enqueue(Json(204, "garbage"));
	auto JsonEmpty = MakeOutcome<FVaRestJson>();
	auto StructEmpty = MakeOutcome<FVaRestHttpTestProfile>();
	auto NoContent = MakeOutcome<FVaRestHttpTestProfile>();
	Observe(Rig.Client.Get(TEXT("/a")), JsonEmpty).Send();
	Observe(Rig.Client.Get<FVaRestHttpTestProfile>(TEXT("/b")), StructEmpty).Send();
	Observe(Rig.Client.Delete<FVaRestHttpTestProfile>(TEXT("/c")), NoContent).Send();
	TestTrue(TEXT("complete"), PumpUntil([&] { return JsonEmpty->IsDone() && StructEmpty->IsDone() && NoContent->IsDone(); }));
	// Spec 3.2: "204/empty -> T default" -- a 200 with an empty JSON body included
	TestTrue(TEXT("an empty 200 JSON body: success with an invalid handle"), JsonEmpty->SuccessCalls == 1 && JsonEmpty->Value.IsSet() && !JsonEmpty->Value->IsValid());
	TestTrue(TEXT("an empty 200 struct body: success with the defaults"), StructEmpty->SuccessCalls == 1 && StructEmpty->Value.IsSet() && StructEmpty->Value->Score == 7);
	TestTrue(TEXT("204: success with the defaults, whatever its body"), NoContent->SuccessCalls == 1 && NoContent->Value.IsSet() && NoContent->Value->Score == 7);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpEnvelopeStructTest, "VaRest.Http.Envelope.Struct",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpEnvelopeStructTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsStructDecodePrivate;
	FTestRig Rig;
	Rig.Client.WithEnvelope(FVaRestEnvelope::DataAt(TEXT("data")).SuccessWhen(TEXT("ok"), { TEXT("true") }));
	Rig.Fake->Enqueue(Json(200, R"({"ok":true,"data":{"id":5,"name":"neo"}})"));
	Rig.Fake->Enqueue(Json(200, R"({"ok":true})"));
	auto Wrapped = MakeOutcome<FVaRestHttpTestProfile>();
	auto Missing = MakeOutcome<FVaRestHttpTestProfile>();
	Observe(Rig.Client.Get<FVaRestHttpTestProfile>(TEXT("/a")), Wrapped).Send();
	Observe(Rig.Client.Get<FVaRestHttpTestProfile>(TEXT("/b")), Missing).Send();
	if (!TestTrue(TEXT("both complete"), PumpUntil([Wrapped, Missing] { return Wrapped->IsDone() && Missing->IsDone(); })))
	{
		// Spec review M-5: drained before the early return
		Rig.Client.CancelAll();
		PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });
		return false;
	}
	const FVaRestHttpTestProfile Unset;
	TestTrue(TEXT("the struct decoded from the data"), Wrapped->SuccessCalls.load() == 1 && Wrapped->Value.Get(Unset).Id == 5);
	// Plan decision 9: a struct names the missing path instead of decoding to its defaults
	TestTrue(TEXT("no data for a struct: Kind Decode at the data path"), Missing->Error.Kind == EVaRestErrorKind::Decode && Missing->Error.Path == TEXT("data")
		&& Missing->Error.Message.Contains(TEXT("\"data\"")));
	TestTrue(TEXT("... with the body as the error body"), Missing->Error.Body.IsValid() && Missing->Error.Body.Get(TEXT("ok")).AsBool());
	return true;
}

#endif
