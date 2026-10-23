// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// VaRest-2rt (W0 Task 11 review I-1): the W0 rig appended Authorization on every APPEND_HANDSHAKE_HEADER, so
// hs.redirect_reappends_headers could not tell whether LWS 3.0 replays an appended header on a redirected request by
// itself. This case appends on the first call only and lets LWS follow the redirect: the target's journal answers.
// A record (AddInfo), not a fact row: the passport stays the W0 run's (Tools/lws_probe_passport.py lists its ids).

#include "VaRestLwsProbe.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeRedirectReplayTest, "Probe.VaRest.Lws.Handshake.RedirectReplay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeRedirectReplayTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	// The engine's ::FEvent (HAL/Event.h:21) would make the name ambiguous under the directive above
	using VaRest::LwsProbe::FEvent;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	FRig Rig(Site);
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	const FString Target = NewKey();
	const FString First = NewKey();
	FConnectSpec Spec;
	Spec.Path = FString::Printf(TEXT("/ws/status/302?location=%s&key=%s"), *UrlValue(FString::Printf(TEXT("/ws/echo?key=%s"), *Target)), *First);
	Spec.Hook = [](FConn& C, lws* Wsi, int32 Reason, void* User, void* In, size_t Len) -> int32
	{
		if (Reason != LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER)
		{
			return KeepDefault;
		}
		int64& Calls = C.Counters.FindOrAdd(FName(TEXT("appends")));
		if (Calls++ > 0)
		{
			// The redirected request: nothing appended, nothing vetoed
			return 0;
		}
		static const char Line[] = "Authorization: Bearer replay-probe-token\r\n";
		const size_t Need = sizeof(Line) - 1;
		if (Need <= Len)
		{
			char** Cursor = static_cast<char**>(In);
			FMemory::Memcpy(*Cursor, Line, Need);
			*Cursor += Need;
		}
		return 0;
	};
	TArray<FEvent> Seen;
	Visit(*this, Rig, MoveTemp(Spec), Seen);
	const FVaRestJson Journal = FetchJournal(*this, Site, Target);
	// Harness: the redirect was followed (hs.redirect_relative), so the question can be answered at all
	if (!TestEqual(TEXT("LWS followed the redirect to the target"), Journal[TEXT("connections")].AsInt(), 1))
	{
		return false;
	}
	const FString Authorization = JournalHeader(Journal, TEXT("Authorization"));
	NoteOutcome(*this, FString::Printf(TEXT("LWS 3.0 replays an appended header on a redirected request by itself (the target received Authorization \"%s\"; the client's callbacks: %s)"),
		*Authorization, *CallSequence(Seen)), !Authorization.Equals(TEXT("(absent)"), ESearchCase::CaseSensitive));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
