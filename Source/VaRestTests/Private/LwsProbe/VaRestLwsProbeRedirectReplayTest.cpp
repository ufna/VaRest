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
		if (Need > Len)
		{
			// The positive control below then fails and names this
			C.Rig->Record(C.Id, Marker, static_cast<int64>(Len), TEXT("append: no room"));
			return 0;
		}
		char** Cursor = static_cast<char**>(In);
		FMemory::Memcpy(*Cursor, Line, Need);
		*Cursor += Need;
		return 0;
	};
	TArray<FEvent> Seen;
	Visit(*this, Rig, MoveTemp(Spec), Seen);
	// Task 10 review M-4, the positive control: "(absent)" at the target answers the question only if the first request
	// carried the header
	const FString Appended = JournalHeader(FetchJournal(*this, Site, First), TEXT("Authorization"));
	const bool bNoRoom = Seen.ContainsByPredicate([](const FEvent& Each) { return Each.Reason == Marker && Each.Note.StartsWith(TEXT("append: no room")); });
	if (!TestTrue(FString::Printf(TEXT("the first request carried the appended Authorization (it had \"%s\"%s)"), *Appended,
		bNoRoom ? TEXT("; the hook found no room for it") : TEXT("")), Appended.Equals(TEXT("Bearer replay-probe-token"), ESearchCase::CaseSensitive)))
	{
		return false;
	}
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

// Task 10 review M-2: where VaRest can first see a followed redirect whose target refuses TCP. In the Release archive the
// engine links (libWebSockets.Build.cs:25), client-handshake.c.o's lws_client_reset closes the socket (call at 0xc48),
// stores -1 as its fd (0xc78) and calls lws_client_connect_2 (0xdc2); with no socket, lws_client_connect_2 makes one
// (0x3e7-0x3ef, socket() at 0x4cc), raises WSI_CREATE (0x562-0x56f, reason 0x1d) and only then reaches connect() (0x40a,
// through the jumps to 0x3f5 at 0x594 and 0x5b4). This case records the order the callbacks come in, and the fd each
// WSI_CREATE saw. A record (AddInfo), as RedirectReplay
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeRedirectClosedPortTest, "Probe.VaRest.Lws.Handshake.RedirectToClosedPort",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeRedirectClosedPortTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
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
	const FString Key = NewKey();
	FConnectSpec Spec;
	// Loopback port 1, where nothing listens ([hs.connect_refused]); the host's TUN proxy completes any other connect
	Spec.Path = FString::Printf(TEXT("/ws/status/302?location=%s&key=%s"), *UrlValue(TEXT("ws://127.0.0.1:1/")), *Key);
	Spec.Hook = [](FConn& C, lws* Wsi, int32 Reason, void* User, void* In, size_t Len) -> int32
	{
		if (Reason == LWS_CALLBACK_WSI_CREATE)
		{
			C.Rig->Record(C.Id, Marker, static_cast<int64>(lws_get_socket_fd(Wsi)), TEXT("WSI_CREATE fd"));
		}
		return KeepDefault;
	};
	TArray<FEvent> Seen;
	Visit(*this, Rig, MoveTemp(Spec), Seen);
	// Harness: the server answered the first request with the 302
	if (!TestEqual(TEXT("the first request reached the server"), FetchJournal(*this, Site, Key)[TEXT("connections")].AsInt(), 1))
	{
		return false;
	}
	int32 FirstAppend = INDEX_NONE;
	int32 RedirectCreate = INDEX_NONE;
	int32 EndAt = INDEX_NONE;
	int32 Appends = 0;
	TArray<FString> Fds;
	for (int32 Index = 0; Index < Seen.Num(); ++Index)
	{
		const FEvent& Each = Seen[Index];
		if (Each.Reason == Marker && Each.Note.Equals(TEXT("WSI_CREATE fd"), ESearchCase::CaseSensitive))
		{
			Fds.Add(FString::Printf(TEXT("%lld"), static_cast<long long>(Each.Len)));
		}
		else if (Each.Reason == LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER)
		{
			++Appends;
			FirstAppend = FirstAppend == INDEX_NONE ? Index : FirstAppend;
		}
		else if (Each.Reason == LWS_CALLBACK_WSI_CREATE && FirstAppend != INDEX_NONE && RedirectCreate == INDEX_NONE)
		{
			RedirectCreate = Index;
		}
		else if (EndAt == INDEX_NONE && (Each.Reason == LWS_CALLBACK_CLIENT_CONNECTION_ERROR || Each.Reason == LWS_CALLBACK_CLOSED_CLIENT_HTTP
			|| Each.Reason == LWS_CALLBACK_CLIENT_CLOSED || Each.Reason == LWS_CALLBACK_WSI_DESTROY))
		{
			EndAt = Index;
		}
	}
	NoteOutcome(*this, FString::Printf(TEXT("a redirect to a closed port raises a WSI_CREATE after the first APPEND_HANDSHAKE_HEADER and before the attempt ends, with no second APPEND (the client's callbacks: %s; the error: %s; the fd at each WSI_CREATE: %s)"),
		*CallSequence(Seen), *ErrorOf(Seen), *FString::Join(Fds, TEXT(", "))),
		RedirectCreate != INDEX_NONE && EndAt != INDEX_NONE && RedirectCreate < EndAt && Appends == 1);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
