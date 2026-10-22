// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "Tasks/Task.h"
#include "VaRestAuth.h"
#include "VaRestClient.h"
#include "VaRestEnvelope.h"
#include "VaRestHttpTestTypes.h"
#include "VaRestHttpTesting.h"
#include "VaRestRetry.h"
#include "VaRestSha256.h"
#include "VaRestTrace.h"

#include <type_traits>

namespace VaRestTestsIntegrationPrivate
{
	/** Decision 14: without the runner's server an integration test fails; it never skips (a skip is a false green) */
	bool ServerUrl(FAutomationTestBase& Test, FString& OutUrl)
	{
		OutUrl = VaRest::Http::Testing::GetTestServerUrl();
		if (OutUrl.IsEmpty())
		{
			Test.AddError(TEXT("no test server: run Tools/run_tests.sh, which starts Tools/testserver/server.py and passes -VaRestTestServer=<url> (or start it and pass the argument yourself)"));
			return false;
		}
		return true;
	}

	/** Logging has its own tests; at Basic every 4xx here would add a Warning */
	FVaRestClient MakeClient(const FString& BaseUrl)
	{
		FVaRestClient Made = FVaRestClient::Make(BaseUrl);
		Made.WithLogLevel(EVaRestLogLevel::None);
		return Made;
	}

	/** The real clock's timers and the HTTP manager run on the core ticker, so it is ticked too */
	template <typename T>
	bool Wait(const TSharedRef<VaRest::Http::Testing::TTestOutcome<T>, ESPMode::ThreadSafe>& Outcome, double Seconds = 20.0)
	{
		return VaRest::Http::Testing::PumpUntil([&Outcome] { return Outcome->IsDone(); }, Seconds, true);
	}

	/** Waits for each one even after another timed out (not Wait(A) && Wait(B)): every test drains what it sends */
	template <typename... OutcomeTypes>
	bool WaitAll(const OutcomeTypes&... Outcomes)
	{
		bool bAll = true;
		((bAll = Wait(Outcomes) && bAll), ...);
		return bAll;
	}

	/**
	 * Review I-1 (task 16): asserts that the request succeeded, before anything reads its value. An error completes a request
	 * too, and a check that only sits under if (Value.IsSet()) is skipped in silence: a false green. On failure the error is
	 * logged, so the run says what went wrong.
	 */
	template <typename T>
	bool Succeeded(FAutomationTestBase& Test, const TCHAR* What, const TSharedRef<VaRest::Http::Testing::TTestOutcome<T>, ESPMode::ThreadSafe>& Outcome)
	{
		bool bOk = Outcome->SuccessCalls.load() == 1 && Outcome->ErrorCalls.load() == 0;
		if constexpr (!std::is_void_v<T>)
		{
			bOk = bOk && Outcome->Value.IsSet();
		}
		if (!bOk && Outcome->Error.IsError())
		{
			Test.AddInfo(FString::Printf(TEXT("%s: %s"), What, *Outcome->Error.Describe()));
		}
		return Test.TestTrue(FString::Printf(TEXT("%s succeeded"), What), bOk);
	}

	FString BytesToText(TConstArrayView<uint8> Bytes)
	{
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		return FString(Converted.Length(), Converted.Get());
	}

	FString Hex(TConstArrayView<uint8> Bytes)
	{
		return VaRest::Http::FSha256::Hash(Bytes).ToHex();
	}

	TArray<uint8> Pattern(int32 Num)
	{
		TArray<uint8> Out;
		Out.SetNumUninitialized(Num);
		for (int32 Index = 0; Index < Num; ++Index)
		{
			Out[Index] = static_cast<uint8>(Index * 31 + 7);
		}
		return Out;
	}

	/** A fresh directory under Saved/ for files a test writes; the test deletes it */
	FString MakeTempDir()
	{
		const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("VaRestIntegration") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
		IFileManager::Get().MakeDirectory(*Dir, true);
		return Dir;
	}

	class FSink final : public IVaRestTraceSink
	{
	public:
		virtual FVaRestTraceDetailSettings GetDetail() const override
		{
			FVaRestTraceDetailSettings Detail;
			Detail.Flags = EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Headers;
			return Detail;
		}
		virtual void OnAttached(const FVaRestTraceSnapshot&) override {}
		virtual void OnEvent(const FVaRestTraceEvent& Event) override
		{
			if (!IsInGameThread())
			{
				bAllOnGameThread = false;
			}
			FScopeLock Lock(&Mutex);
			Events.Add(Event);
		}

		std::atomic<bool> bAllOnGameThread{ true };
		FCriticalSection Mutex;
		TArray<FVaRestTraceEvent> Events;
	};

	struct FProgressLog
	{
		TArray<FVaRestProgress> Seen;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationEchoTest, "VaRest.Integration.Http.EchoJson",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationEchoTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	FVaRestClient Client = MakeClient(BaseUrl);
	Client.WithHeader(TEXT("X-Client"), TEXT("varest"));
	const int32 OffBefore = Counters().DecodesOffGameThread;
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Client.Post(TEXT("/echo/{kind}?a=1"))
		.Path(TEXT("kind"), TEXT("a b/c"))
		.Query(TEXT("a"), TEXT("2"))
		.Header(TEXT("X-Test"), TEXT("yes"))
		.Body(FVaRestJson::Object().Set(TEXT("hello"), TEXT("мир"))), Outcome).Send();
	TestTrue(TEXT("completes"), Wait(Outcome));
	TestTrue(TEXT("one OnSuccess, one OnComplete, on the game thread (spec 1)"), Outcome->SuccessCalls == 1 && Outcome->CompleteCalls == 1 && Outcome->bAllOnGameThread);
	if (!Succeeded(*this, TEXT("the echo"), Outcome))
	{
		return false;
	}
	const FVaRestJson& Echo = Outcome->Value.GetValue();
	TestEqualSensitive(TEXT("the verb"), Echo[TEXT("method")].AsString(), FString(TEXT("POST")));
	TestEqualSensitive(TEXT("a path parameter is one encoded segment"), Echo[TEXT("path")].AsString(), FString(TEXT("/echo/a%20b%2Fc")));
	TestTrue(TEXT("a repeated query key appends (N9)"), Echo[TEXT("query")][TEXT("a")].Num() == 2 && Echo[TEXT("query")][TEXT("a")][1].AsString() == TEXT("2"));
	TestEqualSensitive(TEXT("the request's header"), Echo[TEXT("headers")][TEXT("x-test")].AsString(), FString(TEXT("yes")));
	TestEqualSensitive(TEXT("the client's default header"), Echo[TEXT("headers")][TEXT("x-client")].AsString(), FString(TEXT("varest")));
	TestTrue(TEXT("JSON goes up as application/json"), Echo[TEXT("content_type")].AsString().StartsWith(TEXT("application/json")));
	TestEqualSensitive(TEXT("the body arrives as UTF-8 JSON"), Echo[TEXT("body")].AsString(), FString(TEXT("{\"hello\":\"мир\"}")));
	TestEqual(TEXT("the response"), Outcome->Response.Status, 200);
	TestTrue(TEXT("the decode ran off the game thread"), Counters().DecodesOffGameThread - OffBefore >= 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationQueryStyleTest, "VaRest.Integration.Http.QueryStyles",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationQueryStyleTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	FVaRestClient Client = MakeClient(BaseUrl);
	// Task 3 (decision 34): the delimiters are literal ',' '|' "%20", a delimiter inside a value stays encoded, non-ASCII is
	// percent-encoded UTF-8. The unit tests see the URL VaRest builds; only the server sees what curl sends of it.
	const TArray<FString> Values = { TEXT("a"), TEXT("b|c"), TEXT("d") };
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Client.Get(TEXT("/raw"))
		.Query(TEXT("p"), Values, EVaRestQueryStyle::Pipes)
		.Query(TEXT("c"), Values, EVaRestQueryStyle::Csv)
		.Query(TEXT("s"), Values, EVaRestQueryStyle::Ssv)
		.Query(TEXT("u"), TEXT("мир")), Outcome).Send();
	TestTrue(TEXT("completes"), Wait(Outcome));
	if (!Succeeded(*this, TEXT("the query request"), Outcome))
	{
		return false;
	}
	const FVaRestJson& Raw = Outcome->Value.GetValue();
	// The request line as the server read it off the socket (http.server decodes it as Latin-1, so ASCII is byte-exact)
	AddInfo(FString::Printf(TEXT("request line received: %s"), *Raw[TEXT("request_line")].AsString()));
	TestEqualSensitive(TEXT("curl sends the query as built: '|' and ',' literal, a delimiter inside a value and a space encoded, non-ASCII as UTF-8 escapes"),
		Raw[TEXT("target")].AsString(), FString(TEXT("/raw?p=a|b%7Cc|d&c=a,b%7Cc,d&s=a%20b%7Cc%20d&u=%D0%BC%D0%B8%D1%80")));
	TestEqualSensitive(TEXT("... and the server reads the Pipes value back"), Raw[TEXT("query")][TEXT("p")][0].AsString(), FString(TEXT("a|b|c|d")));
	TestEqualSensitive(TEXT("... and the non-ASCII one"), Raw[TEXT("query")][TEXT("u")][0].AsString(), FString(TEXT("мир")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationBodiesTest, "VaRest.Integration.Http.Bodies",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationBodiesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	FVaRestClient Client = MakeClient(BaseUrl);
	const FString Dir = MakeTempDir();
	const TArray<uint8> FileBytes = Pattern(3 * 1024 * 1024);
	const FString FilePath = Dir / TEXT("upload.bin");
	FFileHelper::SaveArrayToFile(FileBytes, *FilePath);
	// Bytes a naive multipart parser would trip on
	const TArray<uint8> Binary = { '\r', '\n', '-', '-', 0, 255, '\r', '\n', '\r', '\n', 'e' };

	FVaRestForm Form;
	Form.Add(TEXT("name"), TEXT("Neo Anderson")).Add(TEXT("city"), TEXT("Zion & co"));
	auto FormOutcome = MakeOutcome<FVaRestJson>();
	Observe(Client.Post(TEXT("/echo")).Body(Form), FormOutcome).Send();

	FVaRestMultipart Parts;
	Parts.AddField(TEXT("note"), TEXT("hi"));
	Parts.AddBytes(TEXT("blob"), TEXT("b.bin"), TEXT("application/octet-stream"), Binary);
	Parts.AddFile(TEXT("doc"), FilePath);
	auto PartsOutcome = MakeOutcome<FVaRestJson>();
	Observe(Client.Post(TEXT("/echo")).Body(Parts), PartsOutcome).Send();

	auto FileOutcome = MakeOutcome<FVaRestJson>();
	Observe(Client.Put(TEXT("/echo")).BodyFile(FilePath), FileOutcome).Send();

	auto TextOutcome = MakeOutcome<FVaRestJson>();
	Observe(Client.Post(TEXT("/echo")).Body(FStringView(TEXT("привет"))), TextOutcome).Send();

	auto BytesOutcome = MakeOutcome<FVaRestJson>();
	Observe(Client.Post(TEXT("/echo")).Body(TArray<uint8>{ 0, 1, 2, 255 }), BytesOutcome).Send();

	TestTrue(TEXT("all complete"), WaitAll(FormOutcome, PartsOutcome, FileOutcome, TextOutcome, BytesOutcome));
	// Each upload's success is asserted first (review I-1); the checks of its echo run only behind that assertion
	const bool bForm = Succeeded(*this, TEXT("the form upload"), FormOutcome);
	const bool bParts = Succeeded(*this, TEXT("the multipart upload"), PartsOutcome);
	const bool bFile = Succeeded(*this, TEXT("the BodyFile upload"), FileOutcome);
	const bool bText = Succeeded(*this, TEXT("the text upload"), TextOutcome);
	const bool bBytes = Succeeded(*this, TEXT("the bytes upload"), BytesOutcome);
	if (bForm)
	{
		const FVaRestJson FormEcho = FormOutcome->Value.GetValue()[TEXT("form")];
		TestTrue(TEXT("a form arrives decoded"), FormEcho[TEXT("name")][0].AsString() == TEXT("Neo Anderson") && FormEcho[TEXT("city")][0].AsString() == TEXT("Zion & co"));
	}
	if (bParts)
	{
		const FVaRestJson Seen = PartsOutcome->Value.GetValue()[TEXT("parts")];
		TestEqual(TEXT("three parts"), Seen.Num(), 3);
		TestEqualSensitive(TEXT("a text field"), Seen[0][TEXT("text")].AsString(), FString(TEXT("hi")));
		TestEqualSensitive(TEXT("an in-memory file part, byte-exact"), Seen[1][TEXT("sha256")].AsString(), Hex(Binary));
		TestEqualSensitive(TEXT("a streamed file part (SetContentFromStream), byte-exact"), Seen[2][TEXT("sha256")].AsString(), Hex(FileBytes));
		TestEqualSensitive(TEXT("... named after the file"), Seen[2][TEXT("filename")].AsString(), FString(TEXT("upload.bin")));
	}
	if (bFile)
	{
		const FVaRestJson& Echo = FileOutcome->Value.GetValue();
		// Task 7 review M-1: a BodyFile is one checked segment of FVaRestSegmentStream (SetContentFromStream), not SetContentAsStreamedFile
		TestTrue(TEXT("a streamed file body (SetContentFromStream, one checked segment), byte-exact"), Echo[TEXT("body_length")].AsInt64() == FileBytes.Num()
			&& Echo[TEXT("body_sha256")].AsString() == Hex(FileBytes));
	}
	if (bText)
	{
		const FVaRestJson& Echo = TextOutcome->Value.GetValue();
		TestTrue(TEXT("text goes up as UTF-8 text/plain"), Echo[TEXT("body")].AsString() == TEXT("привет") && Echo[TEXT("content_type")].AsString().StartsWith(TEXT("text/plain; charset=utf-8")));
	}
	if (bBytes)
	{
		TestEqualSensitive(TEXT("bytes go up as they are"), BytesOutcome->Value.GetValue()[TEXT("body_base64")].AsString(), FString(TEXT("AAEC/w==")));
	}
	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationStatusTest, "VaRest.Integration.Http.StatusAndErrors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationStatusTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	// curl reports the refused connection as Warnings with its own text (CurlHttp.cpp:1348), and the engine logs the failed
	// request once with its reason (HttpRequestCommon.cpp:333)
	AddExpectedMessagePlain(TEXT("libcurl"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	AddExpectedMessagePlain(TEXT("completed with reason 'ConnectionError'"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FVaRestClient Client = MakeClient(BaseUrl);
	auto NotFound = MakeOutcome<FVaRestJson>();
	Observe(Client.Get(TEXT("/status/404")), NotFound).Send();
	auto Unavailable = MakeOutcome<FVaRestJson>();
	Observe(Client.Get(TEXT("/status/503")), Unavailable).Send();
	auto NoContent = MakeOutcome<FVaRestJson>();
	Observe(Client.Delete(TEXT("/status/204")), NoContent).Send();
	auto Head = MakeOutcome<void>();
	Observe(Client.Head<void>(TEXT("/echo")), Head).Send();
	FVaRestClient Nowhere = MakeClient(TEXT("http://127.0.0.1:1"));
	auto Refused = MakeOutcome<FVaRestJson>();
	Observe(Nowhere.Get(TEXT("/x")), Refused).Send();
	TestTrue(TEXT("all complete"), WaitAll(NotFound, Unavailable, NoContent, Head, Refused));

	TestTrue(TEXT("404 is an Http error"), NotFound->ErrorCalls == 1 && NotFound->Error.Kind == EVaRestErrorKind::Http && NotFound->Error.HttpStatus == 404);
	TestEqual(TEXT("... with its JSON body parsed (spec 3.3)"), NotFound->Error.Body[TEXT("status")].AsInt(), 404);
	TestFalse(TEXT("... not retryable"), NotFound->Error.bRetryable);
	TestTrue(TEXT("503 is retryable (spec 4.1)"), Unavailable->Error.Kind == EVaRestErrorKind::Http && Unavailable->Error.bRetryable);
	TestTrue(TEXT("204 is a success with an invalid handle (spec 3.2)"), NoContent->SuccessCalls == 1 && NoContent->Value.IsSet() && !NoContent->Value.GetValue().IsValid());
	TestTrue(TEXT("HEAD succeeds without a body"), Head->SuccessCalls == 1 && Head->Response.Status == 200);
	TestTrue(TEXT("a refused connection is a Connection error"), Refused->ErrorCalls == 1 && Refused->Error.Kind == EVaRestErrorKind::Connection && Refused->Error.HttpStatus == 0);
	TestTrue(TEXT("... and retryable"), Refused->Error.bRetryable);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationTimeoutTest, "VaRest.Integration.Http.Timeouts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationTimeoutTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	AddExpectedMessagePlain(TEXT("libcurl"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	// The engine's total timeout logs once (HttpRequestCommon.cpp:580); its activity timeout logs at Log level (:489)
	AddExpectedMessagePlain(TEXT("HTTP request timed out after"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FVaRestClient Client = MakeClient(BaseUrl);
	const double TotalStart = FPlatformTime::Seconds();
	auto Total = MakeOutcome<FVaRestJson>();
	// Decision 30 (task 7): without an activity timeout the activity follows the total, so both are 0.5 s here. The total is
	// armed at ProcessRequest (HttpRequestCommon.cpp:203), the activity at curl's first activity (CurlHttp.cpp:855), so
	// the total fires first: Timeout, not the activity timeout's Connection.
	Observe(Client.Get(TEXT("/delay/3000")).Timeout(0.5f), Total).Send();
	TestTrue(TEXT("completes"), Wait(Total));
	const double TotalElapsed = FPlatformTime::Seconds() - TotalStart;
	TestTrue(TEXT("the total timeout is a Timeout error (spec 4.7)"), Total->Error.Kind == EVaRestErrorKind::Timeout);
	TestTrue(TEXT("... at about its time, not the server's"), TotalElapsed < 2.5);

	const double ActivityStart = FPlatformTime::Seconds();
	auto Activity = MakeOutcome<TArray<uint8>>();
	// One byte now, the next after 4 s: the 1 s activity timeout fires in the gap
	Observe(Client.Get<TArray<uint8>>(TEXT("/drip?bytes=2&chunks=2&interval_ms=4000")).Timeout(0.f, 1.f), Activity).Send();
	TestTrue(TEXT("completes"), Wait(Activity));
	const double ActivityElapsed = FPlatformTime::Seconds() - ActivityStart;
	TestTrue(TEXT("the activity timeout is a Connection error, as the engine reports it (decision 19)"), Activity->Error.Kind == EVaRestErrorKind::Connection);
	TestTrue(TEXT("... before the server's next byte"), ActivityElapsed < 3.5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationDownloadTest, "VaRest.Integration.Http.LargeDownload",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationDownloadTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	constexpr int32 Size = 8 * 1024 * 1024;
	FVaRestClient Client = MakeClient(BaseUrl);
	TSharedRef<FProgressLog, ESPMode::ThreadSafe> Progress = MakeShared<FProgressLog, ESPMode::ThreadSafe>();
	auto Outcome = MakeOutcome<TArray<uint8>>();
	Observe(Client.Get<TArray<uint8>>(FString::Printf(TEXT("/large?bytes=%d&seed=3"), Size))
		.OnProgress([Progress](const FVaRestProgress& Seen) { Progress->Seen.Add(Seen); }), Outcome).Send();
	TestTrue(TEXT("completes"), Wait(Outcome, 60.0));
	if (!Succeeded(*this, TEXT("the download"), Outcome))
	{
		return false;
	}
	const TArray<uint8>& Bytes = Outcome->Value.GetValue();
	TestEqual(TEXT("every byte"), Bytes.Num(), Size);
	TestEqualSensitive(TEXT("byte-exact"), Hex(Bytes), Outcome->Response.Headers.Get(TEXT("X-Sha256")));
	TestTrue(TEXT("progress was reported"), Progress->Seen.Num() >= 1);
	bool bMonotonic = true;
	for (int32 Index = 1; Index < Progress->Seen.Num(); ++Index)
	{
		bMonotonic &= Progress->Seen[Index].BytesReceived >= Progress->Seen[Index - 1].BytesReceived;
	}
	TestTrue(TEXT("... never backwards"), bMonotonic);
	TestTrue(TEXT("... ending at the whole body, against Content-Length (decision 8)"), Progress->Seen.Num() >= 1
		&& Progress->Seen.Last().BytesReceived == Size && Progress->Seen.Last().DownloadTotal == Size);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationUploadProgressTest, "VaRest.Integration.Http.UploadProgress",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationUploadProgressTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	// Decision 29 (review I-2): under CompleteOnHttpThread curl reports no progress while it uploads; the counting
	// archive does. The server reads 1 MiB per 100 ms, so TCP backpressure keeps the 16 MiB upload going for about 1.5 s.
	constexpr int32 Size = 16 * 1024 * 1024;
	FVaRestClient Client = MakeClient(BaseUrl);
	TSharedRef<FProgressLog, ESPMode::ThreadSafe> Progress = MakeShared<FProgressLog, ESPMode::ThreadSafe>();
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Client.Post(TEXT("/slowread?chunk=1048576&interval_ms=100")).Body(Pattern(Size))
		.OnProgress([Progress](const FVaRestProgress& Seen) { Progress->Seen.Add(Seen); }), Outcome).Send();
	TestTrue(TEXT("completes"), Wait(Outcome, 60.0));
	if (Succeeded(*this, TEXT("the upload"), Outcome))
	{
		TestEqual(TEXT("the server got every byte"), Outcome->Value.GetValue()[TEXT("received")].AsInt64(), static_cast<int64>(Size));
	}
	TestTrue(TEXT("upload progress arrives while the body goes up, not only at the end"),
		Progress->Seen.ContainsByPredicate([](const FVaRestProgress& Seen) { return Seen.BytesSent > 0 && Seen.BytesSent < Size; }));
#if PLATFORM_APPLE
	// Bead VaRest-3dj (5): curl's finish reports progress once more (CurlHttp.cpp:1274); Apple's FinishRequest does not (AppleHttp.cpp:777)
	AddInfo(TEXT("Apple: the last OnProgress may show less than the whole body (no final progress call); not checked"));
#else
	TestTrue(TEXT("... ending at the whole body"), Progress->Seen.Num() > 0 && Progress->Seen.Last().BytesSent == Size && Progress->Seen.Last().UploadTotal == Size);
#endif
	// The numbers, for the record (Task 7 I-2: a loopback socket buffer large enough to swallow the body would leave none in between)
	FString Samples;
	int32 InBetween = 0;
	for (const FVaRestProgress& Sample : Progress->Seen)
	{
		Samples += FString::Printf(TEXT(" %lld"), Sample.BytesSent);
		InBetween += Sample.BytesSent > 0 && Sample.BytesSent < Size ? 1 : 0;
	}
	AddInfo(FString::Printf(TEXT("%d progress calls, %d of them mid-upload; BytesSent:%s"), Progress->Seen.Num(), InBetween, *Samples));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationSaveToTest, "VaRest.Integration.Http.SaveTo",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationSaveToTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	FVaRestClient Client = MakeClient(BaseUrl);
	const FString Dir = MakeTempDir();
	const FString Target = Dir / TEXT("download.bin");
	auto Outcome = MakeOutcome<FVaRestResponse>();
	Observe(Client.Get<FVaRestResponse>(TEXT("/large?bytes=4194304&seed=4")).SaveTo(Target), Outcome).Send();
	TestTrue(TEXT("completes"), Wait(Outcome, 60.0));
	TArray<uint8> OnDisk;
	TestTrue(TEXT("the file is there"), Succeeded(*this, TEXT("the SaveTo download"), Outcome) && FFileHelper::LoadFileToArray(OnDisk, *Target));
	TestEqual(TEXT("... whole"), OnDisk.Num(), 4194304);
	TestEqualSensitive(TEXT("... byte-exact"), Hex(OnDisk), Outcome->Response.Headers.Get(TEXT("X-Sha256")));
	TestTrue(TEXT("... named in the response"), Outcome->Value.IsSet() && Outcome->Value.GetValue().SavedFile == Target);
	TArray<FString> Leftovers;
	IFileManager::Get().FindFiles(Leftovers, *Dir, TEXT(".part"));
	TestEqual(TEXT("no .part file is left (spec 3.2: atomic rename)"), Leftovers.Num(), 0);
	// Task 10: the temp file is <target>.varest-<id>-<guid>.part, and Windows adds a .bak while it replaces: nothing but the target stays
	TArray<FString> Everything;
	IFileManager::Get().FindFiles(Everything, *(Dir / TEXT("*")), true, false);
	TestEqual(TEXT("... nor any other file next to the target"), Everything.Num(), 1);
	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationShapesTest, "VaRest.Integration.Http.ChunkedRedirectCap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationShapesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	// The cap stops the body in curl's write callback: curl fails the transfer with CURLE_WRITE_ERROR and logs it and its
	// info cache as Warnings (CurlHttp.cpp:1348, :1360); the engine then logs the request's end once (HttpRequestCommon.cpp:333)
	AddExpectedMessagePlain(TEXT("libcurl"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	AddExpectedMessagePlain(TEXT("completed with reason 'Other'"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FVaRestClient Client = MakeClient(BaseUrl);
	auto Chunked = MakeOutcome<FVaRestJson>();
	Observe(Client.Get(TEXT("/chunked?chunks=16&size=4096")), Chunked).Send();
	auto Redirected = MakeOutcome<FVaRestJson>();
	// KeepBody: the raw bytes the sink received stay in Response.Body, so the check below sees every one of them
	Observe(Client.Get(TEXT("/redirect?to=/echo/after")).KeepBody(), Redirected).Send();
	auto Capped = MakeOutcome<FVaRestJson>();
	Observe(Client.Get(TEXT("/large/json?bytes=2097152")).MaxJsonBytes(1024 * 1024), Capped).Send();
	auto Uncapped = MakeOutcome<FVaRestJson>();
	Observe(Client.Get(TEXT("/large/json?bytes=2097152")), Uncapped).Send();
	TestTrue(TEXT("all complete"), WaitAll(Chunked, Redirected, Capped, Uncapped));

	if (Succeeded(*this, TEXT("the chunked request"), Chunked))
	{
		TestTrue(TEXT("a chunked body without Content-Length decodes"), Chunked->Value.GetValue().Num() == 16 && Chunked->Value.GetValue()[15].AsString().Len() == 4096);
	}
	// Review I-2 (task 16): the 302 carries a JSON body of its own (server.py REDIRECT_BODY). libcurl discards a followed
	// redirect's body (m3-plan-review-concurrency.md:427-428); a transport or sink that kept it would put it before the final
	// body, which would then fail to parse, carry the marker, or outgrow its Content-Length.
	if (Succeeded(*this, TEXT("the redirected request"), Redirected))
	{
		const FVaRestJson& After = Redirected->Value.GetValue();
		TestEqualSensitive(TEXT("a redirect is followed (CurlHttp.cpp:183)"), After[TEXT("path")].AsString(), FString(TEXT("/echo/after")));
		TestFalse(TEXT("... the 302's own body never reaches the result"), After.Has(TEXT("redirect")));
		const TArray<uint8>& Received = Redirected->Response.Body;
		TestFalse(TEXT("... nor the sink"), BytesToText(Received).Contains(TEXT("body-must-not-leak"), ESearchCase::CaseSensitive));
		TestEqual(TEXT("... which holds exactly the final response's bytes"), static_cast<int64>(Received.Num()),
			FCString::Atoi64(*Redirected->Response.Headers.Get(TEXT("Content-Length"))));
	}
	TestTrue(TEXT("... and the effective URL says where it ended"), Redirected->Response.EffectiveUrl.EndsWith(TEXT("/echo/after")));
	// curl keeps no header of a 3xx it follows (CurlHttp.cpp:509 skips them while bRedirected, set at :560)
	TestFalse(TEXT("... and the 302's headers are not the response's"), Redirected->Response.Headers.Contains(TEXT("Location")));
	TestTrue(TEXT("a JSON body over MaxJsonBytes is a Decode error (decision 20)"), Capped->Error.Kind == EVaRestErrorKind::Decode && Capped->Error.Message.Contains(TEXT("MaxJsonBytes")));
	TestTrue(TEXT("... stopped by the transport, not retryable, nothing kept (reviews M-6, M13)"), !Capped->Error.bRetryable && Capped->Response.Body.Num() == 0);
	if (Succeeded(*this, TEXT("the uncapped request"), Uncapped))
	{
		TestEqual(TEXT("... and the same body under the default cap decodes whole"), Uncapped->Value.GetValue().Num(),
			FCString::Atoi(*Uncapped->Response.Headers.Get(TEXT("X-Item-Count"))));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationCancelTest, "VaRest.Integration.Http.Cancel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationCancelTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	AddExpectedMessagePlain(TEXT("libcurl"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	FVaRestClient Client = MakeClient(BaseUrl);
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Client.Get(TEXT("/delay/5000")), Outcome).Send();
	PumpUntil([] { return false; }, 0.2, true);
	const double CancelAt = FPlatformTime::Seconds();
	Handle.Cancel();
	TestTrue(TEXT("completes"), Wait(Outcome));
	TestTrue(TEXT("Canceled, to OnError (decision 2)"), Outcome->ErrorCalls == 1 && Outcome->Error.Kind == EVaRestErrorKind::Canceled);
	TestTrue(TEXT("... at once, not when the server answers"), FPlatformTime::Seconds() - CancelAt < 2.0);
	TestFalse(TEXT("the handle is no longer active"), Handle.IsActive());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationLimiterTest, "VaRest.Integration.Http.Limiter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationLimiterTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	// The server counts the requests it holds at once, per key (Tools/testserver/server.py /delay and /stats)
	auto MaxInFlight = [this, &BaseUrl](int32 Limit) -> int32
	{
		const FString Key = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		FVaRestClient Throttled = MakeClient(BaseUrl);
		Throttled.WithMaxConcurrent(Limit);
		TArray<TSharedRef<TTestOutcome<FVaRestJson>, ESPMode::ThreadSafe>> Outcomes;
		for (int32 Index = 0; Index < 6; ++Index)
		{
			Outcomes.Add(MakeOutcome<FVaRestJson>());
			Observe(Throttled.Get(FString::Printf(TEXT("/delay/300?key=%s"), *Key)), Outcomes.Last()).Send();
		}
		for (const TSharedRef<TTestOutcome<FVaRestJson>, ESPMode::ThreadSafe>& Each : Outcomes)
		{
			TestTrue(TEXT("completes"), Wait(Each));
			Succeeded(*this, TEXT("a held request"), Each);
		}
		auto Stats = MakeOutcome<FVaRestJson>();
		Observe(MakeClient(BaseUrl).Get(FString::Printf(TEXT("/stats?key=%s"), *Key)), Stats).Send();
		TestTrue(TEXT("completes"), Wait(Stats));
		return Succeeded(*this, TEXT("the stats request"), Stats) ? Stats->Value.GetValue()[TEXT("max_in_flight")].AsInt() : -1;
	};
	const int32 Limited = MaxInFlight(2);
	TestTrue(TEXT("WithMaxConcurrent(2): never more than two at the server (spec 4.6)"), Limited >= 1 && Limited <= 2);
	// The control: the server's count works, and the engine would have sent more at once (16 per host: HttpModule.cpp:101,
	// handed to curl at CurlHttpManager.cpp:308-317)
	const int32 Unlimited = MaxInFlight(16);
	TestTrue(TEXT("without the limit, more than two at once"), Unlimited > 2);
	AddInfo(FString::Printf(TEXT("most requests the server held at once: %d with WithMaxConcurrent(2), %d with 16"), Limited, Unlimited));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationWorkerTest, "VaRest.Integration.Http.SendFromWorker",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationWorkerTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	FVaRestClient Client = MakeClient(BaseUrl);
	auto Outcome = MakeOutcome<FVaRestJson>();
	TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> bSentOffGameThread = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	UE::Tasks::Launch(TEXT("VaRest.Test.SendFromWorker"), [Client, Outcome, bSentOffGameThread]
	{
		*bSentOffGameThread = !IsInGameThread();
		VaRest::Http::Testing::Observe(Client.Get(TEXT("/echo/worker")), Outcome).Send();
	});
	TestTrue(TEXT("completes"), Wait(Outcome));
	TestTrue(TEXT("sent from a worker"), bSentOffGameThread->load());
	TestTrue(TEXT("spec 1: marshalled to the game thread; the callbacks run there"), Succeeded(*this, TEXT("the worker's request"), Outcome) && Outcome->bAllOnGameThread);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationTraceLogTest, "VaRest.Integration.Http.TraceAndLogs",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationTraceLogTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	FVaRestTrace::FScopedSinkIsolation Isolation;
	const TSharedRef<FSink, ESPMode::ThreadSafe> Sink = MakeShared<FSink, ESPMode::ThreadSafe>();
	FVaRestTrace::AddSink(Sink);
	FLogCapture Capture;
	FVaRestClient Client = FVaRestClient::Make(BaseUrl);
	Client.WithLogLevel(EVaRestLogLevel::Body);
	Client.SetSecretValue(TEXT("bearer"), TEXT("canary-bearer-0123456789"));

	// The server echoes all of it back: path, query, the Authorization header, inside a JSON body
	auto Echo = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle EchoHandle = Observe(Client.Get(TEXT("/echo/canary-bearer-0123456789?token=canary-query"))
		.Header(TEXT("Authorization"), TEXT("Bearer canary-bearer-0123456789")), Echo).Send();
	auto Cookie = MakeOutcome<FVaRestJson>();
	Observe(Client.Get(TEXT("/headers/set?Set-Cookie=sid%3Dcanary-cookie")), Cookie).Send();
	TestTrue(TEXT("both complete"), WaitAll(Echo, Cookie));
	// Review I-1: a failed request would leave less to scan for leaks
	Succeeded(*this, TEXT("the echo request"), Echo);
	Succeeded(*this, TEXT("the cookie request"), Cookie);
	FlushRedactionPipe();
	FVaRestTrace::RemoveSink(Sink);

	int32 Lines = 0;
	int32 Leaks = 0;
	for (const FString& Line : Capture.GetLines())
	{
		if (!Line.StartsWith(TEXT("LogVaRestHttp: ")))
		{
			continue; // the engine's own lines are not ours to redact
		}
		++Lines;
		if (Line.Contains(TEXT("canary"), ESearchCase::CaseSensitive))
		{
			++Leaks;
			AddInfo(FString::Printf(TEXT("leaked: %s"), *Line));
		}
	}
	TestTrue(TEXT("Body-level lines were written"), Lines >= 8);
	TestEqual(TEXT("no secret in any line against a real server (Inspector 2.2)"), Leaks, 0);

	TArray<FVaRestTraceEvent> Events;
	{
		FScopeLock Lock(&Sink->Mutex);
		const uint64 Id = EchoHandle.GetId();
		Events = Sink->Events.FilterByPredicate([Id](const FVaRestTraceEvent& Event)
		{
			return Event.Kind == EVaRestTraceKind::AttemptEnded && Event.Payload.Get<FVaRestTraceAttemptEnded>().RequestId == Id;
		});
	}
	TestTrue(TEXT("Inspector 9: nothing is emitted from the HTTP thread, against the real transport"), Sink->bAllOnGameThread.load());
	TestEqual(TEXT("one AttemptEnded"), Events.Num(), 1);
	if (Events.Num() == 1)
	{
		const FVaRestTraceAttemptEnded& Ended = Events[0].Payload.Get<FVaRestTraceAttemptEnded>();
		TestTrue(TEXT("IN-3: the HTTP-thread timestamps arrived, in order"), Ended.StatusAt >= 0.0 && Ended.TransportDoneAt >= Ended.StatusAt);
		TestFalse(TEXT("... the effective URL redacted"), Ended.EffectiveUrl.Contains(TEXT("canary")));
		TestEqualSensitive(TEXT("... and its response headers"), Ended.Headers.Get(TEXT("Content-Type")), FString(TEXT("application/json")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationStructTest, "VaRest.Integration.Http.StructDecode",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationStructTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	FVaRestClient Client = MakeClient(BaseUrl);
	// Review M9 (VaRest.Http.Decode.Struct): process-wide counters -- let an earlier test's late tasks land before the baseline
	FlushRedactionPipe();
	PumpUntil([] { return false; }, 0.05, true);
	const int32 OffBefore = Counters().DecodesOffGameThread;
	auto Outcome = MakeOutcome<FVaRestHttpTestProfile>();
	Observe(Client.Get<FVaRestHttpTestProfile>(TEXT("/json/profile")), Outcome).Send();
	TestTrue(TEXT("completes"), Wait(Outcome));
	if (!Succeeded(*this, TEXT("the struct request"), Outcome))
	{
		return false;
	}
	const FVaRestHttpTestProfile& Profile = Outcome->Value.GetValue();
	TestEqual(TEXT("an int64 past 2^53 stays exact end to end"), Profile.Id, static_cast<int64>(9007199254740993ll));
	TestEqualSensitive(TEXT("camelCase keys onto PascalCase fields (spec 2.4)"), Profile.Name, FString(TEXT("Neo")));
	TestTrue(TEXT("an array of structs"), Profile.Items.Num() == 2 && Profile.Items[0].Name == TEXT("sword") && Profile.Items[0].Level == 3);
	TestEqual(TEXT("a field the server leaves out keeps its default"), Profile.Score, 7);
	TestEqual(TEXT("decoded off the game thread"), Counters().DecodesOffGameThread - OffBefore, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationFallbackTest, "VaRest.Integration.Http.GameThreadFallback",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationFallbackTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	// Task 17 review M6 (P17.7): spec 9's fallback is what docs/dev/http-thread-completion.md tells a failing platform to
	// ship, so it runs end to end through FVaRestClient: a body the sink takes on the game thread, the decode hop, SaveTo
	IConsoleVariable* const Fallback = IConsoleManager::Get().FindConsoleVariable(TEXT("VaRest.Http.CompleteOnGameThread"));
	if (!TestNotNull(TEXT("the cvar exists"), Fallback))
	{
		return false;
	}
	const bool bBefore = Fallback->GetBool();
	// The cvar's guard lives in this lambda, so it is restored on every return from it (~TGuardConsoleVariable,
	// IConsoleManager.h:1441-1447); the check after it proves that
	const bool bRan = [this, &BaseUrl]() -> bool
	{
		TGuardConsoleVariable<bool> GameThreadCompletion(TEXT("VaRest.Http.CompleteOnGameThread"), true);
		FVaRestClient Client = MakeClient(BaseUrl);
		const FString Dir = MakeTempDir();
		ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Dir, false, true); };
		const FString Target = Dir / TEXT("fallback.bin");
		const int32 OffBefore = Counters().DecodesOffGameThread;
		auto Json = MakeOutcome<FVaRestJson>();
		Observe(Client.Get(TEXT("/json/profile")), Json).Send();
		auto Saved = MakeOutcome<FVaRestResponse>();
		Observe(Client.Get<FVaRestResponse>(TEXT("/large?bytes=1048576&seed=5")).SaveTo(Target), Saved).Send();
		TestTrue(TEXT("both complete"), WaitAll(Json, Saved));
		// Each request's success first (review I-1); its checks only behind it
		const bool bJson = Succeeded(*this, TEXT("the JSON GET"), Json);
		const bool bSaved = Succeeded(*this, TEXT("the SaveTo download"), Saved);
		TestTrue(TEXT("every callback on the game thread"), Json->bAllOnGameThread.load() && Saved->bAllOnGameThread.load());
		TestTrue(TEXT("the decode still ran off the game thread (spec 9: decoding stays on UE::Tasks)"), Counters().DecodesOffGameThread - OffBefore >= 1);
		if (bJson)
		{
			const FVaRestJson& Profile = Json->Value.GetValue();
			TestEqualSensitive(TEXT("the decoded value"), Profile[TEXT("name")].AsString(), FString(TEXT("Neo")));
			TestEqual(TEXT("... an int64 past 2^53, exact"), Profile[TEXT("id")].AsInt64(), static_cast<int64>(9007199254740993ll));
			TestTrue(TEXT("... and its array"), Profile[TEXT("items")].Num() == 2 && Profile[TEXT("items")][1][TEXT("level")].AsInt() == 1);
		}
		if (bSaved)
		{
			TArray<uint8> OnDisk;
			TestTrue(TEXT("the file is there"), FFileHelper::LoadFileToArray(OnDisk, *Target));
			TestEqual(TEXT("... whole"), OnDisk.Num(), 1048576);
			TestEqualSensitive(TEXT("... byte-exact"), Hex(OnDisk), Saved->Response.Headers.Get(TEXT("X-Sha256")));
			TestTrue(TEXT("... named in the response"), Saved->Value.IsSet() && Saved->Value.GetValue().SavedFile == Target);
		}
		return bJson && bSaved;
	}();
	TestEqual(TEXT("the cvar is restored"), Fallback->GetBool(), bBefore);
	return bRan;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationRepeatedHeadersTest, "VaRest.Integration.Http.RepeatedHeaders",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationRepeatedHeadersTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	// Plan decision 14: curl joins repeats with ", " in GetAllHeaders (CurlHttp.cpp:519-530); an Expires date has a comma,
	// so two cookies could not be split back. Collected from OnHeaderReceived they arrive apart.
	FVaRestClient Client = MakeClient(BaseUrl);
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Client.Get(TEXT("/headers/set")).Query(TEXT("Set-Cookie"), TEXT("a=1; Expires=Wed, 21 Oct 2026 07:28:00 GMT")).Query(TEXT("Set-Cookie"), TEXT("b=2")), Outcome).Send();
	if (!TestTrue(TEXT("completes"), Wait(Outcome)) || !Succeeded(*this, TEXT("the request"), Outcome))
	{
		return false;
	}
	TArray<FString> Cookies;
	for (const FVaRestHeaders::FPair& Pair : Outcome->Response.Headers)
	{
		if (Pair.Key.Equals(TEXT("Set-Cookie"), ESearchCase::IgnoreCase))
		{
			Cookies.Add(Pair.Value);
		}
	}
#if PLATFORM_APPLE
	// NSURLSession folds repeats itself (allHeaderFields, AppleHttp.cpp:55); the delegate replays that dictionary
	AddInfo(TEXT("Apple: repeated headers arrive as NSURLSession folded them"));
#else
	if (!TestEqual(TEXT("two Set-Cookie headers"), Cookies.Num(), 2))
	{
		return false;
	}
	TestEqualSensitive(TEXT("the first, its comma intact"), Cookies[0], FString(TEXT("a=1; Expires=Wed, 21 Oct 2026 07:28:00 GMT")));
	TestEqualSensitive(TEXT("the second"), Cookies[1], FString(TEXT("b=2")));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationDuplicatedProtocolTest, "VaRest.Integration.Http.DuplicatedProtocolWarnsOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationDuplicatedProtocolTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	// Task 2 review minor 3: outside Shipping, IsDomainAllowed logs the whole URL as a Warning when "://" appears twice
	// (CheckNoDuplicatedProtocol, HttpManager.cpp:279-297, the line at :286). The engine's own PreCheck calls it once per
	// request (HttpRequestCommon.cpp:109, from PreProcess at :184); VaRest's PreCheck mirror hands the filter only
	// scheme://authority (DomainFilterInput), so it adds no second raw-URL line. A Query() value is percent-encoded and has
	// no "://": the absolute URL is written into the path's query, which goes out as written.
	AddExpectedMessagePlain(TEXT("Duplicated delimiter '://' found in url"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FVaRestClient Client = MakeClient(BaseUrl);
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Client.Get(TEXT("/echo?next=https://example.invalid/after")), Outcome).Send();
	if (!TestTrue(TEXT("completes"), Wait(Outcome)) || !Succeeded(*this, TEXT("the request"), Outcome))
	{
		return false;
	}
	TestEqualSensitive(TEXT("the absolute URL in the query went out as written"), Outcome->Value.GetValue()[TEXT("query")][TEXT("next")][0].AsString(),
		FString(TEXT("https://example.invalid/after")));
	return true;
}

namespace VaRestTestsIntegrationPrivate
{
	/** Short real-clock delays, no jitter */
	FVaRestRetryPolicy QuickRetry(int32 MaxRetries)
	{
		FVaRestRetryPolicy Policy = FVaRestRetryPolicy::Exponential(MaxRetries);
		Policy.Backoff.BaseDelay = 0.1f;
		Policy.Backoff.Jitter = EVaRestJitter::None;
		return Policy;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationRetryTest, "VaRest.Integration.Http.Retry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationRetryTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	FVaRestClient Client = MakeClient(BaseUrl);
	Client.WithRetry(QuickRetry(3));
	// Own keys per run: the server counts calls per key, so no other test or run shares this state
	const FString Key = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	{
		// The burst below must outnumber the idle connections curl's pool can hold. While reuse is on that cap
		// (CURLMOPT_MAXCONNECTS) is this cvar (CurlHttpManager.cpp:139; default 5, HTTP.Build.cs:66, a project may raise it
		// with [HTTP.Curl] MaxConnects, CurlHttpManager.cpp:250-254), so it is read before the guard turns reuse off
		const IConsoleVariable* const MaxIdleConnections = IConsoleManager::Get().FindConsoleVariable(TEXT("HTTP.Curl.MaxIdleConnections"));
		if (!TestNotNull(TEXT("curl's idle-connection cvar exists (this half needs the curl transport)"), MaxIdleConnections))
		{
			return false;
		}
		const int32 WarmUps = FMath::Max(8, MaxIdleConnections->GetInt() + 1);
		// libcurl's Curl_retry_request resends any HTTP request on a fresh connect when a REUSED connection yields zero bytes
		// (lib/transfer.c:690-735 in the engine's 8.12.1 source; the engine ships 8.15.0): on a pooled keep-alive socket the
		// flaky route's refusals can be absorbed inside one VaRest attempt, and the test would not prove VaRest's retry. With
		// reuse off no connection is reused, so every refusal reaches VaRest. CurlHttp.cpp:204-207 sets CURLOPT_FORBID_REUSE
		// per handle from this cvar, and CurlHttpManager.cpp:131-141 (re-applied on change, :359-367, on the HTTP thread)
		// drops the idle cap to 0. The cvar is ECVF_SaveForNextBoot: the guard restores it with Set(), which writes no ini.
		TGuardConsoleVariable<bool> NoReuse(TEXT("http.CurlReuseConnectionEnabled"), false);
		// FORBID_REUSE closes a connection after its transfer; it does not evict the idle ones earlier tests left in curl's
		// pool, and one of them would be picked for the flaky request. So a burst of requests goes first: concurrent, so each
		// takes a different idle connection (or opens a new one), and each closes its own when done. WarmUps (above) is more
		// than the pool's cap, and at least eight: more than the tests before this one keep idle at once (the limiter test runs
		// the most, with at most four in flight).
		TArray<TSharedRef<TTestOutcome<FVaRestJson>, ESPMode::ThreadSafe>> Burst;
		for (int32 Index = 0; Index < WarmUps; ++Index)
		{
			Burst.Add(MakeOutcome<FVaRestJson>());
			Observe(Client.Get(TEXT("/echo")), Burst.Last()).Send();
		}
		bool bBurstDone = true;
		for (const auto& Done : Burst)
		{
			bBurstDone = Wait(Done) && bBurstDone;
		}
		if (!TestTrue(TEXT("the connection burst completes"), bBurstDone))
		{
			return false;
		}
		// The refusals are curl Warnings with its own text, and the engine logs the failed request (as in StatusAndErrors);
		// -1 because the number of lines depends on how many refusals reach VaRest. "libcurl" is the narrowest text curl
		// gives: the warning's wording ("Empty reply" / "Connection reset") varies with where the close lands.
		AddExpectedMessagePlain(TEXT("libcurl"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
		AddExpectedMessagePlain(TEXT("completed with reason 'ConnectionError'"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
		auto Flaky = MakeOutcome<FVaRestJson>();
		// Plan decision 15: curl reports a connection closed without a response as EHttpFailureReason::Other; it is retried
		Observe(Client.Get(TEXT("/flaky")).Query(TEXT("n"), TEXT("2")).Query(TEXT("key"), Key), Flaky).Send();
		if (!TestTrue(TEXT("the flaky route completes"), Wait(Flaky)))
		{
			return false;
		}
		if (Succeeded(*this, TEXT("the flaky route"), Flaky))
		{
			TestEqual(TEXT("... the server's third call"), Flaky->Value.GetValue()[TEXT("attempt")].AsInt(), 3);
			// Reuse is forbidden and the pool is drained, so curl cannot absorb a refusal: each one is a VaRest attempt
			TestEqual(TEXT("... after two VaRest retries (both refusals reached VaRest)"), Flaky->Response.Attempts, 3);
		}
	}
	auto After = MakeOutcome<FVaRestJson>();
	const double Start = FPlatformTime::Seconds();
	Observe(Client.Get(TEXT("/retry-after/2")).Query(TEXT("key"), Key).Query(TEXT("seconds"), TEXT("1")), After).Send();
	if (!TestTrue(TEXT("the Retry-After route completes"), Wait(After)))
	{
		return false;
	}
	if (Succeeded(*this, TEXT("Retry-After"), After))
	{
		TestEqual(TEXT("... on the third attempt"), After->Value.GetValue()[TEXT("attempt")].AsInt(), 3);
		TestTrue(TEXT("... after two Retry-After: 1 waits"), FPlatformTime::Seconds() - Start >= 2.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationRetryStreamTest, "VaRest.Integration.Http.RetryResendsStreamBody",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationRetryStreamTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	// M4a final review A8 (review C M-6; docs/dev/http-thread-completion.md, Apple 5): a multipart body with a file part goes
	// to the engine as a stream (SetContentFromStream). An Idempotent() POST is retried after the server's 503, and the
	// retry is a new engine request with a new stream; the server's 200 echoes the bytes the second attempt brought, so a
	// transport that did not send the whole stream again (NSURLSession re-reading a spent stream) fails this on a device.
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	FVaRestClient Client = MakeClient(BaseUrl);
	const FString Dir = MakeTempDir();
	const FString FilePath = Dir / TEXT("resent.bin");
	FFileHelper::SaveArrayToFile(Pattern(256 * 1024), *FilePath);
	FVaRestMultipart Streamed;
	Streamed.AddField(TEXT("note"), TEXT("again"));
	Streamed.AddFile(TEXT("doc"), FilePath);
	const TValueOrError<FVaRestRequestBody, FString> Built = FVaRestRequestBody::FromMultipart(Streamed);
	if (!TestTrue(TEXT("the body builds, streamed"), Built.HasValue() && Built.GetValue().IsStreamed()))
	{
		IFileManager::Get().DeleteDirectory(*Dir, false, true);
		return false;
	}
	const int64 Size = Built.GetValue().ContentLength;
	const FString Key = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	auto Resent = MakeOutcome<FVaRestJson>();
	Observe(Client.Post(TEXT("/retry-after/1")).Query(TEXT("key"), Key).Query(TEXT("seconds"), TEXT("0")).Body(Streamed).Idempotent().Retry(QuickRetry(3)), Resent).Send();
	const bool bDone = TestTrue(TEXT("the retried upload completes"), Wait(Resent));
	if (bDone && Succeeded(*this, TEXT("the retried upload"), Resent))
	{
		TestEqual(TEXT("... on the server's second call"), Resent->Value.GetValue()[TEXT("attempt")].AsInt(), 2);
		TestEqual(TEXT("... after one VaRest retry"), Resent->Response.Attempts, 2);
		TestEqual(TEXT("... whose attempt sent the whole streamed body again"), Resent->Value.GetValue()[TEXT("received")].AsInt64(), Size);
	}
	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationAuthTest, "VaRest.Integration.Http.AuthRefresh",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationAuthTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	// The token store is the server's, shared by every test: this one logs in fresh (which replaces both tokens) before it
	// expires anything, so it does not depend on what ran before it
	FVaRestBearerRefreshConfig Config;
	Config.RefreshPath = TEXT("/refresh");
	FVaRestClient Client = MakeClient(BaseUrl);
	Client.WithAuth(FVaRestAuth::BearerRefresh(Config));
	auto Login = MakeOutcome<FVaRestJson>();
	Observe(Client.Post(TEXT("/login")).NoAuth().Body(FVaRestJson::Object().Set(TEXT("user"), TEXT("neo")).Set(TEXT("password"), TEXT("trinity"))), Login).Send();
	if (!TestTrue(TEXT("login completes"), Wait(Login)) || !Succeeded(*this, TEXT("the login"), Login))
	{
		return false;
	}
	const FVaRestJson Tokens = Login->Value.GetValue();
	const FString FirstAccess = Tokens[TEXT("accessToken")].AsString();
	Client.SetSession(FirstAccess, Tokens[TEXT("refreshToken")].AsString(), Tokens[TEXT("expiresIn")].AsDouble());
	auto Expire = MakeOutcome<FVaRestJson>();
	Observe(Client.Post(TEXT("/expire")).NoAuth(), Expire).Send();
	if (!TestTrue(TEXT("expire completes"), Wait(Expire)) || !Succeeded(*this, TEXT("the expiry"), Expire))
	{
		return false;
	}
	auto Protected = MakeOutcome<FVaRestJson>();
	Observe(Client.Get(TEXT("/protected")), Protected).Send();
	if (!TestTrue(TEXT("completes"), Wait(Protected)))
	{
		return false;
	}
	if (Succeeded(*this, TEXT("401, refresh, replay"), Protected))
	{
		TestEqual(TEXT("... in two attempts"), Protected->Response.Attempts, 2);
		TestFalse(TEXT("... with a new access token in the session"), Client.GetSession().AccessToken == FirstAccess);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationEnvelopeTest, "VaRest.Integration.Http.Envelope",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationEnvelopeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	FVaRestClient Client = MakeClient(BaseUrl);
	Client.WithEnvelope(FVaRestEnvelope::DataAt(TEXT("data")).SuccessWhen(TEXT("ok"), { TEXT("true") }).ErrorCodeAt(TEXT("code")).ErrorMessageAt(TEXT("message"))
		.ParseErrorBodies());
	auto Ok = MakeOutcome<FVaRestJson>();
	auto Err = MakeOutcome<FVaRestJson>();
	auto Gone = MakeOutcome<FVaRestJson>();
	Observe(Client.Get(TEXT("/envelope/ok")), Ok).Send();
	Observe(Client.Get(TEXT("/envelope/err")), Err).Send();
	Observe(Client.Get(TEXT("/envelope/gone")), Gone).Send();
	if (!TestTrue(TEXT("all complete"), WaitAll(Ok, Err, Gone)))
	{
		return false;
	}
	if (Succeeded(*this, TEXT("the ok envelope"), Ok))
	{
		TestEqual(TEXT("... unwrapped"), Ok->Value.GetValue()[TEXT("id")].AsInt(), 7);
	}
	TestTrue(TEXT("a failed predicate: Kind Server with the envelope's code and message"), Err->Error.Kind == EVaRestErrorKind::Server
		&& Err->Error.Code == TEXT("E42") && Err->Error.Message == TEXT("the envelope says no"));
	TestTrue(TEXT("a 404 envelope: Kind Http, its code read (bParseErrorBodies)"), Gone->Error.Kind == EVaRestErrorKind::Http && Gone->Error.HttpStatus == 404
		&& Gone->Error.Code == TEXT("E404") && Gone->Error.Message == TEXT("not here"));
	return true;
}

#endif
