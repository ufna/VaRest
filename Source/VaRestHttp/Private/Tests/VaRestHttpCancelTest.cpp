// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Tasks/Task.h"
#include "UObject/Package.h"
#include "VaRestClient.h"
#include "VaRestClientState.h"
#include "VaRestDecode.h"
#include "VaRestHttpSettings.h"
#include "VaRestHttpTesting.h"

namespace VaRest::Http::CancelTestPrivate
{
	FVaRestFakeResponse Json(int32 Status, const char* Text)
	{
		return FVaRestFakeResponse::Json(Status, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text)));
	}

	/** An Outer stage that keeps the exchange and resumes only when told */
	class FHoldingStage final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("Hold"); }
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			Resume = [Exchange, Next, Done = MoveTemp(OnOutcome)]() mutable { Next.Proceed(Exchange, MoveTemp(Done)); };
		}
		TUniqueFunction<void()> Resume;
	};

	/** An Outer stage that holds the outcome on its way up until the test forwards it (review C-3) */
	class FOutcomeHoldingStage final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("HoldOutcome"); }
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			Next.Proceed(Exchange, [this, Done = MoveTemp(OnOutcome)](FVaRestAttemptOutcome&& Outcome) mutable
			{
				Kept = MoveTemp(Outcome);
				Forward = [this, Callback = MoveTemp(Done)]() mutable { Callback(MoveTemp(Kept)); };
				bHolding = true;
			});
		}
		FVaRestAttemptOutcome Kept;
		TUniqueFunction<void()> Forward;
		bool bHolding = false;
	};

	/** An Outer stage that counts the exchanges it sees and records whether the outcome came back up as canceled */
	class FWatchingStage final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("Watch"); }
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			++Entered;
			Next.Proceed(Exchange, [this, Exchange, Done = MoveTemp(OnOutcome)](FVaRestAttemptOutcome&& Outcome) mutable
			{
				bSawCancel = Exchange->IsCancelRequested();
				Done(MoveTemp(Outcome));
			});
		}
		int32 Entered = 0;
		bool bSawCancel = false;
	};

	UObject* NewOwner()
	{
		// Any UObject will do; a transient settings instance needs no extra class
		return NewObject<UVaRestHttpSettings>(GetTransientPackage(), NAME_None, RF_Transient);
	}

	/** Review I-6: run nothing on the game thread until the worker is done; the worker never needs the game thread */
	void SpinUntil(const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe>& Flag)
	{
		const double Until = FPlatformTime::Seconds() + 10.0;
		while (!Flag->load() && FPlatformTime::Seconds() < Until)
		{
			FPlatformProcess::Yield();
		}
	}

	/** As SpinUntil, for a condition: no game-thread task runs meanwhile. True when Done() became true within 10 s. */
	bool SpinUntilTrue(TFunctionRef<bool()> Done)
	{
		const double Until = FPlatformTime::Seconds() + 10.0;
		while (!Done())
		{
			if (FPlatformTime::Seconds() >= Until)
			{
				return false;
			}
			FPlatformProcess::Yield();
		}
		return true;
	}

	FString DownloadDir()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("VaRestTests") / TEXT("cancel-downloads"));
	}

	int32 CountPartFiles()
	{
		TArray<FString> Found;
		IFileManager::Get().FindFiles(Found, *(DownloadDir() / TEXT("*.part")), true, false);
		return Found.Num();
	}

	/** The file's bytes; empty when it is missing (silently) */
	TArray<uint8> ReadBytes(const FString& Path)
	{
		TArray<uint8> Bytes;
		FFileHelper::LoadFileToArray(Bytes, *Path, FILEREAD_Silent);
		return Bytes;
	}

	bool HoldsOld(const FString& Path)
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *Path, FFileHelper::EHashOptions::None, FILEREAD_Silent);
		return Text.Equals(TEXT("old"), ESearchCase::CaseSensitive);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpCancelInTransportTest, "VaRest.Http.Cancel.InTransport",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpCancelInTransportTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/slow")), Outcome).Send();
	PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; });
	TestTrue(TEXT("active while in flight"), Handle.IsActive());
	Handle.Cancel();
	TestEqual(TEXT("decision 3: nothing is delivered inside Cancel()"), Outcome->CompleteCalls.load(), 0);
	TestTrue(TEXT("completes"), Rig.Wait(Outcome));
	TestTrue(TEXT("decision 2: an explicit Cancel delivers Kind Canceled"), Outcome->ErrorCalls == 1 && Outcome->Error.Kind == EVaRestErrorKind::Canceled);
	TestEqual(TEXT("... never a success"), Outcome->SuccessCalls.load(), 0);
	TestEqual(TEXT("the transport call was canceled"), Rig.Fake->NumCanceled(), 1);
	TestFalse(TEXT("inactive afterwards"), Handle.IsActive());
	Handle.Cancel();
	PumpUntil([] { return false; }, 0.05);
	TestEqual(TEXT("a Cancel after the end changes nothing"), Outcome->CompleteCalls.load(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpCancelRacesTest, "VaRest.Http.Cancel.Races",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpCancelRacesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::CancelTestPrivate;

	// The answer is already on its way (queued for the game thread) when Cancel runs
	FTestRig Racing;
	Racing.Fake->bCompleteOnWorker = false;
	Racing.Fake->Enqueue(FVaRestFakeResponse::Manual());
	auto Raced = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle RacedHandle = Observe(Racing.Client.Get(TEXT("/r")), Raced).Send();
	Racing.Fake->Complete(0, Json(200, R"({"ok":true})"));
	RacedHandle.Cancel();
	TestTrue(TEXT("completes"), Racing.Wait(Raced));
	TestTrue(TEXT("after Cancel() returns, no OnSuccess runs: the answer that raced it is replaced by Canceled"),
		Raced->SuccessCalls == 0 && Raced->Error.Kind == EVaRestErrorKind::Canceled);

	// Canceled from a worker before it reached the game thread: no transport call at all
	FTestRig Early;
	auto Before = MakeOutcome<FVaRestJson>();
	FVaRestClient Client = Early.Client;
	TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> bCanceled = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	UE::Tasks::Launch(TEXT("VaRest.Test.CancelEarly"), [Client, Before, bCanceled]
	{
		Observe(Client.Get(TEXT("/never")), Before).Send().Cancel();
		*bCanceled = true;
	});
	SpinUntil(bCanceled); // review I-6: a pump here could run Enter between Send() and Cancel() on the worker
	TestTrue(TEXT("the worker ran"), bCanceled->load());
	TestTrue(TEXT("completes"), Early.Wait(Before));
	TestTrue(TEXT("canceled before entry: Kind Canceled"), Before->Error.Kind == EVaRestErrorKind::Canceled);
	TestEqual(TEXT("... and never sent"), Early.Fake->NumCalls(), 0);

	// Held by a stage that never resumes by itself
	FTestRig Held;
	TSharedRef<FHoldingStage, ESPMode::ThreadSafe> Stage = MakeShared<FHoldingStage, ESPMode::ThreadSafe>();
	Held.Client.Use(Stage, EVaRestStageSlot::Outer);
	auto Parked = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle ParkedHandle = Observe(Held.Client.Get(TEXT("/parked")), Parked).Send();
	ParkedHandle.Cancel();
	TestTrue(TEXT("completes while the stage holds it"), Held.Wait(Parked));
	TestTrue(TEXT("Canceled"), Parked->Error.Kind == EVaRestErrorKind::Canceled);
	{
		// Review M8: moved out first, so the stage keeps no exchange (and no client state) after it ran
		TUniqueFunction<void()> Run = MoveTemp(Stage->Resume);
		Run();
	}
	PumpUntil([] { return false; }, 0.05);
	TestEqual(TEXT("a stage that resumes later reaches no transport (Proceed reports Canceled)"), Held.Fake->NumCalls(), 0);
	TestEqual(TEXT("... and nothing is delivered twice"), Parked->CompleteCalls.load(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpCancelHeldOutcomeTest, "VaRest.Http.Cancel.WhileOutcomeHeld",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpCancelHeldOutcomeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::CancelTestPrivate;
	// Review C-3: Cancel() defers its Complete (decision 3); before that task runs, a stage forwards the 200 it held, and
	// the decode task takes the handler. The deferred Complete must leave the exchange to the decode task.
	FTestRig Rig;
	TSharedRef<FOutcomeHoldingStage, ESPMode::ThreadSafe> Stage = MakeShared<FOutcomeHoldingStage, ESPMode::ThreadSafe>();
	Rig.Client.Use(Stage, EVaRestStageSlot::Outer);
	Rig.Fake->Enqueue(Json(200, R"({"ok":true})"));
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/held")), Outcome).Send();
	TestTrue(TEXT("the stage holds the outcome"), PumpUntil([&Stage] { return Stage->bHolding; }));
	Handle.Cancel();
	{
		TUniqueFunction<void()> Run = MoveTemp(Stage->Forward); // synchronously, before any pump
		Run();
	}
	TestTrue(TEXT("completes"), Rig.Wait(Outcome));
	PumpUntil([] { return false; }, 0.05);
	TestTrue(TEXT("exactly one OnError(Canceled) and one OnComplete"), Outcome->ErrorCalls == 1 && Outcome->CompleteCalls == 1
		&& Outcome->SuccessCalls == 0 && Outcome->Error.Kind == EVaRestErrorKind::Canceled);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpCancelThenOwnerGoneTest, "VaRest.Http.Cancel.CancelThenOwnerGone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpCancelThenOwnerGoneTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::CancelTestPrivate;
	// Review C-2: the usual "cancel my requests in EndPlay", then the owner is destroyed before the Canceled is delivered
	FTestRig Rig;
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	UObject* Owner = NewOwner();
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/owned")).Owner(Owner), Outcome).Send();
	TestTrue(TEXT("dispatched"), PumpUntil([&Rig] { return Rig.Fake->NumInFlight() == 1; }));
	Handle.Cancel();
	Owner->MarkAsGarbage(); // before any pump
	PumpUntil([&Handle] { return !Handle.IsActive(); });
	PumpUntil([] { return false; }, 0.1);
	TestEqual(TEXT("owner gone wins over the earlier Cancel(): no callbacks"), Outcome->SuccessCalls + Outcome->ErrorCalls + Outcome->CompleteCalls, 0);
	TestEqual(TEXT("... and the request is over"), Rig.Client.GetInFlightCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpOwnerGoneTest, "VaRest.Http.Cancel.OwnerGone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpOwnerGoneTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::CancelTestPrivate;
	FTestRig Rig;
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	UObject* Owner = NewOwner();
	auto InFlight = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/owned")).Owner(Owner), InFlight).Send();
	PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; });
	Owner->MarkAsGarbage(); // TWeakObjectPtr::IsValid is false for garbage (WeakObjectPtr.h:290)
	Rig.Fake->Complete(0, Json(200, R"({"late":1})"));
	PumpUntil([&Handle] { return !Handle.IsActive(); });
	PumpUntil([] { return false; }, 0.1);
	TestEqual(TEXT("spec 3.2: owner gone -> no callbacks"), InFlight->SuccessCalls + InFlight->ErrorCalls + InFlight->CompleteCalls, 0);
	TestFalse(TEXT("... and the request is over"), Handle.IsActive());
	TestEqual(TEXT("... and nothing is left in flight"), Rig.Client.GetInFlightCount(), 0);

	UObject* Gone = NewOwner();
	Gone->MarkAsGarbage();
	auto Never = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle NeverHandle = Observe(Rig.Client.Get(TEXT("/gone")).Owner(Gone), Never).Send();
	TestTrue(TEXT("the request whose owner was gone before entry finishes"), PumpUntil([&NeverHandle] { return !NeverHandle.IsActive(); }));
	TestEqual(TEXT("... and leaves nothing in flight"), Rig.Client.GetInFlightCount(), 0);
	TestEqual(TEXT("an owner gone before entry: nothing sent"), Rig.Fake->NumCalls(), 1);
	TestEqual(TEXT("... and nothing delivered"), Never->CompleteCalls.load(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpOwnerCheckpointsTest, "VaRest.Http.Cancel.OwnerCheckpoints",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpOwnerCheckpointsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::CancelTestPrivate;
	// Each place the owner is checked, on its own: every later check would still silence the request, so each part is
	// built so that only its own check can make the difference

	// On entry: no stage sees a request whose owner is already gone
	FTestRig Entry;
	TSharedRef<FWatchingStage, ESPMode::ThreadSafe> Watcher = MakeShared<FWatchingStage, ESPMode::ThreadSafe>();
	Entry.Client.Use(Watcher, EVaRestStageSlot::Outer);
	UObject* GoneAtEntry = NewOwner();
	GoneAtEntry->MarkAsGarbage();
	auto Entered = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle EnteredHandle = Observe(Entry.Client.Get(TEXT("/entry")).Owner(GoneAtEntry), Entered).Send();
	TestTrue(TEXT("finishes (entry)"), PumpUntil([&EnteredHandle] { return !EnteredHandle.IsActive(); }));
	TestEqual(TEXT("... nothing left in flight (entry)"), Entry.Client.GetInFlightCount(), 0);
	TestEqual(TEXT("an owner gone on entry: no stage runs"), Watcher->Entered, 0);
	TestEqual(TEXT("... and nothing is delivered (entry)"), Entered->CompleteCalls.load(), 0);

	// Before the dispatch: a stage held the exchange while its owner went, then resumed it
	FTestRig Dispatch;
	TSharedRef<FHoldingStage, ESPMode::ThreadSafe> Holder = MakeShared<FHoldingStage, ESPMode::ThreadSafe>();
	Dispatch.Client.Use(Holder, EVaRestStageSlot::Outer);
	UObject* GoneWhileHeld = NewOwner();
	auto Dispatched = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle DispatchedHandle = Observe(Dispatch.Client.Get(TEXT("/dispatch")).Owner(GoneWhileHeld), Dispatched).Send();
	GoneWhileHeld->MarkAsGarbage();
	{
		TUniqueFunction<void()> Resume = MoveTemp(Holder->Resume);
		Resume();
	}
	TestTrue(TEXT("finishes (dispatch)"), PumpUntil([&DispatchedHandle] { return !DispatchedHandle.IsActive(); }));
	TestEqual(TEXT("... nothing left in flight (dispatch)"), Dispatch.Client.GetInFlightCount(), 0);
	TestEqual(TEXT("an owner gone before the dispatch: no transport call"), Dispatch.Fake->NumCalls(), 0);
	TestEqual(TEXT("... and nothing is delivered (dispatch)"), Dispatched->CompleteCalls.load(), 0);

	// When the attempt ends: the outcome travels up as canceled, so a stage (M4's retry) does not try again
	FTestRig Ending;
	TSharedRef<FWatchingStage, ESPMode::ThreadSafe> Retrier = MakeShared<FWatchingStage, ESPMode::ThreadSafe>();
	Ending.Client.Use(Retrier, EVaRestStageSlot::Outer);
	Ending.Fake->Enqueue(FVaRestFakeResponse::Manual());
	UObject* GoneInFlight = NewOwner();
	auto Attempted = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle AttemptedHandle = Observe(Ending.Client.Get(TEXT("/attempt")).Owner(GoneInFlight), Attempted).Send();
	GoneInFlight->MarkAsGarbage();
	Ending.Fake->Complete(0, Json(200, R"({"late":1})"));
	TestTrue(TEXT("finishes (attempt)"), PumpUntil([&AttemptedHandle] { return !AttemptedHandle.IsActive(); }));
	TestEqual(TEXT("... nothing left in flight (attempt)"), Ending.Client.GetInFlightCount(), 0);
	TestTrue(TEXT("an owner gone in flight: the outcome comes back up as canceled"), Retrier->bSawCancel);
	TestEqual(TEXT("... and nothing is delivered (attempt)"), Attempted->CompleteCalls.load(), 0);

	// At completion (review C-2): Cancel() on a stage-held exchange defers its Canceled; the owner goes before it runs
	FTestRig Completion;
	TSharedRef<FHoldingStage, ESPMode::ThreadSafe> Parker = MakeShared<FHoldingStage, ESPMode::ThreadSafe>();
	Completion.Client.Use(Parker, EVaRestStageSlot::Outer);
	UObject* GoneBeforeDelivery = NewOwner();
	auto Completed = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle CompletedHandle = Observe(Completion.Client.Get(TEXT("/complete")).Owner(GoneBeforeDelivery), Completed).Send();
	CompletedHandle.Cancel();
	GoneBeforeDelivery->MarkAsGarbage(); // before any pump
	TestTrue(TEXT("finishes (completion)"), PumpUntil([&CompletedHandle] { return !CompletedHandle.IsActive(); }));
	TestEqual(TEXT("... nothing left in flight (completion)"), Completion.Client.GetInFlightCount(), 0);
	PumpUntil([] { return false; }, 0.05);
	TestEqual(TEXT("an owner gone before its Canceled is delivered: nothing is delivered"), Completed->CompleteCalls.load(), 0);
	{
		TUniqueFunction<void()> Resume = MoveTemp(Parker->Resume); // the stage lets go; nothing is sent
		Resume();
	}
	TestEqual(TEXT("... and nothing is sent (completion)"), Completion.Fake->NumCalls(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpCancelAllTest, "VaRest.Http.Cancel.AllAndShutdown",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpCancelAllTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::CancelTestPrivate;
	FTestRig Rig;
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	auto A1 = MakeOutcome<FVaRestJson>();
	auto A2 = MakeOutcome<FVaRestJson>();
	auto B = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a1")).Tag(TEXT("A")), A1).Send();
	Observe(Rig.Client.Get(TEXT("/a2")).Tag(TEXT("A")), A2).Send();
	Observe(Rig.Client.Get(TEXT("/b")).Tag(TEXT("B")), B).Send();
	PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 3; });
	Rig.Client.CancelAll(TEXT("A"));
	TestTrue(TEXT("the tagged ones complete"), PumpUntil([&] { return A1->IsDone() && A2->IsDone(); }));
	TestTrue(TEXT("as Canceled"), A1->Error.Kind == EVaRestErrorKind::Canceled && A2->Error.Kind == EVaRestErrorKind::Canceled);
	TestFalse(TEXT("the other tag is untouched"), B->IsDone());
	TestEqual(TEXT("... still in flight"), Rig.Client.GetInFlightCount(), 1);

	// Engine exit: every live client's requests stop, and no callback runs into a world that is going away.
	// CancelForShutdown() is the process-wide one the OnEnginePreExit hook calls: run inside a working editor, it cancels
	// every live VaRest client's in-flight requests, not only this rig's, and none of them ever gets a callback; until the
	// reset below, every Send() anywhere completes silently without being sent. Run this test where no other VaRest
	// traffic matters (a headless test run), never with a live session using VaRest (task 11 review, minor 6).
	VaRest::Http::FClientState::CancelForShutdown();
	// Review N-6: the editor goes on, so every later test must send again -- also when this test returns early
	ON_SCOPE_EXIT { VaRest::Http::FClientState::ResetShutdownInternal(); };
	TestTrue(TEXT("shutdown finishes every request"), PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; }));
	PumpUntil([] { return false; }, 0.05);
	TestEqual(TEXT("shutdown cancels the transport call"), Rig.Fake->NumCanceled(), 3);
	TestEqual(TEXT("... and delivers nothing"), B->CompleteCalls.load(), 0);

	// Review M-14: after OnEnginePreExit the HTTP module is about to unload; a late Send() never reaches it
	auto Late = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle LateHandle = Observe(Rig.Client.Get(TEXT("/late")), Late).Send();
	TestTrue(TEXT("a request sent during shutdown finishes"), PumpUntil([&LateHandle] { return !LateHandle.IsActive(); }));
	TestEqual(TEXT("... and leaves nothing in flight"), Rig.Client.GetInFlightCount(), 0);
	PumpUntil([] { return false; }, 0.05);
	TestEqual(TEXT("a request sent during shutdown is not sent"), Rig.Fake->NumCalls(), 3);
	TestEqual(TEXT("... and delivers nothing"), Late->CompleteCalls.load(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpCancelFromCallbackTest, "VaRest.Http.Cancel.FromCallback",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpCancelFromCallbackTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::CancelTestPrivate;
	FTestRig Rig;
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request)
	{
		return Request.Url.EndsWith(TEXT("/c")) ? FVaRestFakeResponse::Manual() : Json(200, "{}");
	});
	auto C = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle CHandle = Observe(Rig.Client.Get(TEXT("/c")), C).Send();
	auto B = MakeOutcome<FVaRestJson>();
	TSharedRef<bool, ESPMode::ThreadSafe> bADone = MakeShared<bool, ESPMode::ThreadSafe>(false);
	FVaRestClient Client = Rig.Client;
	Rig.Client.Get(TEXT("/a")).OnSuccess([Client, B, CHandle, bADone](const FVaRestJson&)
	{
		// From inside a callback: send another request and cancel a third one (Review Focus)
		Observe(Client.Get(TEXT("/b")), B).Send();
		CHandle.Cancel();
		*bADone = true;
	}).Send();
	TestTrue(TEXT("all three finish"), PumpUntil([&] { return *bADone && B->IsDone() && C->IsDone(); }));
	TestTrue(TEXT("the request sent from the callback succeeds"), B->SuccessCalls == 1);
	TestTrue(TEXT("the one canceled from the callback is Canceled"), C->Error.Kind == EVaRestErrorKind::Canceled);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpCancelSaveToTest, "VaRest.Http.Cancel.SaveTo",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpCancelSaveToTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::CancelTestPrivate;
	IFileManager::Get().DeleteDirectory(*DownloadDir(), false, true);
	const FString VoidFile = DownloadDir() / TEXT("void.bin");
	const FString ResponseFile = DownloadDir() / TEXT("response.bin");
	const FString SlowFile = DownloadDir() / TEXT("slow.bin");
	const FString LateFile = DownloadDir() / TEXT("late.bin");
	for (const FString& Destination : { VoidFile, ResponseFile, SlowFile, LateFile })
	{
		FFileHelper::SaveStringToFile(TEXT("old"), *Destination);
	}

	// Task 10 carry: two answers are already in their temp files and queued for the game thread when Cancel() runs, so
	// RunDecode reads the flag set and never renames; a third is still in its transport call
	FTestRig Rig;
	Rig.Fake->bCompleteOnWorker = false;
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	auto Void = MakeOutcome<void>();
	auto Response = MakeOutcome<FVaRestResponse>();
	auto Slow = MakeOutcome<void>();
	const FVaRestRequestHandle VoidHandle = Observe(Rig.Client.Get<void>(TEXT("/void")).SaveTo(VoidFile), Void).Send();
	const FVaRestRequestHandle ResponseHandle = Observe(Rig.Client.Get<FVaRestResponse>(TEXT("/response")).SaveTo(ResponseFile), Response).Send();
	const FVaRestRequestHandle SlowHandle = Observe(Rig.Client.Get<void>(TEXT("/slow")).SaveTo(SlowFile), Slow).Send();
	Rig.Fake->Complete(0, FVaRestFakeResponse::Bytes(200, { 1, 2, 3 }));
	Rig.Fake->Complete(1, FVaRestFakeResponse::Bytes(200, { 4, 5, 6 }));
	TestEqual(TEXT("the two answers are in their temp files"), CountPartFiles(), 2);
	VoidHandle.Cancel();
	ResponseHandle.Cancel();
	SlowHandle.Cancel();
	TestTrue(TEXT("all three complete"), PumpUntil([&] { return Void->IsDone() && Response->IsDone() && Slow->IsDone(); }));
	TestTrue(TEXT("a canceled void SaveTo delivers Canceled, never an OnSuccess without a file"),
		Void->SuccessCalls == 0 && Void->ErrorCalls == 1 && Void->Error.Kind == EVaRestErrorKind::Canceled);
	TestTrue(TEXT("... so does an FVaRestResponse one, naming no saved file"), Response->SuccessCalls == 0 && Response->ErrorCalls == 1
		&& Response->Error.Kind == EVaRestErrorKind::Canceled && Response->Response.SavedFile.IsEmpty());
	TestTrue(TEXT("... and one canceled in its transport call"), Slow->SuccessCalls == 0 && Slow->Error.Kind == EVaRestErrorKind::Canceled);
	TestEqual(TEXT("only the call still running was canceled at the transport"), Rig.Fake->NumCanceled(), 1);
	TestTrue(TEXT("no destination was touched"), HoldsOld(VoidFile) && HoldsOld(ResponseFile) && HoldsOld(SlowFile));
	TestTrue(TEXT("... and no temp file is left"), PumpUntil([] { return CountPartFiles() == 0; }, 2.0));

	// Review M-4: a cancel that lands after the rename delivers the success, the truthful outcome. The stage holds the
	// downloaded outcome; forwarding it starts the decode task, and no game-thread task runs until Cancel() has.
	FTestRig Renaming;
	TSharedRef<FOutcomeHoldingStage, ESPMode::ThreadSafe> Stage = MakeShared<FOutcomeHoldingStage, ESPMode::ThreadSafe>();
	Renaming.Client.Use(Stage, EVaRestStageSlot::Outer);
	Renaming.Fake->Enqueue(FVaRestFakeResponse::Bytes(200, { 7, 8, 9 }));
	auto Renamed = MakeOutcome<void>();
	const FVaRestRequestHandle RenamedHandle = Observe(Renaming.Client.Get<void>(TEXT("/late")).SaveTo(LateFile), Renamed).Send();
	TestTrue(TEXT("the stage holds the downloaded outcome"), PumpUntil([&Stage] { return Stage->bHolding; }));
	{
		TUniqueFunction<void()> Run = MoveTemp(Stage->Forward);
		Run();
	}
	TestTrue(TEXT("the rename replaced the destination"), SpinUntilTrue([&LateFile] { return ReadBytes(LateFile) == TArray<uint8>({ 7, 8, 9 }); }));
	RenamedHandle.Cancel();
	TestTrue(TEXT("completes"), Renaming.Wait(Renamed));
	TestTrue(TEXT("review M-4: a cancel after the rename delivers the success with the saved file"), Renamed->SuccessCalls == 1
		&& Renamed->ErrorCalls == 0 && Renamed->Response.SavedFile.Equals(LateFile, ESearchCase::CaseSensitive));

	// FinalizeSaveTo's own read of the flag, right before the rename (review M-4): a cancel that lands while the decode task
	// runs. Called directly, as the moment between RunDecode's first read and the rename cannot be reached deterministically.
	VaRest::Http::FFinish Direct;
	Direct.SaveTo = DownloadDir() / TEXT("direct.bin");
	FFileHelper::SaveStringToFile(TEXT("old"), *Direct.SaveTo);
	Direct.TempFile = VaRest::Http::MakeTempFileName(Direct.SaveTo, 1);
	FFileHelper::SaveStringToFile(TEXT("new"), *Direct.TempFile);
	Direct.bBodyInFile = true;
	Direct.Response.BytesDown = 3;
	const bool bMoved = VaRest::Http::FinalizeSaveTo(Direct, [] { return true; });
	TestTrue(TEXT("a cancel seen before the rename: no move, no error (Complete makes it Canceled), no saved file"),
		!bMoved && !Direct.Error.IsError() && Direct.Response.SavedFile.IsEmpty());
	TestTrue(TEXT("... the destination untouched"), HoldsOld(Direct.SaveTo));
	TestEqual(TEXT("... and its temp file deleted"), CountPartFiles(), 0);
	return true;
}

#endif
