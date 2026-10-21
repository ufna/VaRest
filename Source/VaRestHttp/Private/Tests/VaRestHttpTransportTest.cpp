// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "VaRestBodySink.h"
#include "VaRestClock.h"
#include "VaRestHttpTesting.h"
#include "VaRestHttpTransportInternal.h"
#include "VaRestTransport.h"
#include "VaRestUrl.h"

namespace VaRest::Http::TransportTestPrivate
{
	struct FDone
	{
		std::atomic<bool> bDone{ false };
		std::atomic<int32> Calls{ 0 };
		bool bOnGameThread = true;
		FVaRestTransportResult Result;
	};

	/**
	 * Sets a string console variable for a scope. TGuardConsoleVariable<FString> does not compile in UE 5.8: it calls
	 * Set(const FString&, FSetContext) (IConsoleManager.h:1424, :1445), and IConsoleVariable has that overload only for
	 * const TCHAR*, bool, int32 and float (IConsoleManager.h:737-761).
	 */
	struct FStringCVarGuard
	{
		FStringCVarGuard(const TCHAR* Name, const TCHAR* Value)
			: Variable(IConsoleManager::Get().FindConsoleVariable(Name))
		{
			if (Variable)
			{
				Previous = Variable->GetString();
				Variable->Set(Value, ECVF_SetByCode);
			}
		}
		~FStringCVarGuard()
		{
			if (Variable)
			{
				Variable->Set(*Previous, ECVF_SetByCode);
			}
		}
		UE_NONCOPYABLE(FStringCVarGuard);

		IConsoleVariable* Variable = nullptr;
		FString Previous;
	};

	/**
	 * Sets [HTTP.Curl] bAllowSeekFunction in GEngineIni for a scope. CurlMayRewindBodies reads it per request; the engine's
	 * curl keeps its own copy, read at init and on config updates (CurlHttpManager.cpp:304, :502-511), so curl itself is
	 * unchanged.
	 */
	struct FCurlSeekConfigGuard
	{
		explicit FCurlSeekConfigGuard(bool bAllow)
		{
			GConfig->GetBool(TEXT("HTTP.Curl"), TEXT("bAllowSeekFunction"), bPrevious, GEngineIni);
			GConfig->SetBool(TEXT("HTTP.Curl"), TEXT("bAllowSeekFunction"), bAllow, GEngineIni);
		}
		~FCurlSeekConfigGuard()
		{
			GConfig->SetBool(TEXT("HTTP.Curl"), TEXT("bAllowSeekFunction"), bPrevious, GEngineIni);
		}
		UE_NONCOPYABLE(FCurlSeekConfigGuard);

		bool bPrevious = false;
	};

	FVaRestTransportComplete Into(const TSharedRef<FDone, ESPMode::ThreadSafe>& Done)
	{
		return [Done](FVaRestTransportResult&& Result)
		{
			Done->bOnGameThread = IsInGameThread();
			Done->Result = MoveTemp(Result);
			++Done->Calls;
			Done->bDone = true;
		};
	}

	/** Every test drains what it sends: a call the wait gave up on is canceled and waited for once more */
	void Drain(const TSharedRef<IVaRestTransportCall, ESPMode::ThreadSafe>& Call, const TSharedRef<FDone, ESPMode::ThreadSafe>& Done)
	{
		if (!Done->bDone.load())
		{
			Call->Cancel();
			VaRest::Http::Testing::PumpUntil([Done] { return Done->bDone.load(); }, 5.0);
		}
	}

	/** Starts Request on the FHttpModule transport and pumps until it completes; Done holds the result only when this returns true */
	bool SendAndWait(FVaRestTransportRequest&& Request, const TSharedRef<FDone, ESPMode::ThreadSafe>& Done, double TimeoutSeconds = 10.0)
	{
		const TSharedRef<IVaRestTransportCall, ESPMode::ThreadSafe> Call = VaRest::Http::GetDefaultTransport()->Start(MoveTemp(Request), Into(Done));
		if (VaRest::Http::Testing::PumpUntil([Done] { return Done->bDone.load(); }, TimeoutSeconds))
		{
			return true;
		}
		Drain(Call, Done);
		return false;
	}

	/** True when Request completed before Start returned: it never reached the engine. Otherwise waits for it and drains it. */
	bool CompletesInsideStart(FVaRestTransportRequest&& Request, const TSharedRef<FDone, ESPMode::ThreadSafe>& Done)
	{
		const TSharedRef<IVaRestTransportCall, ESPMode::ThreadSafe> Call = VaRest::Http::GetDefaultTransport()->Start(MoveTemp(Request), Into(Done));
		if (Done->bDone.load())
		{
			return true;
		}
		VaRest::Http::Testing::PumpUntil([Done] { return Done->bDone.load(); }, 10.0);
		Drain(Call, Done);
		return false;
	}

	/**
	 * A network-free peer for the real transport: libcurl's file:// handler moves a file's bytes through the same write
	 * callback as an HTTP body (CURLOPT_WRITEFUNCTION, CurlHttp.cpp:1007-1009) and reads a PUT body through the same read
	 * callback (CURLOPT_UPLOAD + CURLOPT_READFUNCTION, CurlHttp.cpp:956-968, :995-1001). It answers no status line and no
	 * headers; FinishRequest fails a status <= 0 only for http(s) URLs (CurlHttp.cpp:1291-1296).
	 */
	FString FileUrl(const FString& Path)
	{
		// Each segment percent-encoded (task 7 review M-8): libcurl rejects a URL holding a space, and reads '#' and '%'
		TArray<FString> Segments;
		FPaths::ConvertRelativePathToFull(Path).ParseIntoArray(Segments, TEXT("/"), /*InCullEmpty*/ true);
		FString Url = TEXT("file://");
		for (const FString& Segment : Segments)
		{
			Url += TEXT("/");
			Url += VaRest::Http::PercentEncode(Segment);
		}
		return Url;
	}

	FVaRestTransportRequest GetFile(const FString& Path)
	{
		FVaRestTransportRequest Request;
		Request.Verb = TEXT("GET");
		Request.Url = FileUrl(Path);
		return Request;
	}

	/** A multipart-shaped upload: five bytes in memory, then PartSize bytes read from PartFile while uploading */
	FVaRestTransportRequest PutSegments(const FString& PartFile, int64 PartSize, const FString& Target, bool bCounted)
	{
		TArray<FVaRestBodySegment> Segments;
		Segments.AddDefaulted_GetRef().Bytes = { 'h', 'e', 'a', 'd', ':' };
		FVaRestBodySegment& FilePart = Segments.AddDefaulted_GetRef();
		FilePart.FilePath = PartFile;
		FilePart.FileSize = PartSize;

		FVaRestTransportRequest Request;
		Request.Verb = TEXT("PUT");
		Request.Url = FileUrl(Target);
		Request.BodySegments = MoveTemp(Segments);
		Request.Probe->UploadTotal = 5 + PartSize;
		// A progress callback sends it through FCountingUpload (ShouldCountUpload); without one, the segment stream goes to the engine as is
		Request.bProgressCallback = bCounted;
		Request.bReportProgress = bCounted;
		return Request;
	}

	/** A streamed file upload whose body was built when File had BuiltSize bytes */
	FVaRestTransportRequest PutFile(const FString& File, int64 BuiltSize, const FString& Target, bool bCounted)
	{
		FVaRestTransportRequest Request;
		Request.Verb = TEXT("PUT");
		Request.Url = FileUrl(Target);
		Request.BodyFile = File;
		Request.Probe->UploadTotal = BuiltSize;
		Request.bProgressCallback = bCounted;
		Request.bReportProgress = bCounted;
		return Request;
	}

	/** Truncates Path to zero bytes in place: an open reader keeps its handle and finds the bytes gone */
	void TruncateInPlace(const FString& Path)
	{
		TUniquePtr<IFileHandle> Truncated(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*Path, /*bAppend*/ false));
	}

	/** An absolute test directory, emptied; its name holds a space, so every file:// URL needs encoding (review M-8) */
	FString FreshDir(const TCHAR* Name)
	{
		const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("VaRestTests") / Name);
		IFileManager::Get().DeleteDirectory(*Dir, false, true);
		IFileManager::Get().MakeDirectory(*Dir, true);
		return Dir;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpMemorySinkTest, "VaRest.Http.Transport.MemorySinkSlack",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpMemorySinkTest::RunTest(const FString& Parameters)
{
	using VaRest::Http::FBodySink;
	TArray<uint8> Chunk;
	Chunk.Init('x', 256);

	FBodySink Sink;
	Sink.Write(Chunk.GetData(), Chunk.Num(), 1024);
	const uint8* FirstBuffer = Sink.GetDataInternal();
	for (int32 i = 1; i < 4; ++i)
	{
		Sink.Write(Chunk.GetData(), Chunk.Num(), -1); // later chunks: the length is already known
	}
	TestTrue(TEXT("no reallocation after the first chunk"), Sink.GetDataInternal() == FirstBuffer);
	TArray<uint8> Body = Sink.TakeBytes();
	TestEqual(TEXT("the whole body"), Body.Num(), 1024);
	TestTrue(TEXT("one byte of slack: ParseOwned appends its terminator without reallocating (spec 2.1)"), Body.Max() > Body.Num());
	TestTrue(TEXT("TakeBytes moves the buffer it filled"), Body.GetData() == FirstBuffer);
	TestFalse(TEXT("review C-4: TakeBytes seals the sink; a later write fails"), Sink.Write(Chunk.GetData(), 1, -1));

	FBodySink Chunked; // no Content-Length (chunked transfer, or compressed)
	for (int32 i = 0; i < 20; ++i)
	{
		Chunked.Write(Chunk.GetData(), Chunk.Num(), -1);
	}
	TArray<uint8> Unsized = Chunked.TakeBytes();
	TestTrue(TEXT("unknown length still ends with slack"), Unsized.Num() == 20 * 256 && Unsized.Max() > Unsized.Num());

	FBodySink Wrong; // a Content-Length smaller than the body
	Wrong.Write(Chunk.GetData(), Chunk.Num(), 10);
	TArray<uint8> Larger = Wrong.TakeBytes();
	TestTrue(TEXT("a wrong Content-Length still ends with slack"), Larger.Num() == 256 && Larger.Max() > Larger.Num());

	// Review M-6: a Content-Length is a claim; a lying one must not allocate gigabytes up front
	FBodySink Liar;
	Liar.Write(Chunk.GetData(), Chunk.Num(), 2000000000);
	// Review N-I2: Reserve(N) quantizes to the allocator's bin (Array.h:3268-3286, ContainerAllocationPolicies.h:224-239),
	// so capacities are bounds, not exact values
	TestTrue(TEXT("a huge Content-Length reserves at most ReserveCap + 1 (quantized)"),
		Liar.GetCapacityInternal() <= static_cast<int64>(FMemory::QuantizeSize(FBodySink::ReserveCap + 1)) && Liar.GetCapacityInternal() < 2000000000);
	TestTrue(TEXT("... and the body still ends with slack"), Liar.TakeBytes().Max() > Chunk.Num());

	// Review M13: a JSON result's cap stops the transfer instead of receiving the whole body first
	FBodySink Capped(300);
	TestTrue(TEXT("within the cap"), Capped.Write(Chunk.GetData(), Chunk.Num(), 1024));
	// Task 7 review M-3: no byte past the cap is kept, so the cap bounds the reserve, below 8 MiB too
	TestTrue(TEXT("a Content-Length over the JSON cap reserves the cap + 1"), Capped.GetCapacityInternal() >= 301
		&& Capped.GetCapacityInternal() <= static_cast<int64>(FMemory::QuantizeSize(301)));
	TestFalse(TEXT("past the cap the write fails"), Capped.Write(Chunk.GetData(), Chunk.Num(), -1));
	TestTrue(TEXT("... and says why"), Capped.IsTooLarge() && Capped.HasError());

	FBodySink Roomy(5000);
	TestTrue(TEXT("under its JSON cap"), Roomy.Write(Chunk.GetData(), Chunk.Num(), 1024));
	TestTrue(TEXT("the reserve follows a Content-Length under the JSON cap"), Roomy.GetCapacityInternal() >= 1025
		&& Roomy.GetCapacityInternal() <= static_cast<int64>(FMemory::QuantizeSize(1025)));

	FBodySink SmallCapLiar(1000);
	SmallCapLiar.Write(Chunk.GetData(), Chunk.Num(), 2000000000);
	TestTrue(TEXT("a lying Content-Length under a small JSON cap reserves the cap + 1, not 8 MiB (review M-3)"),
		SmallCapLiar.GetCapacityInternal() <= static_cast<int64>(FMemory::QuantizeSize(1001)));

	FBodySink Empty;
	TArray<uint8> Nothing = Empty.TakeBytes();
	TestTrue(TEXT("an empty body has room for the terminator too"), Nothing.Num() == 0 && Nothing.Max() >= 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpFileSinkTest, "VaRest.Http.Transport.FileSink",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpFileSinkTest::RunTest(const FString& Parameters)
{
	using VaRest::Http::FBodySink;
	const FString Dir = FPaths::ProjectSavedDir() / TEXT("VaRestTests") / TEXT("sink");
	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	const FString Path = Dir / TEXT("sub") / TEXT("download.part");
	{
		FBodySink Sink{ FString(Path) };
		TestFalse(TEXT("nothing is created before the first byte"), IFileManager::Get().FileExists(*Path));
		const uint8 A[] = { 1, 2, 3 };
		const uint8 B[] = { 4, 5 };
		TestTrue(TEXT("write"), Sink.Write(A, 3, -1) && Sink.Write(B, 2, -1));
		TestTrue(TEXT("close"), Sink.CloseFile());
		TestTrue(TEXT("a file sink is a file sink"), Sink.IsFile() && Sink.Num() == 5);
		TestFalse(TEXT("CloseFile seals it"), Sink.Write(A, 1, -1));
	}
	TArray<uint8> OnDisk;
	TestTrue(TEXT("the directory is created and the bytes written"), FFileHelper::LoadFileToArray(OnDisk, *Path) && OnDisk == TArray<uint8>({ 1, 2, 3, 4, 5 }));

	// A parent that is a file: the directory cannot be created, so the write fails and the sink says so
	const FString Blocker = Dir / TEXT("blocker");
	FFileHelper::SaveStringToFile(TEXT("x"), *Blocker);
	FBodySink Blocked{ Blocker / TEXT("child.part") };
	const uint8 C[] = { 9 };
	TestFalse(TEXT("an unwritable path fails"), Blocked.Write(C, 1, -1));
	TestTrue(TEXT("... and remembers it"), Blocked.HasError() && !Blocked.CloseFile());
	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpCountingUploadTest, "VaRest.Http.Transport.CountingUpload",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpCountingUploadTest::RunTest(const FString& Parameters)
{
	// Review I-2: the archive curl reads a progress-watched upload through
	TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> Clock = MakeShared<FVaRestVirtualClock, ESPMode::ThreadSafe>();
	Clock->Advance(2.0);
	TSharedRef<FVaRestTransportProbe, ESPMode::ThreadSafe> Probe = MakeShared<FVaRestTransportProbe, ESPMode::ThreadSafe>();
	VaRest::Http::FCountingUpload Upload(TArray<uint8>{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 }, Probe, Clock, /*bStamp*/ true);
	TestEqual(TEXT("TotalSize is the Content-Length curl sends (CurlHttp.cpp:896-902)"), Upload.TotalSize(), static_cast<int64>(10));
	uint8 Out[10] = {};
	Upload.Serialize(Out, 4);
	TestEqual(TEXT("counted as curl reads"), Probe->BytesSent.load(), static_cast<int64>(4));
	TestTrue(TEXT("not done yet"), Probe->UploadDoneAt.load() < 0.0);
	Upload.Seek(4);
	Upload.Serialize(Out + 4, 6);
	TestEqual(TEXT("all of it"), Probe->BytesSent.load(), static_cast<int64>(10));
	TestEqual(TEXT("UploadDoneAt stamped at the last byte (IN-3)"), Probe->UploadDoneAt.load(), 2.0);
	TestTrue(TEXT("the bytes are the body's"), Out[0] == 1 && Out[9] == 10);
	Upload.Serialize(Out, 1);
	TestTrue(TEXT("reading past the end is an error, not garbage: zeroed, and the request will fail (review N-4)"), Upload.IsError() && Upload.HasReadFailed() && Out[0] == 0);

	TArray<FVaRestBodySegment> Segments;
	Segments.AddDefaulted_GetRef().Bytes = { 'a', 'b' };
	Segments.AddDefaulted_GetRef().Bytes = { 'c' };
	TSharedRef<FVaRestTransportProbe, ESPMode::ThreadSafe> SegmentProbe = MakeShared<FVaRestTransportProbe, ESPMode::ThreadSafe>();
	VaRest::Http::FCountingUpload Parts(MakeShared<FVaRestSegmentStream, ESPMode::ThreadSafe>(MoveTemp(Segments)), SegmentProbe, Clock, false);
	TestEqual(TEXT("an inner archive's size is its own TotalSize(): Content-Length and the bytes agree (review N-4)"), Parts.TotalSize(), static_cast<int64>(3));
	uint8 Abc[3] = {};
	Parts.Serialize(Abc, 3);
	TestTrue(TEXT("an inner archive (multipart segments) is read through and counted"), Abc[0] == 'a' && Abc[2] == 'c' && SegmentProbe->BytesSent.load() == 3);
	TestTrue(TEXT("... without stamping when not asked"), SegmentProbe->UploadDoneAt.load() < 0.0);

	// Review N-4: the engine ignores the archive's error (HttpRequestPayload.cpp:86-96), so a file that cannot be read
	// must leave zeroes, not whatever the buffer held, and mark the upload failed
	TSharedRef<FVaRestTransportProbe, ESPMode::ThreadSafe> GoneProbe = MakeShared<FVaRestTransportProbe, ESPMode::ThreadSafe>();
	VaRest::Http::FCountingUpload Gone(FPaths::ProjectSavedDir() / TEXT("VaRestTests") / TEXT("no-such-upload.bin"), 4, GoneProbe, Clock, false);
	uint8 Stale[4] = { 0xAA, 0xAA, 0xAA, 0xAA };
	Gone.Serialize(Stale, 4);
	TestTrue(TEXT("a file that cannot be opened fails the read"), Gone.IsError() && Gone.HasReadFailed());
	TestTrue(TEXT("... and leaves zeroes in the engine's buffer"), Stale[0] == 0 && Stale[3] == 0);
	TestEqual(TEXT("... and counts nothing"), GoneProbe->BytesSent.load(), static_cast<int64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpCountingGateTest, "VaRest.Http.Transport.CountingGate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpCountingGateTest::RunTest(const FString& Parameters)
{
	using VaRest::Http::ShouldCountUpload;
	// Decision 29 (reviews N-1, N-9): when an upload goes through the counting archive
#if PLATFORM_APPLE
	TestFalse(TEXT("Apple: never (NSURLSession reports upload progress, and cannot resend a body stream)"), ShouldCountUpload(100, true, true, false));
#else
	TestTrue(TEXT("a progress callback counts the upload"), ShouldCountUpload(100, true, true, false));
	TestTrue(TEXT("... also when curl may rewind bodies (OnProgress documents the cost)"), ShouldCountUpload(100, true, true, true));
	TestTrue(TEXT("trace metadata alone counts it when curl cannot rewind a body anyway (the default)"), ShouldCountUpload(100, false, true, false));
	TestFalse(TEXT("... but not when [HTTP.Curl] bAllowSeekFunction lets curl rewind an in-memory body: the trace never changes an outcome"), ShouldCountUpload(100, false, true, true));
	TestFalse(TEXT("nothing asked: the zero-copy path"), ShouldCountUpload(100, false, false, false));
#endif
	TestFalse(TEXT("no body: nothing to count"), ShouldCountUpload(0, true, true, false));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTransportTimeoutsTest, "VaRest.Http.Transport.ActivityFollowsTotal",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTransportTimeoutsTest::RunTest(const FString& Parameters)
{
	using VaRest::Http::EffectiveActivityTimeout;
	// Review I-5. IHttpRequest has GetTimeout (IHttpRequest.h:341-347) but no activity getter, so the applied value is
	// the function the transport calls, tested here
	TestEqual(TEXT("a total timeout alone also bounds silence, so the engine's 30 s default cannot fire first"), EffectiveActivityTimeout(60.f, 0.f), 60.f);
	TestEqual(TEXT("an explicit activity timeout wins"), EffectiveActivityTimeout(60.f, 5.f), 5.f);
	TestEqual(TEXT("neither: VaRest sets none (the engine's defaults apply)"), EffectiveActivityTimeout(0.f, 0.f), 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTransportThreadTest, "VaRest.Http.Transport.CompletesOffGameThread",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTransportThreadTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::TransportTestPrivate;
	// curl reports the refused connection as a Warning with its own text; how many lines depends on curl
	AddExpectedMessagePlain(TEXT("libcurl"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	TSharedRef<FDone, ESPMode::ThreadSafe> Done = MakeShared<FDone, ESPMode::ThreadSafe>();
	FVaRestTransportRequest Request;
	Request.Verb = TEXT("GET");
	Request.Url = TEXT("http://127.0.0.1:1/"); // port 1: nothing listens, the connection is refused at once
	Request.Timeout = 10.f;
	TestTrue(TEXT("completes"), SendAndWait(MoveTemp(Request), Done, 15.0));
	// Spec 9 risk: HTTP-thread completion per backend. This is the Linux (curl) check; docs/dev/http-thread-completion.md lists the others.
	TestFalse(TEXT("CompleteOnHttpThread: the completion runs on the HTTP thread, not the game thread"), Done->bOnGameThread);
	TestTrue(TEXT("a refused connection is a Connection failure"), Done->Result.Failure == EVaRestTransportFailure::Connection);
	TestEqual(TEXT("no status"), Done->Result.Status, 0);
	VaRest::Http::Testing::PumpUntil([] { return false; }, 0.2);
	TestEqual(TEXT("exactly once"), Done->Calls.load(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTransportRefusedTest, "VaRest.Http.Transport.RefusedRequestCompletesOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTransportRefusedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::TransportTestPrivate;
	// FHttpRequestCommon::PreCheck refuses an empty URL (HttpRequestCommon.cpp:96-101) and still finishes the request,
	// from an HTTP-thread task under CompleteOnHttpThread (HttpRequestCommon.cpp:224-237)
	AddExpectedMessagePlain(TEXT("No URL was specified"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	TSharedRef<FDone, ESPMode::ThreadSafe> Done = MakeShared<FDone, ESPMode::ThreadSafe>();
	FVaRestTransportRequest Request;
	Request.Verb = TEXT("GET");
	TestTrue(TEXT("a refused request still completes"), SendAndWait(MoveTemp(Request), Done, 5.0));
	TestTrue(TEXT("... as a failure"), Done->Result.Failure != EVaRestTransportFailure::None);
	VaRest::Http::Testing::PumpUntil([] { return false; }, 0.2);
	TestEqual(TEXT("... exactly once"), Done->Calls.load(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTransportMockTest, "VaRest.Http.Transport.EngineMockResponse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTransportMockTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::TransportTestPrivate;
	// Review M-15: an engine mock response (http.UrlPatternsToMockResponse, HttpManager.cpp:43-48, parsed at :773-839) is
	// appended to the engine's payload, not to the body stream (HttpRequestCommon.cpp:124-160, triggered at :190)
	FStringCVarGuard Mock(TEXT("http.UrlPatternsToMockResponse"), TEXT("mock.varest.test->200++{\"mocked\":true}++Content-Type:application/json"));
	if (!TestNotNull(TEXT("the engine's mock variable exists"), Mock.Variable))
	{
		return false;
	}
	TSharedRef<FDone, ESPMode::ThreadSafe> Done = MakeShared<FDone, ESPMode::ThreadSafe>();
	FVaRestTransportRequest Request;
	Request.Verb = TEXT("GET");
	Request.Url = TEXT("http://mock.varest.test/x");
	TestTrue(TEXT("completes"), SendAndWait(MoveTemp(Request), Done, 5.0));
	const FString Body = FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Done->Result.Body.GetData()), Done->Result.Body.Num());
	TestTrue(TEXT("the mocked status"), Done->Result.Status == 200 && Done->Result.Failure == EVaRestTransportFailure::None);
	TestEqualSensitive(TEXT("the engine's payload is taken when the stream got nothing"), Body, FString(TEXT("{\"mocked\":true}")));
	TestTrue(TEXT("... with the slack ParseOwned needs"), Done->Result.Body.Max() > Done->Result.Body.Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpUploadStampGateTest, "VaRest.Http.Transport.UploadStampGate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpUploadStampGateTest::RunTest(const FString& Parameters)
{
	using VaRest::Http::ShouldStampUploadFromProgress;
	// Task 7 review I-1: when the engine's progress delegate may stamp UploadDoneAt (IN-3)
	TestFalse(TEXT("no body: never -- the first progress call would qualify, after StatusAt"), ShouldStampUploadFromProgress(0, true));
	TestTrue(TEXT("a body, where the backend reports upload progress itself (Apple)"), ShouldStampUploadFromProgress(100, true));
	TestFalse(TEXT("a body on curl: its progress cannot see the upload end; FCountingUpload stamps, or nothing does"), ShouldStampUploadFromProgress(100, false));
	TestFalse(TEXT("neither"), ShouldStampUploadFromProgress(0, false));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpDroppedCompletionTest, "VaRest.Http.Transport.DroppedCompletion",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpDroppedCompletionTest::RunTest(const FString& Parameters)
{
	using VaRest::Http::FBodySink;
	using VaRest::Http::FHttpTransportCompletion;
	// Review I-3, task 7 review M-6: what the engine's delegates share dies with the request; uncompleted, it still finishes it
	struct FSeen
	{
		int32 Calls = 0;
		FVaRestTransportResult Last;
	};
	const TSharedRef<FSeen, ESPMode::ThreadSafe> Seen = MakeShared<FSeen, ESPMode::ThreadSafe>();
	auto Make = [Seen]()
	{
		return MakeShared<FHttpTransportCompletion, ESPMode::ThreadSafe>([Seen](FVaRestTransportResult&& Result)
		{
			++Seen->Calls;
			Seen->Last = MoveTemp(Result);
		}, MakeShared<FBodySink, ESPMode::ThreadSafe>(), MakeShared<FVaRestTransportProbe, ESPMode::ThreadSafe>(), nullptr);
	};

	{
		const TSharedRef<FHttpTransportCompletion, ESPMode::ThreadSafe> Dropped = Make();
		TestEqual(TEXT("nothing before it dies"), Seen->Calls, 0);
	}
	TestEqual(TEXT("destroyed without completing: exactly one call"), Seen->Calls, 1);
	TestTrue(TEXT("... Failure Other, detail TransportDropped"),
		Seen->Last.Failure == EVaRestTransportFailure::Other && Seen->Last.FailureDetail == FName(TEXT("TransportDropped")));

	{
		const TSharedRef<FHttpTransportCompletion, ESPMode::ThreadSafe> Completed = Make();
		FVaRestTransportComplete First = Completed->Claim();
		FVaRestTransportComplete Second = Completed->Claim();
		TestTrue(TEXT("the first claim gets OnComplete"), static_cast<bool>(First));
		TestFalse(TEXT("... a later one nothing"), static_cast<bool>(Second));
		TestTrue(TEXT("... and the completion is done"), Completed->IsDone());
		if (First)
		{
			FVaRestTransportResult Answered;
			Answered.Status = 200;
			First(MoveTemp(Answered));
		}
	}
	TestEqual(TEXT("destroyed after completing: nothing more"), Seen->Calls, 2);
	TestEqual(TEXT("... the one call was the completion"), Seen->Last.Status, 200);
	return true;
}

#if PLATFORM_LINUX
// libcurl backs FHttpModule on Linux (UnixPlatformHttp); these use its file:// handler as a network-free peer, and POSIX
// in-place truncation under an open reader
// (TransportTestPrivate::FileUrl). The Task 16 test server covers the same paths over HTTP.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTransportSinkTest, "VaRest.Http.Transport.ReceivesThroughTheSink",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTransportSinkTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::TransportTestPrivate;
	// The capped download ends in CURLE_WRITE_ERROR, which curl and the engine log as Warnings (CurlHttp.cpp:1346-1349, HttpRequestCommon.cpp:330-334)
	AddExpectedMessagePlain(TEXT("libcurl"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	AddExpectedMessagePlain(TEXT("completed with reason"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	TestEqual(TEXT("the file:// helper percent-encodes each segment (review M-8)"), FileUrl(TEXT("/tmp/a b/c#d%e")), FString(TEXT("file:///tmp/a%20b/c%23d%25e")));
	const FString Dir = FreshDir(TEXT("transport sink"));
	const FString Source = Dir / TEXT("body.bin");
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(100000);
	for (int32 i = 0; i < Bytes.Num(); ++i)
	{
		Bytes[i] = static_cast<uint8>(i % 251);
	}
	if (!TestTrue(TEXT("the source file is written"), FFileHelper::SaveArrayToFile(Bytes, *Source)))
	{
		return false;
	}

	{
		VaRest::Http::Testing::FLogCapture Capture;
		TSharedRef<FDone, ESPMode::ThreadSafe> Memory = MakeShared<FDone, ESPMode::ThreadSafe>();
		if (!TestTrue(TEXT("a download to memory completes"), SendAndWait(GetFile(Source), Memory)))
		{
			return false;
		}
		TestFalse(TEXT("on the HTTP thread"), Memory->bOnGameThread);
		TestTrue(TEXT("the body arrives through the stream delegate and the sink, whole"), Memory->Result.Failure == EVaRestTransportFailure::None && Memory->Result.Body == Bytes);
		TestTrue(TEXT("... with the slack ParseOwned needs"), Memory->Result.Body.Max() > Memory->Result.Body.Num());
		TestEqual(TEXT("BytesDown is what the sink took"), Memory->Result.BytesDown, static_cast<int64>(Bytes.Num()));
		// Review M-16 as built: a response header read before the response is ready logs a Warning and reads nothing
		// (HttpResponseCommon.cpp:76-92, bIsReady is set only at HttpRequestCommon.cpp:870-873)
		TestEqual(TEXT("no header is read from the response mid-transfer"), Capture.Count(TEXT("Can't get cached header")), 0);
	}

	// Review M13: the JSON cap stops the transfer while it runs
	FVaRestTransportRequest Capped = GetFile(Source);
	Capped.MaxBodyBytes = 1000;
	TSharedRef<FDone, ESPMode::ThreadSafe> TooLarge = MakeShared<FDone, ESPMode::ThreadSafe>();
	if (!TestTrue(TEXT("a capped download completes"), SendAndWait(MoveTemp(Capped), TooLarge)))
	{
		return false;
	}
	TestTrue(TEXT("past MaxBodyBytes: ResponseTooLarge"), TooLarge->Result.Failure == EVaRestTransportFailure::Other && TooLarge->Result.FailureDetail == FName(TEXT("ResponseTooLarge")));
	TestTrue(TEXT("... nothing kept, and never more than the cap held"), TooLarge->Result.Body.Num() == 0 && TooLarge->Result.BytesDown <= 1000);

	// Spec 3.2 SaveTo: the sink streams to the file instead of memory
	const FString Target = Dir / TEXT("out") / TEXT("download.part");
	FVaRestTransportRequest Save = GetFile(Source);
	Save.ResponseFile = Target;
	TSharedRef<FDone, ESPMode::ThreadSafe> Saved = MakeShared<FDone, ESPMode::ThreadSafe>();
	if (!TestTrue(TEXT("a download to a file completes"), SendAndWait(MoveTemp(Save), Saved)))
	{
		return false;
	}
	TArray<uint8> OnDisk;
	TestTrue(TEXT("the body is in the file, not in memory"), Saved->Result.Failure == EVaRestTransportFailure::None && Saved->Result.bBodyInFile && Saved->Result.Body.Num() == 0);
	TestTrue(TEXT("... all of it"), FFileHelper::LoadFileToArray(OnDisk, *Target) && OnDisk == Bytes && Saved->Result.BytesDown == Bytes.Num());

	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTransportUploadReadTest, "VaRest.Http.Transport.UploadReadFailed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTransportUploadReadTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::TransportTestPrivate;
	// Task 3 review M4: the engine ignores an upload archive's error (FillOutputBuffer, HttpRequestPayload.cpp:77-97), so a
	// multipart file that shrank after its part was added sends zeroes (FVaRestSegmentStream's one warning per stream) and
	// the request would end as the peer answered. Both upload paths must fail it instead.
	AddExpectedMessagePlain(TEXT("could not be read at byte"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);
	AddExpectedMessagePlain(TEXT("libcurl"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	AddExpectedMessagePlain(TEXT("completed with reason"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	const FString Dir = FreshDir(TEXT("transport upload"));
	const FString Part = Dir / TEXT("part.bin");
	const FString Target = Dir / TEXT("put.bin");
	TArray<uint8> PartBytes;
	PartBytes.Init('p', 1000);
	if (!TestTrue(TEXT("the part file is written"), FFileHelper::SaveArrayToFile(PartBytes, *Part)))
	{
		return false;
	}

	// The control: an intact file is read through the engine and sent
	TSharedRef<FDone, ESPMode::ThreadSafe> Intact = MakeShared<FDone, ESPMode::ThreadSafe>();
	if (!TestTrue(TEXT("an intact upload completes"), SendAndWait(PutSegments(Part, PartBytes.Num(), Target, false), Intact)))
	{
		return false;
	}
	TArray<uint8> Sent;
	TArray<uint8> Expected = { 'h', 'e', 'a', 'd', ':' };
	Expected.Append(PartBytes);
	TestTrue(TEXT("the segments were read by the engine and sent"), FFileHelper::LoadFileToArray(Sent, *Target) && Sent == Expected);
	TestTrue(TEXT("... and nothing says the read failed"), Intact->Result.FailureDetail != FName(TEXT("UploadReadFailed")));

	// The file shrinks after its part was added
	TArray<uint8> Shrunk;
	Shrunk.Init('s', 10);
	if (!TestTrue(TEXT("the part file shrinks"), FFileHelper::SaveArrayToFile(Shrunk, *Part)))
	{
		return false;
	}
	TSharedRef<FDone, ESPMode::ThreadSafe> Plain = MakeShared<FDone, ESPMode::ThreadSafe>();
	if (!TestTrue(TEXT("the default path completes"), SendAndWait(PutSegments(Part, PartBytes.Num(), Target, false), Plain)))
	{
		return false;
	}
	TestTrue(TEXT("default path (the segment stream given to the engine): UploadReadFailed"),
		Plain->Result.Failure == EVaRestTransportFailure::Other && Plain->Result.FailureDetail == FName(TEXT("UploadReadFailed")));
	TestEqual(TEXT("... with no body"), Plain->Result.Body.Num(), 0);

	TSharedRef<FDone, ESPMode::ThreadSafe> Counted = MakeShared<FDone, ESPMode::ThreadSafe>();
	if (!TestTrue(TEXT("the counting path completes"), SendAndWait(PutSegments(Part, PartBytes.Num(), Target, true), Counted)))
	{
		return false;
	}
	TestTrue(TEXT("counting path (FCountingUpload around the stream): UploadReadFailed"),
		Counted->Result.Failure == EVaRestTransportFailure::Other && Counted->Result.FailureDetail == FName(TEXT("UploadReadFailed")));

	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpCountingUploadShrinksTest, "VaRest.Http.Transport.CountingUploadShrinks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpCountingUploadShrinksTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::TransportTestPrivate;
	// Task 7 review M-5: a counted file upload that shrinks while it is read fails once, as UploadReadFailed, with no reader log
	// (without FILEREAD_Silent: FileManagerGeneric.cpp:858, or the Error at :875)
	const FString Dir = FreshDir(TEXT("transport shrinks"));
	const FString Path = Dir / TEXT("counted.bin");
	TArray<uint8> Big;
	Big.Init(7, 300000);
	if (!TestTrue(TEXT("the upload file is written"), FFileHelper::SaveArrayToFile(Big, *Path)))
	{
		return false;
	}
	TSharedRef<FVaRestTransportProbe, ESPMode::ThreadSafe> Probe = MakeShared<FVaRestTransportProbe, ESPMode::ThreadSafe>();
	VaRest::Http::FCountingUpload Upload(FString(Path), Big.Num(), Probe, nullptr, false);
	uint8 Head[1000] = {};
	Upload.Serialize(Head, 1000);
	TestTrue(TEXT("the first read is the file's"), !Upload.HasReadFailed() && Head[0] == 7 && Head[999] == 7);

	TruncateInPlace(Path);
	VaRest::Http::Testing::FLogCapture Capture;
	Upload.Seek(250000);
	uint8 Tail[1000];
	FMemory::Memset(Tail, 0xAA, sizeof(Tail));
	Upload.Serialize(Tail, 1000);
	TestTrue(TEXT("a file that shrank mid-upload fails the read"), Upload.HasReadFailed() && Upload.IsError());
	TestTrue(TEXT("... zeroes in the engine's buffer"), Tail[0] == 0 && Tail[999] == 0);
	TestEqual(TEXT("... and the reader logs nothing: UploadReadFailed says it once (review M-5)"), Capture.Count(TEXT("LogFileManager")), 0);
	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpBodyFileStreamTest, "VaRest.Http.Transport.BodyFileStreamChanges",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpBodyFileStreamTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::TransportTestPrivate;
	// Task 7 review M-1: a BodyFile goes to the engine as one FVaRestSegmentStream segment of the size measured when its
	// body was built. A change while it uploads -- shrunk under an open reader, or another size at the first read -- fails
	// the read with HasReadFailed (task 7 review M-2), which the completion turns into UploadReadFailed.
	AddExpectedMessagePlain(TEXT("could not be read at byte"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);
	const FString Dir = FreshDir(TEXT("transport body stream"));
	const FString Path = Dir / TEXT("upload.bin");
	TArray<uint8> Big;
	Big.Init(9, 300000);
	if (!TestTrue(TEXT("the upload file is written"), FFileHelper::SaveArrayToFile(Big, *Path)))
	{
		return false;
	}
	auto OneSegment = [Path](int64 BuiltSize)
	{
		TArray<FVaRestBodySegment> Segments;
		FVaRestBodySegment& FileSegment = Segments.AddDefaulted_GetRef();
		FileSegment.FilePath = Path;
		FileSegment.FileSize = BuiltSize;
		return Segments;
	};

	FVaRestSegmentStream Shrinking(OneSegment(Big.Num()));
	uint8 Head[1000] = {};
	Shrinking.Serialize(Head, 1000);
	TestTrue(TEXT("an intact file reads"), !Shrinking.HasReadFailed() && Head[0] == 9);
	TruncateInPlace(Path);
	Shrinking.Seek(250000);
	uint8 Tail[1000];
	FMemory::Memset(Tail, 0xAA, sizeof(Tail));
	Shrinking.Serialize(Tail, 1000);
	TestTrue(TEXT("shrunk under the open reader: HasReadFailed, zeroes"), Shrinking.HasReadFailed() && Tail[0] == 0 && Tail[999] == 0);

	TArray<uint8> Grown;
	Grown.Init(3, 300500);
	FFileHelper::SaveArrayToFile(Grown, *Path);
	FVaRestSegmentStream Replaced(OneSegment(Big.Num()));
	uint8 First[16];
	FMemory::Memset(First, 0xAA, sizeof(First));
	Replaced.Serialize(First, 16);
	TestTrue(TEXT("another size at the first read (grown or replaced): HasReadFailed, zeroes"), Replaced.HasReadFailed() && First[0] == 0);
	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTransportBodyFileTest, "VaRest.Http.Transport.BodyFileUpload",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTransportBodyFileTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::TransportTestPrivate;
	// Task 7 review M-1: a BodyFile upload through the real transport. A PUT to file:// has no response, so even a sent
	// body ends as Failure Other ("completed with reason") -- the target's bytes say what was sent.
	AddExpectedMessagePlain(TEXT("libcurl"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	AddExpectedMessagePlain(TEXT("completed with reason"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	const FString Dir = FreshDir(TEXT("transport body file"));
	const FString Source = Dir / TEXT("upload.bin");
	TArray<uint8> Bytes;
	Bytes.Init('u', 2000);
	if (!TestTrue(TEXT("the upload file is written"), FFileHelper::SaveArrayToFile(Bytes, *Source)))
	{
		return false;
	}
	const FName Missing(TEXT("BodyFileMissing"));
	const FName ReadFailed(TEXT("UploadReadFailed"));

	for (const bool bCounted : { false, true })
	{
		const FString Target = Dir / (bCounted ? TEXT("counted.bin") : TEXT("plain.bin"));
		const FString Path = bCounted ? TEXT("counted (FCountingUpload)") : TEXT("default (one-segment stream)");
		TSharedRef<FDone, ESPMode::ThreadSafe> Sent = MakeShared<FDone, ESPMode::ThreadSafe>();
		if (!TestTrue(FString::Printf(TEXT("%s: an intact file uploads"), *Path), SendAndWait(PutFile(Source, Bytes.Num(), Target, bCounted), Sent)))
		{
			return false;
		}
		TArray<uint8> OnDisk;
		TestTrue(FString::Printf(TEXT("%s: the peer got the whole file"), *Path), FFileHelper::LoadFileToArray(OnDisk, *Target) && OnDisk == Bytes);
		TestTrue(FString::Printf(TEXT("%s: ... and no read failed"), *Path), Sent->Result.FailureDetail != ReadFailed && Sent->Result.FailureDetail != Missing);

		// Missing: fails inside Start, before ProcessRequest -- the stream would have sent zeroes
		const FString Never = Dir / (bCounted ? TEXT("never counted.bin") : TEXT("never plain.bin"));
		TSharedRef<FDone, ESPMode::ThreadSafe> Gone = MakeShared<FDone, ESPMode::ThreadSafe>();
		TestTrue(FString::Printf(TEXT("%s: a missing file fails before connecting (inside Start)"), *Path),
			CompletesInsideStart(PutFile(Dir / TEXT("no such file.bin"), Bytes.Num(), Never, bCounted), Gone));
		TestTrue(FString::Printf(TEXT("%s: ... as BodyFileMissing"), *Path), Gone->Result.Failure == EVaRestTransportFailure::Other && Gone->Result.FailureDetail == Missing);
		TestFalse(FString::Printf(TEXT("%s: ... and the peer saw nothing"), *Path), IFileManager::Get().FileExists(*Never));
	}

	// Another size than the body was built with: Content-Length came from the old file, so it fails before connecting
	const FString Never = Dir / TEXT("never resized.bin");
	TSharedRef<FDone, ESPMode::ThreadSafe> Resized = MakeShared<FDone, ESPMode::ThreadSafe>();
	TestTrue(TEXT("a file whose size changed since its body was built fails before connecting"),
		CompletesInsideStart(PutFile(Source, Bytes.Num() + 1, Never, false), Resized));
	TestTrue(TEXT("... as UploadReadFailed"), Resized->Result.Failure == EVaRestTransportFailure::Other && Resized->Result.FailureDetail == ReadFailed);
	TestFalse(TEXT("... and the peer saw nothing"), IFileManager::Get().FileExists(*Never));

	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTransportTimesTest, "VaRest.Http.Transport.UploadTimesAndBytes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTransportTimesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::TransportTestPrivate;
	// Task 7 review I-1 (IN-3 UploadDoneAt never after StatusAt) and M-4 (BytesUp is what was seen sent)
	AddExpectedMessagePlain(TEXT("libcurl"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	AddExpectedMessagePlain(TEXT("completed with reason"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	const FString Dir = FreshDir(TEXT("transport times"));
	const FString Source = Dir / TEXT("body.bin");
	const FString Part = Dir / TEXT("part.bin");
	TArray<uint8> Body;
	Body.Init('b', 50000);
	TArray<uint8> PartBytes;
	PartBytes.Init('p', 1000);
	if (!TestTrue(TEXT("the files are written"), FFileHelper::SaveArrayToFile(Body, *Source) && FFileHelper::SaveArrayToFile(PartBytes, *Part)))
	{
		return false;
	}
	TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> Clock = MakeShared<FVaRestVirtualClock, ESPMode::ThreadSafe>();
	Clock->Advance(5.0);

	// A traced request without a body: curl's progress fires with the response bytes, after StatusAt
	FVaRestTransportRequest Bodyless = GetFile(Source);
	Bodyless.bReportProgress = true;
	Bodyless.bReportStatusTime = true;
	Bodyless.Clock = Clock;
	const TSharedRef<FVaRestTransportProbe, ESPMode::ThreadSafe> BodylessProbe = Bodyless.Probe;
	TSharedRef<FDone, ESPMode::ThreadSafe> Got = MakeShared<FDone, ESPMode::ThreadSafe>();
	if (!TestTrue(TEXT("a traced download completes"), SendAndWait(MoveTemp(Bodyless), Got)))
	{
		return false;
	}
	TestEqual(TEXT("the progress delegate ran, with every response byte"), BodylessProbe->BytesReceived.load(), static_cast<int64>(Body.Num()));
	TestTrue(TEXT("no body: UploadDoneAt is never stamped, so the Inspector merges Send into Wait (review I-1)"), BodylessProbe->UploadDoneAt.load() < 0.0);
	TestEqual(TEXT("the transport end is stamped on the request's clock"), Got->Result.TransportDoneAt, 5.0);

	{
		// Trace metadata alone, with [HTTP.Curl] bAllowSeekFunction on: not counted (review N-9), progress bound
		FCurlSeekConfigGuard Seek(true);
		FVaRestTransportRequest Uncounted = PutSegments(Part, PartBytes.Num(), Dir / TEXT("put.bin"), false);
		Uncounted.bReportProgress = true;
		Uncounted.bReportStatusTime = true;
		Uncounted.Clock = Clock;
		const TSharedRef<FVaRestTransportProbe, ESPMode::ThreadSafe> UncountedProbe = Uncounted.Probe;
		TSharedRef<FDone, ESPMode::ThreadSafe> Put = MakeShared<FDone, ESPMode::ThreadSafe>();
		if (!TestTrue(TEXT("an uncounted traced upload completes"), SendAndWait(MoveTemp(Uncounted), Put)))
		{
			return false;
		}
		TestEqual(TEXT("curl's last progress call saw the whole body sent"), UncountedProbe->BytesSent.load(), static_cast<int64>(PartBytes.Num() + 5));
		TestTrue(TEXT("... and stamped no UploadDoneAt: curl's progress cannot see the upload end (review I-1, N-9)"), UncountedProbe->UploadDoneAt.load() < 0.0);
		TestEqual(TEXT("BytesUp is what progress saw sent (review M-4)"), Put->Result.BytesUp, static_cast<int64>(PartBytes.Num() + 5));

		// The peer refuses before reading a byte: nothing was sent, whatever the body's size
		FVaRestTransportRequest Refused = PutSegments(Part, PartBytes.Num(), Dir / TEXT("no such dir") / TEXT("put.bin"), false);
		Refused.bReportProgress = true;
		Refused.bReportStatusTime = true;
		Refused.Clock = Clock;
		const TSharedRef<FVaRestTransportProbe, ESPMode::ThreadSafe> RefusedProbe = Refused.Probe;
		TSharedRef<FDone, ESPMode::ThreadSafe> Nothing = MakeShared<FDone, ESPMode::ThreadSafe>();
		if (!TestTrue(TEXT("an upload the peer refuses completes"), SendAndWait(MoveTemp(Refused), Nothing)))
		{
			return false;
		}
		TestEqual(TEXT("nothing was read from the body"), RefusedProbe->BytesSent.load(), static_cast<int64>(0));
		TestEqual(TEXT("BytesUp is 0, not the body's size (review M-4)"), Nothing->Result.BytesUp, static_cast<int64>(0));
	}

	// The positive control: a counted upload is stamped, by FCountingUpload, at the last byte read
	FVaRestTransportRequest Counted = PutSegments(Part, PartBytes.Num(), Dir / TEXT("counted.bin"), true);
	Counted.bReportStatusTime = true;
	Counted.Clock = Clock;
	const TSharedRef<FVaRestTransportProbe, ESPMode::ThreadSafe> CountedProbe = Counted.Probe;
	TSharedRef<FDone, ESPMode::ThreadSafe> Stamped = MakeShared<FDone, ESPMode::ThreadSafe>();
	if (!TestTrue(TEXT("a counted traced upload completes"), SendAndWait(MoveTemp(Counted), Stamped)))
	{
		return false;
	}
	TestEqual(TEXT("a counted upload is stamped when its last byte is read"), CountedProbe->UploadDoneAt.load(), 5.0);

	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	return true;
}

#endif // PLATFORM_LINUX

#endif
