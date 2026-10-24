// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestInspector.h"

#if WITH_DEV_AUTOMATION_TESTS && VAREST_WITH_INSPECTOR

#include "Async/Async.h"
#include "CoreGlobals.h"
#include "GenericPlatform/GenericPlatformMemory.h"
#include "HAL/Event.h"
#include "HAL/MallocDoubleFreeFinder.h"
#include "HAL/MallocFrameProfiler.h"
#include "HAL/MallocStomp2.h"
#include "HAL/MemoryBase.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"
#include "Templates/SharedPointerInternals.h"
#include "VaRestInspectorAllocation.h"
#include "VaRestInspectorPacket.h"
#include "VaRestInspectorPolicy.h"
#include "VaRestInspectorQueue.h"

namespace VaRest::Inspector::Private::Testing
{
bool ReadLiveProfile(FAutomationTestBase& Test)
{
	bool bValid = true;
#if defined(PLATFORM_LINUX) && PLATFORM_LINUX && defined(FORCE_ANSI_ALLOCATOR) && !FORCE_ANSI_ALLOCATOR && !defined(__clang_analyzer__) && defined(IS_MONOLITHIC) && !IS_MONOLITHIC && defined(UE_BUILD_DEVELOPMENT) && UE_BUILD_DEVELOPMENT && defined(WITH_EDITOR) && WITH_EDITOR
	Test.AddInfo(FString::Printf(TEXT("AllocatorQualification build: Linux=%d ForceAnsi=%d Monolithic=%d Development=%d Editor=%d PointerBytes=%llu OrdinaryNewAlignment=%llu Stomp2=%d; no arbitrary runtime GMalloc replacement supported"),
		PLATFORM_LINUX, FORCE_ANSI_ALLOCATOR, IS_MONOLITHIC, UE_BUILD_DEVELOPMENT, WITH_EDITOR,
		uint64(sizeof(void*)), uint64(__STDCPP_DEFAULT_NEW_ALIGNMENT__), WITH_MALLOC_STOMP2));
#else
	Test.AddError(TEXT("AllocatorQualification unsupported defined build profile"));
	bValid = false;
#endif
	const auto& MemoryConstants = FPlatformMemory::GetConstants();
	Test.AddInfo(FString::Printf(TEXT("AllocatorQualification memory: PageSize=%llu BinnedPageSize=%llu BinnedAllocationGranularity=%llu"),
		uint64(MemoryConstants.PageSize), uint64(MemoryConstants.BinnedPageSize), uint64(MemoryConstants.BinnedAllocationGranularity)));
	const FSupportedAllocator Support = GetSupportedAllocator();
	FMalloc* Live = UE::Private::GMalloc;
	bValid &= Test.TestNotNull(TEXT("live allocator exists"), Live);
	Test.AddInfo(FString::Printf(TEXT("AllocatorQualification live: Selected=%d Name=%s GMalloc=%p DoubleFree=%p DoubleFreeEnabled=%d FrameProfiler=%p FrameProfilerEnabled=%d"),
		int32(FPlatformMemory::AllocatorToUse), Live ? Live->GetDescriptiveName() : TEXT("<null>"),
		Live, GMallocDoubleFreeFinder, GMallocDoubleFreeFinderEnabled, GMallocFrameProfiler, GMallocFrameProfilerEnabled));
#if WITH_MALLOC_STOMP2
	Test.AddInfo(FString::Printf(TEXT("AllocatorQualification Stomp2=%p Enabled=%d"), GMallocStomp2, GMallocStomp2Enabled));
	bValid &= Test.TestFalse(TEXT("no Stomp2 proxy"), GMallocStomp2 || GMallocStomp2Enabled);
#endif
	Test.AddInfo(FString::Printf(TEXT("AllocatorQualification commandline: %s"), FCommandLine::Get()));
	bValid &= Test.TestFalse(TEXT("no double-free proxy"), GMallocDoubleFreeFinder || GMallocDoubleFreeFinderEnabled);
	bValid &= Test.TestFalse(TEXT("no frame-profiler proxy"), GMallocFrameProfiler || GMallocFrameProfilerEnabled);
	bValid &= Test.TestFalse(TEXT("no replay startup"), FParse::Param(FCommandLine::Get(), TEXT("mallocsavereplay")));
	bValid &= Test.TestFalse(TEXT("no profiler startup"), FParse::Param(FCommandLine::Get(), TEXT("mallocframeprofiler")));
	bValid &= Test.TestFalse(TEXT("no purgatory startup"), FParse::Param(FCommandLine::Get(), TEXT("purgatorymallocproxy")));
	Test.AddInfo(FString::Printf(TEXT("AllocatorQualification bootstrap: Family=%d ModuleNew=%d KnownChain=%d Calibrated=%d"),
		int32(Support.Family), Support.bVerifiedModuleNew, Support.bVerifiedNoInflatingProxy, Support.bCalibrationPassed));
	bValid &= Test.TestTrue(TEXT("provisional audited family"), Support.Family != FSupportedAllocator::EFamily::Unsupported);
	bValid &= Test.TestTrue(TEXT("provisional module constraints"), Support.bVerifiedModuleNew);
	bValid &= Test.TestTrue(TEXT("provisional known chain constraints"), Support.bVerifiedNoInflatingProxy);
	bValid &= Test.TestFalse(TEXT("module bootstrap remains uncalibrated"), Support.bCalibrationPassed);
	bValid &= Test.TestFalse(TEXT("bootstrap cannot allocate controllers"), Support.SupportsController(64, 8));
	return bValid;
}

bool CalibrateForTest(FAutomationTestBase& Test, FAllocationAdapter& Allocator)
{
	if (!ReadLiveProfile(Test))
		return false;
	// This exact controller is the current fixture's actual MakeShared account type.
	using FController = SharedPointerInternals::TIntrusiveReferenceController<FPendingAccount, ESPMode::ThreadSafe>;
	std::atomic<int64> StartupBytes{0};
	const bool bCalibrated = Allocator.Calibrate(StartupBytes, 4096, sizeof(FController), alignof(FController));
	Test.AddInfo(FString::Printf(TEXT("AllocatorCalibration actual: Request=%lld Alignment=%lld Capacity=%lld StartupLimit=4096"),
		Allocator.Counters().CalibrationRequest.load(), Allocator.Counters().CalibrationAlignment.load(), Allocator.Counters().CalibrationCapacity.load()));
	Test.TestEqual(TEXT("calibration free precedes startup credit release"), StartupBytes.load(), int64(0));
	Test.TestEqual(TEXT("calibration leaves no backing"), Allocator.Counters().LiveBacking.load(), int64(0));
	return Test.TestTrue(TEXT("real charged calibration"), bCalibrated);
}
} // namespace VaRest::Inspector::Private::Testing

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorAllocatorQualificationTest,
	"VaRest.Inspector.AllocatorQualification",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorAllocatorQualificationTest::RunTest(const FString& Parameters)
{
	// Read-only: no adapter, calibration, account, packet, queue or Inspector backing.
	return VaRest::Inspector::Private::Testing::ReadLiveProfile(*this);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorTicketTest,
	"VaRest.Inspector.Store.QueueTicket",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaRestInspectorTicketTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Inspector::Private;
	auto Account = MakeShared<FPendingAccount, ESPMode::ThreadSafe>(8, 100);
	auto First = FPendingTicket::Acquire(Account, 60);
	TestTrue(TEXT("first admitted"), First.IsSet());
	auto Denied = FPendingTicket::Acquire(Account, 41);
	TestFalse(TEXT("byte limit rejects with count slack"), Denied.IsSet());
	TestEqual(TEXT("failed admission rolls back count"), Account->Count.load(), int64(1));
	TestEqual(TEXT("failed admission preserves bytes"), Account->Bytes.load(), int64(60));
	auto Exact = FPendingTicket::Acquire(Account, 40);
	TestTrue(TEXT("exact equality admitted"), Exact.IsSet());
	TestEqual(TEXT("full byte capacity"), Account->Bytes.load(), int64(100));
	if (First.IsSet())
	{
		FPendingTicket Moved(MoveTemp(First.GetValue()));
		First.Reset();
		TestEqual(TEXT("moved owner retains charge"), Account->Bytes.load(), int64(100));
		Moved.Reset();
	}
	TestEqual(TEXT("one retained owner"), Account->Bytes.load(), int64(40));
	Exact.Reset();
	TestEqual(TEXT("all bytes released"), Account->Bytes.load(), int64(0));
	TestEqual(TEXT("all counts released"), Account->Count.load(), int64(0));
	TestFalse(TEXT("negative rejected"), FPendingTicket::Acquire(Account, -1).IsSet());
	TestFalse(TEXT("oversize rejected"), FPendingTicket::Acquire(Account, 101).IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorQueueAdmissionTest,
	"VaRest.Inspector.Store.QueueAdmission",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorQueueAdmissionTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Inspector::Private;
	FAllocationCounters Probe;
	FAllocationAdapter Allocator(Probe, GetSupportedAllocator());
	if (!Testing::CalibrateForTest(*this, Allocator))
		return false;
	FCapturePolicy Policy;
	auto Bytes = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>();
	Bytes->Reserve(2048);
	Bytes->Add(uint8('x'));
	FVaRestTraceExcerptEvent Excerpt;
	Excerpt.Owner = EVaRestTraceExcerptOwner::Response;
	Excerpt.RequestId = 77;
	Excerpt.DispatchId = 5;
	Excerpt.Attempt = 1;
	Excerpt.Excerpt.StopReason = TEXT("safe capture sentinel");
	Excerpt.Excerpt.Bytes = Bytes;
	Excerpt.Excerpt.TotalBytes = 1;
	FVaRestTraceEvent Source;
	Source.Kind = EVaRestTraceKind::Excerpt;
	Source.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceExcerptEvent>(), MoveTemp(Excerpt));
	const FVaRestTraceEvent& Borrowed = Source;
	const int32 SourceRefs = Bytes.GetSharedReferenceCount();
	FPacketLayout Layout;
	if (!TestTrue(TEXT("full borrowed layout"),
			FPacketCodec::Measure(Borrowed, Policy, Allocator, Layout) == EMeasure::Full))
		return false;
	TestEqual(TEXT("measure no text copy"), Probe.TextBytesCopied.load(), int64(0));
	TestEqual(TEXT("measure no excerpt acquisition"), Probe.ExcerptRefsAcquired.load(), int64(0));
	TestEqual(TEXT("measure leaves actual refs"), Bytes.GetSharedReferenceCount(), SourceRefs);
	auto Base = FCaptureQueue::BaselineCharge(Allocator);
	if (!TestTrue(TEXT("supported queue baseline"), Base.IsSet()))
		return false;
	const int64 Baseline = Base.GetValue();
	auto DeniedAccount = MakeShared<FPendingAccount, ESPMode::ThreadSafe>(8,
		Baseline + Layout.PendingCharge - 1);
	{
		FCaptureQueue Denied(Policy, 1, DeniedAccount, Allocator);
		ON_SCOPE_EXIT
		{
			Denied.Close();
			while (Denied.TryPopMetadata().IsSet())
			{
			}
			while (Denied.TryPopExcerpt().IsSet())
			{
			}
		};
		if (!TestTrue(TEXT("denied queue baseline available"), Denied.IsSupported()))
			return false;
		TestEqual(TEXT("count slack before byte refusal"), DeniedAccount->Count.load(), int64(0));
		const EPush Result = Denied.TryPush(Borrowed);
		TestTrue(TEXT("byte refusal highwater stays within limit"), DeniedAccount->PeakBytes.load() <= DeniedAccount->ByteLimit);
		TestTrue(TEXT("full payload refused by bytes"), Result == EPush::ScalarOnly || Result == EPush::Dropped);
		TestEqual(TEXT("no rejected text copied"), Probe.TextBytesCopied.load(), int64(0));
		TestEqual(TEXT("no rejected excerpt acquired"), Probe.ExcerptRefsAcquired.load(), int64(0));
		TestEqual(TEXT("actual refs unchanged by refusal"), Bytes.GetSharedReferenceCount(), SourceRefs);
		auto Fallback = Denied.TryPopExcerpt();
		if (Result == EPush::ScalarOnly)
		{
			if (!TestTrue(TEXT("backlog descriptor actually queued"), Fallback.IsSet()))
				return false;
			TestTrue(TEXT("backlog reason"), Fallback->Fixed().Scalars.Skip == EVaRestExcerptSkip::Backlog);
			TestEqual(TEXT("backlog keeps request"), Fallback->Fixed().Scalars.RequestId, uint64(77));
			TestEqual(TEXT("backlog keeps dispatch"), Fallback->Fixed().Scalars.DispatchId, uint64(5));
			TestEqual(TEXT("backlog keeps total"), Fallback->Fixed().Scalars.TotalBytes, int64(1));
			TestFalse(TEXT("backlog has no excerpt ref"), Fallback->Excerpt().IsValid());
			TestTrue(TEXT("backlog has no text"), Fallback->Text(EText::StopReason).IsEmpty());
		}
		else
			TestFalse(TEXT("drop has no queued fallback"), Fallback.IsSet());
	}
	TestEqual(TEXT("denied count cleaned"), DeniedAccount->Count.load(), int64(0));
	TestEqual(TEXT("denied bytes cleaned"), DeniedAccount->Bytes.load(), int64(0));
	TestEqual(TEXT("denied backing freed"), Probe.LiveBacking.load(), int64(0));
	auto Pending = MakeShared<FPendingAccount, ESPMode::ThreadSafe>(8,
		Baseline + Layout.PendingCharge);
	{
		FCaptureQueue Queue(Policy, 2, Pending, Allocator);
		ON_SCOPE_EXIT
		{
			Queue.Close();
			while (Queue.TryPopMetadata().IsSet())
			{
			}
			while (Queue.TryPopExcerpt().IsSet())
			{
			}
		};
		if (!TestTrue(TEXT("exact-capacity queue available"), Queue.IsSupported()))
			return false;
		if (!TestTrue(TEXT("exact bytes admitted"), Queue.TryPush(Borrowed) == EPush::Accepted))
			return false;
		TestEqual(TEXT("one acquired excerpt"), Probe.ExcerptRefsAcquired.load(), int64(1));
		TestEqual(TEXT("actual packet ref"), Bytes.GetSharedReferenceCount(), SourceRefs + 1);
		TestTrue(TEXT("accepted text actually copied"), Probe.TextBytesCopied.load() > 0);
		TestTrue(TEXT("queued actual backing and retained excerpt covered"),
			Probe.LiveBacking.load() + int64(Bytes->GetAllocatedSize()) <= Pending->Bytes.load());
		auto Held = Queue.TryPopExcerpt();
		if (!TestTrue(TEXT("actual dequeued packet"), Held.IsSet()))
			return false;
		TestEqual(TEXT("dequeued count retained"), Pending->Count.load(), int64(1));
		TestEqual(TEXT("dequeued bytes retained"), Pending->Bytes.load(), Baseline + Layout.PendingCharge);
		TestTrue(TEXT("held actual backing and retained excerpt covered"),
			Probe.LiveBacking.load() + int64(Bytes->GetAllocatedSize()) <= Pending->Bytes.load());
		TestEqual(TEXT("dispatch identity"), Held->Fixed().Scalars.DispatchId, uint64(5));
		TestEqual(TEXT("text survives callback"), FString(Held->Text(EText::StopReason)),
			FString(TEXT("safe capture sentinel")));
		TestTrue(TEXT("source bytes retained"), Held->Excerpt().IsValid() && Held->Excerpt()->Num() == 1);
		Queue.Close();
		TestTrue(TEXT("closed queue cannot admit"), Queue.TryPush(Borrowed) == EPush::Closed);
		TestEqual(TEXT("held charge survives close"), Pending->Bytes.load(), Baseline + Layout.PendingCharge);
		Held.Reset();
		TestEqual(TEXT("payload destruction returns count"), Pending->Count.load(), int64(0));
		TestEqual(TEXT("payload destruction leaves baseline only"), Pending->Bytes.load(), Baseline);
		TestEqual(TEXT("actual ref released"), Bytes.GetSharedReferenceCount(), SourceRefs);
	}
	TestEqual(TEXT("last queue baseline released"), Pending->Bytes.load(), int64(0));
	TestEqual(TEXT("all actual Inspector backing freed"), Probe.LiveBacking.load(), int64(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorQueueCodecTest,
	"VaRest.Inspector.Store.QueueCodec",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorQueueCodecTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Inspector::Private;
	FAllocationCounters Probe;
	FAllocationAdapter Allocator(Probe, GetSupportedAllocator());
	if (!Testing::CalibrateForTest(*this, Allocator))
		return false;
	FCapturePolicy Policy;
	auto Account = MakeShared<FPendingAccount, ESPMode::ThreadSafe>(8, 8 * 1024 * 1024);
	auto Encode = [&](const FVaRestTraceEvent& Event, EMeasure Expected) -> TOptional<FPacket> {
		FPacketLayout Layout;
		const int64 Copies = Probe.TextBytesCopied.load();
		const int64 Refs = Probe.ExcerptRefsAcquired.load();
		const EMeasure Measured = FPacketCodec::Measure(Event, Policy, Allocator, Layout);
		TestTrue(TEXT("expected measurement"), Measured == Expected);
		TestEqual(TEXT("measurement borrows text"), Probe.TextBytesCopied.load(), Copies);
		TestEqual(TEXT("measurement borrows excerpt"), Probe.ExcerptRefsAcquired.load(), Refs);
		if (Measured == EMeasure::Unsupported)
			return {};
		auto Ticket = FPendingTicket::Acquire(Account, Layout.PendingCharge);
		if (!TestTrue(TEXT("codec admission"), Ticket.IsSet()))
			return {};
		return FPacketCodec::Write(Event, Layout, MoveTemp(Ticket.GetValue()), Allocator);
	};
	FVaRestTraceStarted Started;
	Started.RequestId = 123;
	Started.ScopeId = 2;
	Started.ClientId = 3;
	Started.Verb = TEXT("PATCH");
	Started.Url = TEXT("https://safe/?token=***");
	Started.PathTemplate = TEXT("/item/{id}");
	Started.QueueReplayOf.QueueId = TEXT("queue-guid");
	Started.QueueReplayOf.OriginalRequestId = 99;
	Started.Initiator.OwnerPath = TEXT("owner");
	Started.Initiator.ScriptObject = TEXT("object");
	Started.Initiator.ScriptFunction = TEXT("function");
	Started.Initiator.SourceFile = TEXT("source.cpp");
	Started.Initiator.ScriptCodeOffset = 16;
	Started.Initiator.SourceLine = 42;
	Started.Body.ContentType = TEXT("application/json");
	Started.Body.Bytes = 57;
	Started.bExcerptPending = true;
	Started.CurrentAttempt = 4;
	Started.StartedAt = 3.5;
	Started.Headers.Add(TEXT("Authorization"), TEXT("***"));
	FVaRestTraceEvent Event;
	Event.Seq = 17;
	Event.Time = 12.25;
	Event.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceStarted>(), Started);
	{
		auto Packet = Encode(Event, EMeasure::Full);
		if (!Packet.IsSet())
			return false;
		TestEqual(TEXT("source seq"), Packet->Fixed().Header.Seq, uint64(17));
		TestEqual(TEXT("source timestamp"), Packet->Fixed().Header.Time, 12.25);
		TestEqual(TEXT("redacted text byte-for-byte"), FString(Packet->Text(EText::Url)), Started.Url);
		TestEqual(TEXT("initiator retained"), FString(Packet->Text(EText::SourceFile)), Started.Initiator.SourceFile);
		TestEqual(TEXT("source line"), Packet->Fixed().Scalars.SourceLine, 42);
		TestEqual(TEXT("queue identity"), Packet->Fixed().Scalars.OriginalRequestId, uint64(99));
		if (TestEqual(TEXT("one header"), Packet->Headers().Num(), 1))
			TestEqual(TEXT("header retained unchanged"), FString(Packet->Read(Packet->Headers()[0].Second)), FString(TEXT("***")));
	}
	Policy.bHeaders = false;
	Policy.bBodies = false;
	{
		auto Packet = Encode(Event, EMeasure::Full);
		if (!Packet.IsSet())
			return false;
		TestEqual(TEXT("local headers off"), Packet->Headers().Num(), 0);
		TestFalse(TEXT("local bodies clear promise"), Packet->Fixed().Scalars.bExcerptPending);
	}
	Policy.bHeaders = true;
	Policy.bBodies = true;
	FPacketHeader SnapshotHeader;
	SnapshotHeader.bSnapshot = true;
	FPacketLayout SnapshotLayout, OrdinaryLayout;
	TestTrue(TEXT("borrowed snapshot sizing"), FPacketCodec::MeasureStarted(Started, SnapshotHeader, Policy, Allocator, SnapshotLayout) == EMeasure::Full);
	TestTrue(TEXT("ordinary sizing"), FPacketCodec::Measure(Event, Policy, Allocator, OrdinaryLayout) == EMeasure::Full);
	TestEqual(TEXT("direct snapshot excludes queue node"), OrdinaryLayout.PendingCharge - SnapshotLayout.PendingCharge, int64(Allocator.QueueNodeBound()));
	auto SnapshotTicket = FPendingTicket::Acquire(Account, SnapshotLayout.PendingCharge);
	if (!TestTrue(TEXT("snapshot admission"), SnapshotTicket.IsSet()))
		return false;
	{
		auto Packet = FPacketCodec::WriteStarted(Started, SnapshotLayout, MoveTemp(SnapshotTicket.GetValue()), Allocator);
		TestTrue(TEXT("explicit snapshot flag"), Packet.Fixed().Header.bSnapshot);
		TestEqual(TEXT("snapshot has no source seq"), Packet.Fixed().Header.Seq, uint64(0));
		TestEqual(TEXT("snapshot preserves started time"), Packet.Fixed().Scalars.StartedAt, 3.5);
	}
	FVaRestTraceAttemptDispatched Dispatch;
	Dispatch.RequestId = 123;
	Dispatch.DispatchId = 7;
	Dispatch.Attempt = 4;
	Dispatch.AttemptStart = 8;
	Dispatch.GapSeconds = 2;
	Dispatch.AuthWait = 3;
	Dispatch.AuthRefreshId = 88;
	Dispatch.LimiterWait = 4;
	Dispatch.bConditional = true;
	Dispatch.Headers.Add(TEXT("If-None-Match"), TEXT("etag"));
	Event.Kind = EVaRestTraceKind::AttemptDispatched;
	Event.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceAttemptDispatched>(), MoveTemp(Dispatch));
	{
		auto Packet = Encode(Event, EMeasure::Full);
		if (!Packet.IsSet())
			return false;
		TestEqual(TEXT("dispatch identity"), Packet->Fixed().Scalars.DispatchId, uint64(7));
		TestEqual(TEXT("dispatch auth"), Packet->Fixed().Scalars.AuthRefreshId, uint64(88));
		TestEqual(TEXT("dispatch limiter"), Packet->Fixed().Scalars.LimiterWait, 4.0);
		TestTrue(TEXT("conditional"), Packet->Fixed().Scalars.bConditional);
	}
	FVaRestTraceAttemptEnded Ended;
	Ended.RequestId = 123;
	Ended.DispatchId = 7;
	Ended.Status = 304;
	Ended.BytesUp = 45;
	Ended.BytesDown = 0;
	Ended.UploadDoneAt = 1;
	Ended.StatusAt = 2;
	Ended.TransportDoneAt = 3;
	Ended.EffectiveUrl = TEXT("https://safe/ended");
	Ended.ContentType = TEXT("wire/type");
	Event.Kind = EVaRestTraceKind::AttemptEnded;
	Event.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceAttemptEnded>(), MoveTemp(Ended));
	{
		auto Packet = Encode(Event, EMeasure::Full);
		if (!Packet.IsSet())
			return false;
		TestEqual(TEXT("wire status"), Packet->Fixed().Scalars.Status, 304);
		TestEqual(TEXT("wire count"), Packet->Fixed().Scalars.BytesUp, int64(45));
		TestEqual(TEXT("wire transport end"), Packet->Fixed().Scalars.TransportDoneAt, 3.0);
		TestEqual(TEXT("wire content type"), FString(Packet->Text(EText::ResponseContentType)), FString(TEXT("wire/type")));
	}
	FVaRestTraceNote Note;
	Note.RequestId = 123;
	Note.Attempt = 0;
	Note.QueueId = TEXT("persistent-guid");
	Note.Bytes = 4096;
	Event.Kind = EVaRestTraceKind::Note;
	Event.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceNote>(), MoveTemp(Note));
	{
		auto Packet = Encode(Event, EMeasure::Full);
		if (!Packet.IsSet())
			return false;
		TestEqual(TEXT("no fabricated attempt"), Packet->Fixed().Scalars.Attempt, 0);
		TestEqual(TEXT("note bytes"), Packet->Fixed().Scalars.NoteBytes, int64(4096));
		TestEqual(TEXT("persistent queue id"), FString(Packet->Text(EText::QueueId)), FString(TEXT("persistent-guid")));
	}
	FVaRestTraceCompleted Completed;
	Completed.RequestId = 123;
	Completed.Outcome = EVaRestTraceOutcome::Error;
	Completed.Error.HttpStatus = 418;
	Completed.Error.Code = TEXT("code");
	Completed.Error.Message = TEXT("redacted ***");
	Completed.Response.Status = 200;
	Completed.Response.BodyBytes = 99;
	Completed.Response.Storage = EVaRestTraceResponseStorage::Memory;
	Completed.Response.EffectiveUrl = TEXT("https://safe/delivered");
	Completed.Response.ContentType = TEXT("delivered/type");
	Completed.CacheLookup = EVaRestTraceCacheLookup::Revalidate;
	Completed.bFromCache = true;
	Completed.AuthRefreshId = 321;
	Completed.DecodeWarningCount = 40;
	Completed.DecodeWarnings.Add({TEXT("$.path"), TEXT("warning ***")});
	Completed.Envelope.bApplied = true;
	Completed.Envelope.DataPath = TEXT("data");
	Completed.Envelope.PredicatePath = TEXT("ok");
	Completed.Envelope.Expected = TEXT("true");
	Completed.Envelope.Actual = TEXT("false");
	Completed.Envelope.Code = TEXT("E");
	Completed.Envelope.Message = TEXT("message");
	Event.Kind = EVaRestTraceKind::Completed;
	Event.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceCompleted>(), Completed);
	{
		auto Packet = Encode(Event, EMeasure::Full);
		if (!Packet.IsSet())
			return false;
		TestEqual(TEXT("delivered status separate from error"), Packet->Fixed().Scalars.Status, 200);
		TestEqual(TEXT("error http status"), Packet->Fixed().Scalars.ErrorHttpStatus, 418);
		TestEqual(TEXT("delivered body count"), Packet->Fixed().Scalars.DeliveredBodyBytes, int64(99));
		TestTrue(TEXT("delivered from cache"), Packet->Fixed().Scalars.bFromCache);
		TestEqual(TEXT("warning total"), Packet->Fixed().Scalars.DecodeWarningCount, 40);
		if (TestEqual(TEXT("one warning"), Packet->Warnings().Num(), 1))
			TestEqual(TEXT("warning bytes"), FString(Packet->Read(Packet->Warnings()[0].Second)), FString(TEXT("warning ***")));
		TestEqual(TEXT("envelope predicate"), FString(Packet->Text(EText::PredicatePath)), FString(TEXT("ok")));
	}
	Completed.Error.Message = FString::ChrN(FNormalizedLimits::MaxStringChars + 1, TCHAR('x'));
	Event.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceCompleted>(), MoveTemp(Completed));
	{
		auto Packet = Encode(Event, EMeasure::ScalarOnly);
		if (!Packet.IsSet())
			return false;
		TestTrue(TEXT("oversized metadata marked"), Packet->Fixed().Header.bMetadataOmitted);
		TestEqual(TEXT("scalar response retained"), Packet->Fixed().Scalars.Status, 200);
		TestEqual(TEXT("scalar error retained"), Packet->Fixed().Scalars.ErrorHttpStatus, 418);
		TestEqual(TEXT("scalar request retained"), Packet->Fixed().Scalars.RequestId, uint64(123));
		TestTrue(TEXT("scalar text empty"), Packet->Text(EText::ErrorMessage).IsEmpty());
		TestEqual(TEXT("scalar warning list empty"), Packet->Warnings().Num(), 0);
	}
	FVaRestTraceScope Scope;
	Scope.ScopeId = 0;
	Scope.Label = TEXT("Process");
	Event.Kind = EVaRestTraceKind::Scope;
	Event.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceScope>(), MoveTemp(Scope));
	{
		auto Packet = Encode(Event, EMeasure::Full);
		if (!Packet.IsSet())
			return false;
		TestEqual(TEXT("process scope"), Packet->Fixed().Scalars.ScopeId, uint64(0));
		TestEqual(TEXT("scope label"), FString(Packet->Text(EText::ScopeLabel)), FString(TEXT("Process")));
	}
	FVaRestTraceClient Client;
	Client.ClientId = 22;
	Client.DisplayName = TEXT("client ***");
	Event.Kind = EVaRestTraceKind::Client;
	Event.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceClient>(), MoveTemp(Client));
	{
		auto Packet = Encode(Event, EMeasure::Full);
		if (!Packet.IsSet())
			return false;
		TestEqual(TEXT("client id"), Packet->Fixed().Scalars.ClientId, uint64(22));
		TestEqual(TEXT("client label"), FString(Packet->Text(EText::ClientLabel)), FString(TEXT("client ***")));
	}
	FVaRestTraceMarker Marker;
	Marker.CauseRequestId = 123;
	Marker.Text = TEXT("marker ***");
	Event.Kind = EVaRestTraceKind::Marker;
	Event.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceMarker>(), MoveTemp(Marker));
	{
		auto Packet = Encode(Event, EMeasure::Full);
		if (!Packet.IsSet())
			return false;
		TestEqual(TEXT("marker cause"), Packet->Fixed().Scalars.CauseRequestId, uint64(123));
		TestEqual(TEXT("marker text"), FString(Packet->Text(EText::MarkerText)), FString(TEXT("marker ***")));
	}
	Event.Kind = EVaRestTraceKind::Started; // deliberately mismatched variant
	TestFalse(TEXT("mismatched variant ignored"), Encode(Event, EMeasure::Unsupported).IsSet());
	Event.Kind = static_cast<EVaRestTraceKind>(255);
	TestFalse(TEXT("unknown kind ignored before variant"), Encode(Event, EMeasure::Unsupported).IsSet());
	Event.Kind = EVaRestTraceKind::Marker;
	Event.Source = EVaRestTraceSource::Socket;
	TestFalse(TEXT("socket ignored before variant"), Encode(Event, EMeasure::Unsupported).IsSet());
	TestEqual(TEXT("codec backing freed"), Probe.LiveBacking.load(), int64(0));
	TestEqual(TEXT("codec tickets freed"), Account->Bytes.load(), int64(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorQueueBoundariesTest,
	"VaRest.Inspector.Store.QueueBoundaries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorQueueBoundariesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Inspector::Private;
	FAllocationCounters Probe;
	FAllocationAdapter Allocator(Probe, GetSupportedAllocator());
	if (!Testing::CalibrateForTest(*this, Allocator))
		return false;
	FCapturePolicy Policy;
	FVaRestTraceEvent Event;
	FVaRestTraceStarted Started;
	Started.RequestId = 1;
	Started.Url = TEXT("safe ***");
	Event.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceStarted>(), Started);
	FPacketLayout Layout;
	if (!TestTrue(TEXT("boundary layout"), FPacketCodec::Measure(Event, Policy, Allocator, Layout) == EMeasure::Full))
		return false;
	const auto Base = FCaptureQueue::BaselineCharge(Allocator);
	if (!TestTrue(TEXT("boundary baseline"), Base.IsSet()))
		return false;
	auto Account = MakeShared<FPendingAccount, ESPMode::ThreadSafe>(2, Base.GetValue() + 8 * Layout.PendingCharge);
	{
		FCaptureQueue Queue(Policy, 1, Account, Allocator);
		TestFalse(TEXT("empty metadata"), Queue.TryPopMetadata().IsSet());
		TestFalse(TEXT("empty excerpts"), Queue.TryPopExcerpt().IsSet());
		TestTrue(TEXT("first item"), Queue.TryPush(Event) == EPush::Accepted);
		Event.Seq = 2;
		TestTrue(TEXT("exact count equality"), Queue.TryPush(Event) == EPush::Accepted);
		const int64 Copied = Probe.TextBytesCopied.load();
		TestTrue(TEXT("count limit with byte slack"), Queue.TryPush(Event) == EPush::Dropped);
		TestEqual(TEXT("count denial performs no copy"), Probe.TextBytesCopied.load(), Copied);
		TestEqual(TEXT("drop has no metadata ordinal"), Queue.LastPublishedMetadata(), uint64(2));
		auto First = Queue.TryPopMetadata();
		auto Second = Queue.TryPopMetadata();
		if (!TestTrue(TEXT("FIFO has both"), First.IsSet() && Second.IsSet()))
			return false;
		TestEqual(TEXT("first FIFO seq"), First->Fixed().Header.Seq, uint64(0));
		TestEqual(TEXT("second FIFO seq"), Second->Fixed().Header.Seq, uint64(2));
		TestEqual(TEXT("generation retained"), First->Fixed().Header.Generation, uint64(1));
		TestEqual(TEXT("first published ordinal"), First->Fixed().Header.MetadataOrdinal, uint64(1));
		TestEqual(TEXT("held counts remain"), Account->Count.load(), int64(2));
		TestTrue(TEXT("loss epoch"), Queue.LossEpoch() > 0);
	}
	TestEqual(TEXT("boundary byte cleanup"), Account->Bytes.load(), int64(0));
	TestEqual(TEXT("count highwater"), Account->PeakCount.load(), int64(2));
	int64 Sum = MAX_int64 - 1;
	TestFalse(TEXT("checked overflow"), AddChecked(Sum, 2, MAX_int64));
	TestEqual(TEXT("overflow unchanged"), Sum, MAX_int64 - 1);
	TestFalse(TEXT("negative arithmetic"), AddChecked(Sum, -1, MAX_int64));
	Started.Url = FString::ChrN(FNormalizedLimits::MaxStringChars, TCHAR('x'));
	Event.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceStarted>(), Started);
	TestTrue(TEXT("exact string cap"), FPacketCodec::Measure(Event, Policy, Allocator, Layout) == EMeasure::Full);
	Started.Url.AppendChar(TCHAR('x'));
	Event.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceStarted>(), Started);
	TestTrue(TEXT("one char over cap"), FPacketCodec::Measure(Event, Policy, Allocator, Layout) == EMeasure::ScalarOnly);
	Started.Url = TEXT("safe");
	for (int32 Index = 0; Index < 257; ++Index)
		Started.Headers.Add(FString::Printf(TEXT("header-%d"), Index), TEXT("value"));
	Event.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceStarted>(), Started);
	TestTrue(TEXT("257 headers fallback"), FPacketCodec::Measure(Event, Policy, Allocator, Layout) == EMeasure::ScalarOnly);
	Policy.bHeaders = false;
	TestTrue(TEXT("disabled headers do not trigger cap"), FPacketCodec::Measure(Event, Policy, Allocator, Layout) == EMeasure::Full);
	Policy.bHeaders = true;
	FVaRestTraceCompleted Completed;
	Completed.RequestId = 73;
	Completed.Response.Status = 201;
	Completed.Outcome = EVaRestTraceOutcome::Queued;
	Completed.CacheLookup = EVaRestTraceCacheLookup::HitDisk;
	Completed.bFromCache = true;
	Completed.Error.Message = FString::ChrN(FNormalizedLimits::MaxStringChars + 1, TCHAR('x'));
	Event.Kind = EVaRestTraceKind::Completed;
	Event.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceCompleted>(), Completed);
	if (!TestTrue(TEXT("completed scalar sizing"), FPacketCodec::Measure(Event, Policy, Allocator, Layout) == EMeasure::ScalarOnly))
		return false;
	auto ScalarAccount = MakeShared<FPendingAccount, ESPMode::ThreadSafe>(4, Base.GetValue() + Layout.PendingCharge);
	{
		FCaptureQueue Queue(Policy, 2, ScalarAccount, Allocator);
		const int64 Copies = Probe.TextBytesCopied.load();
		TestTrue(TEXT("scalar independently admitted"), Queue.TryPush(Event) == EPush::ScalarOnly);
		TestEqual(TEXT("scalar has no text copy"), Probe.TextBytesCopied.load(), Copies);
		TestEqual(TEXT("metadata omission counted"), ScalarAccount->MetadataOmitted.load(), uint64(1));
		auto Packet = Queue.TryPopMetadata();
		if (!TestTrue(TEXT("scalar actually queued"), Packet.IsSet()))
			return false;
		TestTrue(TEXT("scalar marked omitted"), Packet->Fixed().Header.bMetadataOmitted);
		TestEqual(TEXT("scalar request"), Packet->Fixed().Scalars.RequestId, uint64(73));
		TestEqual(TEXT("scalar response"), Packet->Fixed().Scalars.Status, 201);
		TestTrue(TEXT("scalar outcome"), Packet->Fixed().Scalars.Outcome == EVaRestTraceOutcome::Queued);
		TestTrue(TEXT("scalar cache"), Packet->Fixed().Scalars.bFromCache);
	}
	auto DeniedScalar = MakeShared<FPendingAccount, ESPMode::ThreadSafe>(4, Base.GetValue() + Layout.PendingCharge - 1);
	{
		FCaptureQueue Queue(Policy, 3, DeniedScalar, Allocator);
		TestTrue(TEXT("one-byte-short scalar refused"), Queue.TryPush(Event) == EPush::Dropped);
		TestFalse(TEXT("refused scalar not queued"), Queue.TryPopMetadata().IsSet());
		TestEqual(TEXT("refused scalar no ordinal"), Queue.LastPublishedMetadata(), uint64(0));
	}
	Completed.Error.Message = TEXT("small");
	const FString Large = FString::ChrN(FNormalizedLimits::MaxStringChars, TCHAR('w'));
	for (int32 Index = 0; Index < 32; ++Index)
		Completed.DecodeWarnings.Add({Large, Large});
	Event.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceCompleted>(), MoveTemp(Completed));
	TestTrue(TEXT("aggregate metadata over 1MiB"), FPacketCodec::Measure(Event, Policy, Allocator, Layout) == EMeasure::ScalarOnly);
	TestEqual(TEXT("all boundary backing freed"), Probe.LiveBacking.load(), int64(0));
	TestTrue(TEXT("actual peak within reserved account"), Account->PeakBytes.load() <= Account->ByteLimit);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorQueueConcurrentTest,
	"VaRest.Inspector.Store.QueueConcurrent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorQueueConcurrentTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Inspector::Private;
	FAllocationCounters Probe;
	FAllocationAdapter Allocator(Probe, GetSupportedAllocator());
	if (!Testing::CalibrateForTest(*this, Allocator))
		return false;
	FCapturePolicy Policy;
	auto Account = MakeShared<FPendingAccount, ESPMode::ThreadSafe>(32, 8 * 1024 * 1024);
	FEvent* Reserved = FPlatformProcess::GetSynchEventFromPool(true);
	FEvent* Publish = FPlatformProcess::GetSynchEventFromPool(true);
	FEvent* ReleaseReserved = FPlatformProcess::GetSynchEventFromPool(true);
	FEvent* ReleasePublish = FPlatformProcess::GetSynchEventFromPool(true);
	ON_SCOPE_EXIT
	{
		ReleaseReserved->Trigger();
		ReleasePublish->Trigger();
		FPlatformProcess::ReturnSynchEventToPool(Reserved);
		FPlatformProcess::ReturnSynchEventToPool(Publish);
		FPlatformProcess::ReturnSynchEventToPool(ReleaseReserved);
		FPlatformProcess::ReturnSynchEventToPool(ReleasePublish);
	};
	FQueueTestHooks Hooks;
	Hooks.ReservedRequestId = 1;
	Hooks.Reserved = Reserved;
	Hooks.ReleaseReserved = ReleaseReserved;
	Hooks.BeforeEnqueueRequestId = 2;
	Hooks.BeforeEnqueue = Publish;
	Hooks.ReleaseEnqueue = ReleasePublish;
	TSharedPtr<FCaptureQueue, ESPMode::ThreadSafe> Queue = MakeShared<FCaptureQueue, ESPMode::ThreadSafe>(Policy, 1, Account, Allocator);
	Queue->SetTestHooks(&Hooks);
	auto Launch = [&](uint64 RequestId) {
		return Async(EAsyncExecution::Thread, [Owner = Queue, RequestId]() mutable {
			FVaRestTraceExcerptEvent Excerpt;
			Excerpt.RequestId = RequestId;
			Excerpt.DispatchId = RequestId + 10;
			Excerpt.Excerpt.StopReason = TEXT("worker borrowed text");
			FVaRestTraceEvent Event;
			Event.Kind = EVaRestTraceKind::Excerpt;
			Event.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceExcerptEvent>(), MoveTemp(Excerpt));
			const EPush Result = Owner->TryPush(Event);
			Owner.Reset();
			return Result;
		});
	};
	auto First = Launch(1);
	auto Second = Launch(2);
	ON_SCOPE_EXIT
	{
		// Release every owned barrier before joining, including failed assertions.
		ReleaseReserved->Trigger();
		ReleasePublish->Trigger();
		if (First.IsValid())
			First.Wait();
		if (Second.IsValid())
			Second.Wait();
	};
	if (!TestTrue(TEXT("worker reserved"), Reserved->Wait(5000)) ||
		!TestTrue(TEXT("worker immediately before Enqueue"), Publish->Wait(5000)))
		return false;
	TestEqual(TEXT("two held reservation counts"), Account->Count.load(), int64(2));
	TestEqual(TEXT("two unpublished excerpts"), Queue->OutstandingExcerptReservations(), int64(2));
	TestFalse(TEXT("held excerpt lane transient empty"), Queue->TryPopExcerpt().IsSet());
	FVaRestTraceEvent Metadata;
	FVaRestTraceStarted Started;
	Started.RequestId = 100;
	Started.Url = TEXT("metadata remains independent");
	Metadata.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceStarted>(), MoveTemp(Started));
	TestTrue(TEXT("metadata publishes while excerpts held"), Queue->TryPush(Metadata) == EPush::Accepted);
	auto Meta = Queue->TryPopMetadata();
	if (!TestTrue(TEXT("metadata independently consumable"), Meta.IsSet()))
		return false;
	TestEqual(TEXT("only metadata advances ordinal"), Queue->LastPublishedMetadata(), uint64(1));
	Meta.Reset();
	TSharedPtr<FCaptureQueue, ESPMode::ThreadSafe> Other = MakeShared<FCaptureQueue, ESPMode::ThreadSafe>(Policy, 2, Account, Allocator);
	TSharedPtr<FCaptureQueue, ESPMode::ThreadSafe> Third = MakeShared<FCaptureQueue, ESPMode::ThreadSafe>(Policy, 3, Account, Allocator);
	TestTrue(TEXT("second generation shared account"), Other->TryPush(Metadata) == EPush::Accepted);
	TestTrue(TEXT("third generation shared account"), Third->TryPush(Metadata) == EPush::Accepted);
	auto OffGT = Async(EAsyncExecution::Thread, [Other, &Metadata]() mutable {
		const EPush Result = Other->TryPush(Metadata);
		// Async fulfills the promise before deleting its captured callable.
		Other.Reset();
		return Result;
	});
	TestTrue(TEXT("off-GT metadata dropped"), OffGT.Get() == EPush::Dropped);
	TestEqual(TEXT("off-GT no ordinal"), Other->LastPublishedMetadata(), uint64(1));
	Queue->Close();
	Queue.Reset();
	Other->Close();
	Third->Close();
	Other.Reset();
	Third.Reset();
	ReleaseReserved->Trigger();
	ReleasePublish->Trigger();
	TestTrue(TEXT("already reserved first finishes old generation"), First.Get() == EPush::Accepted);
	TestTrue(TEXT("already reserved second finishes old generation"), Second.Get() == EPush::Accepted);
	TestEqual(TEXT("all generation counts freed"), Account->Count.load(), int64(0));
	TestEqual(TEXT("all generation bytes freed"), Account->Bytes.load(), int64(0));
	TestEqual(TEXT("actual node and packet backing freed"), Probe.LiveBacking.load(), int64(0));
	TestTrue(TEXT("summed generation byte peak bounded"), Account->PeakBytes.load() <= Account->ByteLimit);
	return true;
}

#endif
