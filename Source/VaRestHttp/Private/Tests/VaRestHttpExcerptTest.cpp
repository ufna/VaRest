// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "VaRestAuth.h"
#include "VaRestClient.h"
#include "VaRestClientState.h"
#include "VaRestHttpTesting.h"
#include "VaRestRedactionPipe.h"
#include "VaRestRetry.h"
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

	/**
	 * Cancels what the rig has in flight and pumps until it is done: the drain of a test that stops early (spec review M-5).
	 * The pipe is flushed too (Task 21 review M-5): a redaction still queued would emit into the next test's recorder.
	 */
	void Abandon(VaRest::Http::Testing::FTestRig& Rig)
	{
		Rig.Client.CancelAll();
		VaRest::Http::Testing::PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });
		VaRest::Http::Testing::FlushRedactionPipe();
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

	/**
	 * Task 21 review M-1: the excerpts request Id's events announced (bExcerptPending) that never came -- "request" or
	 * "response #N" -- comma-separated; empty when every announced excerpt resolved
	 */
	FString Unresolved(const VaRest::Http::Testing::FTraceRecorder& Recorder, uint64 Id)
	{
		const TArray<FVaRestTraceEvent> Events = Recorder.For(Id);
		const auto Has = [&Events](EVaRestTraceExcerptOwner Owner, uint64 DispatchId)
		{
			return Events.ContainsByPredicate([Owner, DispatchId](const FVaRestTraceEvent& Event)
			{
				return Event.Kind == EVaRestTraceKind::Excerpt && Event.Payload.Get<FVaRestTraceExcerptEvent>().Owner == Owner
					&& Event.Payload.Get<FVaRestTraceExcerptEvent>().DispatchId == DispatchId;
			});
		};
		TArray<FString> Missing;
		for (const FVaRestTraceEvent& Event : Events)
		{
			if (Event.Kind == EVaRestTraceKind::Started && Event.Payload.Get<FVaRestTraceStarted>().bExcerptPending && !Has(EVaRestTraceExcerptOwner::Request, 0))
			{
				Missing.Add(TEXT("request"));
			}
			else if (Event.Kind == EVaRestTraceKind::AttemptEnded && Event.Payload.Get<FVaRestTraceAttemptEnded>().bExcerptPending)
			{
				const uint64 DispatchId = Event.Payload.Get<FVaRestTraceAttemptEnded>().DispatchId;
				if (!Has(EVaRestTraceExcerptOwner::Response, DispatchId))
				{
					Missing.Add(FString::Printf(TEXT("response dispatch #%llu"), DispatchId));
				}
			}
		}
		return FString::Join(Missing, TEXT(", "));
	}

	/** Request Id's response excerpt events for Attempt, copied out with their Seq */
	TArray<FVaRestTraceEvent> ResponseExcerpts(const VaRest::Http::Testing::FTraceRecorder& Recorder, uint64 Id, int32 Attempt)
	{
		TArray<FVaRestTraceEvent> Out;
		for (const FVaRestTraceEvent& Event : Recorder.For(Id))
		{
			if (Event.Kind == EVaRestTraceKind::Excerpt && Event.Payload.Get<FVaRestTraceExcerptEvent>().Owner == EVaRestTraceExcerptOwner::Response
				&& Event.Payload.Get<FVaRestTraceExcerptEvent>().Attempt == Attempt)
			{
				Out.Add(Event);
			}
		}
		return Out;
	}

	/** The Seq of request Id's AttemptEnded for Attempt (Kind Completed: its Completed, Attempt ignored); 0 when there is none */
	uint64 SeqOfAttempt(const VaRest::Http::Testing::FTraceRecorder& Recorder, uint64 Id, EVaRestTraceKind Kind, int32 Attempt)
	{
		for (const FVaRestTraceEvent& Event : Recorder.For(Id))
		{
			if (Event.Kind == Kind && (Kind != EVaRestTraceKind::AttemptEnded || Event.Payload.Get<FVaRestTraceAttemptEnded>().Attempt == Attempt))
			{
				return Event.Seq;
			}
		}
		return 0;
	}

	/** True when Excerpts is one Skipped: Discarded excerpt */
	bool IsOneDiscarded(const TArray<FVaRestTraceEvent>& Excerpts)
	{
		if (Excerpts.Num() != 1)
		{
			return false;
		}
		const FVaRestTraceExcerpt& Excerpt = Excerpts[0].Payload.Get<FVaRestTraceExcerptEvent>().Excerpt;
		return Excerpt.Kind == EVaRestExcerptKind::Skipped && Excerpt.SkipReason == EVaRestExcerptSkip::Discarded;
	}

	/**
	 * A sink whose Hook sees each game-thread event (not excerpts: those may come from a worker). Hook is set before AddSink
	 * and left alone while the sink is attached.
	 */
	class FHookSink final : public IVaRestTraceSink
	{
	public:
		explicit FHookSink(EVaRestTraceDetail Flags)
		{
			Detail.Flags = Flags;
			Detail.BodyCap = 1024;
		}
		virtual FVaRestTraceDetailSettings GetDetail() const override { return Detail; }
		virtual void OnAttached(const FVaRestTraceSnapshot&) override {}
		virtual void OnEvent(const FVaRestTraceEvent& Event) override
		{
			if (Event.Kind != EVaRestTraceKind::Excerpt && Hook)
			{
				Hook(Event);
			}
		}

		FVaRestTraceDetailSettings Detail;
		TFunction<void(const FVaRestTraceEvent&)> Hook;
	};

	/** An Outer stage that answers a decodable 200 for a URL ending in /replaced with a 502 of its own: the 2xx never reaches the decoder */
	class FReplaceOkStage final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("ReplaceOk"); }
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			const bool bReplace = Exchange->GetUrl().EndsWith(TEXT("/replaced"));
			Next.Proceed(Exchange, [bReplace, Done = MoveTemp(OnOutcome)](FVaRestAttemptOutcome&& Outcome) mutable
			{
				if (bReplace && Outcome.Status == 200)
				{
					FVaRestAttemptOutcome Replaced;
					Replaced.Status = 502;
					Done(MoveTemp(Replaced));
					return;
				}
				Done(MoveTemp(Outcome));
			});
		}
	};

	/** A PerAttempt stage that turns the first 200 for a URL ending in /again into an empty 503: the retry stage re-issues it */
	class FFirstOkIsBusyStage final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("FirstOkIsBusy"); }
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			const bool bAgain = Exchange->GetUrl().EndsWith(TEXT("/again"));
			Next.Proceed(Exchange, [bAgain, Once = Turned, Done = MoveTemp(OnOutcome)](FVaRestAttemptOutcome&& Outcome) mutable
			{
				if (bAgain && Outcome.Status == 200 && !*Once)
				{
					*Once = true;
					FVaRestAttemptOutcome Busy;
					Busy.Status = 503;
					Done(MoveTemp(Busy));
					return;
				}
				Done(MoveTemp(Outcome));
			});
		}
		TSharedRef<bool, ESPMode::ThreadSafe> Turned = MakeShared<bool, ESPMode::ThreadSafe>(false);
	};

	/**
	 * M4a final review A1: a PerAttempt stage that holds the first attempt of a request tagged OnlyTag (any request when it
	 * is none). It proceeds, and AnswerNow() answers that attempt itself while its call may still run (a user's own
	 * timeout); what the call reports afterwards is counted in LateAnswers and goes no further. Every other pass goes
	 * through. A held answer captures the exchange: answer it, or reset Held, before the test ends.
	 */
	class FHoldFirstAttempt final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("HoldFirstAttempt"); }
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			if ((!OnlyTag.IsNone() && Exchange->GetTag() != OnlyTag) || Exchange->GetAttempt() != 1 || Held)
			{
				Next.Proceed(Exchange, MoveTemp(OnOutcome));
				return;
			}
			Held = MoveTemp(OnOutcome);
			Next.Proceed(Exchange, [Late = LateAnswers](FVaRestAttemptOutcome&&) { ++*Late; });
		}

		void AnswerNow(int32 Code)
		{
			if (!Held)
			{
				return;
			}
			FVaRestOutcomeCallback Answer = MoveTemp(Held);
			Held.Reset();
			FVaRestAttemptOutcome Early;
			Early.Status = Code;
			Answer(MoveTemp(Early));
		}

		FName OnlyTag;
		FVaRestOutcomeCallback Held;
		TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> LateAnswers = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	};

	/**
	 * M4a final review A1 (review C M-2): a PerAttempt stage that proceeds a second time while its first trip's call still
	 * runs (a hedge), under the same attempt number. Its answer is the second trip's outcome -- at once, or, with
	 * bHoldSecond, when AnswerHeld() is called; the first trip's is counted in FirstEnds and goes no further. Release() at
	 * the end of a test: the kept answer holds the exchange.
	 */
	class FProceedTwice final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("ProceedTwice"); }
		virtual bool MayReissue() const override { return true; }
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			Kept = Exchange;
			KeptNext.Emplace(Next);
			Done = MakeShared<FVaRestOutcomeCallback, ESPMode::ThreadSafe>(MoveTemp(OnOutcome));
			Next.Proceed(Exchange, [First = FirstEnds](FVaRestAttemptOutcome&&) { ++*First; });
		}

		void ProceedAgain()
		{
			const TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> Pinned = Kept.Pin();
			if (!Pinned.IsValid() || !KeptNext.IsSet())
			{
				return;
			}
			KeptNext.GetValue().Proceed(Pinned.ToSharedRef(), [Up = Done, Hold = bHoldSecond, Into = HeldSecond](FVaRestAttemptOutcome&& Outcome)
			{
				if (Hold)
				{
					Into->Emplace(MoveTemp(Outcome));
					return;
				}
				if (Up.IsValid() && *Up)
				{
					FVaRestOutcomeCallback Last = MoveTemp(*Up);
					Up->Reset();
					Last(MoveTemp(Outcome));
				}
			});
		}

		/** bHoldSecond: answers with the second trip's outcome, kept since it came back */
		void AnswerHeld()
		{
			if (!HeldSecond->IsSet() || !Done.IsValid() || !*Done)
			{
				return;
			}
			FVaRestAttemptOutcome Outcome = MoveTemp(HeldSecond->GetValue());
			HeldSecond->Reset();
			FVaRestOutcomeCallback Last = MoveTemp(*Done);
			Done->Reset();
			Last(MoveTemp(Outcome));
		}

		bool HasHeldSecond() const { return HeldSecond->IsSet(); }

		void Release()
		{
			Done.Reset();
			KeptNext.Reset();
			HeldSecond->Reset();
		}

		bool bHoldSecond = false;
		TSharedRef<TOptional<FVaRestAttemptOutcome>, ESPMode::ThreadSafe> HeldSecond = MakeShared<TOptional<FVaRestAttemptOutcome>, ESPMode::ThreadSafe>();

		TWeakPtr<FVaRestExchange, ESPMode::ThreadSafe> Kept;
		TOptional<FVaRestStageNext> KeptNext;
		TSharedPtr<FVaRestOutcomeCallback, ESPMode::ThreadSafe> Done;
		TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> FirstEnds = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	};

	/** Request Id's physical dispatch starts, in order (Attempt need not be unique). */
	TArray<FVaRestTraceAttemptDispatched> DispatchesOf(const VaRest::Http::Testing::FTraceRecorder& Recorder, uint64 Id)
	{
		TArray<FVaRestTraceAttemptDispatched> Out;
		for (const FVaRestTraceEvent& Event : Recorder.For(Id))
		{
			if (Event.Kind == EVaRestTraceKind::AttemptDispatched)
			{
				Out.Add(Event.Payload.Get<FVaRestTraceAttemptDispatched>());
			}
		}
		return Out;
	}

	/** Request Id's AttemptEnded payloads, in order */
	TArray<FVaRestTraceAttemptEnded> EndsOf(const VaRest::Http::Testing::FTraceRecorder& Recorder, uint64 Id)
	{
		TArray<FVaRestTraceAttemptEnded> Out;
		for (const FVaRestTraceEvent& Event : Recorder.For(Id))
		{
			if (Event.Kind == EVaRestTraceKind::AttemptEnded)
			{
				Out.Add(Event.Payload.Get<FVaRestTraceAttemptEnded>());
			}
		}
		return Out;
	}

	/** How many of request Id's excerpts are its responses' (any attempt), and how many of those are Skipped: Discarded */
	int32 CountResponseExcerpts(const VaRest::Http::Testing::FTraceRecorder& Recorder, uint64 Id, bool bOnlyDiscarded)
	{
		int32 Count = 0;
		for (const FVaRestTraceExcerptEvent& One : Recorder.ExcerptsOf(Id))
		{
			if (One.Owner == EVaRestTraceExcerptOwner::Response
				&& (!bOnlyDiscarded || (One.Excerpt.Kind == EVaRestExcerptKind::Skipped && One.Excerpt.SkipReason == EVaRestExcerptSkip::Discarded)))
			{
				++Count;
			}
		}
		return Count;
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
	TestTrue(TEXT("the request's, before any attempt, redacted"), Sent->Attempt == 0 && Sent->DispatchId == 0 && Sent->Excerpt.Kind == EVaRestExcerptKind::Json
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
		&& Skipped->Excerpt.SkipReason == EVaRestExcerptSkip::Backlog && Skipped->Excerpt.TotalBytes == 1000 && Skipped->DispatchId == 0);
	const TOptional<FVaRestTraceExcerptEvent> Admitted = Find(Scoped.Recorder->ExcerptsOf(SmallId), EVaRestTraceExcerptOwner::Request);
	TestTrue(TEXT("under it: the excerpt"), Admitted.IsSet() && Admitted->Excerpt.Kind == EVaRestExcerptKind::Text);
	TestEqual(TEXT("the counter is back at 0"), VaRest::Http::Excerpts::PendingBytes(), static_cast<int64>(0));

	Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200, FString::ChrN(1000, TEXT('z'))));
	auto Response = MakeOutcome<TArray<uint8>>();
	const uint64 ResponseId = Observe(Rig.Client.Get<TArray<uint8>>(TEXT("/response-backlog")), Response).Send().GetId();
	if (!TestTrue(TEXT("raw response completed"), Rig.Wait(Response))) { Abandon(Rig); return false; }
	const auto ResponseEnd = First<FVaRestTraceAttemptEnded>(*Scoped.Recorder, ResponseId, EVaRestTraceKind::AttemptEnded);
	const auto ResponseSkip = Find(Scoped.Recorder->ExcerptsOf(ResponseId), EVaRestTraceExcerptOwner::Response);
	TestTrue(TEXT("real response Backlog resolves the captured dispatch promise"), ResponseEnd.IsSet() && ResponseSkip.IsSet()
		&& ResponseEnd->bExcerptPending && ResponseEnd->DispatchId != 0 && ResponseSkip->DispatchId == ResponseEnd->DispatchId
		&& ResponseSkip->Excerpt.Kind == EVaRestExcerptKind::Skipped && ResponseSkip->Excerpt.SkipReason == EVaRestExcerptSkip::Backlog);
	TestEqual(TEXT("response refusal leaves no admission"), VaRest::Http::Excerpts::PendingBytes(), int64(0));
	TestEqual(TEXT("response promise resolved"), Unresolved(*Scoped.Recorder, ResponseId), FString());

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
	// the largest, so a sink asking for less never shrinks a default sink's budget. Only a sink that asks for excerpts (Bodies) has a say.
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpExcerptDiscardedTest, "VaRest.Http.Trace.ExcerptDiscarded",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpExcerptDiscardedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::ExcerptTestPrivate;
	// Task 21 review M-1: every announced excerpt resolves. AttemptEnded announces a decodable 2xx's excerpt for the decode
	// task; when a stage replaces that outcome the decode task never sees the body, and the excerpt resolves as Skipped:
	// Discarded -- before Completed (a custom Outer stage's 502), or before the next attempt's AttemptEnded (a PerAttempt
	// stage's 503 that the retry stage re-issues). A plain decoded 2xx keeps its one excerpt from the decode task.
	FTestRig Rig;
	FVaRestRetryPolicy Retry = FVaRestRetryPolicy::Exponential(1);
	Retry.Backoff.Jitter = EVaRestJitter::None;
	Rig.Client.WithRetry(Retry);
	Rig.Client.Use(MakeShared<FReplaceOkStage, ESPMode::ThreadSafe>(), EVaRestStageSlot::Outer);
	Rig.Client.Use(MakeShared<FFirstOkIsBusyStage, ESPMode::ThreadSafe>(), EVaRestStageSlot::PerAttempt);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return Json(200, R"({"n":1})"); });
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Bodies, 1024);
	const FTraceRecorder& Recorder = *Scoped.Recorder;
	auto Replaced = MakeOutcome<FVaRestJson>();
	auto Again = MakeOutcome<FVaRestJson>();
	auto Plain = MakeOutcome<FVaRestJson>();
	const uint64 ReplacedId = Observe(Rig.Client.Get(TEXT("/replaced")), Replaced).Send().GetId();
	const uint64 AgainId = Observe(Rig.Client.Get(TEXT("/again")), Again).Send().GetId();
	const uint64 PlainId = Observe(Rig.Client.Get(TEXT("/plain")), Plain).Send().GetId();
	if (!TestTrue(TEXT("/again backs off, the others complete"), PumpUntil([&Rig, Replaced, Plain] { return Replaced->IsDone() && Plain->IsDone() && Rig.Clock->NumPendingTimers() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(0.5);
	if (!TestTrue(TEXT("/again completes"), Rig.Wait(Again)))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	FlushRedactionPipe();

	// A custom Outer stage replaced the 2xx: resolved before Completed, once
	TestTrue(TEXT("/replaced: the stage's 502 is the answer"), Replaced->ErrorCalls.load() == 1 && Replaced->Error.HttpStatus == 502);
	const TOptional<FVaRestTraceAttemptEnded> ReplacedEnded = First<FVaRestTraceAttemptEnded>(Recorder, ReplacedId, EVaRestTraceKind::AttemptEnded);
	TestTrue(TEXT("/replaced: AttemptEnded announced the decode task's excerpt"), ReplacedEnded.IsSet() && ReplacedEnded->bExcerptPending && ReplacedEnded->Status == 200);
	const TArray<FVaRestTraceEvent> ReplacedExcerpts = ResponseExcerpts(Recorder, ReplacedId, 1);
	TestTrue(TEXT("/replaced: one excerpt, Skipped: Discarded"), IsOneDiscarded(ReplacedExcerpts));
	TestTrue(TEXT("discard preserves the announced physical id"), ReplacedEnded.IsSet() && ReplacedExcerpts.Num() == 1
		&& ReplacedEnded->DispatchId != 0 && ReplacedExcerpts[0].Payload.Get<FVaRestTraceExcerptEvent>().DispatchId == ReplacedEnded->DispatchId);
	TestTrue(TEXT("/replaced: ... with the announced response's size"), ReplacedExcerpts.Num() == 1
		&& ReplacedExcerpts[0].Payload.Get<FVaRestTraceExcerptEvent>().Excerpt.TotalBytes == 7);
	TestTrue(TEXT("/replaced: ... between its AttemptEnded and its Completed (nothing pending at Completed)"), ReplacedExcerpts.Num() == 1
		&& ReplacedExcerpts[0].Seq > SeqOfAttempt(Recorder, ReplacedId, EVaRestTraceKind::AttemptEnded, 1)
		&& ReplacedExcerpts[0].Seq < SeqOfAttempt(Recorder, ReplacedId, EVaRestTraceKind::Completed, 0));
	TestEqualSensitive(TEXT("/replaced: no excerpt pending at the end"), Unresolved(Recorder, ReplacedId), FString());

	// A PerAttempt stage turned attempt 1's 2xx into a 503 and the retry re-issued it: attempt 1's resolves before attempt 2's AttemptEnded
	TestTrue(TEXT("/again: two attempts, the second decoded"), Again->SuccessCalls.load() == 1 && Again->Response.Attempts == 2);
	const TArray<FVaRestTraceEvent> FirstAttempt = ResponseExcerpts(Recorder, AgainId, 1);
	TestTrue(TEXT("/again: attempt 1's excerpt is Skipped: Discarded"), IsOneDiscarded(FirstAttempt));
	TestTrue(TEXT("/again: ... emitted before attempt 2's AttemptEnded"), FirstAttempt.Num() == 1
		&& FirstAttempt[0].Seq > SeqOfAttempt(Recorder, AgainId, EVaRestTraceKind::AttemptEnded, 1)
		&& FirstAttempt[0].Seq < SeqOfAttempt(Recorder, AgainId, EVaRestTraceKind::AttemptEnded, 2));
	const TArray<FVaRestTraceEvent> SecondAttempt = ResponseExcerpts(Recorder, AgainId, 2);
	TestTrue(TEXT("/again: attempt 2's excerpt is the decode task's"), SecondAttempt.Num() == 1
		&& SecondAttempt[0].Payload.Get<FVaRestTraceExcerptEvent>().Excerpt.Kind == EVaRestExcerptKind::Json);
	TestEqualSensitive(TEXT("/again: no excerpt pending at the end"), Unresolved(Recorder, AgainId), FString());

	// The control: handed to the decode task, which resolves it -- nothing else is emitted for it at Completed
	const TArray<FVaRestTraceEvent> PlainExcerpts = ResponseExcerpts(Recorder, PlainId, 1);
	TestTrue(TEXT("/plain: one excerpt, the decode task's"), PlainExcerpts.Num() == 1
		&& PlainExcerpts[0].Payload.Get<FVaRestTraceExcerptEvent>().Excerpt.Kind == EVaRestExcerptKind::Json);
	TestEqualSensitive(TEXT("/plain: no excerpt pending at the end"), Unresolved(Recorder, PlainId), FString());
	TestTrue(TEXT("Inspector 9: only excerpts arrive off the game thread"), Recorder.OnlyExcerptsOffGameThread());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpExcerptDiscardedOnCancelTest, "VaRest.Http.Trace.ExcerptDiscardedOnCancel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpExcerptDiscardedOnCancelTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::ExcerptTestPrivate;
	// Task 21 review M-1: a game-thread Cancel() from a sink's OnEvent at the AttemptEnded of a decodable 2xx. AttemptEnded
	// has announced the decode task's excerpt; the cancel finishes the request (Complete, deferred out of Cancel), so
	// OnChainDone never hands the body to a decode task. The excerpt resolves as Skipped: Discarded before Completed.
	FTestRig Rig;
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return Json(200, R"({"n":1})"); });
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Bodies, 1024);
	const FTraceRecorder& Recorder = *Scoped.Recorder;
	const TSharedRef<FVaRestRequestHandle, ESPMode::ThreadSafe> Target = MakeShared<FVaRestRequestHandle, ESPMode::ThreadSafe>();
	const TSharedRef<FHookSink, ESPMode::ThreadSafe> Canceler = MakeShared<FHookSink, ESPMode::ThreadSafe>(EVaRestTraceDetail::Metadata);
	Canceler->Hook = [Target](const FVaRestTraceEvent& Event)
	{
		if (Event.Kind == EVaRestTraceKind::AttemptEnded && Event.Payload.Get<FVaRestTraceAttemptEnded>().RequestId == Target->GetId())
		{
			Target->Cancel();
		}
	};
	FVaRestTrace::AddSink(Canceler);
	auto Outcome = MakeOutcome<FVaRestJson>();
	// The answer comes from a later game-thread task (the fake queues it for a worker), never inside Send()
	*Target = Observe(Rig.Client.Get(TEXT("/c")), Outcome).Send();
	const uint64 Id = Target->GetId();
	const bool bDone = Rig.Wait(Outcome);
	FVaRestTrace::RemoveSink(Canceler);
	if (!TestTrue(TEXT("completes"), bDone))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	FlushRedactionPipe();
	TestTrue(TEXT("canceled from the sink"), Outcome->Error.Kind == EVaRestErrorKind::Canceled && Outcome->SuccessCalls.load() == 0);
	TestEqualSensitive(TEXT("the sequence"), Recorder.KindsOf(Id), FString(TEXT("Started,Dispatched,Ended,Completed")));
	const TOptional<FVaRestTraceAttemptEnded> Ended = First<FVaRestTraceAttemptEnded>(Recorder, Id, EVaRestTraceKind::AttemptEnded);
	TestTrue(TEXT("AttemptEnded announced the decode task's excerpt"), Ended.IsSet() && Ended->bExcerptPending && Ended->Status == 200);
	const TArray<FVaRestTraceEvent> Excerpts = ResponseExcerpts(Recorder, Id, 1);
	TestTrue(TEXT("one excerpt, Skipped: Discarded"), IsOneDiscarded(Excerpts));
	TestTrue(TEXT("cancel discard preserves the announced physical id"), Ended.IsSet() && Excerpts.Num() == 1
		&& Ended->DispatchId != 0 && Excerpts[0].Payload.Get<FVaRestTraceExcerptEvent>().DispatchId == Ended->DispatchId);
	TestTrue(TEXT("... before Completed"), Excerpts.Num() == 1 && Excerpts[0].Seq > SeqOfAttempt(Recorder, Id, EVaRestTraceKind::AttemptEnded, 1)
		&& Excerpts[0].Seq < SeqOfAttempt(Recorder, Id, EVaRestTraceKind::Completed, 0));
	TestEqualSensitive(TEXT("no excerpt pending at the end"), Unresolved(Recorder, Id), FString());
	const TOptional<FVaRestTraceCompleted> Done = First<FVaRestTraceCompleted>(Recorder, Id, EVaRestTraceKind::Completed);
	TestTrue(TEXT("Completed Canceled"), Done.IsSet() && Done->Outcome == EVaRestTraceOutcome::Canceled);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpExcerptDecodeSinkGoneTest, "VaRest.Http.Trace.ExcerptOnDecodeSinkGone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpExcerptDecodeSinkGoneTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::ExcerptTestPrivate;
	// Task 21 review M-2: the decode task re-checks that a sink still wants Bodies, as the pipe's task does (LaunchJob). The
	// only Bodies sink removes itself at the AttemptEnded that announced the decode task's excerpt; the Metadata sink left
	// receives no excerpt (Emit delivers every event to every sink, so without the check it would).
	FTestRig Rig;
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return Json(200, R"({"n":1})"); });
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	const FTraceRecorder& Recorder = *Scoped.Recorder;
	const TSharedRef<uint64, ESPMode::ThreadSafe> TargetId = MakeShared<uint64, ESPMode::ThreadSafe>(0);
	const TSharedRef<FHookSink, ESPMode::ThreadSafe> Leaving = MakeShared<FHookSink, ESPMode::ThreadSafe>(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Bodies);
	Leaving->Hook = [TargetId, Weak = TWeakPtr<FHookSink, ESPMode::ThreadSafe>(Leaving)](const FVaRestTraceEvent& Event)
	{
		if (Event.Kind == EVaRestTraceKind::AttemptEnded && Event.Payload.Get<FVaRestTraceAttemptEnded>().RequestId == *TargetId)
		{
			if (const TSharedPtr<FHookSink, ESPMode::ThreadSafe> Self = Weak.Pin())
			{
				FVaRestTrace::RemoveSink(Self.ToSharedRef());
			}
		}
	};
	FVaRestTrace::AddSink(Leaving);
	auto Outcome = MakeOutcome<FVaRestJson>();
	*TargetId = Observe(Rig.Client.Get(TEXT("/d")), Outcome).Send().GetId();
	const uint64 Id = *TargetId;
	const bool bDone = Rig.Wait(Outcome);
	FVaRestTrace::RemoveSink(Leaving);
	if (!TestTrue(TEXT("completes"), bDone))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	FlushRedactionPipe();
	TestTrue(TEXT("decoded"), Outcome->SuccessCalls.load() == 1);
	const TOptional<FVaRestTraceAttemptEnded> Ended = First<FVaRestTraceAttemptEnded>(Recorder, Id, EVaRestTraceKind::AttemptEnded);
	TestTrue(TEXT("AttemptEnded announced the excerpt while the Bodies sink was there"), Ended.IsSet() && Ended->bExcerptPending);
	TestFalse(TEXT("no sink wants Bodies any more"), FVaRestTrace::Wants(EVaRestTraceDetail::Bodies));
	TestEqual(TEXT("the decode task emitted nothing"), Recorder.ExcerptsOf(Id).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpExcerptMultipartAdmissionTest, "VaRest.Http.Trace.ExcerptMultipartAdmission",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpExcerptMultipartAdmissionTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::ExcerptTestPrivate;
	// Task 21 review M-3: a multipart request's excerpt is sized, admitted, then copied -- a refused admission copies nothing
	FTestRig Rig;
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Bodies, 4096);
	Scoped.Recorder->Detail.MaxPendingExcerptBytes = 100;
	FVaRestTrace::RefreshDetail();
	// Each request's whole life is measured (its Started may run from a later task than Send); its 200 "{}" is excerpted
	// by the decode task, which copies nothing, so the request's excerpt is all the counter sees
	auto Big = MakeOutcome<FVaRestJson>();
	const int64 BeforeBig = Counters().TraceExcerptBytesCopied.load();
	const uint64 BigId = Observe(Rig.Client.Post(TEXT("/big")).Body(FVaRestMultipart().AddField(TEXT("blob"), FString::ChrN(1000, TEXT('x')))), Big).Send().GetId();
	if (!TestTrue(TEXT("completes"), Rig.Wait(Big) && Rig.Drain()))
	{
		Abandon(Rig);
		return false;
	}
	const int64 CopiedForBig = Counters().TraceExcerptBytesCopied.load() - BeforeBig;
	auto Small = MakeOutcome<FVaRestJson>();
	const int64 BeforeSmall = Counters().TraceExcerptBytesCopied.load();
	const uint64 SmallId = Observe(Rig.Client.Post(TEXT("/small")).Body(FVaRestMultipart().AddField(TEXT("name"), TEXT("neo"))), Small).Send().GetId();
	if (!TestTrue(TEXT("completes"), Rig.Wait(Small) && Rig.Drain()))
	{
		Abandon(Rig);
		return false;
	}
	const int64 CopiedForSmall = Counters().TraceExcerptBytesCopied.load() - BeforeSmall;
	FlushRedactionPipe();
	const TOptional<FVaRestTraceExcerptEvent> Skipped = Find(Scoped.Recorder->ExcerptsOf(BigId), EVaRestTraceExcerptOwner::Request);
	TestTrue(TEXT("past the admission cap: Skipped: Backlog"), Skipped.IsSet() && Skipped->Excerpt.Kind == EVaRestExcerptKind::Skipped
		&& Skipped->Excerpt.SkipReason == EVaRestExcerptSkip::Backlog);
	TestEqual(TEXT("... and nothing was copied for it"), CopiedForBig, static_cast<int64>(0));
	const TOptional<FVaRestTraceExcerptEvent> Admitted = Find(Scoped.Recorder->ExcerptsOf(SmallId), EVaRestTraceExcerptOwner::Request);
	TestTrue(TEXT("under it: the parts"), Admitted.IsSet() && Admitted->Excerpt.Kind == EVaRestExcerptKind::Multipart && Admitted->Excerpt.ToText().Contains(TEXT("neo")));
	TestEqual(TEXT("... its field's bytes copied, once"), CopiedForSmall, static_cast<int64>(3));
	TestEqual(TEXT("the counter is back at 0"), VaRest::Http::Excerpts::PendingBytes(), static_cast<int64>(0));
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpGoldenStaleEndTest, "VaRest.Http.Trace.Golden.StaleAttemptEnd",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpGoldenStaleEndTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::ExcerptTestPrivate;
	// M4a final review A1, review A I-1 row R3 (VaRest-tqt; ruling: relabel, not drop): attempt 1's transport call finished
	// (a 2xx) while a PerAttempt stage timed the attempt out with its own 503; Retry dispatched attempt 2 before attempt 1's
	// end reached the game thread. That stale end is reported under its own attempt, after attempt 2's dispatch, announcing
	// no excerpt; attempt 2's response gets the one decode excerpt, with no Discarded for it. Inspector 3.3 holds.
	FTestRig Rig;
	Rig.Fake->bCompleteOnWorker = false;
	const TSharedRef<FHoldFirstAttempt, ESPMode::ThreadSafe> Early = MakeShared<FHoldFirstAttempt, ESPMode::ThreadSafe>();
	FVaRestRetryPolicy Retry = FVaRestRetryPolicy::Exponential(3);
	Retry.Backoff.Jitter = EVaRestJitter::None;
	Rig.Client.Use(Early, EVaRestStageSlot::PerAttempt).WithRetry(Retry);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Bodies, 1024);
	const FTraceRecorder& Recorder = *Scoped.Recorder;
	auto Outcome = MakeOutcome<FVaRestJson>();
	const uint64 Id = Observe(Rig.Client.Get(TEXT("/stale")), Outcome).Send().GetId();
	if (!TestEqual(TEXT("attempt 1 is on the wire"), Rig.Fake->NumCalls(), 1))
	{
		Early->Held.Reset();
		Abandon(Rig);
		return false;
	}
	// The transport finished while the stage timed out: the end is queued for the game thread, the stage answers first
	Rig.Fake->Complete(0, Json(200, R"({"n":1})"));
	Early->AnswerNow(503);
	Rig.Clock->Advance(0.5);
	if (!TestEqual(TEXT("attempt 2 dispatched before attempt 1's end ran"), Rig.Fake->NumCalls(), 2))
	{
		Abandon(Rig);
		return false;
	}
	const TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Late = Early->LateAnswers;
	if (!TestTrue(TEXT("attempt 1's end arrives"), PumpUntil([Late] { return Late->load() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Fake->Complete(1, Json(200, R"({"n":2})"));
	if (!TestTrue(TEXT("attempt 2's 200 is the answer"), Rig.Wait(Outcome) && Outcome->SuccessCalls.load() == 1 && Outcome->Response.Attempts == 2))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	FlushRedactionPipe();
	TestEqualSensitive(TEXT("the sequence: the stale end after attempt 2's dispatch, paired, not dropped"), Recorder.KindsOf(Id),
		FString(TEXT("Started,Dispatched,Note,Dispatched,Ended,Ended,Completed")));
	const TArray<FVaRestTraceAttemptEnded> Ends = EndsOf(Recorder, Id);
	if (!TestEqual(TEXT("two ends"), Ends.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("the stale end: attempt 1, its own 200, no excerpt announced"), Ends[0].Attempt == 1 && Ends[0].Status == 200 && !Ends[0].bExcerptPending);
	TestTrue(TEXT("attempt 2's end: exactly one, announcing the decode excerpt"), Ends[1].Attempt == 2 && Ends[1].bExcerptPending);
	const TArray<FVaRestTraceEvent> Second = ResponseExcerpts(Recorder, Id, 2);
	TestTrue(TEXT("one Response excerpt for attempt 2, the decode task's"), Second.Num() == 1
		&& Second[0].Payload.Get<FVaRestTraceExcerptEvent>().Excerpt.Kind == EVaRestExcerptKind::Json);
	TestEqual(TEXT("... none for attempt 1"), ResponseExcerpts(Recorder, Id, 1).Num(), 0);
	TestEqual(TEXT("... and no Discarded"), CountResponseExcerpts(Recorder, Id, /*bOnlyDiscarded*/ true), 0);
	TestEqualSensitive(TEXT("every announced excerpt resolved"), Unresolved(Recorder, Id), FString());
	FString Why;
	TestTrue(TEXT("Inspector 3.3: the phases hold"), PhasesHold(Recorder, Id, Why));
	TestEqualSensitive(TEXT("... (why not)"), Why, FString());
	return true;
}

// M4a final review M-5: R4 needs only Metadata, so it stays outside the excerpts guard and an excerpts-off build still pins
// the stale end's attempt label.
#endif // VAREST_WITH_TRACE_EXCERPTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpGoldenStaleEndQueuedTest, "VaRest.Http.Trace.Golden.StaleAttemptEndQueued",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpGoldenStaleEndQueuedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::ExcerptTestPrivate;
	// M4a final review A1, review A I-1 row R4: as Golden.StaleAttemptEnd, but attempt 2 waits in the limiter's queue
	// behind a blocker when attempt 1's end lands -- the attempt boundary has already numbered it 2. The end is attempt 1's.
	FTestRig Rig;
	Rig.Fake->bCompleteOnWorker = false;
	const TSharedRef<FHoldFirstAttempt, ESPMode::ThreadSafe> Early = MakeShared<FHoldFirstAttempt, ESPMode::ThreadSafe>();
	Early->OnlyTag = TEXT("Held");
	FVaRestRetryPolicy Retry = FVaRestRetryPolicy::Exponential(3);
	Retry.Backoff.Jitter = EVaRestJitter::None;
	Rig.Client.Use(Early, EVaRestStageSlot::PerAttempt).WithRetry(Retry).WithMaxConcurrent(1);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request)
	{
		return Request.Url.EndsWith(TEXT("/stale")) && Request.Attempt == 2 ? Json(200, R"({"n":2})") : FVaRestFakeResponse::Manual();
	});
	const VaRest::Http::FLimiter& Slots = VaRest::Http::FClientAccess::State(Rig.Client)->GetLimiter();
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	const FTraceRecorder& Recorder = *Scoped.Recorder;
	auto Outcome = MakeOutcome<FVaRestJson>();
	auto Blocker = MakeOutcome<FVaRestJson>();
	const uint64 Id = Observe(Rig.Client.Get(TEXT("/stale")).Tag(TEXT("Held")), Outcome).Send().GetId();
	Observe(Rig.Client.Get(TEXT("/blocker")), Blocker).Send();
	if (!TestTrue(TEXT("attempt 1 holds the slot, the blocker waits"), Rig.Fake->NumCalls() == 1 && Slots.NumQueued() == 1))
	{
		Early->Held.Reset();
		Abandon(Rig);
		return false;
	}
	// The stage's answer frees the slot: the blocker takes it before Retry's backoff ends
	Early->AnswerNow(503);
	Rig.Clock->Advance(0.5);
	if (!TestTrue(TEXT("the blocker runs; attempt 2 waits in the queue"), Rig.Fake->NumCalls() == 2 && Slots.NumQueued() == 1))
	{
		Abandon(Rig);
		return false;
	}
	const TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Late = Early->LateAnswers;
	if (!TestTrue(TEXT("attempt 1's end arrives while attempt 2 is queued"), PumpUntil([Late] { return Late->load() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	const TArray<FVaRestTraceAttemptEnded> Ends = EndsOf(Recorder, Id);
	TestTrue(TEXT("it is attempt 1's end, not attempt 2's"), Ends.Num() == 1 && Ends[0].Attempt == 1 && Ends[0].Failure == EVaRestTransportFailure::Canceled);
	const auto OldStarts = DispatchesOf(Recorder, Id);
	TestTrue(TEXT("queued later attempt cannot relabel the old dispatch"), OldStarts.Num() == 1 && Ends.Num() == 1
		&& OldStarts[0].DispatchId != 0 && Ends[0].DispatchId == OldStarts[0].DispatchId);
	Rig.Fake->Complete(1, Json(200, "{}"));
	if (!TestTrue(TEXT("both complete"), PumpUntil([Outcome, Blocker] { return Outcome->IsDone() && Blocker->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestTrue(TEXT("attempt 2's 200 is the answer"), Outcome->SuccessCalls.load() == 1 && Outcome->Response.Attempts == 2);
	TestEqualSensitive(TEXT("the sequence"), Recorder.KindsOf(Id), FString(TEXT("Started,Dispatched,Note,Ended,Dispatched,Ended,Completed")));
	FString Why;
	TestTrue(TEXT("Inspector 3.3: the phases hold"), PhasesHold(Recorder, Id, Why));
	TestEqualSensitive(TEXT("... (why not)"), Why, FString());
	return true;
}

#if VAREST_WITH_TRACE_EXCERPTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpExcerptPerDispatchTest, "VaRest.Http.Trace.ExcerptOnePerDispatch",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpExcerptPerDispatchTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::ExcerptTestPrivate;
	// M4a final review A1, review C M-2: a PerAttempt stage proceeds again while its first call runs, so two dispatches share
	// attempt 1. Both are reported (two pairs under one number, Inspector IN-2/IN-6); only the live dispatch's end may carry
	// an excerpt, so the first call's late 2xx -- not the request's answer -- announces none, and the request has one
	// Response excerpt, the decode task's, with no Discarded.
	FTestRig Rig;
	Rig.Fake->bCompleteOnWorker = false;
	const TSharedRef<FProceedTwice, ESPMode::ThreadSafe> Hedge = MakeShared<FProceedTwice, ESPMode::ThreadSafe>();
	Rig.Client.Use(Hedge, EVaRestStageSlot::PerAttempt);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Bodies, 1024);
	const FTraceRecorder& Recorder = *Scoped.Recorder;
	auto Outcome = MakeOutcome<FVaRestJson>();
	const uint64 Id = Observe(Rig.Client.Get(TEXT("/hedged")), Outcome).Send().GetId();
	Hedge->ProceedAgain();
	if (!TestTrue(TEXT("two calls under attempt 1"), Rig.Fake->NumCalls() == 2 && Rig.Fake->GetCall(1).Request.Attempt == 1))
	{
		Hedge->Release();
		Abandon(Rig);
		return false;
	}
	Rig.Fake->Complete(0, Json(200, R"({"n":1})"));
	const TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> First = Hedge->FirstEnds;
	if (!TestTrue(TEXT("the first call's end arrives"), PumpUntil([First] { return First->load() == 1; })))
	{
		Hedge->Release();
		Abandon(Rig);
		return false;
	}
	Rig.Fake->Complete(1, Json(200, R"({"n":2})"));
	const bool bDone = TestTrue(TEXT("the second call's 200 is the answer"), Rig.Wait(Outcome) && Outcome->SuccessCalls.load() == 1);
	Hedge->Release();
	if (!bDone)
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	FlushRedactionPipe();
	TestTrue(TEXT("... decoded from the second call"), Outcome->Value.IsSet() && Outcome->Value->Get(TEXT("n")).AsInt64() == 2);
	TestEqualSensitive(TEXT("two pairs under one attempt number"), Recorder.KindsOf(Id), FString(TEXT("Started,Dispatched,Dispatched,Ended,Ended,Completed")));
	const TArray<FVaRestTraceAttemptEnded> Ends = EndsOf(Recorder, Id);
	TestTrue(TEXT("the first (stale) end announces no excerpt; the live one does"), Ends.Num() == 2 && Ends[0].Attempt == 1 && Ends[1].Attempt == 1
		&& !Ends[0].bExcerptPending && Ends[1].bExcerptPending);
	const auto Starts = DispatchesOf(Recorder, Id);
	if (!TestTrue(TEXT("two nonzero distinct dispatch ids under Attempt1"), Starts.Num() == 2 && Ends.Num() == 2
		&& Starts[0].DispatchId != 0 && Starts[1].DispatchId != 0 && Starts[0].DispatchId != Starts[1].DispatchId)) return false;
	TestEqual(TEXT("first end matches first dispatch"), Ends[0].DispatchId, Starts[0].DispatchId);
	TestEqual(TEXT("second end matches second dispatch"), Ends[1].DispatchId, Starts[1].DispatchId);
	const auto FirstOrderExcerpts = ResponseExcerpts(Recorder, Id, 1);
	TestTrue(TEXT("decode excerpt belongs to second dispatch"), FirstOrderExcerpts.Num() == 1
		&& FirstOrderExcerpts[0].Payload.Get<FVaRestTraceExcerptEvent>().DispatchId == Starts[1].DispatchId);
	TestEqual(TEXT("one Response excerpt for the request"), CountResponseExcerpts(Recorder, Id, /*bOnlyDiscarded*/ false), 1);
	TestEqual(TEXT("... no Discarded"), CountResponseExcerpts(Recorder, Id, /*bOnlyDiscarded*/ true), 0);
	TestEqualSensitive(TEXT("every announced excerpt resolved"), Unresolved(Recorder, Id), FString());

	// The other order: the live call's 2xx comes back first and the stage keeps it a while; the stale call's end arrives
	// before the stage answers. That end must not resolve the live one's pending excerpt (Excerpts::ResolveDiscarded): the
	// response still reaches the decoder, which excerpts it.
	const TSharedRef<FProceedTwice, ESPMode::ThreadSafe> Keeper = MakeShared<FProceedTwice, ESPMode::ThreadSafe>();
	Keeper->bHoldSecond = true;
	FTestRig Later;
	Later.Fake->bCompleteOnWorker = false;
	Later.Client.Use(Keeper, EVaRestStageSlot::PerAttempt);
	Later.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	auto Kept = MakeOutcome<FVaRestJson>();
	const uint64 KeptId = Observe(Later.Client.Get(TEXT("/kept")), Kept).Send().GetId();
	Keeper->ProceedAgain();
	if (!TestEqual(TEXT("two calls under attempt 1, again"), Later.Fake->NumCalls(), 2))
	{
		Keeper->Release();
		Abandon(Later);
		return false;
	}
	Later.Fake->Complete(1, Json(200, R"({"n":2})"));
	const TSharedRef<FProceedTwice, ESPMode::ThreadSafe> Waiting = Keeper;
	if (!TestTrue(TEXT("the live call's 200 is back, kept by the stage"), PumpUntil([Waiting] { return Waiting->HasHeldSecond(); })))
	{
		Keeper->Release();
		Abandon(Later);
		return false;
	}
	Later.Fake->Complete(0, Json(200, R"({"n":1})"));
	const TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Stale = Keeper->FirstEnds;
	if (!TestTrue(TEXT("then the stale call's end arrives"), PumpUntil([Stale] { return Stale->load() == 1; })))
	{
		Keeper->Release();
		Abandon(Later);
		return false;
	}
	Keeper->AnswerHeld();
	const bool bKept = TestTrue(TEXT("the kept 200 is the answer"), Later.Wait(Kept) && Kept->SuccessCalls.load() == 1);
	Keeper->Release();
	if (!bKept)
	{
		Abandon(Later);
		return false;
	}
	Later.Drain();
	FlushRedactionPipe();
	const auto KeptStarts = DispatchesOf(Recorder, KeptId);
	const auto KeptEnds = EndsOf(Recorder, KeptId);
	if (!TestTrue(TEXT("reverse order has two distinct actual starts and ends"), KeptStarts.Num() == 2 && KeptEnds.Num() == 2
		&& KeptStarts[0].DispatchId != 0 && KeptStarts[1].DispatchId != 0 && KeptStarts[0].DispatchId != KeptStarts[1].DispatchId)) return false;
	TestEqual(TEXT("live end arrives first with second dispatch id"), KeptEnds[0].DispatchId, KeptStarts[1].DispatchId);
	TestEqual(TEXT("stale end arrives second with first dispatch id"), KeptEnds[1].DispatchId, KeptStarts[0].DispatchId);
	const TArray<FVaRestTraceEvent> KeptExcerpts = ResponseExcerpts(Recorder, KeptId, 1);
	TestTrue(TEXT("the live response's one excerpt is the decode task's, not Discarded"), KeptExcerpts.Num() == 1
		&& KeptExcerpts[0].Payload.Get<FVaRestTraceExcerptEvent>().Excerpt.Kind == EVaRestExcerptKind::Json
		&& KeptExcerpts[0].Payload.Get<FVaRestTraceExcerptEvent>().DispatchId == KeptStarts[1].DispatchId);
	TestEqualSensitive(TEXT("... and every announced excerpt resolved"), Unresolved(Recorder, KeptId), FString());
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpMixedExcerptDispatchTest,
    "VaRest.Http.Trace.MixedExcerptDispatchIdentity",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestHttpMixedExcerptDispatchTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    using namespace VaRest::Http::ExcerptTestPrivate;
    if (!FPlatformProcess::SupportsMultithreading() || FParse::Param(FCommandLine::Get(), TEXT("nothreading")))
    {
        AddError(TEXT("MixedExcerptDispatchIdentity requires worker concurrency; unavailable in this configuration"));
        return false;
    }
    FVaRestTrace::FScopedSinkIsolation Isolation;
    FTestRig Rig;
    Rig.Fake->bCompleteOnWorker = false;
    auto Twice = MakeShared<FProceedTwice, ESPMode::ThreadSafe>();
    Rig.Client.Use(Twice, EVaRestStageSlot::PerAttempt);
    Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
    FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Bodies, 1024);
    auto Result = MakeOutcome<FVaRestJson>();
    const uint64 Id = Observe(Rig.Client.Get(TEXT("/mixed-excerpt")), Result).Send().GetId();
    auto Start = First<FVaRestTraceAttemptDispatched>(*Scoped.Recorder, Id, EVaRestTraceKind::AttemptDispatched);
    if (!TestTrue(TEXT("real first dispatch"), Start.IsSet() && Start->DispatchId != 0)) { Twice->Release(); Abandon(Rig); return false; }
    const uint64 FirstId = Start->DispatchId;
    FRedactionGate Gate;
    auto Release = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
    Gate.Start();
    ON_SCOPE_EXIT
    {
        Release->store(true);
        Gate.Wait(); // release BEFORE waiting, including assertion failures
        Twice->Release();
        Abandon(Rig);
        Gate.Stop();
    };
    HoldPipe(Gate, Release);
    Excerpts::FScopedResponseGate Route(Id, FirstId, Gate);
    Rig.Fake->Complete(0, Json(503, R"({"n":1})"));
    if (!TestTrue(TEXT("first actual current end"), PumpUntil([Twice] { return Twice->FirstEnds->load() == 1; }))) return false;
    TestTrue(TEXT("raw response admitted to owned held pipe"), Excerpts::PendingBytes() > 0);
    TestEqual(TEXT("first excerpt still held"), ResponseExcerpts(*Scoped.Recorder, Id, 1).Num(), 0);
    Twice->ProceedAgain();
    if (!TestEqual(TEXT("second physical dispatch"), Rig.Fake->NumCalls(), 2)) return false;
    TestEqual(TEXT("same retry attempt label"), Rig.Fake->GetCall(1).Request.Attempt, 1);
    const auto Starts = DispatchesOf(*Scoped.Recorder, Id);
    if (!TestTrue(TEXT("actual second start has a distinct nonzero id"), Starts.Num() == 2
        && Starts[0].DispatchId == FirstId && Starts[1].DispatchId != 0 && Starts[1].DispatchId != FirstId)) return false;
    Rig.Fake->Complete(1, Json(200, R"({"n":2})"));
    if (!TestTrue(TEXT("decoded second outcome"), Rig.Wait(Result))) return false;
    TArray<FVaRestTraceEvent> Before = ResponseExcerpts(*Scoped.Recorder, Id, 1);
    if (!TestEqual(TEXT("decode excerpt precedes held pipe"), Before.Num(), 1)) return false;
    const auto& Second = Before[0].Payload.Get<FVaRestTraceExcerptEvent>();
    const uint64 SecondId = Second.DispatchId;
    const auto Ends = EndsOf(*Scoped.Recorder, Id);
    if (!TestEqual(TEXT("both real ends observed"), Ends.Num(), 2)) return false;
    TestEqual(TEXT("first end matches first start"), Ends[0].DispatchId, FirstId);
    TestEqual(TEXT("second end matches second start"), Ends[1].DispatchId, Starts[1].DispatchId);
    TestEqual(TEXT("second excerpt matches second start"), SecondId, Starts[1].DispatchId);
    TestTrue(TEXT("both ends promised their own response"), Ends[0].bExcerptPending && Ends[1].bExcerptPending);
    TestTrue(TEXT("both ends retain Attempt1"), Ends[0].Attempt == 1 && Ends[1].Attempt == 1);
    TestTrue(TEXT("distinct actual dispatch identities"), FirstId != 0 && SecondId != 0 && FirstId != SecondId);
    TestEqual(TEXT("decode payload"), Second.Excerpt.ToText(), FString(TEXT("{\"n\":2}")));
    TestTrue(TEXT("decode is not discarded/backlog"), Second.Excerpt.SkipReason == EVaRestExcerptSkip::None);
    Release->store(true);
    Gate.Wait();
    TArray<FVaRestTraceEvent> After = ResponseExcerpts(*Scoped.Recorder, Id, 1);
    if (!TestEqual(TEXT("both actual promises resolved"), After.Num(), 2)) return false;
    const auto& FirstLate = After[1].Payload.Get<FVaRestTraceExcerptEvent>();
    TestEqual(TEXT("late pipe retains first captured identity"), FirstLate.DispatchId, FirstId);
    TestEqual(TEXT("late pipe payload"), FirstLate.Excerpt.ToText(), FString(TEXT("{\"n\":1}")));
    TestTrue(TEXT("late pipe not discarded/backlog"), FirstLate.Excerpt.SkipReason == EVaRestExcerptSkip::None);
    TestEqual(TEXT("each physical promise resolved"), Unresolved(*Scoped.Recorder, Id), FString());
    TestEqual(TEXT("source admissions released"), Excerpts::PendingBytes(), int64(0));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpExcerptBinaryNoCopyTest, "VaRest.Http.Trace.ExcerptBinaryOverCapCopiesNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpExcerptBinaryNoCopyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::ExcerptTestPrivate;
	// M4a final review A7 (review C M-4): a 1 MiB binary download with a Bodies sink gets its descriptor (size and type, no
	// SHA-256: that needs the whole body) with no byte copied for it on the game thread and nothing admitted. A binary body
	// within the cap keeps its copy, which the SHA-256 prefix reads; a body over the cap that sniffs as JSON is still copied
	// up to the cap. One request at a time: the copy counter is the process's.
	FTestRig Rig;
	TArray<uint8> Large;
	Large.SetNumUninitialized(1024 * 1024);
	for (int32 Index = 0; Index < Large.Num(); ++Index)
	{
		Large[Index] = static_cast<uint8>(Index * 31 + 7);
	}
	Large[0] = 0x89; // not '{' or '[': nothing to sniff
	TArray<uint8> Sniffed;
	Sniffed.Init('1', 2048);
	Sniffed[0] = '[';
	Rig.Fake->SetResponder([Large, Sniffed](const FVaRestTransportRequest& Request)
	{
		if (Request.Url.EndsWith(TEXT("/large")))
		{
			return FVaRestFakeResponse::Bytes(200, Large);
		}
		if (Request.Url.EndsWith(TEXT("/sniffed")))
		{
			return FVaRestFakeResponse::Bytes(200, Sniffed, TEXTVIEW(""));
		}
		return FVaRestFakeResponse::Bytes(200, TArray<uint8>{ 1, 2, 3, 4 });
	});
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Bodies, 1024);
	const FTraceRecorder& Recorder = *Scoped.Recorder;
	const auto Fetch = [this, &Rig](const TCHAR* Path, int64& OutCopied) -> uint64
	{
		auto Outcome = MakeOutcome<TArray<uint8>>();
		const int64 Before = Counters().TraceExcerptBytesCopied.load();
		const uint64 Id = Observe(Rig.Client.Get<TArray<uint8>>(Path), Outcome).Send().GetId();
		TestTrue(FString::Printf(TEXT("%s completes"), Path), Rig.Wait(Outcome) && Rig.Drain() && Outcome->SuccessCalls.load() == 1);
		OutCopied = Counters().TraceExcerptBytesCopied.load() - Before;
		return Id;
	};
	int64 CopiedLarge = -1;
	int64 CopiedSmall = -1;
	int64 CopiedSniffed = -1;
	const uint64 LargeId = Fetch(TEXT("/large"), CopiedLarge);
	const uint64 SmallId = Fetch(TEXT("/small"), CopiedSmall);
	const uint64 SniffedId = Fetch(TEXT("/sniffed"), CopiedSniffed);
	FlushRedactionPipe();
	TestEqual(TEXT("1 MiB of binary over the cap: nothing copied"), CopiedLarge, static_cast<int64>(0));
	const TOptional<FVaRestTraceExcerptEvent> LargeExcerpt = Find(Recorder.ExcerptsOf(LargeId), EVaRestTraceExcerptOwner::Response);
	TestTrue(TEXT("... still described: binary, its size and type, no SHA-256"), LargeExcerpt.IsSet() && LargeExcerpt->Excerpt.Kind == EVaRestExcerptKind::Binary
		&& LargeExcerpt->Excerpt.TotalBytes == 1024 * 1024 && LargeExcerpt->Excerpt.Sha256Prefix.IsEmpty()
		&& LargeExcerpt->Excerpt.ToText().Equals(TEXT("<binary: 1048576 bytes, application/octet-stream>"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("binary within the cap: its 4 bytes copied"), CopiedSmall, static_cast<int64>(4));
	const TOptional<FVaRestTraceExcerptEvent> SmallExcerpt = Find(Recorder.ExcerptsOf(SmallId), EVaRestTraceExcerptOwner::Response);
	TestTrue(TEXT("... for its SHA-256 prefix"), SmallExcerpt.IsSet() && SmallExcerpt->Excerpt.Kind == EVaRestExcerptKind::Binary
		&& !SmallExcerpt->Excerpt.Sha256Prefix.IsEmpty());
	TestEqual(TEXT("over the cap, untyped but sniffed as JSON: copied up to the cap"), CopiedSniffed, static_cast<int64>(1024));
	const TOptional<FVaRestTraceExcerptEvent> SniffedExcerpt = Find(Recorder.ExcerptsOf(SniffedId), EVaRestTraceExcerptOwner::Response);
	TestTrue(TEXT("... and excerpted as JSON"), SniffedExcerpt.IsSet() && SniffedExcerpt->Excerpt.Kind == EVaRestExcerptKind::Json);
	TestEqual(TEXT("the admission counter is back at 0"), VaRest::Http::Excerpts::PendingBytes(), static_cast<int64>(0));
	return true;
}
#endif // VAREST_WITH_TRACE_EXCERPTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTraceCancelAtDispatchTest, "VaRest.Http.Trace.CancelAtDispatch",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTraceCancelAtDispatchTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::ExcerptTestPrivate;
	// M4a final review A2 (review C M-1): a sink's OnEvent at AttemptDispatched cancels the request (code that is not
	// VaRest's may cancel at any emit, concurrency review M-5). The transport is never started, and the dispatch the sinks
	// saw gets its matching AttemptEnded{Canceled}. The request leaves the limiter's queue from a later task, after a
	// blocker, so its handle is known when it is dispatched.
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(1);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request) { return Request.Url.EndsWith(TEXT("/blocker")) ? FVaRestFakeResponse::Manual() : Json(200, "{}"); });
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	const FTraceRecorder& Recorder = *Scoped.Recorder;
	const TSharedRef<FVaRestRequestHandle, ESPMode::ThreadSafe> Target = MakeShared<FVaRestRequestHandle, ESPMode::ThreadSafe>();
	const TSharedRef<FHookSink, ESPMode::ThreadSafe> Canceler = MakeShared<FHookSink, ESPMode::ThreadSafe>(EVaRestTraceDetail::Metadata);
	Canceler->Hook = [Target](const FVaRestTraceEvent& Event)
	{
		if (Event.Kind == EVaRestTraceKind::AttemptDispatched && Event.Payload.Get<FVaRestTraceAttemptDispatched>().RequestId == Target->GetId())
		{
			Target->Cancel();
		}
	};
	FVaRestTrace::AddSink(Canceler);
	auto Blocker = MakeOutcome<FVaRestJson>();
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/blocker")), Blocker).Send();
	*Target = Observe(Rig.Client.Get(TEXT("/target")), Outcome).Send();
	const uint64 Id = Target->GetId();
	if (!TestEqual(TEXT("the blocker holds the one slot"), Rig.Fake->NumCalls(), 1))
	{
		FVaRestTrace::RemoveSink(Canceler);
		Abandon(Rig);
		return false;
	}
	Rig.Fake->Complete(0, Json(200, "{}"));
	const bool bDone = Rig.Wait(Outcome) && Rig.Wait(Blocker);
	FVaRestTrace::RemoveSink(Canceler);
	if (!TestTrue(TEXT("both complete"), bDone))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestTrue(TEXT("canceled from the sink"), Outcome->Error.Kind == EVaRestErrorKind::Canceled && Outcome->ErrorCalls.load() == 1 && Outcome->SuccessCalls.load() == 0);
	TestEqual(TEXT("its transport call was never started"), Rig.Fake->NumCalls(), 1);
	TestEqualSensitive(TEXT("the dispatch is paired with its end"), Recorder.KindsOf(Id), FString(TEXT("Started,Dispatched,Ended,Completed")));
	const TOptional<FVaRestTraceAttemptEnded> Ended = First<FVaRestTraceAttemptEnded>(Recorder, Id, EVaRestTraceKind::AttemptEnded);
	TestTrue(TEXT("... AttemptEnded{1, Failure Canceled}, no excerpt"), Ended.IsSet() && Ended->Attempt == 1 && Ended->Failure == EVaRestTransportFailure::Canceled
		&& !Ended->bExcerptPending && Ended->Status == 0);
	const auto Starts = DispatchesOf(Recorder, Id);
	TestTrue(TEXT("synthetic canceled end preserves captured dispatch id"), Starts.Num() == 1 && Ended.IsSet()
		&& Starts[0].DispatchId != 0 && Ended->DispatchId == Starts[0].DispatchId);
	FString Why;
	TestTrue(TEXT("Inspector 3.3: the phases hold"), PhasesHold(Recorder, Id, Why));
	TestEqualSensitive(TEXT("... (why not)"), Why, FString());
	return true;
}

#endif
