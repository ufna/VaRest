// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "VaRestClient.h"
#include "VaRestHttpSettings.h"
#include "VaRestHttpTesting.h"

#include <atomic>

namespace VaRest::Http::DecodeTestPrivate
{
	FVaRestFakeResponse Json(int32 Status, const char* Text)
	{
		return FVaRestFakeResponse::Json(Status, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text)));
	}

	FString DownloadDir()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("VaRestTests") / TEXT("downloads"));
	}

	int32 CountPartFiles()
	{
		TArray<FString> Parts;
		IFileManager::Get().FindFiles(Parts, *(DownloadDir() / TEXT("*.part")), true, false);
		return Parts.Num();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpZeroCopyTest, "VaRest.Http.Decode.ZeroCopy",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpZeroCopyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::DecodeTestPrivate;
	FTestRig Rig;
	Rig.Fake->Enqueue(Json(200, R"({"s":"hello, in situ"})"));
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/z")), Outcome).Send();
	TestTrue(TEXT("completes"), Rig.Wait(Outcome));
	const FUtf8StringView Parsed = Outcome->Value.IsSet() ? (*Outcome->Value)[TEXT("s")].AsUtf8View() : FUtf8StringView();
	const uint8* Begin = Rig.Fake->GetCall(0).BodyData;
	const uint8* End = Begin + Rig.Fake->GetCall(0).BodyNum;
	const uint8* At = reinterpret_cast<const uint8*>(Parsed.GetData());
	TestEqualSensitive(TEXT("the string"), FString::ConstructFromPtrSize(Parsed.GetData(), Parsed.Len()), FString(TEXT("hello, in situ")));
	// Spec 2.1: ParseOwned steals the body; with its byte of slack the parse is in situ, so the string lies inside the
	// very buffer the transport filled -- no copy anywhere between the HTTP thread and the document
	TestTrue(TEXT("zero-copy: the parsed string points into the transport's buffer"), At >= Begin && At < End);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpContentTypesTest, "VaRest.Http.Decode.ContentTypes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpContentTypesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::DecodeTestPrivate;
	FTestRig Rig;
	FVaRestFakeResponse Problem = Json(200, R"({"a":1})");
	Problem.Headers.Set(TEXT("Content-Type"), TEXT("application/problem+json; charset=utf-8"));
	FVaRestFakeResponse Untyped = Json(200, R"({"a":2})");
	Untyped.Headers.Remove(TEXT("Content-Type"));
	Rig.Fake->Enqueue(Problem);
	Rig.Fake->Enqueue(Untyped);
	Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200, TEXT("<html>login</html>"), TEXT("text/html")));
	Rig.Fake->Enqueue(Json(200, R"({"a":)"));
	TArray<TSharedRef<TTestOutcome<FVaRestJson>, ESPMode::ThreadSafe>> O;
	for (int32 i = 0; i < 4; ++i)
	{
		O.Add(MakeOutcome<FVaRestJson>());
		Observe(Rig.Client.Get(FString::Printf(TEXT("/ct/%d"), i)), O.Last()).Send();
	}
	TestTrue(TEXT("complete"), PumpUntil([&O] { for (const auto& X : O) { if (!X->IsDone()) { return false; } } return true; }));
	TestTrue(TEXT("+json is JSON"), O[0]->Value.IsSet() && (*O[0]->Value)[TEXT("a")].AsInt() == 1);
	TestTrue(TEXT("no Content-Type is parsed (decision 6)"), O[1]->Value.IsSet() && (*O[1]->Value)[TEXT("a")].AsInt() == 2);
	TestTrue(TEXT("text/html for a JSON result is a Decode error (spec 3.2)"), O[2]->Error.Kind == EVaRestErrorKind::Decode && O[2]->Error.Message.Contains(TEXT("text/html")));
	TestTrue(TEXT("... the raw body kept and quoted"), O[2]->Response.Body.Num() > 0 && O[2]->Error.RawBody.Contains(TEXT("login")));
	TestTrue(TEXT("invalid JSON is a Decode error with the parser's message"), O[3]->Error.Kind == EVaRestErrorKind::Decode && O[3]->Error.Message.Contains(TEXT("not valid JSON")));
	TestTrue(TEXT("... and the first bytes as RawBody, from before the in-situ parse"), O[3]->Error.RawBody.Contains(TEXT("{\"a\":")));
	TestEqual(TEXT("decision 26: an in-situ parse error keeps no raw body, only the 4 KiB prefix"), O[3]->Response.Body.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpKeepBodyTest, "VaRest.Http.Decode.KeepBodyAndSizeCap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpKeepBodyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::DecodeTestPrivate;
	FTestRig Rig;
	const char* Body = R"({"padding":"0123456789012345678901234567890123456789"})";
	Rig.Fake->SetResponder([Body](const FVaRestTransportRequest& Request)
	{
		return Request.Url.Contains(TEXT("/error")) ? Json(500, Body) : Json(200, Body);
	});
	auto Kept = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/keep")).KeepBody(), Kept).Send();
	TestTrue(TEXT("KeepBody"), Rig.Wait(Kept));
	TestTrue(TEXT("KeepBody keeps the original bytes next to the decoded value (spec 3.2)"),
		Kept->Value.IsSet() && Kept->Response.Body.Num() == FCStringAnsi::Strlen(Body) && FMemory::Memcmp(Kept->Response.Body.GetData(), Body, Kept->Response.Body.Num()) == 0);

	// Decision 20: the arena DOM is about 4.2x its input, so JSON bodies have a size cap
	Rig.Client.WithMaxJsonBytes(16);
	auto Capped = MakeOutcome<FVaRestJson>();
	auto Uncapped = MakeOutcome<FVaRestJson>();
	auto CappedError = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/big")), Capped).Send();
	Observe(Rig.Client.Get(TEXT("/big")).MaxJsonBytes(0), Uncapped).Send();
	Observe(Rig.Client.Get(TEXT("/error")), CappedError).Send();
	TestTrue(TEXT("complete"), PumpUntil([&] { return Capped->IsDone() && Uncapped->IsDone() && CappedError->IsDone(); }));
	TestTrue(TEXT("over the client's cap: a Decode error naming the limit"), Capped->Error.Kind == EVaRestErrorKind::Decode && Capped->Error.Message.Contains(TEXT("MaxJsonBytes")));
	// Reviews M-6, M13: the transport stops the body at the cap (the fake mirrors the HTTP transport's sink)
	TestTrue(TEXT("... the transfer stopped there: nothing kept, not retryable"), Capped->Response.Body.Num() == 0 && !Capped->Error.bRetryable);
	TestEqual(TEXT("... the cap reached the transport"), Rig.Fake->GetCall(1).Request.MaxBodyBytes, static_cast<int64>(16));
	TestTrue(TEXT("a request's MaxJsonBytes(0) lifts it"), Uncapped->Value.IsSet() && Uncapped->Value->IsObject());
	TestTrue(TEXT("an error body over the cap: still the HTTP error, its body not kept"), CappedError->Error.Kind == EVaRestErrorKind::Http
		&& CappedError->Error.HttpStatus == 500 && !CappedError->Error.Body.IsValid() && CappedError->Error.Message.Contains(TEXT("MaxJsonBytes")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpOnHttpThreadTest, "VaRest.Http.Decode.OnHttpThread",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpOnHttpThreadTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/heavy")).OnHttpThread(), Outcome).Send();
	TestTrue(TEXT("completes"), Rig.Wait(Outcome));
	TestFalse(TEXT("OnHttpThread: callbacks off the game thread (decision 4)"), Outcome->bAllOnGameThread.load());
	TestEqual(TEXT("... still exactly once"), Outcome->CompleteCalls.load(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpOnHttpThreadPinTest, "VaRest.Http.Decode.OnHttpThreadPinsOwner",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpOnHttpThreadPinTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	// Owner() holds its owner weakly; once the local is cleared, nothing else refers to it, so a GC frees it unless the
	// delivery pins it (decision 28, review C-5)
	UObject* Owner = NewObject<UVaRestHttpSettings>(GetTransientPackage(), NAME_None, RF_Transient);
	const TWeakObjectPtr<UObject> Weak(Owner);
	struct FState
	{
		std::atomic<bool> bInside{false};
		std::atomic<bool> bRelease{false};
		std::atomic<int32> CompleteCalls{0};
	};
	const TSharedRef<FState, ESPMode::ThreadSafe> State = MakeShared<FState, ESPMode::ThreadSafe>();
	Rig.Client.Get(TEXT("/pinned")).Owner(Owner).OnHttpThread()
		.OnSuccess([State](const FVaRestJson&, const FVaRestResponse&)
		{
			// Where a [this] callback would touch the owner, on a worker: hold here until the game thread has collected
			State->bInside = true;
			const double Until = FPlatformTime::Seconds() + 5.0;
			while (!State->bRelease.load() && FPlatformTime::Seconds() < Until)
			{
				FPlatformProcess::Yield();
			}
		})
		.OnComplete([State](const TVaRestResult<FVaRestJson>&)
		{
			++State->CompleteCalls;
		})
		.Send();
	Owner = nullptr;
	TestTrue(TEXT("the callback runs on a worker"), PumpUntil([State] { return State->bInside.load(); }));
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
	TestTrue(TEXT("decision 28: a GC while an OnHttpThread callback runs does not free its owner"), Weak.IsValid());
	State->bRelease = true;
	TestTrue(TEXT("completes"), PumpUntil([State] { return State->CompleteCalls.load() == 1; }));
	// The pin drops on the worker right after the callbacks return (Task 10 Step 6), so poll with a GC each time
	TestTrue(TEXT("... and the pin is dropped after the callbacks (no leak)"), PumpUntil([&Weak]
	{
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
		return !Weak.IsValid();
	}, 2.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSaveToTest, "VaRest.Http.Decode.SaveTo",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSaveToTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::DecodeTestPrivate;
	IFileManager::Get().DeleteDirectory(*DownloadDir(), false, true);
	FTestRig Rig;
	Rig.Fake->Enqueue(FVaRestFakeResponse::Bytes(200, { 1, 2, 3, 4, 5 }));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Bytes(204, {}));
	const FString File = DownloadDir() / TEXT("pack.bin");
	const FString Empty = DownloadDir() / TEXT("empty.bin");
	auto Saved = MakeOutcome<FVaRestResponse>();
	auto Nothing = MakeOutcome<void>();
	Observe(Rig.Client.Get<FVaRestResponse>(TEXT("/pack")).SaveTo(File), Saved).Send();
	Observe(Rig.Client.Get<void>(TEXT("/empty")).SaveTo(Empty), Nothing).Send();
	TestTrue(TEXT("complete"), PumpUntil([&] { return Saved->IsDone() && Nothing->IsDone(); }));
	TArray<uint8> OnDisk;
	TestTrue(TEXT("the body was streamed to the destination"), FFileHelper::LoadFileToArray(OnDisk, *File) && OnDisk == TArray<uint8>({ 1, 2, 3, 4, 5 }));
	TestTrue(TEXT("through a temp file next to it (spec 3.2)"), Rig.Fake->GetCall(0).Request.ResponseFile.StartsWith(File + TEXT(".varest-")));
	TestEqualSensitive(TEXT("the response names the file"), Saved->Value.IsSet() ? Saved->Value->SavedFile : FString(), File);
	TestEqual(TEXT("an empty body still makes the (empty) file"), IFileManager::Get().FileSize(*Empty), static_cast<int64>(0));
	TestEqual(TEXT("no temp file is left"), CountPartFiles(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSaveToErrorTest, "VaRest.Http.Decode.SaveToKeepsOldFileOnError",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSaveToErrorTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::DecodeTestPrivate;
	IFileManager::Get().DeleteDirectory(*DownloadDir(), false, true);
	const FString File = DownloadDir() / TEXT("save.dat");
	FFileHelper::SaveStringToFile(TEXT("old"), *File);
	FTestRig Rig;
	Rig.Fake->Enqueue(Json(500, R"({"error":"disk full"})"));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Failed(EVaRestTransportFailure::Connection, TEXT("ConnectionError")));
	auto Failed = MakeOutcome<void>();
	auto Dropped = MakeOutcome<void>();
	Observe(Rig.Client.Get<void>(TEXT("/save")).SaveTo(File), Failed).Send();
	Observe(Rig.Client.Get<void>(TEXT("/save")).SaveTo(File), Dropped).Send();
	TestTrue(TEXT("complete"), PumpUntil([&] { return Failed->IsDone() && Dropped->IsDone(); }));
	FString Content;
	FFileHelper::LoadFileToString(Content, *File);
	TestEqualSensitive(TEXT("a failed download never touches the destination"), Content, FString(TEXT("old")));
	TestEqual(TEXT("... and leaves no temp file"), CountPartFiles(), 0);
	TestTrue(TEXT("the error body, read back from the temp file, is parsed"), Failed->Error.Kind == EVaRestErrorKind::Http
		&& Failed->Error.Body[TEXT("error")].AsString().Equals(TEXT("disk full"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("a connection failure is reported"), Dropped->Error.Kind == EVaRestErrorKind::Connection);
	return true;
}

#endif
