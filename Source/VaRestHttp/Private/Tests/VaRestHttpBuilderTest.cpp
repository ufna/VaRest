// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Tasks/Task.h"
#include "UObject/Package.h"
#include "VaRestClient.h"
#include "VaRestHttpSettings.h"
#include "VaRestHttpTesting.h"

namespace VaRest::Http::BuilderTestPrivate
{
	FString Text(const TArray<uint8>& Bytes)
	{
		return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
	}

	/** Sends Request through Rig and waits; true when it completed */
	template <typename T>
	bool Run(VaRest::Http::Testing::FTestRig& Rig, TVaRestRequest<T>&& Request)
	{
		auto Outcome = VaRest::Http::Testing::MakeOutcome<T>();
		VaRest::Http::Testing::Observe(MoveTemp(Request), Outcome).Send();
		return Rig.Wait(Outcome);
	}

	/** A PerAttempt stage that signs with a value holding CRLF (review I3) */
	class FBadSigner final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("BadSigner"); }
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			Exchange->GetAttemptHeaders().Set(TEXT("X-Signed"), TEXT("a\r\nX-Evil: 1"));
			Next.Proceed(Exchange, MoveTemp(OnOutcome));
		}
	};

	/** A PerAttempt stage that removes the Content-Type of a request with a body (review N-5) */
	class FTypeRemover final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("TypeRemover"); }
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			Exchange->GetAttemptHeaders().Remove(TEXT("Content-Type"));
			Next.Proceed(Exchange, MoveTemp(OnOutcome));
		}
	};

	/** What M4's retry does: an Outer stage that sends the request a second time once the first outcome is back */
	class FReissuer final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("Reissuer"); }
		virtual bool MayReissue() const override { return true; }
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			Next.Proceed(Exchange, [Exchange, Next, Done = MoveTemp(OnOutcome)](FVaRestAttemptOutcome&&) mutable
			{
				Next.Proceed(Exchange, MoveTemp(Done));
			});
		}
	};

	/** The Content-Type values a call sent, in order, joined by '|': one value means one header */
	FString ContentTypesOf(const FVaRestTransportRequest& Request)
	{
		TArray<FString> Values;
		for (const FVaRestHeaders::FPair& Pair : Request.Headers)
		{
			if (Pair.Key.Equals(TEXT("Content-Type"), ESearchCase::IgnoreCase))
			{
				Values.Add(Pair.Value);
			}
		}
		return FString::Join(Values, TEXT("|"));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpBuilderBodiesTest, "VaRest.Http.Builder.Bodies",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpBuilderBodiesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::BuilderTestPrivate;
	VaRest::Http::Testing::FTestRig Rig;
	const FIntPoint Point(3, 4);

	TestTrue(TEXT("json"), Run(Rig, Rig.Client.Post(TEXT("/j")).Body(FVaRestJson::Object().Set(TEXT("a"), 1))));
	TestTrue(TEXT("struct, the client's naming (camelCase by default)"), Run(Rig, Rig.Client.Post(TEXT("/s")).Body(Point)));
	TestTrue(TEXT("struct, the request's naming, set first"), Run(Rig, Rig.Client.Post(TEXT("/n")).Naming(EVaRestJsonNaming::PascalCase).Body(Point)));
	TestTrue(TEXT("text"), Run(Rig, Rig.Client.Put(TEXT("/t")).Body(TEXT("café"))));
	TestTrue(TEXT("bytes"), Run(Rig, Rig.Client.Put(TEXT("/b")).Body(TArray<uint8>{ 1, 2, 3 })));
	TestTrue(TEXT("form"), Run(Rig, Rig.Client.Post(TEXT("/f")).Body(FVaRestForm().Add(TEXT("a b"), TEXT("c&d")))));
	TestTrue(TEXT("multipart"), Run(Rig, Rig.Client.Post(TEXT("/m")).Body(FVaRestMultipart(TEXT("BB")).AddField(TEXT("k"), TEXT("v")))));
	TestTrue(TEXT("a Content-Type header wins over the body's, set before or after"),
		Run(Rig, Rig.Client.Post(TEXT("/o")).Header(TEXT("content-type"), TEXT("application/vnd.game+json")).Body(FVaRestJson::Object())));

	auto Call = [&Rig](int32 Index) -> const FVaRestTransportRequest& { return Rig.Fake->GetCall(Index).Request; };
	TestEqualSensitive(TEXT("json body"), Text(Call(0).Body), FString(TEXT("{\"a\":1}")));
	TestEqualSensitive(TEXT("json type"), Call(0).Headers.Get(TEXT("Content-Type")), FString(TEXT("application/json")));
	TestEqualSensitive(TEXT("struct body, camelCase"), Text(Call(1).Body), FString(TEXT("{\"x\":3,\"y\":4}")));
	TestEqualSensitive(TEXT("struct body, PascalCase"), Text(Call(2).Body), FString(TEXT("{\"X\":3,\"Y\":4}")));
	TestEqualSensitive(TEXT("text type"), Call(3).Headers.Get(TEXT("Content-Type")), FString(TEXT("text/plain; charset=utf-8")));
	TestTrue(TEXT("text is UTF-8"), Call(3).Body == TArray<uint8>({ 'c', 'a', 'f', 0xC3, 0xA9 }));
	TestEqualSensitive(TEXT("bytes type"), Call(4).Headers.Get(TEXT("Content-Type")), FString(TEXT("application/octet-stream")));
	TestEqualSensitive(TEXT("form body"), Text(Call(5).Body), FString(TEXT("a+b=c%26d")));
	TestEqualSensitive(TEXT("form type"), Call(5).Headers.Get(TEXT("Content-Type")), FString(TEXT("application/x-www-form-urlencoded")));
	TestEqualSensitive(TEXT("multipart type names the boundary"), Call(6).Headers.Get(TEXT("Content-Type")), FString(TEXT("multipart/form-data; boundary=BB")));
	TestTrue(TEXT("multipart body"), Text(Call(6).Body).StartsWith(TEXT("--BB\r\nContent-Disposition: form-data; name=\"k\"")));
	TestEqualSensitive(TEXT("the request's Content-Type wins"), Call(7).Headers.Get(TEXT("Content-Type")), FString(TEXT("application/vnd.game+json")));
	TestEqual(TEXT("... once"), Call(7).Headers.Num(), 1);

	// Streamed uploads: a file, and multipart with a file part (spec 3.2: "file parts streamed with SetContentFromStream")
	const FString File = FPaths::ProjectSavedDir() / TEXT("VaRestTests") / TEXT("builder-upload.bin");
	TArray<uint8> FileBytes;
	FileBytes.Init(7, 5000);
	FFileHelper::SaveArrayToFile(FileBytes, *File);
	TestTrue(TEXT("file"), Run(Rig, Rig.Client.Put(TEXT("/up")).BodyFile(File, TEXT("image/png"))));
	TestTrue(TEXT("multipart with a file"), Run(Rig, Rig.Client.Post(TEXT("/mp")).Body(FVaRestMultipart(TEXT("CC")).AddFile(TEXT("f"), File))));
	TestEqualSensitive(TEXT("a file body streams from the file"), Call(8).BodyFile, File);
	TestTrue(TEXT("... with its bytes"), Rig.Fake->ReadBody(8) == FileBytes);
	TestEqualSensitive(TEXT("... and its type"), Call(8).Headers.Get(TEXT("Content-Type")), FString(TEXT("image/png")));
	TestTrue(TEXT("multipart with a file streams segments"), Call(9).BodySegments.Num() == 3 && Call(9).Body.Num() == 0);
	TestEqual(TEXT("upload sizes reach the probe"), Call(8).Probe->UploadTotal.load(), static_cast<int64>(5000));
	IFileManager::Get().Delete(*File);

	// OpenAPI N9 array styles through the builder (review I7)
	const TArray<FString> Tags = { TEXT("a"), TEXT("b") };
	TestTrue(TEXT("array query"), Run(Rig, Rig.Client.Get(TEXT("/q")).Query(TEXT("tag"), Tags).Query(TEXT("csv"), Tags, EVaRestQueryStyle::Csv)));
	TestEqualSensitive(TEXT("Repeat by default, then Csv"), Call(10).Url, FString(TEXT("https://api.test/q?tag=a&tag=b&csv=a,b")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpBuilderMisuseTest, "VaRest.Http.Builder.HeaderInjection",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpBuilderMisuseTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	auto Injected = MakeOutcome<FVaRestJson>();
	auto BadName = MakeOutcome<FVaRestJson>();
	auto MissingFile = MakeOutcome<FVaRestJson>();
	auto LateNaming = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/h")).Header(TEXT("X-Name"), TEXT("neo\r\nX-Admin: 1")), Injected).Send();
	Observe(Rig.Client.Get(TEXT("/h")).Header(TEXT("Bad Name"), TEXT("v")), BadName).Send();
	Observe(Rig.Client.Put(TEXT("/h")).BodyFile(FPaths::ProjectSavedDir() / TEXT("VaRestTests") / TEXT("missing.bin")), MissingFile).Send();
	Observe(Rig.Client.Post(TEXT("/h")).Body(FIntPoint(1, 2)).Naming(EVaRestJsonNaming::SnakeCase), LateNaming).Send();
	TestTrue(TEXT("all complete"), PumpUntil([&] { return Injected->IsDone() && BadName->IsDone() && MissingFile->IsDone() && LateNaming->IsDone(); }));
	TestTrue(TEXT("a header value with CRLF is Kind Config (header injection)"), Injected->Error.Kind == EVaRestErrorKind::Config);
	TestTrue(TEXT("an invalid header name is Kind Config"), BadName->Error.Kind == EVaRestErrorKind::Config);
	TestTrue(TEXT("a missing upload file is Kind Config"), MissingFile->Error.Kind == EVaRestErrorKind::Config && MissingFile->Error.Message.Contains(TEXT("missing.bin")));
	TestTrue(TEXT("Naming() after a struct body is Kind Config"), LateNaming->Error.Kind == EVaRestErrorKind::Config && LateNaming->Error.Message.Contains(TEXT("Naming")));
	TestEqual(TEXT("none reached the transport"), Rig.Fake->NumCalls(), 0);

	// Review I3: the multipart boundary goes into Content-Type
	auto BadBoundary = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Post(TEXT("/h")).Body(FVaRestMultipart(TEXT("b\r\nX-Evil: 1")).AddField(TEXT("a"), TEXT("1"))), BadBoundary).Send();
	TestTrue(TEXT("completes"), Rig.Wait(BadBoundary));
	TestTrue(TEXT("a boundary with CRLF is Kind Config"), BadBoundary->Error.Kind == EVaRestErrorKind::Config && BadBoundary->Error.Message.Contains(TEXT("boundary")));
	TestEqual(TEXT("... and never reached the transport"), Rig.Fake->NumCalls(), 0);

	// Review I3: a PerAttempt stage edits the headers after every builder check; the transport stage checks them again.
	// Review N-I1: a pattern only the transport stage's line holds -- the WithHeader line below says "holds CR, LF or NUL"
	// too, and the engine counts a message against the first pattern it matches (AutomationTest.cpp:2692-2714)
	AddExpectedErrorPlain(TEXT("missing or empty Content-Type with a body"), EAutomationExpectedErrorFlags::Contains, 2);
	FTestRig Signed;
	Signed.Client.Use(MakeShared<VaRest::Http::BuilderTestPrivate::FBadSigner, ESPMode::ThreadSafe>(), EVaRestStageSlot::PerAttempt);
	auto Stage = MakeOutcome<FVaRestJson>();
	Observe(Signed.Client.Get(TEXT("/signed")), Stage).Send();
	TestTrue(TEXT("completes"), Signed.Wait(Stage));
	TestTrue(TEXT("a stage's CRLF header value is Kind Config, not retryable"), Stage->Error.Kind == EVaRestErrorKind::Config && !Stage->Error.bRetryable);
	TestEqual(TEXT("... and the transport never saw it"), Signed.Fake->NumCalls(), 0);

	// Review N-5: without a Content-Type, curl's check on a body that is not URL-encoded fails (CurlHttp.cpp:945, :983)
	FTestRig Untyped;
	Untyped.Client.Use(MakeShared<VaRest::Http::BuilderTestPrivate::FTypeRemover, ESPMode::ThreadSafe>(), EVaRestStageSlot::PerAttempt);
	auto NoType = MakeOutcome<FVaRestJson>();
	Observe(Untyped.Client.Post(TEXT("/untyped")).Body(FVaRestJson::Object().Set(TEXT("a"), 1)), NoType).Send();
	TestTrue(TEXT("completes"), Untyped.Wait(NoType));
	TestTrue(TEXT("a body whose Content-Type a stage removed is Kind Config, not retryable"), NoType->Error.Kind == EVaRestErrorKind::Config && !NoType->Error.bRetryable);
	TestEqual(TEXT("... and the transport never saw it"), Untyped.Fake->NumCalls(), 0);

	AddExpectedErrorPlain(TEXT("invalid default header"), EAutomationExpectedErrorFlags::Contains, 1);
	Rig.Client.WithHeader(TEXT("X-Bad"), TEXT("a\nb"));
	auto Plain = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/ok")), Plain).Send();
	TestTrue(TEXT("completes"), Rig.Wait(Plain));
	TestFalse(TEXT("an invalid client header is refused, not sent"), Rig.Fake->GetCall(0).Request.Headers.Contains(TEXT("X-Bad")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpBuilderClientTypeTest, "VaRest.Http.Builder.ClientContentTypeVsBody",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpBuilderClientTypeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::BuilderTestPrivate;
	// Final review I2: Make(Url).WithHeader("Content-Type", "application/json") is a common setup (v1 habits, vendor +json
	// types). Three levels: the request's Header() over the body's own type over the client default.
	VaRest::Http::Testing::FTestRig Rig;
	Rig.Client.WithHeader(TEXT("Content-Type"), TEXT("application/json"));
	TestTrue(TEXT("multipart"), Run(Rig, Rig.Client.Post(TEXT("/m")).Body(FVaRestMultipart(TEXT("BB")).AddField(TEXT("k"), TEXT("v")))));
	TestTrue(TEXT("form"), Run(Rig, Rig.Client.Post(TEXT("/f")).Body(FVaRestForm().Add(TEXT("a"), TEXT("b")))));
	TestTrue(TEXT("text"), Run(Rig, Rig.Client.Post(TEXT("/t")).Body(TEXT("hi"))));
	TestTrue(TEXT("the request's own Content-Type"), Run(Rig, Rig.Client.Post(TEXT("/o")).Header(TEXT("content-type"), TEXT("application/vnd.game+json"))
		.Body(FVaRestMultipart(TEXT("CC")).AddField(TEXT("k"), TEXT("v")))));
	TestTrue(TEXT("no body"), Run(Rig, Rig.Client.Get(TEXT("/g"))));
	if (!TestEqual(TEXT("all five reached the transport"), Rig.Fake->NumCalls(), 5))
	{
		return false;
	}
	auto Call = [&Rig](int32 Index) -> const FVaRestTransportRequest& { return Rig.Fake->GetCall(Index).Request; };
	TestEqualSensitive(TEXT("a multipart body beats the client default: its own type, naming the boundary, once"), ContentTypesOf(Call(0)),
		FString(TEXT("multipart/form-data; boundary=BB")));
	TestEqualSensitive(TEXT("a form body beats the client default"), ContentTypesOf(Call(1)), FString(TEXT("application/x-www-form-urlencoded")));
	TestEqualSensitive(TEXT("a text body beats the client default"), ContentTypesOf(Call(2)), FString(TEXT("text/plain; charset=utf-8")));
	TestEqualSensitive(TEXT("the request's Header() beats the body and the client"), ContentTypesOf(Call(3)), FString(TEXT("application/vnd.game+json")));
	TestEqualSensitive(TEXT("without a body the client default stays (nothing to relabel)"), ContentTypesOf(Call(4)), FString(TEXT("application/json")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpBuilderVerbTest, "VaRest.Http.Builder.InvalidVerb",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpBuilderVerbTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	// Final review I3: curl sends an uncommon verb verbatim as CURLOPT_CUSTOMREQUEST (CurlHttp.cpp:990-993), so CR LF or a
	// space would rewrite the request line; the engine refuses only an empty one, as a retryable-looking Connection error
	FTestRig Rig;
	auto Injected = MakeOutcome<FVaRestJson>();
	auto Spaced = MakeOutcome<FVaRestJson>();
	auto Empty = MakeOutcome<FVaRestJson>();
	auto Custom = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Request(TEXT("GET /admin HTTP/1.1\r\nX-Evil: 1\r\nX"), TEXT("/v")), Injected).Send();
	Observe(Rig.Client.Request(TEXT(" GET"), TEXT("/v")), Spaced).Send();
	Observe(Rig.Client.Request(TEXT(""), TEXT("/v")), Empty).Send();
	Observe(Rig.Client.Request(TEXT("PROPFIND"), TEXT("/v")), Custom).Send();
	TestTrue(TEXT("all complete"), PumpUntil([&] { return Injected->IsDone() && Spaced->IsDone() && Empty->IsDone() && Custom->IsDone(); }));
	TestTrue(TEXT("a verb with CR LF is Kind Config, not retryable"), Injected->Error.Kind == EVaRestErrorKind::Config && !Injected->Error.bRetryable);
	TestTrue(TEXT("... the message names the problem"), Injected->Error.Message.Contains(TEXT("is not an RFC 9110 token")));
	TestTrue(TEXT("... with the verb escaped, so the message is one line"), Injected->Error.Message.Contains(TEXT("HTTP/1.1\\r\\nX-Evil: 1"))
		&& !Injected->Error.Message.Contains(TEXT("\r")) && !Injected->Error.Message.Contains(TEXT("\n")));
	TestTrue(TEXT("a verb with a space is Kind Config, not retryable: never trimmed into GET"), Spaced->Error.Kind == EVaRestErrorKind::Config
		&& !Spaced->Error.bRetryable && Spaced->Error.Message.Contains(TEXT("\" GET\"")));
	TestTrue(TEXT("an empty verb is Kind Config, not retryable, named (not the engine's retryable Connection)"), Empty->Error.Kind == EVaRestErrorKind::Config
		&& !Empty->Error.bRetryable && Empty->Error.Message.Contains(TEXT("verb is empty")));
	TestTrue(TEXT("a token verb is any verb: sent"), Custom->SuccessCalls.load() == 1);
	if (!TestEqual(TEXT("only the token verb reached the transport"), Rig.Fake->NumCalls(), 1))
	{
		return false;
	}
	TestEqualSensitive(TEXT("... as it was given"), Rig.Fake->GetCall(0).Request.Verb, FString(TEXT("PROPFIND")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpBuilderEmptyClientTest, "VaRest.Http.Builder.EmptyClient",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpBuilderEmptyClientTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	// Final review M7: a client member that never got Make() must not leave a UI waiting on OnComplete forever
	AddExpectedErrorPlain(TEXT("Send() on an empty FVaRestClient"), EAutomationExpectedErrorFlags::Contains, 4);
	const FVaRestClient Empty;
	auto OnGame = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Empty.Get(TEXT("/users?token=canary-empty")).Tag(TEXT("profile")), OnGame).Send();
	// Nothing ran on the game thread between Send() and here, so a callback seen now ran inside Send()
	TestEqual(TEXT("decision 3: nothing is delivered inside Send()"), OnGame->CompleteCalls.load() + OnGame->ErrorCalls.load(), 0);
	TestTrue(TEXT("the handle is empty"), !Handle.IsActive() && Handle.GetId() == 0);
	if (!TestTrue(TEXT("OnComplete runs"), PumpUntil([OnGame] { return OnGame->IsDone(); })))
	{
		return false;
	}
	TestTrue(TEXT("Kind Config through OnError, once, then OnComplete once"), OnGame->Error.Kind == EVaRestErrorKind::Config && OnGame->ErrorCalls.load() == 1
		&& OnGame->CompleteCalls.load() == 1 && OnGame->SuccessCalls.load() == 0);
	TestTrue(TEXT("... saying what to do"), OnGame->Error.Message.Contains(TEXT("FVaRestClient::Make")) && !OnGame->Error.bRetryable);
	TestTrue(TEXT("... on the game thread"), OnGame->bAllOnGameThread.load());
	TestTrue(TEXT("... with the tag"), OnGame->Error.Tag == FName(TEXT("profile")));
	TestEqualSensitive(TEXT("... and the URL, redacted (spec 3.3)"), OnGame->Error.Url, FString(TEXT("/users?token=***")));

	// From a worker, as other requests may be sent (spec 1): delivered on the game thread too
	auto FromWorker = MakeOutcome<FVaRestJson>();
	TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> bSent = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	UE::Tasks::Launch(TEXT("VaRest.Test.EmptyClientWorker"), [Empty, FromWorker, bSent]
	{
		Observe(Empty.Post(TEXT("/w")).Body(TEXT("x")), FromWorker).Send();
		*bSent = true;
	});
	TestTrue(TEXT("from a worker: completes"), PumpUntil([FromWorker, bSent] { return bSent->load() && FromWorker->IsDone(); }));
	TestTrue(TEXT("... Kind Config, on the game thread"), FromWorker->Error.Kind == EVaRestErrorKind::Config && FromWorker->bAllOnGameThread.load());

	// Spec 3.2: an owner that is gone gets nothing, here too. The delivery task was queued inside Send() on this thread,
	// so one pass over the game-thread queue runs it.
	UObject* Gone = NewObject<UVaRestHttpSettings>(GetTransientPackage(), NAME_None, RF_Transient);
	Gone->MarkAsGarbage();
	auto Silent = MakeOutcome<FVaRestJson>();
	Observe(Empty.Get(TEXT("/o")).Owner(Gone), Silent).Send();
	PumpUntil([] { return true; });
	TestEqual(TEXT("owner gone: no callbacks"), Silent->CompleteCalls.load() + Silent->ErrorCalls.load(), 0);

	// Decision 4: OnHttpThread() callbacks run on a worker
	auto OffThread = MakeOutcome<FVaRestJson>();
	Observe(Empty.Get(TEXT("/h")).OnHttpThread(), OffThread).Send();
	TestTrue(TEXT("OnHttpThread: completes"), PumpUntil([OffThread] { return OffThread->IsDone(); }));
	TestTrue(TEXT("... Kind Config, off the game thread"), OffThread->Error.Kind == EVaRestErrorKind::Config && !OffThread->bAllOnGameThread.load());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpBuilderNamingAfterReplacedTest, "VaRest.Http.Builder.NamingAfterBodyReplaced",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpBuilderNamingAfterReplacedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::BuilderTestPrivate;
	// Task 9 review minor 1 (P9.1): Naming() fails only while a struct body is the current one
	FTestRig Rig;
	auto ByJson = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Post(TEXT("/j")).Body(FIntPoint(1, 2)).Body(FVaRestJson::Object().Set(TEXT("a"), 1)).Naming(EVaRestJsonNaming::SnakeCase), ByJson).Send();
	auto Restructured = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Post(TEXT("/s")).Body(FIntPoint(1, 2)).Body(TEXT("text")).Naming(EVaRestJsonNaming::PascalCase).Body(FIntPoint(3, 4)), Restructured).Send();
	TestTrue(TEXT("both complete"), PumpUntil([ByJson, Restructured] { return ByJson->IsDone() && Restructured->IsDone(); }));
	TestTrue(TEXT("a JSON body after a struct body: Naming() is no misuse"), ByJson->SuccessCalls.load() == 1 && !ByJson->Error.IsError());
	TestTrue(TEXT("a struct body after a replaced one takes the new naming"), Restructured->SuccessCalls.load() == 1);
	if (!TestEqual(TEXT("both reached the transport"), Rig.Fake->NumCalls(), 2))
	{
		return false;
	}
	TestEqualSensitive(TEXT("... the JSON body sent"), Text(Rig.Fake->GetCall(0).Request.Body), FString(TEXT("{\"a\":1}")));
	TestEqualSensitive(TEXT("... the second struct in PascalCase"), Text(Rig.Fake->GetCall(1).Request.Body), FString(TEXT("{\"X\":3,\"Y\":4}")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpBuilderGetBodyTest, "VaRest.Http.Builder.GetBodyDropped",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpBuilderGetBodyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::BuilderTestPrivate;
	VaRest::Http::Testing::FTestRig Rig;
	AddExpectedMessagePlain(TEXT("has a body; it is dropped"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	TestTrue(TEXT("completes"), Run(Rig, Rig.Client.Get(TEXT("/g?token=abc")).Body(FVaRestJson::Object().Set(TEXT("a"), 1))));
	TestEqual(TEXT("spec 3.2: a GET body is dropped (the engine refuses one, HttpRequestCommon.cpp:764-768)"), Rig.Fake->GetCall(0).Request.Body.Num(), 0);
	TestFalse(TEXT("... with its Content-Type"), Rig.Fake->GetCall(0).Request.Headers.Contains(TEXT("Content-Type")));
	TestTrue(TEXT("other verbs keep theirs"), Run(Rig, Rig.Client.Delete(TEXT("/d")).Body(FVaRestJson::Object().Set(TEXT("a"), 1))) && Rig.Fake->GetCall(1).Request.Body.Num() > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpBuilderIdempotentTest, "VaRest.Http.Builder.Idempotent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpBuilderIdempotentTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::BuilderTestPrivate;
	VaRest::Http::Testing::FTestRig Rig;
	TestTrue(TEXT("idempotent"), Run(Rig, Rig.Client.Post(TEXT("/orders")).Idempotent().Body(FVaRestJson::Object())));
	TestTrue(TEXT("idempotent again"), Run(Rig, Rig.Client.Post(TEXT("/orders")).Idempotent().Body(FVaRestJson::Object())));
	TestTrue(TEXT("plain"), Run(Rig, Rig.Client.Post(TEXT("/orders")).Body(FVaRestJson::Object())));
	TestTrue(TEXT("caller's own key"), Run(Rig, Rig.Client.Post(TEXT("/orders")).Header(TEXT("Idempotency-Key"), TEXT("mine")).Idempotent()));
	const FString First = Rig.Fake->GetCall(0).Request.Headers.Get(TEXT("Idempotency-Key"));
	FGuid Parsed;
	TestTrue(TEXT("Idempotent() adds a UUID key (spec 3.2)"), FGuid::Parse(First, Parsed) && First.Len() == 36);
	TestNotEqual(TEXT("one key per logical request"), Rig.Fake->GetCall(1).Request.Headers.Get(TEXT("Idempotency-Key")), First);
	TestFalse(TEXT("no key without Idempotent()"), Rig.Fake->GetCall(2).Request.Headers.Contains(TEXT("Idempotency-Key")));
	TestEqualSensitive(TEXT("a key the caller set is kept"), Rig.Fake->GetCall(3).Request.Headers.Get(TEXT("Idempotency-Key")), FString(TEXT("mine")));

	// Task 9 review minor 4 (P9.4): the key is made once per logical request (Resolve, on entry) and each attempt copies the
	// request's headers at the attempt boundary. The seam M3 has for a second attempt is an Outer stage that proceeds again,
	// which is what M4's retry is; Resolve itself runs once per request, so "resolving twice" is not a path the core takes.
	VaRest::Http::Testing::FTestRig Retried;
	Retried.Client.Use(MakeShared<FReissuer, ESPMode::ThreadSafe>(), EVaRestStageSlot::Outer);
	TestTrue(TEXT("re-issued"), Run(Retried, Retried.Client.Post(TEXT("/orders")).Idempotent().Body(FVaRestJson::Object())));
	if (!TestEqual(TEXT("two attempts reached the transport"), Retried.Fake->NumCalls(), 2))
	{
		return false;
	}
	const FVaRestTransportRequest& FirstAttempt = Retried.Fake->GetCall(0).Request;
	const FVaRestTransportRequest& SecondAttempt = Retried.Fake->GetCall(1).Request;
	TestTrue(TEXT("... as attempts 1 and 2"), FirstAttempt.Attempt == 1 && SecondAttempt.Attempt == 2);
	TestFalse(TEXT("... the first with a key"), FirstAttempt.Headers.Get(TEXT("Idempotency-Key")).IsEmpty());
	TestEqualSensitive(TEXT("spec 3.2: the key is stable across attempts"), SecondAttempt.Headers.Get(TEXT("Idempotency-Key")),
		FirstAttempt.Headers.Get(TEXT("Idempotency-Key")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpBuilderUnsentTest, "VaRest.Http.Builder.UnsentLogsAnError",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpBuilderUnsentTest::RunTest(const FString& Parameters)
{
	VaRest::Http::Testing::FTestRig Rig;
	VaRest::Http::Testing::FLogCapture Capture;
	AddExpectedErrorPlain(TEXT("was built but never sent"), EAutomationExpectedErrorFlags::Contains, 1);
	{
		// [[nodiscard]] already warns at compile time for a temporary (spec 3.2); a named builder dropped unsent logs at run time
		TVaRestRequest<FVaRestJson> Forgotten = Rig.Client.Get(TEXT("/never?token=canary-unsent")).Query(TEXT("api_key"), TEXT("canary-query")).Query(TEXT("page"), 2);
	}
	{
		TVaRestRequest<FVaRestJson> Sent = Rig.Client.Get(TEXT("/sent"));
		MoveTemp(Sent).Send(); // the moved-from builder logs nothing
	}
	VaRest::Http::Testing::PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0 && Rig.Fake->NumCalls() == 1; });
	TestTrue(TEXT("the error names the URL, its Query() pairs included (task 9 review minor 2)"), Capture.Contains(TEXT("https://api.test/never?token=***&api_key=***&page=2 ")));
	TestFalse(TEXT("... redacted"), Capture.Contains(TEXT("canary-unsent")) || Capture.Contains(TEXT("canary-query")));
	return true;
}

#endif
