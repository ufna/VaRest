// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "VaRestLwsService.h"
#include "VaRestSocketInbound.h"
#include "VaRestSocketLink.h"
#include <atomic>

namespace VaRestSocketPressureTest
{
using namespace VaRest::WebSocket;

FVaRestSocketEvent Event(EVaRestSocketEventType Type)
{
	FVaRestSocketEvent Out; Out.Type = Type; return Out;
}
FVaRestSocketEvent Binary(TArray<uint8> Bytes = {})
{
	auto Out = Event(EVaRestSocketEventType::Message);
	Out.Kind = EVaRestSocketFrameKind::Binary; Out.Bytes = MoveTemp(Bytes); return Out;
}

/** Own native seam around the real queue. No context, network, affine user callback or fake timer is driven. */
class FConnection final : public IVaRestSocketConnection
{
public:
	explicit FConnection(TFunction<void()> OnEvents)
		: Link(MakeShared<FSocketLink, ESPMode::ThreadSafe>(MoveTemp(OnEvents), TEXT("pressure-test"))) {}
	~FConnection() override { Link->bReleased.store(true, std::memory_order_release); }
	int32 PopEvents(TArray<FVaRestSocketEvent>& Out) override
	{
		if (IsInGameThread()) WrongThread.store(true, std::memory_order_release);
		PopCalls.fetch_add(1, std::memory_order_relaxed); return Link->PopAll(Out);
	}
	bool Send(FVaRestSocketFrame&&) override { return false; }
	void Close(int32, FStringView) override {}
	void Consumed(int64 Bytes) override
	{
		ConsumedBytes.fetch_add(Bytes, std::memory_order_relaxed);
		Link->PendingInboundBytes.fetch_sub(Bytes, std::memory_order_relaxed);
	}
	void ConsumedMessages(int64 Bytes, int32 Messages) override
	{
		Consumed(Bytes); ConsumedFrames.fetch_add(Messages, std::memory_order_relaxed);
		Link->PendingInboundMessages.fetch_sub(Messages, std::memory_order_relaxed);
	}
	int64 GetBufferedAmount() const override { return 0; }
	int64 GetPendingInboundBytes() const override { return Link->PendingInboundBytes.load(std::memory_order_acquire); }
	bool HasEnded() const override { return Link->bEnded.load(std::memory_order_acquire); }
	void Open() { Link->Push(Event(EVaRestSocketEventType::Open)); }
	void Message(TArray<uint8> Bytes = {}) { Link->Push(Binary(MoveTemp(Bytes))); }
	TSharedRef<FSocketLink, ESPMode::ThreadSafe> Link;
	std::atomic<int32> PopCalls{0}, ConsumedFrames{0};
	std::atomic<int64> ConsumedBytes{0};
	std::atomic<bool> WrongThread{false};
};

struct FFixture
{
	TSharedRef<FSocketInbound, ESPMode::ThreadSafe> Inbound = MakeShared<FSocketInbound, ESPMode::ThreadSafe>(771);
	TSharedPtr<FConnection, ESPMode::ThreadSafe> Connection;
	FFixture()
	{
		const TWeakPtr<FSocketInbound, ESPMode::ThreadSafe> Weak = Inbound;
		Connection = MakeShared<FConnection, ESPMode::ThreadSafe>([Weak] { if (auto Native = Weak.Pin()) Native->SchedulePump(); });
	}
	~FFixture() { Inbound->StopAndWaitOnGT(); Connection.Reset(); }
	void Attach() { Inbound->AttachOnGT(Connection.ToSharedRef()); Quiesce(); }
	// Only this owned worker pipe; GTK delivery remains deliberately held, with no task-graph Drain or parser help on GTK.
	void Quiesce() { Inbound->WaitForPipeForTests(); }
};

class FPairDecoder final : public IVaRestSocketDecoder
{
	int32 Frames = 0;
	TArray<uint8> Pending;
public:
	FVaRestSocketDecodeResult Decode(FVaRestSocketInboundFrame Frame, const FVaRestSocketCodecContext&) override
	{
		check(!IsInGameThread()); Pending.Append(Frame.Bytes);
		if (++Frames != 2) return {};
		Frames = 0;
		FVaRestSocketDecodeResult Out; Out.Status = EVaRestSocketDecode::Message;
		Out.Message = FVaRestSocketMessage::FromDecoded(Frame.Kind, MoveTemp(Pending), {}, {}); return Out;
	}
};
class FPairCodec final : public IVaRestSocketCodec
{
	TSharedRef<IVaRestSocketCodec, ESPMode::ThreadSafe> Base = MakeJsonCodec();
public:
	FName GetProtocolTag() const override { return FName(TEXT("PressurePairTest")); }
	TUniquePtr<IVaRestSocketDecoder> MakeDecoder() const override { return MakeUnique<FPairDecoder>(); }
	TValueOrError<TArray<FVaRestSocketFrame>, FVaRestError> Encode(FStringView Channel, FStringView Route,
		const FVaRestSocketPayloadWriter& Payload, const FVaRestSocketId& Id, const FVaRestSocketCodecContext& Context) const override
	{ return Base->Encode(Channel, Route, Payload, Id, Context); }
	TValueOrError<TArray<FVaRestSocketFrame>, FVaRestError> EncodeReply(FStringView Channel, const FVaRestSocketId& Id,
		const FVaRestSocketPayloadWriter& Payload, bool bError, const FVaRestSocketCodecContext& Context) const override
	{ return Base->EncodeReply(Channel, Id, Payload, bError, Context); }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketPressureLinkTest, "VaRest.WebSocket.Pressure.Link",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestSocketPressureLinkTest::RunTest(const FString& Parameters)
{
	using namespace VaRestSocketPressureTest;
	using namespace VaRest::WebSocket::Lws;
	FQueuedEventWatermarks Default, Small{4, 2};
	TestTrue(TEXT("default private slot policy"), Default.IsValid() && Default.High == 4096 && Default.Low == 2048);
	TestFalse(TEXT("high boundary is not over high"), Default.IsHigh(4096));
	TestTrue(TEXT("one above high pauses"), Default.IsHigh(4097));
	TestTrue(TEXT("inclusive low boundary resumes"), Default.IsLow(2048));
	TestFalse(TEXT("above low stays pressured"), Default.IsLow(2049));
	TestTrue(TEXT("owned smaller override"), Small.IsValid());
	TestFalse(TEXT("invalid private thresholds refuse"), FQueuedEventWatermarks{0, 0}.IsValid());
	TestFalse(TEXT("low must be below high"), FQueuedEventWatermarks{2, 2}.IsValid());
	FSocketLink Link(nullptr, TEXT("entry-snapshot"));
	Link.Push(Event(EVaRestSocketEventType::Open)); Link.Push(Binary());
	struct FRefill { FSocketLink* Link = nullptr; int32 Slots = -1; };
	const auto Refill = MakeShared<FRefill>(); Refill->Link = &Link;
	Link.SetPopSnapshotHookForTests([Refill]
	{
		Refill->Link->Push(Binary({1, 2, 3, 4, 5}), false);
		Refill->Link->Push(Event(EVaRestSocketEventType::Pong), false);
		Refill->Link->Push(Event(EVaRestSocketEventType::Drained), false);
		Refill->Link->Push(Event(EVaRestSocketEventType::Closed), false);
		Refill->Slots = Refill->Link->GetQueuedEventCount();
	});
	TArray<FVaRestSocketEvent> Out; auto Sentinel = Event(EVaRestSocketEventType::Pong); Sentinel.Seconds = 99; Out.Add(MoveTemp(Sentinel));
	TestEqual(TEXT("actual pop excludes post-snapshot refill"), Link.PopAll(Out), 2);
	Link.SetPopSnapshotHookForTests({}); // release the synchronous interleaving before another pull
	TestEqual(TEXT("all six event kinds/empty messages count, not only bytes"), Refill->Slots, 6);
	TestTrue(TEXT("metadata alone crossed the small threshold"), Small.IsHigh(Refill->Slots));
	TestEqual(TEXT("only dequeued slots retired"), Link.GetQueuedEventCount(), 4);
	TestEqual(TEXT("append keeps existing entries"), Out.Num(), 3);
	TestEqual(TEXT("existing entry unchanged"), Out[0].Seconds, 99.0);
	TestTrue(TEXT("first snapshot FIFO"), Out[1].Type == EVaRestSocketEventType::Open && Out[2].Type == EVaRestSocketEventType::Message && Out[2].Bytes.IsEmpty());
	TestTrue(TEXT("terminal published after the first entry snapshot"), Link.bEnded.load(std::memory_order_acquire));
	TestEqual(TEXT("next pull reaches refill and terminal"), Link.PopAll(Out), 4);
	TestTrue(TEXT("refill FIFO and terminal preserved"), Out.Num() == 7 && Out[3].Bytes.Num() == 5
		&& Out[4].Type == EVaRestSocketEventType::Pong && Out[5].Type == EVaRestSocketEventType::Drained && Out[6].IsTerminal());
	TestEqual(TEXT("all queue slots released without Consumed0"), Link.GetQueuedEventCount(), 0);
	TestTrue(TEXT("dequeue alone clears slot pressure"), Small.IsLow(Link.GetQueuedEventCount()));
	TestEqual(TEXT("payload still charged after dequeue"), Link.PendingInboundBytes.load(), int64(5));
	Link.PendingInboundBytes.fetch_sub(3); TestEqual(TEXT("partial byte ACK"), Link.PendingInboundBytes.load(), int64(2));
	Link.PendingInboundBytes.fetch_sub(0); TestEqual(TEXT("zero ACK remains harmless"), Link.PendingInboundBytes.load(), int64(2));
	Link.PendingInboundBytes.fetch_sub(2); TestEqual(TEXT("batched remainder ACK"), Link.PendingInboundBytes.load(), int64(0));
	TestEqual(TEXT("byte ACKs do not recreate slots"), Link.GetQueuedEventCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketPressureBatchTest, "VaRest.WebSocket.Pressure.Batch",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestSocketPressureBatchTest::RunTest(const FString& Parameters)
{
	using namespace VaRestSocketPressureTest;
	FFixture Case; Case.Connection->Open(); Case.Connection->Message(); Case.Connection->Message({7}); Case.Attach();
	TestEqual(TEXT("initial native snapshot delivered"), Case.Inbound->GetDeliveryCountOnGT(), 3);
	const int32 FirstPops = Case.Connection->PopCalls.load();
	FSocketDelivery Item;
	if (!TestTrue(TEXT("open available"), Case.Inbound->PopOnGT(Item))) return false;
	Case.Inbound->ConsumedOnGT(Item);
	Case.Connection->Message({8, 9}); Case.Connection->Message(); Case.Quiesce();
	TestEqual(TEXT("notification cannot admit another batch while GTK owns inputs"), Case.Connection->PopCalls.load(), FirstPops);
	if (!TestTrue(TEXT("empty message available"), Case.Inbound->PopOnGT(Item))) return false;
	TestTrue(TEXT("empty raw input survives"), Item.Message.IsBinary() && Item.Message.Bytes().IsEmpty());
	Case.Inbound->ConsumedOnGT(Item); Case.Quiesce();
	TestEqual(TEXT("partial batch retirement does not pull"), Case.Connection->PopCalls.load(), FirstPops);
	if (!TestTrue(TEXT("last initial input available"), Case.Inbound->PopOnGT(Item))) return false;
	Case.Quiesce();
	TestEqual(TEXT("GTK pop alone does not retire in-flight delivery"), Case.Connection->PopCalls.load(), FirstPops);
	Case.Inbound->ConsumedOnGT(Item); Case.Quiesce();
	TestEqual(TEXT("last delivery releases exactly the next waiting batch without a new notification"), Case.Connection->PopCalls.load(), FirstPops + 1);
	TestEqual(TEXT("second batch delivered"), Case.Inbound->GetDeliveryCountOnGT(), 2);
	TestEqual(TEXT("only undelivered payload remains charged"), Case.Connection->GetPendingInboundBytes(), int64(2));
	if (!TestTrue(TEXT("second batch first input"), Case.Inbound->PopOnGT(Item))) return false;
	const auto Bytes = Item.Message.Bytes(); TestTrue(TEXT("ordered binary payload"), Bytes.Num() == 2 && Bytes[0] == 8 && Bytes[1] == 9);
	Case.Inbound->ConsumedOnGT(Item);
	if (!TestTrue(TEXT("second batch empty input"), Case.Inbound->PopOnGT(Item))) return false;
	Case.Inbound->ConsumedOnGT(Item); Case.Quiesce();
	TestEqual(TEXT("all payload returned"), Case.Connection->GetPendingInboundBytes(), int64(0));
	TestEqual(TEXT("exact payload receipts"), Case.Connection->ConsumedBytes.load(), int64(3));
	TestEqual(TEXT("empty and nonempty input receipts exact"), Case.Connection->ConsumedFrames.load(), 4);
	TestFalse(TEXT("native pump never ran on GTK"), Case.Connection->WrongThread.load());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketPressureFragmentsTest, "VaRest.WebSocket.Pressure.Fragments",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestSocketPressureFragmentsTest::RunTest(const FString& Parameters)
{
	using namespace VaRestSocketPressureTest;
	FFixture Case; Case.Inbound->ConfigureOnGT(MakeShared<FPairCodec, ESPMode::ThreadSafe>(), {});
	Case.Connection->Open(); Case.Connection->Message({1}); Case.Attach();
	FSocketDelivery Item;
	if (!TestTrue(TEXT("open available before partial packet"), Case.Inbound->PopOnGT(Item))) return false;
	Case.Inbound->ConsumedOnGT(Item); Case.Quiesce();
	TestEqual(TEXT("NeedMore does not invent GTK delivery"), Case.Inbound->GetDeliveryCountOnGT(), 0);
	TestEqual(TEXT("partial packet retains payload credit"), Case.Connection->GetPendingInboundBytes(), int64(1));
	Case.Connection->Message({2}); Case.Quiesce();
	if (!TestTrue(TEXT("next batch completes the partial packet"), Case.Inbound->PopOnGT(Item))) return false;
	const auto Bytes = Item.Message.Bytes(); TestTrue(TEXT("both fragments survived"), Bytes.Num() == 2 && Bytes[0] == 1 && Bytes[1] == 2);
	TestEqual(TEXT("payload carries both old and current input credits"), Item.MessageCharge, 2);
	Case.Inbound->ConsumedOnGT(Item); Case.Quiesce();
	TestEqual(TEXT("merged packet receipts exact"), Case.Connection->ConsumedFrames.load(), 2);
	TestEqual(TEXT("merged payload returned"), Case.Connection->GetPendingInboundBytes(), int64(0));
	Case.Connection->Message({3}); Case.Quiesce();
	TestEqual(TEXT("another partial packet can start after one current batch credit"), Case.Inbound->GetDeliveryCountOnGT(), 0);
	Case.Inbound->StopAndWaitOnGT();
	TestEqual(TEXT("stop discards retained decoder bytes"), Case.Connection->GetPendingInboundBytes(), int64(0));
	TestEqual(TEXT("stop returns the final fragment exactly once"), Case.Connection->ConsumedFrames.load(), 3);
	TestFalse(TEXT("worker parsing and pulls stayed off GTK"), Case.Connection->WrongThread.load());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketPressureWakeStopTest, "VaRest.WebSocket.Pressure.WakeAndStop",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestSocketPressureWakeStopTest::RunTest(const FString& Parameters)
{
	using namespace VaRestSocketPressureTest;
	FFixture Case; Case.Connection->Open(); Case.Attach();
	const auto Fired = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	const auto Link = Case.Connection->Link;
	// The GT producer is quiescent. The hook hands this one native publication to the consumer task after its empty
	// snapshot; its notification races the pump's scheduled-bit release, without a sleep or another GTK tick.
	Case.Connection->Link->SetPopSnapshotHookForTests([Fired, WeakLink = TWeakPtr<FSocketLink, ESPMode::ThreadSafe>(Link)]
	{
		if (!Fired->exchange(true)) if (auto NativeLink = WeakLink.Pin()) NativeLink->Push(Binary({4}));
	});
	FSocketDelivery Item;
	if (!TestTrue(TEXT("open retires before refill interleaving"), Case.Inbound->PopOnGT(Item))) return false;
	Case.Inbound->ConsumedOnGT(Item); Case.Quiesce();
	Case.Connection->Link->SetPopSnapshotHookForTests({});
	TestTrue(TEXT("owned refill ran"), Fired->load());
	TestEqual(TEXT("clear/recheck preserves a notification during the empty snapshot"), Case.Inbound->GetDeliveryCountOnGT(), 1);
	const int32 BeforeStop = Case.Connection->PopCalls.load();
	Case.Inbound->StopAndWaitOnGT();
	TestEqual(TEXT("stop returns undelivered payload"), Case.Connection->GetPendingInboundBytes(), int64(0));
	TestEqual(TEXT("stop returns undelivered frame once"), Case.Connection->ConsumedFrames.load(), 1);
	Case.Connection->Message({5}); Case.Quiesce();
	TestEqual(TEXT("stopped scheduling cannot pull again"), Case.Connection->PopCalls.load(), BeforeStop);
	TestFalse(TEXT("all parser pulls stayed off GTK"), Case.Connection->WrongThread.load());
	return true;
}

#if VAREST_WITH_TRACE_EXCERPTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketTraceBinaryCaptureTest, "VaRest.WebSocket.Trace.BinaryCapture",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestSocketTraceBinaryCaptureTest::RunTest(const FString& Parameters)
{
	using namespace VaRestSocketPressureTest;
	class FDetailSink final : public IVaRestTraceSink
	{
	public:
		int32 BinaryCap = 0;
		FVaRestTraceDetailSettings GetDetail() const override
		{ FVaRestTraceDetailSettings Out; Out.Flags = EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Bodies; Out.FrameCap = 8; Out.BinaryCap = BinaryCap; return Out; }
		void OnAttached(const FVaRestTraceSnapshot&) override {}
		void OnEvent(const FVaRestTraceEvent&) override {}
	};
	FVaRestTrace::FScopedSinkIsolation Isolation;
	const auto Sink = MakeShared<FDetailSink, ESPMode::ThreadSafe>(); FVaRestTrace::AddSink(Sink);
	ON_SCOPE_EXIT { FVaRestTrace::RemoveSink(Sink); };
	const TArray<uint8> Raw{0, 0xff, 0x89, 1, 2, 3, 4, 5, 6, 7};
	for (int32 Cap : {0, 3})
	{
		Sink->BinaryCap = Cap; FVaRestTrace::RefreshDetail();
		FFixture Case;
		Case.Inbound->SetTraceOnGT(771, 1, MakeShared<const FVaRestRedactor, ESPMode::ThreadSafe>(), nullptr);
		Case.Connection->Open(); Case.Connection->Message(Raw); Case.Attach();
		FSocketDelivery Item;
		if (!TestTrue(TEXT("open delivery available"), Case.Inbound->PopOnGT(Item))) return false;
		Case.Inbound->ConsumedOnGT(Item);
		if (!TestTrue(TEXT("binary delivery available"), Case.Inbound->PopOnGT(Item))) return false;
		const auto Message = Item.Message.Bytes();
		TestTrue(TEXT("trace capture preserves the decoder's original binary bytes"), Message.Num() == Raw.Num() && FMemory::Memcmp(Message.GetData(), Raw.GetData(), Raw.Num()) == 0);
		if (!TestEqual(TEXT("one native frame trace"), Item.TraceFrames.Num(), 1)) return false;
		const auto& Excerpt = Item.TraceFrames[0].Excerpt;
		TestTrue(TEXT("inbound producer labels binary descriptors and captures"), Excerpt.Kind == EVaRestExcerptKind::Binary && Excerpt.bBinaryCaptured == (Cap > 0) && Excerpt.TotalBytes == Raw.Num());
		if (Cap > 0) TestTrue(TEXT("inbound producer captures only the requested prefix"), Excerpt.bTruncated && Excerpt.Bytes && *Excerpt.Bytes == TArray<uint8>{0, 0xff, 0x89});
		else TestTrue(TEXT("default inbound descriptor contains no payload"), Excerpt.ToText().StartsWith(TEXT("<binary: ")));
		Case.Inbound->ConsumedOnGT(Item); Case.Quiesce();
		TestEqual(TEXT("capture does not alter payload receipts"), Case.Connection->GetPendingInboundBytes(), int64(0));
		TestFalse(TEXT("all inbound decoding stayed off GTK"), Case.Connection->WrongThread.load());
	}
	return true;
}
#endif

#endif
