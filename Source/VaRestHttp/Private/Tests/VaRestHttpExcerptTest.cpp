// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "VaRestAuth.h"
#include "VaRestClient.h"
#include "VaRestClientState.h"
#include "VaRestHttpTesting.h"
#include "VaRestRedactionPipe.h"
#include "VaRestSha256.h"
#include "VaRestTraceExcerpts.h"

#include <atomic>

namespace VaRest::Http::ExcerptTestPrivate
{
	FVaRestFakeResponse Json(int32 Code, const char* Text)
	{
		return FVaRestFakeResponse::Json(Code, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text)));
	}

	/**
	 * The first excerpt of Owner's, copied out: ExcerptsOf returns a temporary, so a pointer into it would dangle at the end
	 * of the statement that found it
	 */
	TOptional<FVaRestTraceExcerptEvent> Find(const TArray<FVaRestTraceExcerptEvent>& List, EVaRestTraceExcerptOwner Owner)
	{
		const FVaRestTraceExcerptEvent* Found = List.FindByPredicate([Owner](const FVaRestTraceExcerptEvent& One) { return One.Owner == Owner; });
		return Found ? TOptional<FVaRestTraceExcerptEvent>(*Found) : TOptional<FVaRestTraceExcerptEvent>();
	}

	/** Cancels what the rig has in flight and pumps until it is done: the drain of a test that stops early (spec review M-5) */
	void Abandon(VaRest::Http::Testing::FTestRig& Rig)
	{
		Rig.Client.CancelAll();
		VaRest::Http::Testing::PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });
	}

	/** The first event of Kind for request Id, its payload copied out (For() returns a copy of the list) */
	template <typename PayloadType>
	TOptional<PayloadType> First(const VaRest::Http::Testing::FTraceRecorder& Recorder, uint64 Id, EVaRestTraceKind Kind)
	{
		for (const FVaRestTraceEvent& Event : Recorder.For(Id))
		{
			if (Event.Kind == Kind)
			{
				return Event.Payload.Get<PayloadType>();
			}
		}
		return TOptional<PayloadType>();
	}

	/** The Seq of request Id's first event of Kind (for an excerpt, of Owner's); 0 when there is none */
	uint64 SeqOf(const VaRest::Http::Testing::FTraceRecorder& Recorder, uint64 Id, EVaRestTraceKind Kind, EVaRestTraceExcerptOwner Owner = EVaRestTraceExcerptOwner::Request)
	{
		for (const FVaRestTraceEvent& Event : Recorder.For(Id))
		{
			if (Event.Kind == Kind && (Kind != EVaRestTraceKind::Excerpt || Event.Payload.Get<FVaRestTraceExcerptEvent>().Owner == Owner))
			{
				return Event.Seq;
			}
		}
		return 0;
	}

	/** The index of the fake's first call to a URL ending in Path, else INDEX_NONE */
	int32 FindCall(const VaRest::Http::Testing::FTestRig& Rig, const TCHAR* Path)
	{
		for (int32 Index = 0; Index < Rig.Fake->NumCalls(); ++Index)
		{
			if (Rig.Fake->GetCall(Index).Request.Url.EndsWith(Path))
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	/**
	 * A response excerpt job built by hand, as PrepareResponse builds one: Text's bytes copied and admitted, a text body to
	 * redact with an empty redactor. Unset (Redact empty) when the admission counter refused it.
	 */
	VaRest::Http::Excerpts::FJob MakeTextJob(uint64 RequestId, const char* Text)
	{
		using namespace VaRest::Http::Excerpts;
		TArray<uint8> Body(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text));
		FJob Job;
		TOptional<FAdmission> Admission = TryAdmit(Body.Num());
		if (!Admission.IsSet())
		{
			return Job;
		}
		Job.Owner = EVaRestTraceExcerptOwner::Response;
		Job.RequestId = RequestId;
		Job.Attempt = 1;
		Job.TotalBytes = Body.Num();
		Job.Admitted = MoveTemp(Admission.GetValue());
		Job.Redactor = MakeShared<const FVaRestRedactor, ESPMode::ThreadSafe>();
		Job.Redact = [Copy = MoveTemp(Body)](const FVaRestRedactor& Redactor)
		{
			return Redactor.RedactBody(TEXT("text/plain"), Copy, Copy.Num(), 1024);
		};
		return Job;
	}

	/** Holds a test-owned gate's pipe busy until Release is set (or 10 s pass): what is launched behind it waits */
	void HoldPipe(VaRest::Http::FRedactionGate& Gate, const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe>& Release)
	{
		Gate.Launch(TEXT("VaRestTests.HoldPipe"), [Release]
		{
			const double Until = FPlatformTime::Seconds() + 10.0;
			while (!Release->load() && FPlatformTime::Seconds() < Until)
			{
				FPlatformProcess::Sleep(0.001f);
			}
		});
	}
}

#if VAREST_WITH_TRACE_EXCERPTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpExcerptsTest, "VaRest.Http.Trace.Excerpts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpExcerptsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::ExcerptTestPrivate;
	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("VaRestExcerptTest") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*Dir, true);
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Dir, false, true); };
	const FString Upload = Dir / TEXT("upload.bin");
	if (!TestTrue(TEXT("fixture written"), FFileHelper::SaveStringToFile(TEXT("abc"), *Upload)))
	{
		return false;
	}
	FTestRig Rig;
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request)
	{
		if (Request.Url.EndsWith(TEXT("/login")))
		{
			return Json(200, R"({"accessToken":"tok-abcdef1234","name":"neo"})");
		}
		if (Request.Url.EndsWith(TEXT("/bytes")))
		{
			return FVaRestFakeResponse::Bytes(200, TArray<uint8>{ 1, 2, 3, 4 });
		}
		if (Request.Url.EndsWith(TEXT("/missing")))
		{
			return FVaRestFakeResponse::Text(404, TEXT("no such thing"));
		}
		return Json(200, "{}");
	});
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Bodies, /*BodyCap*/ 1024);
	const FTraceRecorder& Recorder = *Scoped.Recorder;
	auto Login = MakeOutcome<FVaRestJson>();
	auto Bytes = MakeOutcome<TArray<uint8>>();
	auto Missing = MakeOutcome<FVaRestJson>();
	auto Parts = MakeOutcome<FVaRestJson>();
	auto File = MakeOutcome<FVaRestJson>();
	auto Empty = MakeOutcome<FVaRestJson>();
	const uint64 LoginId = Observe(Rig.Client.Post(TEXT("/login")).Body(FVaRestJson::Object().Set(TEXT("user"), TEXT("neo")).Set(TEXT("password"), TEXT("trinity-123"))), Login).Send().GetId();
	const uint64 BytesId = Observe(Rig.Client.Get<TArray<uint8>>(TEXT("/bytes")), Bytes).Send().GetId();
	const uint64 MissingId = Observe(Rig.Client.Get(TEXT("/missing")), Missing).Send().GetId();
	const uint64 PartsId = Observe(Rig.Client.Post(TEXT("/parts")).Body(FVaRestMultipart().AddField(TEXT("secret"), TEXT("shh-123456")).AddField(TEXT("name"), TEXT("neo"))), Parts).Send().GetId();
	const uint64 FileId = Observe(Rig.Client.Put(TEXT("/file")).BodyFile(Upload), File).Send().GetId();
	const uint64 EmptyId = Observe(Rig.Client.Get(TEXT("/empty")), Empty).Send().GetId();
	if (!TestTrue(TEXT("all complete"), PumpUntil([&] { return Login->IsDone() && Bytes->IsDone() && Missing->IsDone() && Parts->IsDone() && File->IsDone() && Empty->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	FlushRedactionPipe();
	Rig.Drain();

	const TArray<FVaRestTraceExcerptEvent> LoginExcerpts = Recorder.ExcerptsOf(LoginId);
	const TOptional<FVaRestTraceExcerptEvent> Sent = Find(LoginExcerpts, EVaRestTraceExcerptOwner::Request);
	const TOptional<FVaRestTraceExcerptEvent> Got = Find(LoginExcerpts, EVaRestTraceExcerptOwner::Response);
	if (!TestTrue(TEXT("IN-6: a request and a response excerpt"), Sent.IsSet() && Got.IsSet()))
	{
		return false;
	}
	TestTrue(TEXT("the request's, before any attempt, redacted"), Sent->Attempt == 0 && Sent->Excerpt.Kind == EVaRestExcerptKind::Json
		&& Sent->Excerpt.ToText().Contains(TEXT("\"password\":\"***\"")) && Sent->Excerpt.ToText().Contains(TEXT("\"user\":\"neo\"")));
	TestTrue(TEXT("the response's, for attempt 1, redacted on the decode task"), Got->Attempt == 1 && Got->Excerpt.ToText().Contains(TEXT("\"accessToken\":\"***\""))
		&& !Got->Excerpt.ToText().Contains(TEXT("tok-abcdef1234")));
	TestTrue(TEXT("... while the decode still read the real body"), Login->Value.IsSet() && Login->Value->Get(TEXT("name")).AsString() == TEXT("neo"));
	const TOptional<FVaRestTraceStarted> LoginStarted = First<FVaRestTraceStarted>(Recorder, LoginId, EVaRestTraceKind::Started);
	TestTrue(TEXT("IN-1: Started says an excerpt is coming"), LoginStarted.IsSet() && LoginStarted->bExcerptPending);
	const TOptional<FVaRestTraceAttemptEnded> LoginEnded = First<FVaRestTraceAttemptEnded>(Recorder, LoginId, EVaRestTraceKind::AttemptEnded);
	TestTrue(TEXT("IN-3: so does AttemptEnded"), LoginEnded.IsSet() && LoginEnded->bExcerptPending);
	// Started is emitted before the request's redaction is launched (and AttemptEnded before the decode task starts), so
	// each excerpt is stamped after the event that announced it
	const uint64 StartedSeq = SeqOf(Recorder, LoginId, EVaRestTraceKind::Started);
	const uint64 RequestSeq = SeqOf(Recorder, LoginId, EVaRestTraceKind::Excerpt, EVaRestTraceExcerptOwner::Request);
	const uint64 EndedSeq = SeqOf(Recorder, LoginId, EVaRestTraceKind::AttemptEnded);
	const uint64 ResponseSeq = SeqOf(Recorder, LoginId, EVaRestTraceKind::Excerpt, EVaRestTraceExcerptOwner::Response);
	TestTrue(TEXT("the request's excerpt follows Started"), StartedSeq > 0 && RequestSeq > StartedSeq);
	TestTrue(TEXT("the response's follows AttemptEnded"), EndedSeq > 0 && ResponseSeq > EndedSeq);

	const TOptional<FVaRestTraceExcerptEvent> Binary = Find(Recorder.ExcerptsOf(BytesId), EVaRestTraceExcerptOwner::Response);
	TestTrue(TEXT("raw bytes: a descriptor with the SHA-256 prefix (copied at the hop, the whole body under the cap)"), Binary.IsSet()
		&& Binary->Excerpt.Kind == EVaRestExcerptKind::Binary && Binary->Excerpt.Sha256Prefix == VaRest::Http::FSha256::Hash(TArray<uint8>{ 1, 2, 3, 4 }).ToHex().Left(8));
	const TOptional<FVaRestTraceExcerptEvent> Error = Find(Recorder.ExcerptsOf(MissingId), EVaRestTraceExcerptOwner::Response);
	TestTrue(TEXT("an error's body: text"), Error.IsSet() && Error->Excerpt.Kind == EVaRestExcerptKind::Text && Error->Excerpt.ToText() == TEXT("no such thing"));
	const TOptional<FVaRestTraceExcerptEvent> Multipart = Find(Recorder.ExcerptsOf(PartsId), EVaRestTraceExcerptOwner::Request);
	TestTrue(TEXT("multipart: parts, a secret field redacted"), Multipart.IsSet() && Multipart->Excerpt.Kind == EVaRestExcerptKind::Multipart
		&& !Multipart->Excerpt.ToText().Contains(TEXT("shh-123456")));
	const TOptional<FVaRestTraceExcerptEvent> Stream = Find(Recorder.ExcerptsOf(FileId), EVaRestTraceExcerptOwner::Request);
	TestTrue(TEXT("a BodyFile: a stream descriptor"), Stream.IsSet() && Stream->Excerpt.Kind == EVaRestExcerptKind::Stream && Stream->Excerpt.TotalBytes == 3);
	const TOptional<FVaRestTraceStarted> EmptyStarted = First<FVaRestTraceStarted>(Recorder, EmptyId, EVaRestTraceKind::Started);
	TestTrue(TEXT("no body: nothing pending"), EmptyStarted.IsSet() && !EmptyStarted->bExcerptPending && !Find(Recorder.ExcerptsOf(EmptyId), EVaRestTraceExcerptOwner::Request).IsSet());
	TestTrue(TEXT("Inspector 9: only excerpts arrive off the game thread"), Recorder.OnlyExcerptsOffGameThread());
	TestEqual(TEXT("every admitted copy released"), VaRest::Http::Excerpts::PendingBytes(), static_cast<int64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpExcerptBacklogTest, "VaRest.Http.Trace.ExcerptBacklog",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpExcerptBacklogTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::ExcerptTestPrivate;
	FTestRig Rig;
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Bodies, 4096);
	Scoped.Recorder->Detail.MaxPendingExcerptBytes = 100;
	FVaRestTrace::RefreshDetail();
	auto Big = MakeOutcome<FVaRestJson>();
	auto Small = MakeOutcome<FVaRestJson>();
	const uint64 BigId = Observe(Rig.Client.Post(TEXT("/big")).Body(FString::ChrN(1000, TEXT('x'))), Big).Send().GetId();
	if (!TestTrue(TEXT("completes"), Rig.Wait(Big)))
	{
		Abandon(Rig);
		return false;
	}
	const uint64 SmallId = Observe(Rig.Client.Post(TEXT("/small")).Body(FString::ChrN(50, TEXT('y'))), Small).Send().GetId();
	if (!TestTrue(TEXT("completes"), Rig.Wait(Small)))
	{
		Abandon(Rig);
		return false;
	}
	FlushRedactionPipe();
	const TOptional<FVaRestTraceExcerptEvent> Skipped = Find(Scoped.Recorder->ExcerptsOf(BigId), EVaRestTraceExcerptOwner::Request);
	TestTrue(TEXT("IN-14: past the admission cap, Skipped: Backlog, nothing copied"), Skipped.IsSet() && Skipped->Excerpt.Kind == EVaRestExcerptKind::Skipped
		&& Skipped->Excerpt.SkipReason == EVaRestExcerptSkip::Backlog && Skipped->Excerpt.TotalBytes == 1000);
	const TOptional<FVaRestTraceExcerptEvent> Admitted = Find(Scoped.Recorder->ExcerptsOf(SmallId), EVaRestTraceExcerptOwner::Request);
	TestTrue(TEXT("under it: the excerpt"), Admitted.IsSet() && Admitted->Excerpt.Kind == EVaRestExcerptKind::Text);
	TestEqual(TEXT("the counter is back at 0"), VaRest::Http::Excerpts::PendingBytes(), static_cast<int64>(0));

	// Concurrency review M-12: an admission is counted out when its holder goes -- a job prepared and never launched, a
	// refused launch -- not only after a redaction ran; a leaked count would make every later excerpt Skipped: Backlog
	{
		TOptional<VaRest::Http::Excerpts::FAdmission> Held = VaRest::Http::Excerpts::TryAdmit(60);
		TestTrue(TEXT("M-12: admitted under the cap"), Held.IsSet() && VaRest::Http::Excerpts::PendingBytes() == 60);
		TestFalse(TEXT("... and the cap still holds"), VaRest::Http::Excerpts::TryAdmit(60).IsSet());
		TOptional<VaRest::Http::Excerpts::FAdmission> Moved = MoveTemp(Held);
		TestEqual(TEXT("... a move keeps one count"), VaRest::Http::Excerpts::PendingBytes(), static_cast<int64>(60));
	}
	TestEqual(TEXT("... and dropping it counts it out"), VaRest::Http::Excerpts::PendingBytes(), static_cast<int64>(0));
	return true;
}
#endif // VAREST_WITH_TRACE_EXCERPTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpExcerptNeedBodiesTest, "VaRest.Http.Trace.ExcerptsNeedBodies",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpExcerptNeedBodiesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::ExcerptTestPrivate;
	// IN-11: without Bodies in the mask no byte is copied for the trace
	{
		FTestRig Rig;
		FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
		auto Outcome = MakeOutcome<FVaRestJson>();
		const uint64 Id = Observe(Rig.Client.Post(TEXT("/x")).Body(TEXT("hello")), Outcome).Send().GetId();
		if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)))
		{
			Abandon(Rig);
			return false;
		}
		FlushRedactionPipe();
		const TOptional<FVaRestTraceStarted> Started = First<FVaRestTraceStarted>(*Scoped.Recorder, Id, EVaRestTraceKind::Started);
		TestTrue(TEXT("nothing pending, nothing sent"), Started.IsSet() && !Started->bExcerptPending && Scoped.Recorder->ExcerptsOf(Id).Num() == 0);
	}
#if !VAREST_WITH_TRACE_EXCERPTS
	// VaRestTrace.h: with VAREST_WITH_TRACE_EXCERPTS 0 nothing is copied even for a sink that wants Bodies, and neither
	// Started nor AttemptEnded says an excerpt is pending
	{
		FTestRig Rig;
		FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Bodies, 1024);
		auto Outcome = MakeOutcome<FVaRestJson>();
		const uint64 Id = Observe(Rig.Client.Post(TEXT("/x")).Body(TEXT("hello")), Outcome).Send().GetId();
		if (!TestTrue(TEXT("compiled out: completes"), Rig.Wait(Outcome)))
		{
			Abandon(Rig);
			return false;
		}
		FlushRedactionPipe();
		const TOptional<FVaRestTraceStarted> Started = First<FVaRestTraceStarted>(*Scoped.Recorder, Id, EVaRestTraceKind::Started);
		const TOptional<FVaRestTraceAttemptEnded> Ended = First<FVaRestTraceAttemptEnded>(*Scoped.Recorder, Id, EVaRestTraceKind::AttemptEnded);
		TestTrue(TEXT("compiled out: nothing pending, nothing sent, with Bodies asked for"), Started.IsSet() && !Started->bExcerptPending && Ended.IsSet()
			&& !Ended->bExcerptPending && Scoped.Recorder->ExcerptsOf(Id).Num() == 0);
	}
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpExcerptAdmissionMergeTest, "VaRest.Http.Trace.ExcerptAdmissionMerge",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpExcerptAdmissionMergeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	// Task 5 review minor 2: a sink's MaxPendingExcerptBytes of 0 is the default, 16 MiB; the merge maps it before taking
	// the largest, so a sink asking for less never shrinks a default sink's budget. Only a sink that receives excerpts has a say.
	constexpr int64 Default = FVaRestTraceDetailSettings::DefaultMaxPendingExcerptBytes;
	TestEqual(TEXT("the default is 16 MiB"), Default, static_cast<int64>(16 * 1024 * 1024));
	FVaRestScopedSinkIsolation Isolation;
	TestEqual(TEXT("no sink: the default"), VaRest::Http::Excerpts::AdmissionCap(), Default);
	const TSharedRef<FTraceRecorder, ESPMode::ThreadSafe> Defaulted = MakeShared<FTraceRecorder, ESPMode::ThreadSafe>(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Bodies);
	const TSharedRef<FTraceRecorder, ESPMode::ThreadSafe> Small = MakeShared<FTraceRecorder, ESPMode::ThreadSafe>(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Bodies);
	Small->Detail.MaxPendingExcerptBytes = 1234;
	const TSharedRef<FTraceRecorder, ESPMode::ThreadSafe> MetadataOnly = MakeShared<FTraceRecorder, ESPMode::ThreadSafe>(EVaRestTraceDetail::Metadata);
	FVaRestTrace::AddSink(Defaulted);
	FVaRestTrace::AddSink(Small);
	TestEqual(TEXT("a default sink and a 1234 sink: the default's 16 MiB"), FVaRestTrace::GetDetailSettings().MaxPendingExcerptBytes, Default);
	TestEqual(TEXT("... the cap the admission counter uses"), VaRest::Http::Excerpts::AdmissionCap(), Default);
	FVaRestTrace::RemoveSink(Defaulted);
	TestEqual(TEXT("the 1234 sink alone: 1234"), FVaRestTrace::GetDetailSettings().MaxPendingExcerptBytes, static_cast<int64>(1234));
	TestEqual(TEXT("... the cap the admission counter uses"), VaRest::Http::Excerpts::AdmissionCap(), static_cast<int64>(1234));
	FVaRestTrace::AddSink(MetadataOnly);
	TestEqual(TEXT("a Metadata sink's default has no say"), VaRest::Http::Excerpts::AdmissionCap(), static_cast<int64>(1234));
	FVaRestTrace::RemoveSink(MetadataOnly);
	FVaRestTrace::RemoveSink(Small);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpExcerptAdmissionPathsTest, "VaRest.Http.Trace.ExcerptAdmissionPaths",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpExcerptAdmissionPathsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::ExcerptTestPrivate;
	// Concurrency review M-12: the admission counter goes down exactly once on every path a copy can take -- redacted,
	// dropped because no sink wants it any more, the pipe stopping, a refused launch -- and never for a moved-from holder
	if (!TestEqual(TEXT("nothing pending at the start"), Excerpts::PendingBytes(), static_cast<int64>(0)))
	{
		return false;
	}
	FVaRestScopedSinkIsolation Quiet;

	// A move hands the count over; Reset counts it out once, however often it is called
	{
		Excerpts::FJob Held = MakeTextJob(FVaRestTrace::NewRequestId(), "abcde");
		if (!TestTrue(TEXT("admitted"), Held.IsSet()))
		{
			return false;
		}
		TestEqual(TEXT("one copy pending"), Excerpts::PendingBytes(), static_cast<int64>(5));
		Excerpts::FJob Moved = MoveTemp(Held);
		TestEqual(TEXT("a moved job keeps one count"), Excerpts::PendingBytes(), static_cast<int64>(5));
		TestEqual(TEXT("... the moved-from one holds none"), Held.Admitted.Num(), static_cast<int64>(0));
		Excerpts::FAdmission Assigned;
		Assigned = MoveTemp(Moved.Admitted);
		TestEqual(TEXT("a move assignment keeps one count"), Excerpts::PendingBytes(), static_cast<int64>(5));
		Assigned.Reset();
		Assigned.Reset();
		TestEqual(TEXT("Reset twice: counted out once"), Excerpts::PendingBytes(), static_cast<int64>(0));
	}
	TestEqual(TEXT("... and the moved-from holders count nothing out when they go"), Excerpts::PendingBytes(), static_cast<int64>(0));

	// Launched when no sink wants bodies (the one that asked detached while the announcing event was emitted): dropped
	{
		Excerpts::FJob Job = MakeTextJob(FVaRestTrace::NewRequestId(), "abcde");
		TestEqual(TEXT("no sink: one copy pending"), Excerpts::PendingBytes(), static_cast<int64>(5));
		Job.Launch();
		TestEqual(TEXT("... dropped at the launch, counted out"), Excerpts::PendingBytes(), static_cast<int64>(0));
	}

#if VAREST_WITH_TRACE_EXCERPTS
	// (Compiled out, every launch is dropped as above: nothing reaches a pipe.)
	// The pipe stops while admissions are pending: Stop waits for the queued redactions, and each counts itself out once
	{
		FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Bodies, 1024);
		const uint64 Id = FVaRestTrace::NewRequestId();
		struct FStopProbe
		{
			TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> Release = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
			int64 PendingAtStop = -1;
		};
		FStopProbe Probe;
		FRedactionGate Gate;
		// Set before any launch on this gate and cleared after Stop (SetTestHook's contract); the gate and Probe outlive Stop
		Gate.SetTestHook([](void* Context, FRedactionGate::EHookPoint Point)
		{
			if (Point == FRedactionGate::EHookPoint::StopAfterDrain)
			{
				FStopProbe& Seen = *static_cast<FStopProbe*>(Context);
				Seen.PendingAtStop = Excerpts::PendingBytes();
				Seen.Release->store(true);
			}
		}, &Probe);
		Gate.Start();
		HoldPipe(Gate, Probe.Release);
		for (int32 Index = 0; Index < 3; ++Index)
		{
			Excerpts::FJob Job = MakeTextJob(Id, "abcde");
			Job.LaunchOn(Gate);
		}
		TestEqual(TEXT("three admitted copies wait behind the held pipe"), Excerpts::PendingBytes(), static_cast<int64>(15));
		Gate.Stop();
		TestEqual(TEXT("... still pending when Stop had drained the launchers"), Probe.PendingAtStop, static_cast<int64>(15));
		TestEqual(TEXT("... each counted out once when Stop returns"), Excerpts::PendingBytes(), static_cast<int64>(0));
		TestEqual(TEXT("... their excerpts came"), Scoped.Recorder->ExcerptsOf(Id).Num(), 3);
		// A launch on the stopped gate is refused: the dropped body takes its admission along
		Excerpts::FJob Late = MakeTextJob(Id, "abcde");
		TestEqual(TEXT("a copy for the stopped pipe"), Excerpts::PendingBytes(), static_cast<int64>(5));
		Late.LaunchOn(Gate);
		TestEqual(TEXT("... refused, counted out"), Excerpts::PendingBytes(), static_cast<int64>(0));
		TestEqual(TEXT("... and its excerpt never comes"), Scoped.Recorder->ExcerptsOf(Id).Num(), 3);
		Gate.SetTestHook(nullptr, nullptr);
	}

	// Every sink that wanted bodies detached while the copy waited on the pipe: dropped there, counted out, nothing emitted
	{
		const uint64 Id = FVaRestTrace::NewRequestId();
		const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> Release = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
		FRedactionGate Gate;
		Gate.Start();
		HoldPipe(Gate, Release);
		TSharedPtr<FTraceRecorder, ESPMode::ThreadSafe> Kept;
		{
			FScopedTraceRecorder Inner(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Bodies, 1024);
			Kept = Inner.Recorder;
			Excerpts::FJob Job = MakeTextJob(Id, "abcde");
			Job.LaunchOn(Gate);
			TestEqual(TEXT("a copy waits on the pipe"), Excerpts::PendingBytes(), static_cast<int64>(5));
		}
		Release->store(true);
		Gate.Wait();
		TestEqual(TEXT("its sink gone: counted out on the pipe"), Excerpts::PendingBytes(), static_cast<int64>(0));
		TestEqual(TEXT("... and nothing emitted"), Kept->ExcerptsOf(Id).Num(), 0);
		Gate.Stop();
	}
#endif
	return true;
}

#if VAREST_WITH_TRACE_EXCERPTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpExcerptAppliedTokenTest, "VaRest.Http.Trace.ExcerptAppliedToken",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpExcerptAppliedTokenTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::ExcerptTestPrivate;
	// Sinks receive only redacted data: an excerpt is redacted with the request's union redactor (RedactorFor). A token
	// set after Send() goes out because AuthApply writes the session when the attempt leaves the limiter's queue (Task 12
	// review M-7); it is in neither the Send() snapshot nor, after a logout while the transport has the request, the
	// client's current redactor. The responses echo it under a key no rule names, so only the value scrub hides it: in a
	// decoded 2xx (the decode task's excerpt) and in an error body (copied at the hop, redacted on the pipe).
	const FString Token = TEXT("tok-late-abcdef123");
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(2).WithAuth(FVaRestAuth::Bearer());
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	const VaRest::Http::FLimiter& Slots = VaRest::Http::FClientAccess::State(Rig.Client)->GetLimiter();
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Bodies, 1024);
	auto HolderA = MakeOutcome<FVaRestJson>();
	auto HolderB = MakeOutcome<FVaRestJson>();
	auto Decoded = MakeOutcome<FVaRestJson>();
	auto Raw = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/hold/a")).NoAuth(), HolderA).Send();
	Observe(Rig.Client.Get(TEXT("/hold/b")).NoAuth(), HolderB).Send();
	const uint64 DecodedId = Observe(Rig.Client.Get(TEXT("/late/json")), Decoded).Send().GetId();
	const uint64 RawId = Observe(Rig.Client.Get(TEXT("/late/text")), Raw).Send().GetId();
	if (!TestTrue(TEXT("no session at Send(): the holders have the slots, both wait"), PumpUntil([&Rig, &Slots] { return Rig.Fake->NumCalls() == 2 && Slots.NumQueued() == 2; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Client.SetBearerToken(Token);
	Rig.Fake->Complete(0, Json(200, "{}"));
	Rig.Fake->Complete(1, Json(200, "{}"));
	if (!TestTrue(TEXT("released: AuthApply wrote the token and the transport has both"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 4; })))
	{
		Abandon(Rig);
		return false;
	}
	const int32 JsonCall = FindCall(Rig, TEXT("/late/json"));
	const int32 TextCall = FindCall(Rig, TEXT("/late/text"));
	if (!TestTrue(TEXT("both calls found"), JsonCall != INDEX_NONE && TextCall != INDEX_NONE))
	{
		Abandon(Rig);
		return false;
	}
	TestEqual(TEXT("... with the token set while they waited"), Rig.Fake->GetCall(JsonCall).Request.Headers.Get(TEXT("Authorization")), FString(TEXT("Bearer ")) + Token);
	Rig.Client.ClearSession();
	const TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> State = VaRest::Http::FClientAccess::State(Rig.Client);
	TestEqualSensitive(TEXT("the logout: the client itself forgot the token"), State.IsValid() ? State->GetRedactor()->ScrubText(Token) : FString(), Token);
	Rig.Fake->Complete(JsonCall, FVaRestFakeResponse::Text(200, FString::Printf(TEXT("{\"note\":\"%s\"}"), *Token), TEXT("application/json")));
	Rig.Fake->Complete(TextCall, FVaRestFakeResponse::Text(404, FString::Printf(TEXT("echo %s"), *Token)));
	if (!TestTrue(TEXT("all complete"), PumpUntil([HolderA, HolderB, Decoded, Raw] { return HolderA->IsDone() && HolderB->IsDone() && Decoded->IsDone() && Raw->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	FlushRedactionPipe();
	TestTrue(TEXT("the decode read the real body"), Decoded->Value.IsSet() && Decoded->Value->Get(TEXT("note")).AsString() == Token);
	const TOptional<FVaRestTraceExcerptEvent> OnDecode = Find(Scoped.Recorder->ExcerptsOf(DecodedId), EVaRestTraceExcerptOwner::Response);
	TestTrue(TEXT("the decode task's excerpt scrubs the applied token"), OnDecode.IsSet() && OnDecode->Excerpt.Kind == EVaRestExcerptKind::Json
		&& OnDecode->Excerpt.ToText().Contains(TEXT("\"note\":\"***\"")) && !OnDecode->Excerpt.ToText().Contains(Token));
	const TOptional<FVaRestTraceExcerptEvent> OnPipe = Find(Scoped.Recorder->ExcerptsOf(RawId), EVaRestTraceExcerptOwner::Response);
	TestTrue(TEXT("... so does the pipe's"), OnPipe.IsSet() && OnPipe->Excerpt.Kind == EVaRestExcerptKind::Text && OnPipe->Excerpt.ToText() == TEXT("echo ***"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpExcerptRefreshTest, "VaRest.Http.Trace.ExcerptRefreshResponse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpExcerptRefreshTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::ExcerptTestPrivate;
	// Task 18: the refresh response holds the new tokens before FinishRefresh makes them session secrets (layer 3). Its
	// excerpt is redacted with the refresh request's RedactorFor, whose client redactor holds each token path's last segment
	// as a key (layer 2). Paths off the defaults: "bearer" and "renewal" match no built-in rule (VaRestJsonRedact.h:20-24).
	FVaRestBearerRefreshConfig Config;
	Config.AccessTokenPath = TEXT("data.bearer");
	Config.RefreshTokenPath = TEXT("/auth/renewal");
	FTestRig Rig;
	Rig.Client.WithAuth(FVaRestAuth::BearerRefresh(Config));
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request)
	{
		if (Request.Url.Contains(TEXT("/auth/")))
		{
			return Json(200, R"({"data":{"bearer":"new-access-123456"},"auth":{"renewal":"new-refresh-123456"},"expiresIn":60})");
		}
		return Request.Headers.Get(TEXT("Authorization")) == TEXT("Bearer new-access-123456") ? Json(200, R"({"ok":true})") : Json(401, "{}");
	});
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Bodies, 4096);
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/p")), Outcome).Send();
	if (!TestTrue(TEXT("the 401, the refresh, the replay"), Rig.Wait(Outcome) && Rig.Drain() && Rig.Fake->NumCalls() == 3 && Outcome->SuccessCalls.load() == 1))
	{
		Abandon(Rig);
		return false;
	}
	FlushRedactionPipe();
	const uint64 RefreshId = Rig.Fake->GetCall(1).Request.RequestId;
	const TArray<FVaRestTraceExcerptEvent> RefreshExcerpts = Scoped.Recorder->ExcerptsOf(RefreshId);
	const TOptional<FVaRestTraceExcerptEvent> Answer = Find(RefreshExcerpts, EVaRestTraceExcerptOwner::Response);
	TestTrue(TEXT("the refresh response's excerpt: both new tokens masked by their path keys"), Answer.IsSet() && Answer->Excerpt.Kind == EVaRestExcerptKind::Json
		&& Answer->Excerpt.ToText().Contains(TEXT("\"bearer\":\"***\"")) && Answer->Excerpt.ToText().Contains(TEXT("\"renewal\":\"***\""))
		&& !Answer->Excerpt.ToText().Contains(TEXT("new-access-123456")) && !Answer->Excerpt.ToText().Contains(TEXT("new-refresh-123456")));
	const TOptional<FVaRestTraceExcerptEvent> Asked = Find(RefreshExcerpts, EVaRestTraceExcerptOwner::Request);
	TestTrue(TEXT("... and its request's excerpt hides the old refresh token"), Asked.IsSet() && !Asked->Excerpt.ToText().Contains(TEXT("old-refresh-123456")));
	return true;
}
#endif // VAREST_WITH_TRACE_EXCERPTS

#endif
