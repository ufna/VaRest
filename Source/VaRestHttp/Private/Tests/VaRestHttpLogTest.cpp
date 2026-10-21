// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "VaRestClient.h"
#include "VaRestHttpTesting.h"

namespace VaRest::Http::LogTestPrivate
{
	FVaRestFakeResponse Json(int32 Status, const char* Text)
	{
		return FVaRestFakeResponse::Json(Status, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text)));
	}

	FString Id(const FVaRestRequestHandle& Handle)
	{
		return FString::Printf(TEXT("#%llu "), Handle.GetId());
	}

	/** Sends and waits for the callbacks and the body lines */
	template <typename T>
	FVaRestRequestHandle Run(VaRest::Http::Testing::FTestRig& Rig, TVaRestRequest<T>&& Request)
	{
		auto Outcome = VaRest::Http::Testing::MakeOutcome<T>();
		const FVaRestRequestHandle Handle = VaRest::Http::Testing::Observe(MoveTemp(Request), Outcome).Send();
		Rig.Wait(Outcome);
		VaRest::Http::Testing::FlushRedactionPipe();
		return Handle;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLogLevelsTest, "VaRest.Http.Log.Levels",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLogLevelsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LogTestPrivate;
	FTestRig Rig;
	FLogCapture Capture;

	Rig.Client.WithLogLevel(EVaRestLogLevel::Basic);
	const FVaRestRequestHandle Basic = Run(Rig, Rig.Client.Get(TEXT("/x?token=abc")).Header(TEXT("Authorization"), TEXT("Bearer abc")));
	TestEqual(TEXT("Basic: one line"), Capture.Count(Id(Basic)), 1);
	TestTrue(TEXT("... verb, redacted URL, status (spec 4.5)"), Capture.Contains(Id(Basic) + TEXT("GET https://api.test/x?token=*** -> 200")));

	Rig.Client.WithLogLevel(EVaRestLogLevel::Headers);
	const FVaRestRequestHandle Headers = Run(Rig, Rig.Client.Get(TEXT("/h")).Header(TEXT("Authorization"), TEXT("Bearer abc")).Header(TEXT("X-Trace"), TEXT("t")));
	TestTrue(TEXT("Headers: the request line"), Capture.Contains(Id(Headers) + TEXT("> GET https://api.test/h (attempt 1)")));
	TestTrue(TEXT("... redacted request headers"), Capture.Contains(Id(Headers) + TEXT(">   Authorization: ***")));
	TestTrue(TEXT("... plain ones as they are"), Capture.Contains(Id(Headers) + TEXT(">   X-Trace: t")));
	TestTrue(TEXT("... the response headers"), Capture.Contains(Id(Headers) + TEXT("<   Content-Type: application/json")));
	TestFalse(TEXT("... no bodies"), Capture.Contains(Id(Headers) + TEXT("< body")));

	Rig.Client.WithLogLevel(EVaRestLogLevel::None);
	const FVaRestRequestHandle None = Run(Rig, Rig.Client.Get(TEXT("/n")));
	TestEqual(TEXT("None: nothing"), Capture.Count(Id(None)), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLogBodyTest, "VaRest.Http.Log.Body",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLogBodyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LogTestPrivate;
	FTestRig Rig;
	FLogCapture Capture;
	Rig.Client.WithLogLevel(EVaRestLogLevel::Body);
	FString Large = TEXT("{\"blob\":\"");
	for (int32 i = 0; i < 10000; ++i)
	{
		Large.AppendChar(TEXT('a' + i % 26));
	}
	Large += TEXT("\"}");
	FTCHARToUTF8 LargeUtf8(*Large);
	Rig.Fake->Enqueue(Json(200, R"({"token":"tok","ok":true})"));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Json(200, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(LargeUtf8.Get()), LargeUtf8.Length())));
	const FVaRestRequestHandle Small = Run(Rig, Rig.Client.Post(TEXT("/login")).Body(FVaRestJson::Object().Set(TEXT("user"), TEXT("neo")).Set(TEXT("password"), TEXT("pw"))));
	TestTrue(TEXT("the request body, JSON keys redacted"), Capture.Contains(Id(Small) + TEXT("> body (json, ")) && Capture.Contains(TEXT("{\"user\":\"neo\",\"password\":\"***\"}")));
	TestTrue(TEXT("the response body, JSON keys redacted"), Capture.Contains(Id(Small) + TEXT("< body (json, ")) && Capture.Contains(TEXT("{\"token\":\"***\",\"ok\":true}")));

	const FVaRestRequestHandle Big = Run(Rig, Rig.Client.Get(TEXT("/big")));
	const TArray<FString> Lines = Capture.GetLines();
	const FString* BodyLine = Lines.FindByPredicate([&Big](const FString& Line) { return Line.Contains(Id(Big) + TEXT("< body")); });
	TestTrue(TEXT("a large body is logged"), BodyLine != nullptr);
	TestTrue(TEXT("... truncated to 4 KiB (spec 4.5)"), BodyLine && BodyLine->Contains(TEXT("truncated")) && BodyLine->Len() < 4096 + 200);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLogCvarTest, "VaRest.Http.Log.CvarOverride",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLogCvarTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LogTestPrivate;
	FTestRig Rig;
	FLogCapture Capture;
	Rig.Client.WithLogLevel(EVaRestLogLevel::Basic);
	FVaRestRequestHandle Silenced;
	{
		// TGuardConsoleVariable restores the previous value when it goes (IConsoleManager.h:1404-1448)
		TGuardConsoleVariable<int32> Guard(TEXT("VaRest.LogLevel"), 0);
		Silenced = Run(Rig, Rig.Client.Get(TEXT("/quiet")));
	}
	TestEqual(TEXT("VaRest.LogLevel 0 silences a Basic client (spec 4.5)"), Capture.Count(Id(Silenced)), 0);
	FVaRestRequestHandle Forced;
	{
		TGuardConsoleVariable<int32> Guard(TEXT("VaRest.LogLevel"), 2);
		Forced = Run(Rig, Rig.Client.Get(TEXT("/loud")));
	}
	TestTrue(TEXT("VaRest.LogLevel 2 raises it to Headers"), Capture.Contains(Id(Forced) + TEXT("> GET")));
	const FVaRestRequestHandle Own = Run(Rig, Rig.Client.Get(TEXT("/own")));
	TestEqual(TEXT("-1 again: the client's own level"), Capture.Count(Id(Own)), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLogCanariesTest, "VaRest.Http.Log.Canaries",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLogCanariesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LogTestPrivate;
	FTestRig Rig;
	FLogCapture Capture;
	AddExpectedMessagePlain(TEXT("-> 500"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	const TArray<FString> Added = { TEXT("player_pin") };
	Rig.Client.WithLogLevel(EVaRestLogLevel::Body).WithRedactedKeys(Added);
	const TCHAR* Bearer = TEXT("canary-bearer-0123456789");
	Rig.Client.SetSecretValue(TEXT("bearer"), Bearer);

	FVaRestFakeResponse Error = Json(500, R"({"refresh_token":"canary-refresh","msg":"bad token canary-bearer-0123456789"})");
	Error.Headers.Add(TEXT("Location"), TEXT("https://app.test/cb?code=canary-code&state=1"));
	Error.Headers.Add(TEXT("Set-Cookie"), TEXT("sid=canary-cookie"));
	Rig.Fake->Enqueue(Error);
	Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200, TEXT("echo canary-bearer-0123456789 echo")));
	Run(Rig, Rig.Client.Post(FString::Printf(TEXT("/u/%s/x?token=canary-query&page=1"), Bearer))
		.Header(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), Bearer))
		.Body(FVaRestJson::Object().Set(TEXT("password"), TEXT("canary-password")).Set(TEXT("playerPin"), TEXT("canary-pin")).Set(TEXT("note"), TEXT("hi"))));
	Run(Rig, Rig.Client.Get<FString>(TEXT("/echo")));

	int32 Leaks = 0;
	for (const FString& Line : Capture.GetLines())
	{
		if (Line.Contains(TEXT("canary"), ESearchCase::CaseSensitive))
		{
			++Leaks;
			AddInfo(FString::Printf(TEXT("leaked: %s"), *Line));
		}
	}
	// Every secret class of Inspector 2.2: header, query key, userinfo-free path value, JSON key, an added key,
	// the known-secret scrub in a path, a 5xx JSON body, a Location header, a Set-Cookie, a text/plain body
	TestEqual(TEXT("no canary in any log line at Body level"), Leaks, 0);
	TestTrue(TEXT("... and the lines were written"), Capture.Contains(TEXT("< body (json")) && Capture.Contains(TEXT("< body (text")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLogIdTest, "VaRest.Http.Log.RequestIdOnEveryLine",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLogIdTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LogTestPrivate;
	FTestRig Rig;
	FLogCapture Capture;
	// Registered before the line is logged: an expected message is matched as it arrives
	AddExpectedMessagePlain(TEXT("-> 404"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	Rig.Client.WithLogLevel(EVaRestLogLevel::Body);
	Rig.Fake->Enqueue(Json(404, R"({"error":"x"})"));
	const FVaRestRequestHandle Handle = Run(Rig, Rig.Client.Post(TEXT("/missing")).Header(TEXT("X-A"), TEXT("1")).Body(TEXT("hello")));
	int32 Lines = 0;
	int32 Tagged = 0;
	for (const FString& Line : Capture.GetLines())
	{
		if (Line.StartsWith(TEXT("LogVaRestHttp: ")))
		{
			++Lines;
			Tagged += Line.Contains(Id(Handle)) ? 1 : 0;
		}
	}
	TestTrue(TEXT("several lines at Body level"), Lines >= 6);
	TestEqual(TEXT("IN-16: every core log line about a request includes #<RequestId>"), Tagged, Lines);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLogRotationTest, "VaRest.Http.Log.SecretRotation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLogRotationTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LogTestPrivate;
	FTestRig Rig;
	FLogCapture Capture;
	Rig.Client.WithLogLevel(EVaRestLogLevel::Basic);
	Rig.Client.SetSecretValue(TEXT("bearer"), TEXT("token-one-abcdefgh"));
	Rig.Client.SetSecretValue(TEXT("bearer"), TEXT("token-two-abcdefgh"));
	const FVaRestRequestHandle Rotated = Run(Rig, Rig.Client.Get(TEXT("/p/token-one-abcdefgh")));
	TestTrue(TEXT("a rotated-out value is still scrubbed (Inspector D21)"), Capture.Contains(Id(Rotated) + TEXT("GET https://api.test/p/*** ")));
	Rig.Clock->Advance(FVaRestSecretSet::PreviousLifetime);
	TestEqual(TEXT("the expiry timer fired once and left nothing armed (review C1)"), Rig.Clock->NumPendingTimers(), 0);
	const FVaRestRequestHandle Expired = Run(Rig, Rig.Client.Get(TEXT("/p/token-one-abcdefgh")));
	TestTrue(TEXT("... until its 10 minutes are up"), Capture.Contains(Id(Expired) + TEXT("GET https://api.test/p/token-one-abcdefgh ")));
	Rig.Client.ClearSecretValues();
	const FVaRestRequestHandle Cleared = Run(Rig, Rig.Client.Get(TEXT("/p/token-two-abcdefgh")));
	TestTrue(TEXT("ClearSecretValues forgets the current value too (logout)"), Capture.Contains(Id(Cleared) + TEXT("GET https://api.test/p/token-two-abcdefgh ")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLogExpiryOffGridTest, "VaRest.Http.Log.SecretExpiryOffGrid",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLogExpiryOffGridTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LogTestPrivate;
	// Reviews N-2, N-I3: rotated at 532.979068556 s, the old value expires at 1132.9790685560001 in doubles, while the
	// clock, in whole nanoseconds, fires the expiry timer at 1132.979068556 -- one ulp early. About one rotation instant
	// in ten does this; SecretRotation rotates at 0, where the sums are exact.
	FTestRig Rig;
	FLogCapture Capture;
	Rig.Client.WithLogLevel(EVaRestLogLevel::Basic);
	Rig.Clock->Advance(532.979068556);
	Rig.Client.SetSecretValue(TEXT("bearer"), TEXT("token-one-abcdefgh"));
	Rig.Client.SetSecretValue(TEXT("bearer"), TEXT("token-two-abcdefgh"));
	TestEqual(TEXT("one expiry timer armed"), Rig.Clock->NumPendingTimers(), 1);
	Rig.Clock->Advance(FVaRestSecretSet::PreviousLifetime); // returns: the expiry does not re-arm itself at a zero delay
	TestEqual(TEXT("the expiry fired once and left nothing armed"), Rig.Clock->NumPendingTimers(), 0);
	const FVaRestRequestHandle Expired = Run(Rig, Rig.Client.Get(TEXT("/p/token-one-abcdefgh")));
	TestTrue(TEXT("... and the expired value is no longer scrubbed"), Capture.Contains(Id(Expired) + TEXT("GET https://api.test/p/token-one-abcdefgh ")));
	return true;
}

#endif
