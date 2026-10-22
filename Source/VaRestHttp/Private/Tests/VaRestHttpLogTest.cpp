// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "VaRestClient.h"
#include "VaRestClientState.h"
#include "VaRestHttpLog.h"
#include "VaRestHttpTesting.h"
#include "VaRestTrace.h"

namespace VaRest::Http::LogTestPrivate
{
	/** Review I-1: the events a Metadata and Headers sink is handed, for the test to read */
	class FHeadersSink final : public IVaRestTraceSink
	{
	public:
		virtual FVaRestTraceDetailSettings GetDetail() const override
		{
			FVaRestTraceDetailSettings Settings;
			Settings.Flags = EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Headers;
			return Settings;
		}
		virtual void OnAttached(const FVaRestTraceSnapshot&) override {}
		virtual void OnEvent(const FVaRestTraceEvent& Event) override
		{
			FScopeLock Lock(&Mutex);
			Events.Add(Event);
		}

		/** One request's AttemptEnded payloads */
		TArray<FVaRestTraceAttemptEnded> AttemptsEnded(uint64 RequestId) const
		{
			FScopeLock Lock(&Mutex);
			TArray<FVaRestTraceAttemptEnded> Out;
			for (const FVaRestTraceEvent& Event : Events)
			{
				if (Event.Kind == EVaRestTraceKind::AttemptEnded && Event.Payload.Get<FVaRestTraceAttemptEnded>().RequestId == RequestId)
				{
					Out.Add(Event.Payload.Get<FVaRestTraceAttemptEnded>());
				}
			}
			return Out;
		}

	private:
		mutable FCriticalSection Mutex;
		TArray<FVaRestTraceEvent> Events;
	};

	FVaRestFakeResponse Json(int32 Status, const char* Text)
	{
		return FVaRestFakeResponse::Json(Status, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text)));
	}

	FString Id(const FVaRestRequestHandle& Handle)
	{
		return FString::Printf(TEXT("#%llu "), Handle.GetId());
	}

	TArray<uint8> BytesOf(const char* Text)
	{
		return TArray<uint8>(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text));
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

	/**
	 * Review M9: VaRest.LogLevel at -1 (each client's own level) for the test, whatever a ConsoleVariables.ini set.
	 * TGuardConsoleVariable restores the previous value when it goes (IConsoleManager.h:1404-1448).
	 */
	struct FLevelIsolation
	{
		TGuardConsoleVariable<int32> Guard{ TEXT("VaRest.LogLevel"), -1 };
	};

	/** LogVaRestHttp's runtime verbosity while it lives (review M2) */
	struct FScopedVerbosity
	{
		explicit FScopedVerbosity(ELogVerbosity::Type Verbosity)
			: Previous(LogVaRestHttp.GetVerbosity())
		{
			LogVaRestHttp.SetVerbosity(Verbosity);
		}
		~FScopedVerbosity()
		{
			LogVaRestHttp.SetVerbosity(Previous);
		}
		const ELogVerbosity::Type Previous;
	};

	/** What the log stage's line would have to resist: a line break, then a whole forged line (review C1) */
	const TCHAR* const Forgery = TEXT("a\r\nFAKE #999 GET http://evil -> 200");

	/** A PerAttempt stage that adds a header whose name and value hold CR LF to a request to .../forged */
	class FForger final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("Forger"); }
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			if (Exchange->GetUrl().EndsWith(TEXT("/forged")))
			{
				Exchange->GetAttemptHeaders().Add(TEXT("X-Bad\r\nFAKE"), Forgery);
			}
			Next.Proceed(Exchange, MoveTemp(OnOutcome));
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLogLevelsTest, "VaRest.Http.Log.Levels",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLogLevelsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LogTestPrivate;
	FLevelIsolation Isolation;
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
	FLevelIsolation Isolation;
	FTestRig Rig;
	FLogCapture Capture;
	Rig.Client.WithLogLevel(EVaRestLogLevel::Body);
	// Review M7: many short values, so the excerpt of a cut body ends within one value of the cap
	FString Large = TEXT("{\"items\":[");
	for (int32 i = 0; i < 2000; ++i)
	{
		Large += i == 0 ? TEXT("\"abcd\"") : TEXT(",\"abcd\"");
	}
	Large += TEXT("]}");
	FTCHARToUTF8 LargeUtf8(*Large);
	Rig.Fake->Enqueue(Json(200, R"({"token":"tok","ok":true})"));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Json(200, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(LargeUtf8.Get()), LargeUtf8.Length())));
	const FVaRestRequestHandle Small = Run(Rig, Rig.Client.Post(TEXT("/login")).Body(FVaRestJson::Object().Set(TEXT("user"), TEXT("neo")).Set(TEXT("password"), TEXT("pw"))));
	TestTrue(TEXT("the request body, JSON keys redacted"), Capture.Contains(Id(Small) + TEXT("> body (json, ")) && Capture.Contains(TEXT("{\"user\":\"neo\",\"password\":\"***\"}")));
	TestTrue(TEXT("the response body, JSON keys redacted"), Capture.Contains(Id(Small) + TEXT("< body (json, ")) && Capture.Contains(TEXT("{\"token\":\"***\",\"ok\":true}")));

	const int64 BeforeBig = Counters().LogBodyBytesCopied.load();
	const FVaRestRequestHandle Big = Run(Rig, Rig.Client.Get(TEXT("/big")));
	TestEqual(TEXT("the game thread copied exactly the cap of the large body (Inspector 7)"), Counters().LogBodyBytesCopied.load() - BeforeBig, static_cast<int64>(VaRest::Http::LogBodyCap));
	const TArray<FString> Lines = Capture.GetLines();
	const FString* BodyLine = Lines.FindByPredicate([&Big](const FString& Line) { return Line.Contains(Id(Big) + TEXT("< body")); });
	if (!TestTrue(TEXT("a large body is logged"), BodyLine != nullptr))
	{
		return false;
	}
	TestTrue(TEXT("... marked truncated"), BodyLine->Contains(TEXT(", truncated): ")));
	const int32 ExcerptAt = BodyLine->Find(TEXT("): "));
	const int32 ExcerptLen = ExcerptAt == INDEX_NONE ? 0 : BodyLine->Len() - ExcerptAt - 3;
	TestTrue(FString::Printf(TEXT("... to 4 KiB (spec 4.5): the excerpt is %d characters"), ExcerptLen), ExcerptLen > VaRest::Http::LogBodyCap - 64 && ExcerptLen <= VaRest::Http::LogBodyCap);

	// Review M3: multipart fields are copied up to the cap (plus the longest needle; this client has no secret) in all
	FString Huge;
	for (int32 i = 0; i < 20000; ++i)
	{
		Huge.AppendChar(TEXT('x'));
	}
	const int64 BeforeParts = Counters().LogBodyBytesCopied.load();
	const FVaRestRequestHandle Parts = Run(Rig, Rig.Client.Post(TEXT("/parts")).Body(FVaRestMultipart().AddField(TEXT("small"), TEXT("s")).AddField(TEXT("huge"), Huge).AddField(TEXT("after"), TEXT("a"))));
	TestEqual(TEXT("multipart: the cap copied for the fields, then the 2-byte response"), Counters().LogBodyBytesCopied.load() - BeforeParts, static_cast<int64>(VaRest::Http::LogBodyCap) + 2);
	TestTrue(TEXT("... and the excerpt shows the parts, truncated"), Capture.Contains(Id(Parts) + TEXT("> body (multipart, ")) && Capture.Contains(TEXT(", truncated): part \"small\": s\\npart \"huge\": xxx")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLogCvarTest, "VaRest.Http.Log.CvarOverride",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLogCvarTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LogTestPrivate;
	FLevelIsolation Isolation;
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
	FLevelIsolation Isolation;
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

	// Review M8: the Inspector 10.1 positions the log stage routes itself -- a multipart text part and a file part, a
	// session and an API key header, X-Amz-Signature, a binary response, URL userinfo, a form field, a password nested in
	// an array and camelCase token keys, and a streamed file upload
	Rig.Fake->Enqueue(FVaRestFakeResponse::Bytes(200, BytesOf("\x0a\x08password\x12\x0e" "canary-protobuf"), TEXT("application/x-protobuf")));
	Run(Rig, Rig.Client.Post<void>(TEXT("/parts?X-Amz-Signature=canary-amz&X-Amz-Date=1"))
		.Header(TEXT("X-Api-Key"), TEXT("canary-apikey"))
		.Header(TEXT("X-Session-Id"), TEXT("canary-session"))
		.Body(FVaRestMultipart().AddField(TEXT("password"), TEXT("canary-mp-password")).AddField(TEXT("note"), TEXT("hi"))
			.AddBytes(TEXT("avatar"), TEXT("a.bin"), TEXT("application/octet-stream"), BytesOf("canary-mp-file"))));
	Rig.Client.WithBaseUrl(TEXT("https://neo:canary-userinfo@api.test"));
	Rig.Fake->Enqueue(Json(200, R"({"items":[{"password":"canary-nested"}],"accessToken":"canary-access","refreshToken":"canary-rt","clientSecret":"canary-cs"})"));
	Run(Rig, Rig.Client.Post(TEXT("/form")).Body(FVaRestForm().Add(TEXT("user"), TEXT("neo")).Add(TEXT("password"), TEXT("canary-form"))));
	const FString Upload = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("VaRestLogTest") / TEXT("upload.bin"));
	if (!TestTrue(TEXT("the upload file is written"), FFileHelper::SaveStringToFile(TEXT("canary-file-content"), *Upload)))
	{
		return false;
	}
	Run(Rig, Rig.Client.Put(TEXT("/file")).BodyFile(Upload));
	IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Upload), false, true);

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
	TestTrue(TEXT("... the multipart, binary, form and stream ones too"), Capture.Contains(TEXT("> body (multipart")) && Capture.Contains(TEXT("< body (binary"))
		&& Capture.Contains(TEXT("> body (form")) && Capture.Contains(TEXT("> body (stream")));
	TestTrue(TEXT("... with the userinfo as ***"), Capture.Contains(TEXT("POST https://***@api.test/form -> 200")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLogIdTest, "VaRest.Http.Log.RequestIdOnEveryLine",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLogIdTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LogTestPrivate;
	FLevelIsolation Isolation;
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLogForgedLinesTest, "VaRest.Http.Log.NoForgedLines",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLogForgedLinesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LogTestPrivate;
	// Review C1: UE writes a message as one block and one terminator (OutputDeviceHelper.cpp:117-138), so a raw CR LF in a
	// value would start a physical line with no timestamp, category or #id -- a forged one, or an IN-16 gap
	FLevelIsolation Isolation;
	FTestRig Rig;
	FLogCapture Capture;
	AddExpectedErrorPlain(TEXT("header \"X-Bad\\r\\nFAKE\" is not a token"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("-> Other InvalidHeader"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("-> Config error: the URL's authority"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	Rig.Client.WithLogLevel(EVaRestLogLevel::Body).Use(MakeShared<FForger, ESPMode::ThreadSafe>(), EVaRestStageSlot::PerAttempt);

	FVaRestFakeResponse Echoed = FVaRestFakeResponse::Text(200, Forgery);
	Echoed.Headers.Add(TEXT("X-Note"), Forgery);
	Rig.Fake->Enqueue(Echoed);
	// Task 14 re-review N1 (P14.1): Run waits for each request's callbacks and its body lines, so the lines captured from one
	// mark to the next are that request's, and each must carry that request's own #id, not any of the four
	const int32 EchoFrom = Capture.GetLines().Num();
	const FVaRestRequestHandle Echo = Run(Rig, Rig.Client.Get<FString>(TEXT("/text")));
	const int32 PartsFrom = Capture.GetLines().Num();
	const FVaRestRequestHandle Parts = Run(Rig, Rig.Client.Post(TEXT("/parts")).Body(FVaRestMultipart().AddField(TEXT("a"), TEXT("1")).AddField(TEXT("b"), Forgery)));
	const int32 ForgedFrom = Capture.GetLines().Num();
	const FVaRestRequestHandle Forged = Run(Rig, Rig.Client.Get(TEXT("/forged")));
	// Fix round 2: an absolute URL -- one a server handed back -- keeps its authority as it is (VaRestUrl.cpp:121, :146).
	// Resolve refuses it (Kind=Config) before the chain, so its Basic line is the Config error's, through the same ShownUrl.
	const int32 RefusedFrom = Capture.GetLines().Num();
	const FVaRestRequestHandle Refused = Run(Rig, Rig.Client.Get(TEXT("https://api.test\r\nFAKE #999 GET http://evil -> 200/x")));

	const TArray<TPair<int32, FString>> Owners = { { EchoFrom, Id(Echo) }, { PartsFrom, Id(Parts) }, { ForgedFrom, Id(Forged) }, { RefusedFrom, Id(Refused) } };
	const TArray<FString> Captured = Capture.GetLines();
	int32 Lines = 0;
	int32 Raw = 0;
	int32 Misattributed = 0;
	TArray<int32> PerRequest = { 0, 0, 0, 0 };
	for (int32 Index = 0; Index < Captured.Num(); ++Index)
	{
		const FString& Line = Captured[Index];
		if (!Line.StartsWith(TEXT("LogVaRestHttp: ")))
		{
			continue;
		}
		++Lines;
		int32 At = INDEX_NONE;
		if (Line.FindChar(TEXT('\r'), At) || Line.FindChar(TEXT('\n'), At))
		{
			++Raw;
			AddInfo(FString::Printf(TEXT("raw line break: %s"), *Line.ReplaceCharWithEscapedChar()));
		}
		// The request whose window holds this line: the last mark at or before it (none before the first)
		int32 Owner = INDEX_NONE;
		for (int32 Request = 0; Request < Owners.Num(); ++Request)
		{
			Owner = Index >= Owners[Request].Key ? Request : Owner;
		}
		const FString Message = Line.RightChop(FCString::Strlen(TEXT("LogVaRestHttp: ")));
		if (Owner == INDEX_NONE || !Message.StartsWith(Owners[Owner].Value, ESearchCase::CaseSensitive))
		{
			++Misattributed;
			AddInfo(FString::Printf(TEXT("not its own request's #id: %s"), *Line.ReplaceCharWithEscapedChar()));
		}
		else
		{
			++PerRequest[Owner];
		}
	}
	TestTrue(TEXT("the four requests wrote their lines"), Lines >= 13);
	TestTrue(TEXT("... each of them"), PerRequest[0] > 0 && PerRequest[1] > 0 && PerRequest[2] > 0 && PerRequest[3] > 0);
	TestEqual(TEXT("no raw CR or LF in any line"), Raw, 0);
	TestEqual(TEXT("IN-16: every line starts with its own request's #<RequestId>"), Misattributed, 0);
	TestTrue(TEXT("a text body, escaped"), Capture.Contains(Id(Echo) + TEXT("< body (text, ")) && Capture.Contains(TEXT("): a\\r\\nFAKE #999 GET http://evil -> 200")));
	TestTrue(TEXT("a response header value, escaped"), Capture.Contains(Id(Echo) + TEXT("<   X-Note: a\\r\\nFAKE #999 GET http://evil -> 200")));
	TestTrue(TEXT("multipart parts on one line"), Capture.Contains(Id(Parts) + TEXT("> body (multipart, ")) && Capture.Contains(TEXT("): part \"a\": 1\\npart \"b\": a\\r\\nFAKE #999")));
	TestTrue(TEXT("a stage's header name and value, escaped"), Capture.Contains(Id(Forged) + TEXT(">   X-Bad\\r\\nFAKE: a\\r\\nFAKE #999")));
	TestTrue(TEXT("a URL's authority, escaped on the Config error's Basic line"), Capture.Contains(Id(Refused) + TEXT("GET https://api.test\\r\\nFAKE #999"))
		&& Capture.Contains(TEXT("-> Config error: the URL's authority")));
	TestEqualSensitive(TEXT("a backslash is escaped too, so no escape can be forged"), VaRest::Http::EscapeForLog(TEXT("a\\r\tb\x1b\x7f\x85\x2028***")),
		FString(TEXT("a\\\\r\tb\\x1B\\x7F\\x85\\u2028***")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLogVerbosityTest, "VaRest.Http.Log.CategoryVerbosity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLogVerbosityTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LogTestPrivate;
	// Review M2: LogVaRestHttp below Log drops the stage's Log lines, so nothing is redacted or copied for them
	FLevelIsolation Isolation;
	FTestRig Rig;
	FLogCapture Capture;
	AddExpectedMessagePlain(TEXT("-> 404"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	Rig.Client.WithLogLevel(EVaRestLogLevel::Body);
	FVaRestRequestHandle Quiet;
	FVaRestRequestHandle Missing;
	int64 Redactions = 0;
	int64 Copied = 0;
	{
		FScopedVerbosity Warnings(ELogVerbosity::Warning);
		const uint64 RedactionsBefore = FVaRestTrace::Counters().RedactorCalls.load();
		const int64 CopiedBefore = Counters().LogBodyBytesCopied.load();
		Quiet = Run(Rig, Rig.Client.Post(TEXT("/quiet?token=abc")).Header(TEXT("Authorization"), TEXT("Bearer abc")).Body(FVaRestJson::Object().Set(TEXT("a"), 1)));
		Redactions = static_cast<int64>(FVaRestTrace::Counters().RedactorCalls.load() - RedactionsBefore);
		Copied = Counters().LogBodyBytesCopied.load() - CopiedBefore;
		Rig.Fake->Enqueue(Json(404, R"({"error":"x"})"));
		Missing = Run(Rig, Rig.Client.Get(TEXT("/gone?token=abc")));
	}
	TestEqual(TEXT("a 200 at Body level, the category at Warning: no redactor call"), Redactions, static_cast<int64>(0));
	TestEqual(TEXT("... no body copied"), Copied, static_cast<int64>(0));
	TestEqual(TEXT("... no line"), Capture.Count(Id(Quiet)), 0);
	TestEqual(TEXT("a 404 still writes its Warning line, alone"), Capture.Count(Id(Missing)), 1);
	TestTrue(TEXT("... with the redacted URL"), Capture.Contains(Id(Missing) + TEXT("GET https://api.test/gone?token=*** -> 404")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLogKeysOnceTest, "VaRest.Http.Log.RedactedKeysOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLogKeysOnceTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LogTestPrivate;
	// Review M4: a key is checked and de-duplicated where it enters, not on each of the republishes that follow
	FLevelIsolation Isolation;
	FTestRig Rig;
	FLogCapture Capture;
	AddExpectedMessagePlain(TEXT("the redaction key \"pin=1\" holds"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	const int32 Before = VaRest::Http::FClientAccess::State(Rig.Client)->GetConfig()->RedactedKeys.Num();
	const TArray<FString> Keys = { TEXT("pin=1"), TEXT("player_pin"), TEXT("playerPin"), TEXT("Player-Pin") };
	Rig.Client.WithRedactedKeys(Keys);
	const TArray<FString> Again = { TEXT("player_pin") };
	Rig.Client.WithRedactedKeys(Again);
	TestEqual(TEXT("one key kept for the spellings of one, the refused one dropped"), VaRest::Http::FClientAccess::State(Rig.Client)->GetConfig()->RedactedKeys.Num(), Before + 1);
	// Each of these publishes again
	Rig.Client.WithLogLevel(EVaRestLogLevel::Body);
	Rig.Client.SetSecretValue(TEXT("bearer"), TEXT("token-abcdefgh"));
	Rig.Client.WithHeader(TEXT("X-A"), TEXT("1"));
	const FVaRestRequestHandle Handle = Run(Rig, Rig.Client.Post(TEXT("/k")).Body(FVaRestJson::Object().Set(TEXT("playerPin"), TEXT("1234")).Set(TEXT("note"), TEXT("hi"))));
	TestTrue(TEXT("the kept key redacts"), Capture.Contains(Id(Handle) + TEXT("> body (json, ")) && Capture.Contains(TEXT("{\"playerPin\":\"***\",\"note\":\"hi\"}")));
	TestEqual(TEXT("the refused key warned once"), Capture.Count(TEXT("the redaction key")), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLogQueueTest, "VaRest.Http.Log.QueueWait",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLogQueueTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LogTestPrivate;
	// Review M5: a Basic line's milliseconds are the attempt's own, from its limiter grant; the queue wait is shown apart
	FLevelIsolation Isolation;
	FTestRig Rig;
	FLogCapture Capture;
	Rig.Client.WithLogLevel(EVaRestLogLevel::Basic).WithMaxConcurrent(1);
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	FVaRestFakeResponse Later = Json(200, "{}");
	Later.Delay = 0.5;
	Rig.Fake->Enqueue(Later);
	auto FirstDone = MakeOutcome<FVaRestJson>();
	auto SecondDone = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle First = Observe(Rig.Client.Get(TEXT("/first")), FirstDone).Send();
	const FVaRestRequestHandle Second = Observe(Rig.Client.Get(TEXT("/second")), SecondDone).Send();
	TestEqual(TEXT("the second request queues"), Rig.Fake->NumCalls(), 1);
	Rig.Clock->Advance(2.0);
	if (Rig.Fake->NumCalls() > 0)
	{
		Rig.Fake->Complete(0, Json(200, "{}"));
	}
	TestTrue(TEXT("the first completes"), Rig.Wait(FirstDone));
	TestTrue(TEXT("... and the second reaches the transport"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; }));
	Rig.Clock->Advance(0.5);
	TestTrue(TEXT("the second completes"), Rig.Wait(SecondDone));
	TestTrue(TEXT("no wait: the attempt's 2000 ms, nothing queued"), Capture.Contains(Id(First) + TEXT("GET https://api.test/first -> 200 (2000 ms, 2 B down")));
	TestTrue(TEXT("queued: its own 500 ms, the 2000 ms queued apart"), Capture.Contains(Id(Second) + TEXT("GET https://api.test/second -> 200 (500 ms, 2000 ms queued, 2 B down")));

	// Task 14 re-review N2 (P14.2): an attempt canceled while it waits in the queue shows the wait apart too, not as its own
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	auto HolderDone = MakeOutcome<FVaRestJson>();
	auto WaiterDone = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Holder = Observe(Rig.Client.Get(TEXT("/holder")), HolderDone).Send();
	const FVaRestRequestHandle Waiter = Observe(Rig.Client.Get(TEXT("/waiter")), WaiterDone).Send();
	TestEqual(TEXT("the fourth request queues"), Rig.Fake->NumCalls(), 3);
	Rig.Clock->Advance(1.5);
	Waiter.Cancel();
	TestTrue(TEXT("the one canceled in the queue completes"), Rig.Wait(WaiterDone));
	TestTrue(TEXT("... its own 0 ms, the 1500 ms queued apart"), Capture.Contains(Id(Waiter) + TEXT("GET https://api.test/waiter -> canceled (0 ms, 1500 ms queued)")));
	Holder.Cancel();
	TestTrue(TEXT("the holder completes"), Rig.Wait(HolderDone));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLogRotationTest, "VaRest.Http.Log.SecretRotation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLogRotationTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LogTestPrivate;
	FLevelIsolation Isolation;
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
	FLevelIsolation Isolation;
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLogExpiryClockTest, "VaRest.Http.Log.SecretExpiryClockSwitch",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLogExpiryClockTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LogTestPrivate;
	// Review M6: WithClock after a rotation. The old value expires at 600 on the rig's clock; the new clock is already at
	// 1000, so without the rebase the value would be expired at once there, and the old clock's timer would still fire.
	FLevelIsolation Isolation;
	FTestRig Rig;
	FLogCapture Capture;
	Rig.Client.WithLogLevel(EVaRestLogLevel::Basic);
	Rig.Client.SetSecretValue(TEXT("bearer"), TEXT("token-one-abcdefgh"));
	Rig.Client.SetSecretValue(TEXT("bearer"), TEXT("token-two-abcdefgh"));
	const TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> Later = MakeShared<FVaRestVirtualClock, ESPMode::ThreadSafe>();
	Later->Advance(1000.0);
	Rig.Client.WithClock(Later);
	TestEqual(TEXT("the old clock's expiry timer is cleared"), Rig.Clock->NumPendingTimers(), 0);
	TestEqual(TEXT("... and one is armed on the new clock"), Later->NumPendingTimers(), 1);
	const FVaRestRequestHandle Moved = Run(Rig, Rig.Client.Get(TEXT("/p/token-one-abcdefgh")));
	TestTrue(TEXT("the rotated value is still scrubbed on the new clock"), Capture.Contains(Id(Moved) + TEXT("GET https://api.test/p/*** ")));
	Later->Advance(599.0);
	const FVaRestRequestHandle Kept = Run(Rig, Rig.Client.Get(TEXT("/p/token-one-abcdefgh")));
	TestTrue(TEXT("... for the time it had left"), Capture.Contains(Id(Kept) + TEXT("GET https://api.test/p/*** ")));
	Later->Advance(1.0);
	TestEqual(TEXT("its expiry fired on the new clock"), Later->NumPendingTimers(), 0);
	const FVaRestRequestHandle Expired = Run(Rig, Rig.Client.Get(TEXT("/p/token-one-abcdefgh")));
	TestTrue(TEXT("... and then it is no longer scrubbed"), Capture.Contains(Id(Expired) + TEXT("GET https://api.test/p/token-one-abcdefgh ")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLogConfigEscapedOnceTest, "VaRest.Http.Log.ConfigErrorEscapedOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLogConfigEscapedOnceTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LogTestPrivate;
	// Bead VaRest-3dj (6): Resolve escapes the verb into Error.Message (a game may log the message as it is); the Basic line
	// escaped it again, so "\r" showed as "\\r". A message with a raw CR LF (a missing file's name) must still be escaped.
	FLevelIsolation Isolation;
	FTestRig Rig;
	Rig.Client.WithLogLevel(EVaRestLogLevel::Basic);
	FLogCapture Capture;
	AddExpectedMessagePlain(TEXT("-> Config error: the verb"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("-> Config error: upload file not found"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	auto Verb = MakeOutcome<FVaRestJson>();
	auto File = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Request(TEXT("GET\r\nX"), TEXT("/v")), Verb).Send();
	Observe(Rig.Client.Put(TEXT("/f")).BodyFile(TEXT("/no/such/dir\r\nfile.bin")), File).Send();
	if (!TestTrue(TEXT("both complete"), PumpUntil([Verb, File] { return Verb->IsDone() && File->IsDone(); })))
	{
		return false;
	}
	FString VerbLine;
	FString FileLine;
	for (const FString& Line : Capture.GetLines())
	{
		if (Line.Contains(TEXT("Config error: the verb")))
		{
			VerbLine = Line;
		}
		else if (Line.Contains(TEXT("Config error: upload file not found")))
		{
			FileLine = Line;
		}
	}
	TestTrue(TEXT("the verb is escaped once: one backslash before r"), VerbLine.Contains(TEXT("\"GET\\r\\nX\"")) && !VerbLine.Contains(TEXT("\\\\r")));
	TestTrue(TEXT("a raw CR LF in a Config message is still escaped in its line"), FileLine.Contains(TEXT("dir\\r\\nfile.bin"))
		&& !FileLine.Contains(TEXT("\r")) && !FileLine.Contains(TEXT("\n")));
	TestTrue(TEXT("Error.Message keeps its single escape for games that log it"), Verb->Error.Message.Contains(TEXT("\"GET\\r\\nX\"")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLogCurrentRedactorTest, "VaRest.Http.Log.CurrentRedactor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLogCurrentRedactorTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LogTestPrivate;
	// Plan decision 17 (carry (b), task 14 review M10): a secret the client learns while a request is out -- what an auth
	// refresh does -- is scrubbed from that request's later lines and from its error's URL
	FLevelIsolation Isolation;
	FTestRig Rig;
	Rig.Client.WithLogLevel(EVaRestLogLevel::Headers);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	FLogCapture Capture;
	AddExpectedMessagePlain(TEXT("-> 404"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	const FString Late = TEXT("zz-late-secret-123456");
	auto Echoed = MakeOutcome<FVaRestJson>();
	auto Failed = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/late")), Echoed).Send();
	Observe(Rig.Client.Get(TEXT("/err")).Query(TEXT("q"), Late), Failed).Send();
	if (!TestTrue(TEXT("both dispatched"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; })))
	{
		Rig.Client.CancelAll();
		PumpUntil([Echoed, Failed] { return Echoed->IsDone() && Failed->IsDone(); });
		return false;
	}
	const int32 Mark = Capture.GetLines().Num();
	Rig.Client.SetSecretValue(TEXT("Late"), Late);
	FVaRestFakeResponse Echo = FVaRestFakeResponse::Json(200, FUtf8StringView(UTF8TEXT("{}")));
	Echo.Headers.Set(TEXT("X-Echo"), Late);
	Rig.Fake->Complete(0, MoveTemp(Echo));
	Rig.Fake->Complete(1, FVaRestFakeResponse::Json(404, FUtf8StringView(UTF8TEXT("{}"))));
	if (!TestTrue(TEXT("both complete"), PumpUntil([Echoed, Failed] { return Echoed->IsDone() && Failed->IsDone(); })))
	{
		return false;
	}
	const TArray<FString> Lines = Capture.GetLines();
	bool bLeaked = false;
	bool bScrubbedHeader = false;
	for (int32 Index = Mark; Index < Lines.Num(); ++Index)
	{
		bLeaked |= Lines[Index].Contains(Late);
		bScrubbedHeader |= Lines[Index].Contains(TEXT("<   X-Echo: ***"));
	}
	TestFalse(TEXT("no line written after the secret was set holds it"), bLeaked);
	TestTrue(TEXT("... the echoed header shows ***"), bScrubbedHeader);
	TestFalse(TEXT("the error's URL is redacted with the current secrets"), Failed->Error.Url.Contains(Late));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLogOwnSecretAfterLogoutTest, "VaRest.Http.Log.OwnSecretAfterLogout",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLogOwnSecretAfterLogoutTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LogTestPrivate;
	// Review I-1 (plan decision 17): the client forgets a secret while a request that carries it is out -- a logout -- and
	// that request's later lines, its error's URL and its trace still scrub it, because its Send() snapshot knew it
	FLevelIsolation Isolation;
	FVaRestTrace::FScopedSinkIsolation SinkIsolation;
	const TSharedRef<FHeadersSink, ESPMode::ThreadSafe> Sink = MakeShared<FHeadersSink, ESPMode::ThreadSafe>();
	FVaRestTrace::AddSink(Sink);
	FTestRig Rig;
	Rig.Client.WithLogLevel(EVaRestLogLevel::Headers);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	FLogCapture Capture;
	AddExpectedMessagePlain(TEXT("GET https://api.test/p/*** -> 404"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	const FString Own = TEXT("zz-own-token-7654321");
	Rig.Client.SetSecretValue(TEXT("bearer"), Own);
	auto Failed = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/p/") + Own), Failed).Send();
	if (!TestTrue(TEXT("dispatched"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; })))
	{
		Rig.Client.CancelAll();
		PumpUntil([Failed] { return Failed->IsDone(); });
		FVaRestTrace::RemoveSink(Sink);
		return false;
	}
	const int32 Mark = Capture.GetLines().Num();
	Rig.Client.ClearSecretValues();
	FVaRestFakeResponse Echo = FVaRestFakeResponse::Json(404, FUtf8StringView(UTF8TEXT("{}")));
	Echo.Headers.Set(TEXT("X-Echo"), Own);
	Rig.Fake->Complete(0, MoveTemp(Echo));
	const bool bDone = PumpUntil([Failed] { return Failed->IsDone(); });
	FVaRestTrace::RemoveSink(Sink);
	if (!TestTrue(TEXT("it completes"), bDone))
	{
		return false;
	}
	const TArray<FString> Lines = Capture.GetLines();
	bool bLeaked = false;
	bool bScrubbedHeader = false;
	for (int32 Index = Mark; Index < Lines.Num(); ++Index)
	{
		bLeaked |= Lines[Index].Contains(Own);
		bScrubbedHeader |= Lines[Index].Contains(TEXT("<   X-Echo: ***"));
	}
	TestFalse(TEXT("no line written after the logout holds the request's own token"), bLeaked);
	TestTrue(TEXT("... the echoed header shows ***"), bScrubbedHeader);
	TestFalse(TEXT("the error's URL still scrubs it"), Failed->Error.Url.Contains(Own));
	const TArray<FVaRestTraceAttemptEnded> Ended = Sink->AttemptsEnded(Handle.GetId());
	if (TestEqual(TEXT("one attempt traced"), Ended.Num(), 1))
	{
		const FString* Echoed = Ended[0].Headers.Find(TEXT("X-Echo"));
		TestTrue(TEXT("the trace's echoed header shows ***"), Echoed && Echoed->Equals(TEXT("***")));
		TestFalse(TEXT("... and its effective URL holds no token"), Ended[0].EffectiveUrl.Contains(Own));
	}
	return true;
}

#endif
