// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "VaRestClient.h"
#include "VaRestDecode.h"
#include "VaRestExchangeState.h"
#include "VaRestHttpSettings.h"
#include "VaRestHttpTesting.h"
#include "VaRestTrace.h"

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

	int32 CountFiles(const TCHAR* Pattern)
	{
		TArray<FString> Found;
		IFileManager::Get().FindFiles(Found, *(DownloadDir() / Pattern), true, false);
		return Found.Num();
	}

	int32 CountPartFiles()
	{
		return CountFiles(TEXT("*.part"));
	}

	/** The file's bytes; empty when it is missing (silently: a missing file is what some checks look for) */
	TArray<uint8> ReadBytes(const FString& Path)
	{
		TArray<uint8> Bytes;
		FFileHelper::LoadFileToArray(Bytes, *Path, FILEREAD_Silent);
		return Bytes;
	}

	FString ReadText(const FString& Path)
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *Path, FFileHelper::EHashOptions::None, FILEREAD_Silent);
		return Text;
	}

	/** A custom stage whose Process is a lambda: outcomes a stage makes, re-issues, files changed under the core */
	class FLambdaStage final : public IVaRestStage
	{
	public:
		using FProcess = TFunction<void(const FVaRestExchangeRef&, const FVaRestStageNext&, FVaRestOutcomeCallback&&)>;

		FLambdaStage(FProcess InProcess, bool bInReissues)
			: ProcessFunc(MoveTemp(InProcess))
			, bReissues(bInReissues)
		{
		}

		virtual FName GetStageName() const override { return TEXT("DecodeTestStage"); }
		virtual bool MayReissue() const override { return bReissues; }

		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			ProcessFunc(Exchange, Next, MoveTemp(OnOutcome));
		}

	private:
		FProcess ProcessFunc;
		bool bReissues;
	};

	FVaRestStageRef MakeStage(FLambdaStage::FProcess Func, bool bReissues = false)
	{
		return MakeShared<FLambdaStage, ESPMode::ThreadSafe>(MoveTemp(Func), bReissues);
	}

	/** A 200 a stage makes itself, its body in memory (a cache hit, a fallback) */
	FVaRestAttemptOutcome StageAnswer(TArray<uint8> Bytes, const TCHAR* ContentType = TEXT("application/octet-stream"))
	{
		FVaRestAttemptOutcome Answer;
		Answer.Status = 200;
		Answer.ContentType = ContentType;
		Answer.Headers.Set(TEXT("Content-Type"), ContentType);
		Answer.Body = MoveTemp(Bytes);
		return Answer;
	}

	TArray<uint8> Utf8Bytes(const char* Text)
	{
		return TArray<uint8>(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text));
	}

	/** What the injected file operations did, and what they refuse */
	struct FMoveScript
	{
		/** MoveFileW's rule (WindowsPlatformFile.cpp:1218, no MOVEFILE_REPLACE_EXISTING): an existing target refuses the move */
		bool bRefuseExistingTarget = false;
		/** Every move from this path fails (the new file locked by a scanner, the target name still delete-pending) */
		FString FailFrom;
		/** Moving a .bak back fails too */
		bool bFailBackupReturn = false;
		int32 Moves = 0;
		TArray<FString> Deleted;
	};

	/** The platform's file operations, under Script's rules */
	FFileOps ScriptedOps(const TSharedRef<FMoveScript, ESPMode::ThreadSafe>& Script)
	{
		FFileOps Ops;
		Ops.Move = [Script](const FString& To, const FString& From)
		{
			++Script->Moves;
			IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
			if ((Script->bRefuseExistingTarget && PlatformFile.FileExists(*To)) || From.Equals(Script->FailFrom, ESearchCase::CaseSensitive)
				|| (Script->bFailBackupReturn && From.EndsWith(TEXT(".bak"), ESearchCase::CaseSensitive)))
			{
				return false;
			}
			return PlatformFile.MoveFile(*To, *From);
		};
		Ops.Delete = [Script](const FString& Path)
		{
			Script->Deleted.Add(Path);
			return FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*Path);
		};
		Ops.IsFile = [](const FString& Path) { return FPlatformFileManager::Get().GetPlatformFile().FileExists(*Path); };
		return Ops;
	}

	/** The requests whose Completed event a trace sink saw (IN-5: on the game thread, before any callback) */
	class FCompletedRecorder final : public IVaRestTraceSink
	{
	public:
		virtual FVaRestTraceDetailSettings GetDetail() const override
		{
			FVaRestTraceDetailSettings Detail;
			Detail.Flags = EVaRestTraceDetail::Metadata;
			return Detail;
		}
		virtual void OnAttached(const FVaRestTraceSnapshot&) override {}
		virtual void OnEvent(const FVaRestTraceEvent& Event) override
		{
			if (Event.Kind == EVaRestTraceKind::Completed)
			{
				FScopeLock Lock(&Mutex);
				Completed.Add(Event.Payload.Get<FVaRestTraceCompleted>().RequestId);
			}
		}
		/** Any thread */
		bool HasCompleted(uint64 RequestId) const
		{
			FScopeLock Lock(&Mutex);
			return Completed.Contains(RequestId);
		}

	private:
		mutable FCriticalSection Mutex;
		TSet<uint64> Completed;
	};

	/** What OnHttpThread() callbacks saw, written on a worker */
	struct FWorkerSeen
	{
		/** The request's id, set on the game thread right after Send(), before the request can complete */
		std::atomic<uint64> RequestId{ 0 };
		std::atomic<int32> ErrorCalls{ 0 };
		std::atomic<int32> CompleteCalls{ 0 };
		std::atomic<int32> SuccessCalls{ 0 };
		std::atomic<uint8> Kind{ 0 };
		std::atomic<bool> bAllOffGameThread{ true };
		std::atomic<bool> bAllAfterCompleted{ true };
	};

	/** Wires OnError, OnSuccess and OnComplete of an OnHttpThread() request into Seen, each checking it runs after Completed */
	TVaRestRequest<FVaRestJson> WatchOnWorker(TVaRestRequest<FVaRestJson>&& Request, const TSharedRef<FWorkerSeen, ESPMode::ThreadSafe>& Seen,
		const TSharedRef<FCompletedRecorder, ESPMode::ThreadSafe>& Recorder)
	{
		auto Note = [Seen, Recorder]
		{
			Seen->bAllOffGameThread = Seen->bAllOffGameThread.load() && !IsInGameThread();
			Seen->bAllAfterCompleted = Seen->bAllAfterCompleted.load() && Recorder->HasCompleted(Seen->RequestId.load());
		};
		return MoveTemp(Request).OnHttpThread()
			.OnSuccess([Seen, Note](const FVaRestJson&) { Note(); ++Seen->SuccessCalls; })
			.OnError([Seen, Note](const FVaRestError& Error)
			{
				Note();
				Seen->Kind = static_cast<uint8>(Error.Kind);
				++Seen->ErrorCalls;
			})
			.OnComplete([Seen, Note](const TVaRestResult<FVaRestJson>&) { Note(); ++Seen->CompleteCalls; });
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
	Rig.Fake->Enqueue(Json(200, R"({"b":)"));
	auto KeptBad = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/ct/kept")).KeepBody(), KeptBad).Send();
	TestTrue(TEXT("complete"), PumpUntil([&O, &KeptBad] { for (const auto& X : O) { if (!X->IsDone()) { return false; } } return KeptBad->IsDone(); }));
	TestTrue(TEXT("+json is JSON"), O[0]->Value.IsSet() && (*O[0]->Value)[TEXT("a")].AsInt() == 1);
	TestTrue(TEXT("no Content-Type is parsed (decision 6)"), O[1]->Value.IsSet() && (*O[1]->Value)[TEXT("a")].AsInt() == 2);
	TestTrue(TEXT("text/html for a JSON result is a Decode error (spec 3.2)"), O[2]->Error.Kind == EVaRestErrorKind::Decode && O[2]->Error.Message.Contains(TEXT("text/html")));
	TestTrue(TEXT("... the raw body kept and quoted"), O[2]->Response.Body.Num() > 0 && O[2]->Error.RawBody.Contains(TEXT("login")));
	TestTrue(TEXT("invalid JSON is a Decode error with the parser's message"), O[3]->Error.Kind == EVaRestErrorKind::Decode && O[3]->Error.Message.Contains(TEXT("not valid JSON")));
	TestTrue(TEXT("... and the first bytes as RawBody, from before the in-situ parse"), O[3]->Error.RawBody.Contains(TEXT("{\"a\":")));
	TestEqual(TEXT("decision 26: an in-situ parse error keeps no raw body, only the 4 KiB prefix"), O[3]->Response.Body.Num(), 0);
	// Review M5: with KeepBody() the prefix comes from the kept, unparsed copy (no second copy is made)
	TestTrue(TEXT("KeepBody on invalid JSON: a Decode error quoting the first bytes"), KeptBad->Error.Kind == EVaRestErrorKind::Decode && KeptBad->Error.RawBody.Contains(TEXT("{\"b\":")));
	TestTrue(TEXT("... with the whole original body kept"), KeptBad->Response.Body == Utf8Bytes(R"({"b":)"));
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpOnHttpThreadErrorTest, "VaRest.Http.Decode.OnHttpThreadErrorAndCancel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpOnHttpThreadErrorTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::DecodeTestPrivate;
	// Final review M11: an error and an explicit Cancel() take the OnHttpThread() path too -- on a worker, after the game
	// thread's Completed (decision 4, Inspector IN-5)
	FVaRestTrace::FScopedSinkIsolation Isolation;
	const TSharedRef<FCompletedRecorder, ESPMode::ThreadSafe> Recorder = MakeShared<FCompletedRecorder, ESPMode::ThreadSafe>();
	FVaRestTrace::AddSink(Recorder);
	FTestRig Rig;
	Rig.Fake->Enqueue(Json(404, R"({"error":"missing"})"));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	const TSharedRef<FWorkerSeen, ESPMode::ThreadSafe> Failed = MakeShared<FWorkerSeen, ESPMode::ThreadSafe>();
	const TSharedRef<FWorkerSeen, ESPMode::ThreadSafe> Canceled = MakeShared<FWorkerSeen, ESPMode::ThreadSafe>();
	Failed->RequestId = WatchOnWorker(Rig.Client.Get(TEXT("/missing")), Failed, Recorder).Send().GetId();
	const FVaRestRequestHandle CancelHandle = WatchOnWorker(Rig.Client.Get(TEXT("/slow")), Canceled, Recorder).Send();
	Canceled->RequestId = CancelHandle.GetId();
	TestTrue(TEXT("dispatched"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; }));
	CancelHandle.Cancel();
	TestTrue(TEXT("both complete"), PumpUntil([Failed, Canceled] { return Failed->CompleteCalls.load() == 1 && Canceled->CompleteCalls.load() == 1; }));
	FVaRestTrace::RemoveSink(Recorder);
	TestTrue(TEXT("an error: one OnError (Kind Http), one OnComplete, no OnSuccess"), Failed->ErrorCalls.load() == 1 && Failed->SuccessCalls.load() == 0
		&& Failed->Kind.load() == static_cast<uint8>(EVaRestErrorKind::Http));
	TestTrue(TEXT("... on a worker"), Failed->bAllOffGameThread.load());
	TestTrue(TEXT("... after Completed"), Failed->bAllAfterCompleted.load());
	TestTrue(TEXT("a Cancel(): one OnError (Kind Canceled), one OnComplete, no OnSuccess"), Canceled->ErrorCalls.load() == 1 && Canceled->SuccessCalls.load() == 0
		&& Canceled->Kind.load() == static_cast<uint8>(EVaRestErrorKind::Canceled));
	TestTrue(TEXT("... on a worker"), Canceled->bAllOffGameThread.load());
	TestTrue(TEXT("... after Completed"), Canceled->bAllAfterCompleted.load());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpOnHttpThreadOwnerGoneTest, "VaRest.Http.Decode.OnHttpThreadOwnerGone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpOnHttpThreadOwnerGoneTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::DecodeTestPrivate;
	// Final review M11: owner gone with OnHttpThread() is silent (spec 3.2) and pins nothing (decision 28 pins only for a
	// delivery that happens)
	FTestRig Rig;
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	UObject* Owner = NewObject<UVaRestHttpSettings>(GetTransientPackage(), NAME_None, RF_Transient);
	const TWeakObjectPtr<UObject> Weak(Owner);
	auto Outcome = MakeOutcome<FVaRestJson>();
	const int32 DeliveriesBefore = Counters().OffGameThreadDeliveries.load();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/owned")).Owner(Owner).OnHttpThread(), Outcome).Send();
	TestTrue(TEXT("dispatched"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; }));
	Owner->MarkAsGarbage();
	Owner = nullptr;
	Rig.Fake->Complete(0, Json(200, R"({"late":1})"));
	TestTrue(TEXT("the request finishes"), PumpUntil([&Handle] { return !Handle.IsActive(); }));
	TestTrue(TEXT("drained"), Rig.Drain());
	// Complete sets IsActive() false and launches the worker delivery in the same game-thread call, so this count is final
	TestEqual(TEXT("silent: no worker delivery was launched"), Counters().OffGameThreadDeliveries.load() - DeliveriesBefore, 0);
	TestEqual(TEXT("... and no callback ran"), Outcome->SuccessCalls.load() + Outcome->ErrorCalls.load() + Outcome->CompleteCalls.load(), 0);
	TestEqual(TEXT("... nothing left in flight"), Rig.Client.GetInFlightCount(), 0);
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
	TestFalse(TEXT("nothing pinned the owner: the collector freed it"), Weak.IsValid(true));
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
	// Review M3: a destination that is a directory, so the final move fails
	const FString Taken = DownloadDir() / TEXT("taken");
	FFileHelper::SaveStringToFile(TEXT("inside"), *(Taken / TEXT("keep.txt")));
	FTestRig Rig;
	// Review M3: a transfer that dies part-way has written some bytes; the fake writes none, so this stage does
	Rig.Client.Use(MakeStage([](const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome)
	{
		Next.Proceed(Exchange, [Exchange, Done = MoveTemp(OnOutcome)](FVaRestAttemptOutcome&& Outcome) mutable
		{
			if (Outcome.IsTransportFailure())
			{
				FFileHelper::SaveStringToFile(TEXT("partial"), *VaRest::Http::Internals(*Exchange).TempFile);
			}
			Done(MoveTemp(Outcome));
		});
	}), EVaRestStageSlot::PerAttempt);
	Rig.Fake->Enqueue(Json(500, R"({"error":"disk full"})"));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Failed(EVaRestTransportFailure::Connection, TEXT("ConnectionError")));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Bytes(200, { 1, 2 }));
	auto Failed = MakeOutcome<void>();
	auto Dropped = MakeOutcome<void>();
	auto Unmovable = MakeOutcome<void>();
	Observe(Rig.Client.Get<void>(TEXT("/save")).SaveTo(File), Failed).Send();
	Observe(Rig.Client.Get<void>(TEXT("/save")).SaveTo(File), Dropped).Send();
	Observe(Rig.Client.Get<void>(TEXT("/save")).SaveTo(Taken), Unmovable).Send();
	TestTrue(TEXT("complete"), PumpUntil([&] { return Failed->IsDone() && Dropped->IsDone() && Unmovable->IsDone(); }));
	FString Content;
	FFileHelper::LoadFileToString(Content, *File);
	TestEqualSensitive(TEXT("a failed download never touches the destination"), Content, FString(TEXT("old")));
	TestEqual(TEXT("... and leaves no temp file, a partial one included"), CountPartFiles(), 0);
	TestTrue(TEXT("the error body, read back from the temp file, is parsed"), Failed->Error.Kind == EVaRestErrorKind::Http
		&& Failed->Error.Body[TEXT("error")].AsString().Equals(TEXT("disk full"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("a connection failure is reported"), Dropped->Error.Kind == EVaRestErrorKind::Connection);
	TestTrue(TEXT("a move that fails is a Decode error and no success"), Unmovable->Error.Kind == EVaRestErrorKind::Decode
		&& Unmovable->Error.Message.Contains(TEXT("could not move")) && Unmovable->SuccessCalls.load() == 0);
	TestTrue(TEXT("... the directory in the way untouched"), IFileManager::Get().DirectoryExists(*Taken)
		&& ReadText(Taken / TEXT("keep.txt")).Equals(TEXT("inside"), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSaveToReissueTest, "VaRest.Http.Decode.SaveToReissue",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSaveToReissueTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::DecodeTestPrivate;
	IFileManager::Get().DeleteDirectory(*DownloadDir(), false, true);
	FTestRig Rig;
	// Spec 4: a stage after the attempt boundary may proceed again (an auth replay): a second transport start, same attempt
	Rig.Client.Use(MakeStage([](const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome)
	{
		Next.Proceed(Exchange, [Exchange, Next, Done = MoveTemp(OnOutcome)](FVaRestAttemptOutcome&&) mutable
		{
			Next.Proceed(Exchange, MoveTemp(Done));
		});
	}, true), EVaRestStageSlot::PerAttempt);
	Rig.Fake->Enqueue(FVaRestFakeResponse::Bytes(200, { 1, 1, 1 }));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Bytes(200, { 2, 2, 2, 2 }));
	const FString File = DownloadDir() / TEXT("replayed.bin");
	auto Saved = MakeOutcome<void>();
	Observe(Rig.Client.Get<void>(TEXT("/replayed")).SaveTo(File), Saved).Send();
	TestTrue(TEXT("completes"), Rig.Wait(Saved));
	const bool bTwoStarts = Rig.Fake->NumCalls() == 2;
	TestTrue(TEXT("two transport starts"), bTwoStarts);
	if (bTwoStarts)
	{
		const FVaRestTransportRequest& First = Rig.Fake->GetCall(0).Request;
		const FVaRestTransportRequest& Second = Rig.Fake->GetCall(1).Request;
		TestEqual(TEXT("... under one attempt number"), Second.Attempt, First.Attempt);
		// Review I1: the second start deletes the first one's file; were the names equal, that delete would hit the live download
		TestFalse(TEXT("... each with its own temp file"), Second.ResponseFile.Equals(First.ResponseFile, ESearchCase::CaseSensitive));
	}
	TestEqual(TEXT("one success"), Saved->SuccessCalls.load(), 1);
	TestTrue(TEXT("the destination holds the second response"), ReadBytes(File) == TArray<uint8>({ 2, 2, 2, 2 }));
	// The first file's delete is a task of its own: let it land
	TestTrue(TEXT("no temp file is left"), PumpUntil([] { return CountPartFiles() == 0; }, 2.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSaveToLostFileTest, "VaRest.Http.Decode.SaveToLostTempFile",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSaveToLostFileTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::DecodeTestPrivate;
	IFileManager::Get().DeleteDirectory(*DownloadDir(), false, true);
	const FString File = DownloadDir() / TEXT("lost.bin");
	FFileHelper::SaveStringToFile(TEXT("old"), *File);
	FTestRig Rig;
	// The bytes arrived and then their file vanished: FinalizeSaveTo must not take that for an empty body
	Rig.Client.Use(MakeStage([](const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome)
	{
		Next.Proceed(Exchange, [Exchange, Done = MoveTemp(OnOutcome)](FVaRestAttemptOutcome&& Outcome) mutable
		{
			IFileManager::Get().Delete(*VaRest::Http::Internals(*Exchange).TempFile, false, false, true);
			Done(MoveTemp(Outcome));
		});
	}), EVaRestStageSlot::PerAttempt);
	Rig.Fake->Enqueue(FVaRestFakeResponse::Bytes(200, { 1, 2, 3 }));
	auto Lost = MakeOutcome<void>();
	Observe(Rig.Client.Get<void>(TEXT("/lost")).SaveTo(File), Lost).Send();
	TestTrue(TEXT("completes"), Rig.Wait(Lost));
	TestEqual(TEXT("review I1: received bytes whose file is gone are never a success"), Lost->SuccessCalls.load(), 0);
	TestTrue(TEXT("... but a Decode error saying so"), Lost->Error.Kind == EVaRestErrorKind::Decode && Lost->Error.Message.Contains(TEXT("is gone")));
	TestTrue(TEXT("... the destination untouched"), ReadText(File).Equals(TEXT("old"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("... and no temp file made in its place"), CountPartFiles(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpMoveIntoPlaceTest, "VaRest.Http.Decode.MoveIntoPlace",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpMoveIntoPlaceTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::DecodeTestPrivate;
	IFileManager::Get().DeleteDirectory(*DownloadDir(), false, true);
	const FString To = DownloadDir() / TEXT("replace.dat");
	const FString From = DownloadDir() / TEXT("replace.dat.new.part");
	FString Stranded;

	// Windows' rule, everything working: the old file moved aside, the new one in, the .bak deleted
	FFileHelper::SaveStringToFile(TEXT("old"), *To);
	FFileHelper::SaveStringToFile(TEXT("new"), *From);
	TSharedRef<FMoveScript, ESPMode::ThreadSafe> Script = MakeShared<FMoveScript, ESPMode::ThreadSafe>();
	Script->bRefuseExistingTarget = true;
	TestTrue(TEXT("Windows: replaced"), MoveIntoPlace(To, From, true, ScriptedOps(Script), Stranded));
	TestTrue(TEXT("... the new content in place"), ReadText(To).Equals(TEXT("new"), ESearchCase::CaseSensitive) && !FPaths::FileExists(From));
	TestEqual(TEXT("... no .bak left"), CountFiles(TEXT("*.bak")), 0);

	// Review I2: the second move fails -- the old file must come back, never be deleted first
	FFileHelper::SaveStringToFile(TEXT("old"), *To);
	FFileHelper::SaveStringToFile(TEXT("new"), *From);
	Script = MakeShared<FMoveScript, ESPMode::ThreadSafe>();
	Script->bRefuseExistingTarget = true;
	Script->FailFrom = From;
	TestFalse(TEXT("Windows, the new file cannot move in: not replaced"), MoveIntoPlace(To, From, true, ScriptedOps(Script), Stranded));
	TestTrue(TEXT("... the old file is still there"), ReadText(To).Equals(TEXT("old"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("... nothing stranded, no .bak left"), Stranded.IsEmpty() && CountFiles(TEXT("*.bak")) == 0);

	// ... and when even the way back fails, the caller learns where the old file is
	Script = MakeShared<FMoveScript, ESPMode::ThreadSafe>();
	Script->bRefuseExistingTarget = true;
	Script->FailFrom = From;
	Script->bFailBackupReturn = true;
	TestFalse(TEXT("Windows, the way back fails too: not replaced"), MoveIntoPlace(To, From, true, ScriptedOps(Script), Stranded));
	TestTrue(TEXT("... the old file is at the stranded .bak"), !Stranded.IsEmpty() && ReadText(Stranded).Equals(TEXT("old"), ESearchCase::CaseSensitive));
	if (!Stranded.IsEmpty())
	{
		IFileManager::Get().Move(*To, *Stranded);
	}

	// Every other platform: rename() replaces atomically, so one failed move is the answer -- nothing deleted, nothing moved aside
	Script = MakeShared<FMoveScript, ESPMode::ThreadSafe>();
	Script->FailFrom = From;
	TestFalse(TEXT("POSIX, the move fails: not replaced"), MoveIntoPlace(To, From, false, ScriptedOps(Script), Stranded));
	TestTrue(TEXT("... one move tried, no delete"), Script->Moves == 1 && Script->Deleted.Num() == 0);
	TestTrue(TEXT("... the old file untouched"), ReadText(To).Equals(TEXT("old"), ESearchCase::CaseSensitive));

	// Windows' rule with a directory in the way: never moved aside
	const FString Dir = DownloadDir() / TEXT("a-directory");
	IFileManager::Get().MakeDirectory(*Dir, true);
	Script = MakeShared<FMoveScript, ESPMode::ThreadSafe>();
	Script->bRefuseExistingTarget = true;
	TestFalse(TEXT("Windows, a directory in the way: not replaced"), MoveIntoPlace(Dir, From, true, ScriptedOps(Script), Stranded));
	TestTrue(TEXT("... and not moved aside"), Script->Moves == 1 && IFileManager::Get().DirectoryExists(*Dir));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSaveToFromStageTest, "VaRest.Http.Decode.SaveToFromStage",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSaveToFromStageTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::DecodeTestPrivate;
	IFileManager::Get().DeleteDirectory(*DownloadDir(), false, true);
	FTestRig Rig;
	// Review M1: an Outer stage answering by itself -- before any attempt (a cache hit), or after one failed (a fallback)
	Rig.Client.Use(MakeStage([](const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome)
	{
		if (Exchange->GetUrl().Contains(TEXT("/cached")))
		{
			OnOutcome(StageAnswer({ 7, 8, 9 }));
			return;
		}
		Next.Proceed(Exchange, [Done = MoveTemp(OnOutcome)](FVaRestAttemptOutcome&&) mutable
		{
			Done(StageAnswer({ 4, 5, 6 }));
		});
	}), EVaRestStageSlot::Outer);
	Rig.Fake->Enqueue(FVaRestFakeResponse::Bytes(503, { 0xE, 0xE, 0xE }));
	const FString CachedFile = DownloadDir() / TEXT("cached.bin");
	const FString FallbackFile = DownloadDir() / TEXT("fallback.bin");
	auto Cached = MakeOutcome<FVaRestResponse>();
	auto Fallback = MakeOutcome<void>();
	Observe(Rig.Client.Get<FVaRestResponse>(TEXT("/cached")).SaveTo(CachedFile), Cached).Send();
	Observe(Rig.Client.Get<void>(TEXT("/fallback")).SaveTo(FallbackFile), Fallback).Send();
	TestTrue(TEXT("complete"), PumpUntil([&] { return Cached->IsDone() && Fallback->IsDone(); }));
	TestTrue(TEXT("a stage's answer is saved too"), Cached->SuccessCalls.load() == 1 && ReadBytes(CachedFile) == TArray<uint8>({ 7, 8, 9 }));
	TestEqualSensitive(TEXT("... and named"), Cached->Value.IsSet() ? Cached->Value->SavedFile : FString(), CachedFile);
	TestEqual(TEXT("... without a transport call"), Rig.Fake->NumCalls(), 1);
	TestTrue(TEXT("a fallback after an attempt saves its own body, not the attempt's file"), Fallback->SuccessCalls.load() == 1
		&& ReadBytes(FallbackFile) == TArray<uint8>({ 4, 5, 6 }));
	TestEqual(TEXT("no temp file is left"), CountPartFiles(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSizeCapSourcesTest, "VaRest.Http.Decode.SizeCapSources",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSizeCapSourcesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::DecodeTestPrivate;
	const char* Big = R"({"padding":"0123456789012345678901234567890123456789"})";

	// The project setting is a client's default, read by FVaRestClient::Make
	UVaRestHttpSettings* Settings = GetMutableDefault<UVaRestHttpSettings>();
	const int64 Configured = Settings->MaxJsonResponseBytes;
	Settings->MaxJsonResponseBytes = 16;
	FTestRig FromSettings;
	Settings->MaxJsonResponseBytes = Configured;
	FromSettings.Fake->SetResponder([Big](const FVaRestTransportRequest&) { return Json(200, Big); });
	auto Capped = MakeOutcome<FVaRestJson>();
	Observe(FromSettings.Client.Get(TEXT("/big")), Capped).Send();
	TestTrue(TEXT("completes"), FromSettings.Wait(Capped));
	TestTrue(TEXT("UVaRestHttpSettings::MaxJsonResponseBytes caps a client"), Capped->Error.Kind == EVaRestErrorKind::Decode && Capped->Error.Message.Contains(TEXT("MaxJsonBytes")));
	TestEqual(TEXT("... and reaches the transport"), FromSettings.Fake->GetCall(0).Request.MaxBodyBytes, static_cast<int64>(16));

	// A body the transport did not stop (a stage's answer): the parser refuses it, keeping the bytes (spec 3.2)
	FTestRig Rig;
	Rig.Client.WithMaxJsonBytes(16);
	Rig.Client.Use(MakeStage([Big](const FVaRestExchangeRef&, const FVaRestStageNext&, FVaRestOutcomeCallback&& OnOutcome)
	{
		OnOutcome(StageAnswer(Utf8Bytes(Big), TEXT("application/json")));
	}), EVaRestStageSlot::Outer);
	auto Refused = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/cached")), Refused).Send();
	TestTrue(TEXT("completes"), Rig.Wait(Refused));
	TestTrue(TEXT("decision 20: over the cap before the parse is a Decode error"), Refused->Error.Kind == EVaRestErrorKind::Decode
		&& Refused->Error.Message.Contains(TEXT("bytes, over the MaxJsonBytes limit of 16")));
	TestTrue(TEXT("... with the bytes kept"), Refused->Response.Body == Utf8Bytes(Big));
	TestEqual(TEXT("... and no transport call"), Rig.Fake->NumCalls(), 0);
	return true;
}

#endif
