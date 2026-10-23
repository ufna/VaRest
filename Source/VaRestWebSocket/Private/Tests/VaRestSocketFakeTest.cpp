// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Tasks/Task.h"
#include "VaRestFakeSocketTransport.h"
#include "VaRestHttpTesting.h"

#include <atomic>

namespace VaRest::WebSocket::VaRestSocketFakeTestPrivate
{
	struct FNotified
	{
		int32 Count = 0;
	};

	FVaRestSocketConnectRequest Request(const TCHAR* Url)
	{
		FVaRestSocketConnectRequest Out;
		Out.Url = Url;
		return Out;
	}

	FString Text(const FVaRestSocketEvent& Event)
	{
		return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Event.Bytes.GetData()), Event.Bytes.Num());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketFakeScriptTest, "VaRest.WebSocket.Fake.Script",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketFakeScriptTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::VaRestSocketFakeTestPrivate;
	FVaRestFakeSocketTransport Fake;
	const TSharedRef<FNotified> Notified = MakeShared<FNotified>();
	const TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Connection = Fake.Connect(Request(TEXT("ws://fake.test/ws")), [Notified] { ++Notified->Count; });
	if (!TestEqual(TEXT("one attempt"), Fake.NumAttempts(), 1))
	{
		return false;
	}
	TestEqualSensitive(TEXT("the request is kept"), Fake.GetLast().GetRequest().Url, FString(TEXT("ws://fake.test/ws")));
	Fake.GetLast().Open(TEXT("chat.v2"), 0.25).Text(TEXT("hello")).ServerClose(4001, TEXT("bye"));
	TestEqual(TEXT("each script call notified at once"), Notified->Count, 3);
	TestEqual(TEXT("the message counts as pending inbound bytes (spec 2.3)"), Connection->GetPendingInboundBytes(), static_cast<int64>(5));
	TArray<FVaRestSocketEvent> Events;
	TestEqual(TEXT("three events popped"), Connection->PopEvents(Events), 3);
	if (!TestEqual(TEXT("three events"), Events.Num(), 3))
	{
		return false;
	}
	TestTrue(TEXT("Open first, with the protocol"), Events[0].Type == EVaRestSocketEventType::Open && Events[0].Protocol.Equals(TEXT("chat.v2"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("then the message"), Events[1].Type == EVaRestSocketEventType::Message && Text(Events[1]).Equals(TEXT("hello"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("... with a slack byte for ParseOwned"), Events[1].Bytes.Max() > Events[1].Bytes.Num());
	TestTrue(TEXT("then Closed by the server, clean"), Events[2].Type == EVaRestSocketEventType::Closed && Events[2].End.By == EVaRestSocketCloseBy::Server
		&& Events[2].End.Code == 4001 && Events[2].End.bWasClean);
	TestTrue(TEXT("the connection has ended"), Connection->HasEnded());
	Connection->Consumed(5);
	TestEqual(TEXT("Consumed lowers the pending bytes"), Connection->GetPendingInboundBytes(), static_cast<int64>(0));
	TestEqual(TEXT("... and the attempt saw it"), Fake.GetLast().GetConsumed(), static_cast<int64>(5));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketFakeConsumerCallTest, "VaRest.WebSocket.Fake.NeverInsideTheCall",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketFakeConsumerCallTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::VaRestSocketFakeTestPrivate;
	FVaRestFakeSocketTransport Fake;
	const TSharedRef<FNotified> Notified = MakeShared<FNotified>();
	// E7: what Connect (through OnConnect) and Close cause is queued, and notified only from a later game-thread task
	Fake.OnConnect = [](FVaRestFakeSocketAttempt& Attempt) { Attempt.Fail(EVaRestSocketFailure::UpgradeRefused, 403, TEXT("scripted")); };
	const TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Refused = Fake.Connect(Request(TEXT("ws://fake.test/a")), [Notified] { ++Notified->Count; });
	TestEqual(TEXT("not notified inside Connect"), Notified->Count, 0);
	TestTrue(TEXT("notified from a game-thread task"), VaRest::Http::Testing::PumpUntil([Notified] { return Notified->Count == 1; }));
	TArray<FVaRestSocketEvent> Events;
	Refused->PopEvents(Events);
	TestTrue(TEXT("an Error before the open: UpgradeRefused 403, code 0"), Events.Num() == 1 && Events[0].Type == EVaRestSocketEventType::Error
		&& Events[0].End.Failure == EVaRestSocketFailure::UpgradeRefused && Events[0].End.HttpStatus == 403 && Events[0].End.Code == 0);

	Fake.OnConnect = nullptr;
	const TSharedRef<FNotified> Closed = MakeShared<FNotified>();
	const TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Pending = Fake.Connect(Request(TEXT("ws://fake.test/b")), [Closed] { ++Closed->Count; });
	Pending->Close(1000, TEXT("never mind"));
	TestEqual(TEXT("a close before the open notifies later"), Closed->Count, 0);
	TestTrue(TEXT("... from a task"), VaRest::Http::Testing::PumpUntil([Closed] { return Closed->Count == 1; }));
	Events.Reset();
	Pending->PopEvents(Events);
	TestTrue(TEXT("a close before the open is Error Canceled, by the client (plan decision 5)"), Events.Num() == 1 && Events[0].Type == EVaRestSocketEventType::Error
		&& Events[0].End.Failure == EVaRestSocketFailure::Canceled && Events[0].End.By == EVaRestSocketCloseBy::Client);

	const TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Open = Fake.Connect(Request(TEXT("ws://fake.test/c")));
	Fake.GetLast().Open();
	// Review I-2: the Warning IVaRestSocketConnection::Close promises, as the real handle logs it, naming the third attempt
	AddExpectedMessagePlain(TEXT("fake#2: close code 1006 cannot be sent"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	Open->Close(1006, TEXT("bad code"));
	TestTrue(TEXT("the close is recorded as it would be sent: 1006 becomes 1000"), Fake.GetLast().WasCloseRequested() && Fake.GetLast().GetCloseCode() == 1000);
	VaRest::Http::Testing::PumpUntil([Open] { return Open->HasEnded(); });
	Events.Reset();
	Open->PopEvents(Events);
	TestTrue(TEXT("an open attempt's close is echoed: Closed by the client, clean, 1000"), Events.Num() == 2 && Events[1].Type == EVaRestSocketEventType::Closed
		&& Events[1].End.By == EVaRestSocketCloseBy::Client && Events[1].End.Code == 1000 && Events[1].End.bWasClean);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketFakeBufferedTest, "VaRest.WebSocket.Fake.Buffered",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketFakeBufferedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::VaRestSocketFakeTestPrivate;
	FVaRestFakeSocketTransport Fake;
	FVaRestSocketConnectRequest Req = Request(TEXT("ws://fake.test/d"));
	Req.SendHighWater = 8;
	const TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Connection = Fake.Connect(MoveTemp(Req));
	Fake.GetLast().Open();
	TestTrue(TEXT("a send is accepted"), Connection->Send(FVaRestSocketFrame::Text(FUtf8StringView(UTF8TEXT("0123456789")))));
	TestEqual(TEXT("... and buffered"), Connection->GetBufferedAmount(), static_cast<int64>(10));
	TestEqual(TEXT("... and recorded"), Fake.GetLast().GetSent().Num(), 1);
	Fake.GetLast().Flush(4);
	TestEqual(TEXT("a partial flush leaves the rest"), Connection->GetBufferedAmount(), static_cast<int64>(6));
	Fake.GetLast().Flush();
	TestEqual(TEXT("the full flush empties it"), Connection->GetBufferedAmount(), static_cast<int64>(0));
	TArray<FVaRestSocketEvent> Events;
	Connection->PopEvents(Events);
	TestTrue(TEXT("Drained once, after the high-water mark was reached (spec 2.4)"), Events.Num() == 2 && Events[1].Type == EVaRestSocketEventType::Drained);
	TestFalse(TEXT("an invalid frame is refused"), Connection->Send(FVaRestSocketFrame()));
	Fake.GetLast().Drop();
	TestFalse(TEXT("a send after the end is refused"), Connection->Send(FVaRestSocketFrame::Text(FUtf8StringView(UTF8TEXT("late")))));
	Events.Reset();
	Connection->PopEvents(Events);
	TestTrue(TEXT("the drop is its end: Closed 1006"), Events.Num() == 1 && Events[0].Type == EVaRestSocketEventType::Closed && Events[0].End.Code == 1006);
	// Review conc M-6: after Close, before the end, a send is refused at the call
	const TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Closing = Fake.Connect(Request(TEXT("ws://fake.test/c")));
	Fake.GetLast().bEchoClose = false;
	Fake.GetLast().Open();
	Closing->Close(1000, TEXT("bye"));
	TestFalse(TEXT("a send after Close is refused"), Closing->Send(FVaRestSocketFrame::Text(FUtf8StringView(UTF8TEXT("after close")))));
	TestEqual(TEXT("... and not recorded"), Fake.GetLast().GetSent().Num(), 0);
	Fake.GetLast().ServerClose(1000);
	Events.Reset();
	Closing->PopEvents(Events);
	TestTrue(TEXT("the server's close is its end"), Events.Num() == 2 && Events[1].Type == EVaRestSocketEventType::Closed && Events[1].End.By == EVaRestSocketCloseBy::Server);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketFakeBeforeOpenTest, "VaRest.WebSocket.Fake.BeforeOpen",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketFakeBeforeOpenTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::VaRestSocketFakeTestPrivate;
	FVaRestFakeSocketTransport Fake;
	FVaRestSocketConnectRequest Req = Request(TEXT("ws://fake.test/early"));
	Req.SendHighWater = 4;
	const TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Connection = Fake.Connect(MoveTemp(Req));
	// Task 3 residual: a send before the open may reach the high-water mark, but nothing comes ahead of the Open -- the real
	// transport writes such a send only after it (plan decision 18), so a flush of a pending attempt takes nothing
	TestTrue(TEXT("a send before the open is accepted"), Connection->Send(FVaRestSocketFrame::Text(FUtf8StringView(UTF8TEXT("early")))));
	Fake.GetLast().Flush();
	TestEqual(TEXT("a flush before the open takes nothing"), Connection->GetBufferedAmount(), static_cast<int64>(5));
	Fake.GetLast().Open();
	Fake.GetLast().Flush();
	TestEqual(TEXT("the flush after the open empties it"), Connection->GetBufferedAmount(), static_cast<int64>(0));
	TArray<FVaRestSocketEvent> Events;
	Connection->PopEvents(Events);
	TestTrue(TEXT("Open first, then the Drained (spec 2.4)"), Events.Num() == 2 && Events[0].Type == EVaRestSocketEventType::Open
		&& Events[1].Type == EVaRestSocketEventType::Drained);
	// Task 8 review M-2: the attempt ends before the test returns
	Fake.GetLast().ServerClose(1000);
	Events.Reset();
	Connection->PopEvents(Events);
	TestTrue(TEXT("the server's close ends it: Closed 1000 by the server"), Events.Num() == 1 && Events[0].Type == EVaRestSocketEventType::Closed
		&& Events[0].End.Code == 1000 && Events[0].End.By == EVaRestSocketCloseBy::Server);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketFakeTerminalTest, "VaRest.WebSocket.Fake.TerminalOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketFakeTerminalTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::VaRestSocketFakeTestPrivate;
	FVaRestFakeSocketTransport Fake;
	TSharedPtr<IVaRestSocketConnection, ESPMode::ThreadSafe> Connection = Fake.Connect(Request(TEXT("ws://fake.test/e")));
	Fake.GetLast().Open().Drop();
	// Plan decision 2: nothing after the terminal event
	// The line names the attempt (review M-1): the request has no LogName, so the fake's own "fake#<index>"
	AddExpectedErrorPlain(TEXT("fake#0: an event (Message) after the connection's terminal event was dropped"), EAutomationExpectedErrorFlags::Contains, 1);
	Fake.GetLast().Text(TEXT("after the end"));
	TArray<FVaRestSocketEvent> Events;
	Connection->PopEvents(Events);
	TestTrue(TEXT("Open and Closed only"), Events.Num() == 2 && Events[1].Type == EVaRestSocketEventType::Closed && Events[1].End.Code == 1006);
	FVaRestFakeSocketAttempt& Attempt = Fake.GetLast();
	Connection.Reset();
	TestTrue(TEXT("dropping the handle releases the attempt"), Attempt.WasReleased());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketFakeCloseRulesTest, "VaRest.WebSocket.Fake.CloseRules",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketFakeCloseRulesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::VaRestSocketFakeTestPrivate;
	FVaRestFakeSocketTransport Fake;
	// Spec 3.5 and review I-2: a code that may not be sent becomes 1000 and a long reason is cut, each with the Warning the
	// real handle logs (VaRest::WebSocket::MakeOutboundClose), named by the request's LogName as the real transport names it
	FVaRestSocketConnectRequest Req = Request(TEXT("ws://fake.test/rules"));
	Req.LogName = TEXT("chat#3");
	const TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Connection = Fake.Connect(MoveTemp(Req));
	Fake.GetLast().Open();
	AddExpectedMessagePlain(TEXT("chat#3: close code 1005 cannot be sent"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("chat#3: the close reason is cut to 123 bytes"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	Connection->Close(1005, FString::ChrN(200, TEXT('r')));
	TestEqual(TEXT("1005 is sent as 1000"), Fake.GetLast().GetCloseCode(), 1000);
	TestEqualSensitive(TEXT("... with the reason cut to 123 bytes"), Fake.GetLast().GetCloseReason(), FString::ChrN(VaRest::WebSocket::MaxCloseReasonBytes, TEXT('r')));
	TArray<FVaRestSocketEvent> Events;
	Connection->PopEvents(Events);
	TestTrue(TEXT("the echo carries what was sent"), Events.Num() == 2 && Events[1].Type == EVaRestSocketEventType::Closed && Events[1].End.Code == 1000
		&& Events[1].End.Reason.Len() == VaRest::WebSocket::MaxCloseReasonBytes);
	// A close that needs no change logs nothing (a Warning here would be unexpected, so an error)
	const TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Quiet = Fake.Connect(Request(TEXT("ws://fake.test/quiet")));
	Fake.GetLast().Open();
	Quiet->Close(4001, TEXT("ok"));
	Events.Reset();
	Quiet->PopEvents(Events);
	TestTrue(TEXT("4001 \"ok\" is sent as given"), Events.Num() == 2 && Events[1].End.Code == 4001 && Events[1].End.Reason.Equals(TEXT("ok"), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketFakeReleaseTest, "VaRest.WebSocket.Fake.Release",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketFakeReleaseTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::VaRestSocketFakeTestPrivate;
	FVaRestFakeSocketTransport Fake;
	const TSharedRef<FNotified> Notified = MakeShared<FNotified>();
	// Plan decision 5, spec 1.4: an open attempt whose last handle goes is closed with 1001 "released"
	TSharedPtr<IVaRestSocketConnection, ESPMode::ThreadSafe> Opened = Fake.Connect(Request(TEXT("ws://fake.test/r1")), [Notified] { ++Notified->Count; });
	FVaRestFakeSocketAttempt& First = Fake.GetLast();
	First.Open().Text(TEXT("never popped"));
	TestEqual(TEXT("two script calls, two notifications"), Notified->Count, 2);
	Opened.Reset();
	TestTrue(TEXT("dropping the last handle releases the attempt"), First.WasReleased());
	TestTrue(TEXT("an open attempt's release is a close: 1001 \"released\""), First.WasCloseRequested() && First.GetCloseCode() == 1001
		&& First.GetCloseReason().Equals(TEXT("released"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("... answered at once (bEchoClose), so the attempt has ended"), First.HasEnded() && !First.IsOpen());
	TestEqual(TEXT("... and its end was not notified: nobody is left to pop it"), Notified->Count, 2);

	// Events after the release are discarded and never notified; the test holds the end back (no echo)
	TSharedPtr<IVaRestSocketConnection, ESPMode::ThreadSafe> Held = Fake.Connect(Request(TEXT("ws://fake.test/r2")), [Notified] { ++Notified->Count; });
	FVaRestFakeSocketAttempt& Second = Fake.GetLast();
	Second.bEchoClose = false;
	Second.Open();
	TestEqual(TEXT("the second attempt's open notified"), Notified->Count, 3);
	Held.Reset();
	TestTrue(TEXT("a release without the echo asks for the close"), Second.WasCloseRequested() && Second.GetCloseCode() == 1001);
	TestTrue(TEXT("... and the attempt stays open until the server answers"), Second.IsOpen());
	Second.Text(TEXT("discarded")).ServerClose(1000);
	TestEqual(TEXT("events after the release are discarded, never notified"), Notified->Count, 3);
	TestTrue(TEXT("... the server's end still ends the attempt"), Second.HasEnded());

	// A pending attempt is cancelled
	TSharedPtr<IVaRestSocketConnection, ESPMode::ThreadSafe> Waiting = Fake.Connect(Request(TEXT("ws://fake.test/r3")));
	FVaRestFakeSocketAttempt& Third = Fake.GetLast();
	Waiting.Reset();
	TestTrue(TEXT("a pending attempt's release cancels it"), Third.WasReleased() && Third.HasEnded() && !Third.IsOpen());

	// Released on another thread: the flag at once, the close from a game-thread task (the attempt's script state is the game thread's)
	TSharedPtr<IVaRestSocketConnection, ESPMode::ThreadSafe> Elsewhere = Fake.Connect(Request(TEXT("ws://fake.test/r4")));
	FVaRestFakeSocketAttempt& Fourth = Fake.GetLast();
	Fourth.Open();
	const TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Where = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(-1);
	const UE::Tasks::FTask Dropping = UE::Tasks::Launch(TEXT("VaRest.Test.ReleaseFake"), [Last = MoveTemp(Elsewhere), Where]() mutable
	{
		Where->store(IsInGameThread() ? 1 : 0);
		Last.Reset();
	});
	// Not Wait(): it may run the task here, on the game thread (Task.h:56-57, :67-68)
	const double Deadline = FPlatformTime::Seconds() + 5.0;
	while (!Dropping.IsCompleted() && FPlatformTime::Seconds() < Deadline)
	{
		FPlatformProcess::YieldThread();
	}
	if (!TestTrue(TEXT("the worker dropped the last handle"), Dropping.IsCompleted()))
	{
		return false;
	}
	TestEqual(TEXT("... off the game thread"), Where->load(), 0);
	TestTrue(TEXT("released at once"), Fourth.WasReleased());
	TestFalse(TEXT("... the close not yet: it waits for the game thread's tasks"), Fourth.WasCloseRequested());
	TestTrue(TEXT("... then 1001, from a game-thread task"), VaRest::Http::Testing::PumpUntil([Attempt = &Fourth] { return Attempt->HasEnded(); }));
	TestTrue(TEXT("... recorded as sent"), Fourth.WasCloseRequested() && Fourth.GetCloseCode() == 1001);
	// The first release's echo was pushed inside the consumer's call, so its notification was posted; the tasks have run now
	TestEqual(TEXT("no notification ever reached a released handle"), Notified->Count, 3);
	return true;
}

#endif
