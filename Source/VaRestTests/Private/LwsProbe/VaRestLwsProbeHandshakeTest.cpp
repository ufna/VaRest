// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// WebSocket spec 16.4 (W0): what LWS 3.0 does with subprotocols, the Origin, appended headers, refused and broken
// upgrades and redirects (WebSocket spec 2.2). Library outcomes are facts, never failed assertions (decision 3, review
// M-2): a test fails only when the harness did not run.

#include "VaRestLwsProbe.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"

#if PLATFORM_LINUX
#include <netdb.h>
#include <sys/socket.h>
#endif

namespace VaRest::LwsProbe::VaRestLwsProbeHandshakeTestPrivate
{
	/** ESTABLISHED's note ("bound=<protocol>"), or empty */
	FString BoundNote(const TArray<FEvent>& Seen)
	{
		const FEvent* Open = Seen.FindByPredicate([](const FEvent& Each) { return Each.Reason == LWS_CALLBACK_CLIENT_ESTABLISHED; });
		return Open ? Open->Note : FString();
	}

	FString FilterNote(const TArray<FEvent>& Seen)
	{
		const FEvent* Filter = Seen.FindByPredicate([](const FEvent& Each) { return Each.Reason == LWS_CALLBACK_CLIENT_FILTER_PRE_ESTABLISH; });
		return Filter ? Filter->Note : FString(TEXT("(no FILTER_PRE_ESTABLISH)"));
	}

	/** "established, bound=<p>" or "refused: <error>" */
	FString Outcome(const TArray<FEvent>& Seen)
	{
		return Opened(Seen) ? FString::Printf(TEXT("established, %s"), *BoundNote(Seen)) : FString::Printf(TEXT("refused: %s"), *ErrorOf(Seen));
	}

	/** The notes of the hook's marker events that start with Prefix */
	TArray<FString> Marks(const TArray<FEvent>& Seen, const TCHAR* Prefix)
	{
		TArray<FString> Out;
		for (const FEvent& Each : Seen)
		{
			if (Each.Reason == Marker && Each.Note.StartsWith(Prefix, ESearchCase::CaseSensitive))
			{
				Out.Add(Each.Note);
			}
		}
		return Out;
	}

	/** A redirect fact: recorded, or unverified when the redirect itself was not reached (Task 6 review M-4) */
	struct FRedirect
	{
		EVerdict Verdict = EVerdict::Recorded;
		FString Observed;
	};

	/**
	 * Whether the redirect happened at all: the first request reached the server and the server sent its 3xx (ws.py's
	 * ws_status journals "response" with the status before it answers). Empty when it did; else what was missing
	 */
	FString RedirectMissing(const FVaRestJson& Journal)
	{
		if (JournalEvents(Journal, TEXT("request"), 1).Num() == 0)
		{
			return TEXT("the first request never reached the server");
		}
		const TArray<FVaRestJson> Responses = JournalEvents(Journal, TEXT("response"), 1);
		const int32 Status = Responses.Num() > 0 ? Responses[0][TEXT("status")].AsInt() : 0;
		return Status >= 300 && Status <= 399 ? FString() : FString::Printf(TEXT("the server sent no 3xx (first response status %d, 0: none)"), Status);
	}

	/**
	 * Whether the redirect was followed (review M-1): a second request at the server, or at the client a second
	 * APPEND_HANDSHAKE_HEADER, or -- when the original connection was plain -- a certificate check or a TLS error. A
	 * followed wss redirect can fail its TLS before any request reaches the server: the review (M-1) reads
	 * lws_client_reset as keeping only the scheme's SSL bit, so SslFlags (ALLOW_SELFSIGNED, the host-name skip) do not
	 * carry over, and the name checked becomes the bare address. "Not followed" only for a redirect the server sent
	 * (review M-4); one that never happened reads unverified
	 */
	FRedirect Redirected(const TArray<FEvent>& Seen, const FVaRestJson& Journal, bool bOriginalTls)
	{
		const TArray<FVaRestJson> Second = JournalEvents(Journal, TEXT("request"), 2);
		const int32 Appends = OfReason(Seen, LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER).Num();
		const TArray<FEvent> Checks = bOriginalTls ? TArray<FEvent>() : OfReason(Seen, LWS_CALLBACK_OPENSSL_PERFORM_SERVER_CERT_VERIFICATION);
		const int32 Verifies = Checks.Num();
		const FString Error = ErrorOf(Seen);
		// A heuristic on LWS's error string, after a plain start only (review M-6). This copy's TLS client fails a handshake
		// with "client connect failed", a string only tls-client.c.o holds, so it is matched exactly; the substrings
		// (FString::Contains ignores case) are for other builds: here they match only client.c.o's "HS: Redirect attempted
		// SSL downgrade", which a plain start cannot give, and not client-handshake.c.o's TCP "connect failed"
		const bool bTlsError = !bOriginalTls && (Error.Equals(TEXT("client connect failed"), ESearchCase::CaseSensitive)
			|| Error.Contains(TEXT("ssl")) || Error.Contains(TEXT("tls")) || Error.Contains(TEXT("cert")));
		// The last check's verdict says why a followed wss attempt failed. Error 62 is X509_V_ERR_HOSTNAME_MISMATCH (OpenSSL
		// 1.1.1t x509_vfy.h:166); a direct wss connection to 127.0.0.1 got it too in a Task 6 diagnostic run, although the
		// test leaf lists IP:127.0.0.1 (Tools/testserver/certs/test-server.pem), and LWS 3.0's client sets the name only with
		// X509_VERIFY_PARAM_set1_host (openssl-client.c.o imports no X509_VERIFY_PARAM_set1_ip*)
		const FString LastCheck = Verifies > 0 ? FString::Printf(TEXT(" (the last: %s)"), *Checks.Last().Note) : FString();
		const FString Client = FString::Printf(TEXT("APPEND_HANDSHAKE_HEADER %d times, %d certificate checks after a plain start%s; the client %s"), Appends, Verifies, *LastCheck, *Outcome(Seen));
		FRedirect Out;
		if (Second.Num() > 0)
		{
			Out.Observed = FString::Printf(TEXT("followed: second request %s; %s"), *Second[0][TEXT("target")].AsString(), *Client);
			return Out;
		}
		if (Appends > 1 || Verifies > 0 || bTlsError)
		{
			Out.Observed = FString::Printf(TEXT("followed, and failed before a second request reached the server; %s"), *Client);
			return Out;
		}
		const FString Missing = RedirectMissing(Journal);
		if (!Missing.IsEmpty())
		{
			Out.Verdict = EVerdict::Unverified;
			Out.Observed = FString::Printf(TEXT("the redirect was not reached: %s; %s"), *Missing, *Client);
			return Out;
		}
		Out.Observed = FString::Printf(TEXT("not followed (no second request, no second attempt at the client); %s"), *Client);
		return Out;
	}

	FString RedirectPath(const FString& Location, const FString& Key)
	{
		return FString::Printf(TEXT("/ws/status/302?location=%s&key=%s"), *UrlValue(Location), *Key);
	}

	/** One client stash token, as a callback reads it with lws_hdr_copy (libwebsockets.h:4071-4073, :4111, :4139) */
	FString StashToken(lws* Wsi, lws_token_indexes Token)
	{
		char Buffer[512] = {};
		const int Total = lws_hdr_total_length(Wsi, Token);
		if (Total <= 0 || Total >= static_cast<int>(sizeof(Buffer)))
		{
			return FString::Printf(TEXT("(%d bytes)"), Total);
		}
		lws_hdr_copy(Wsi, Buffer, static_cast<int>(sizeof(Buffer)), Token);
		return Utf8Text(Buffer, FCStringAnsi::Strlen(Buffer));
	}

	/**
	 * Records what each APPEND_HANDSHAKE_HEADER can see of the target (review I-5) as a marker "append <n>: ...", and
	 * optionally vetoes (ruling Q2: a redirected connection fails at the first callback that can): returns 1 in the
	 * VetoAppend-th APPEND_HANDSHAKE_HEADER (0: never; libwebsockets.h:1316-1343 shows a non-zero return failing the
	 * handshake), or in FILTER_PRE_ESTABLISH once a second APPEND_HANDSHAKE_HEADER ran (bVetoFilter)
	 */
	FHook TargetHook(int32 VetoAppend, bool bVetoFilter)
	{
		return [VetoAppend, bVetoFilter](FConn& C, lws* Wsi, int32 Reason, void* User, void* In, size_t Len) -> int32
		{
			const FName AppendsKey(TEXT("appends"));
			if (Reason == LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER && Wsi)
			{
				int64& Appends = C.Counters.FindOrAdd(AppendsKey);
				++Appends;
				C.Rig->Record(C.Id, Marker, Appends, FString::Printf(TEXT("append %lld: host=%s uri=%s peer=%s"), Appends,
					*StashToken(Wsi, _WSI_TOKEN_CLIENT_HOST), *StashToken(Wsi, _WSI_TOKEN_CLIENT_URI), *StashToken(Wsi, _WSI_TOKEN_CLIENT_PEER_ADDRESS)));
				if (Appends == VetoAppend)
				{
					C.Rig->Record(C.Id, Marker, Appends, TEXT("veto in APPEND_HANDSHAKE_HEADER"));
					return 1;
				}
			}
			if (Reason == LWS_CALLBACK_CLIENT_FILTER_PRE_ESTABLISH && bVetoFilter && C.Counters.FindRef(AppendsKey) >= 2)
			{
				C.Rig->Record(C.Id, Marker, 0, TEXT("veto in FILTER_PRE_ESTABLISH"));
				return 1;
			}
			return KeepDefault;
		};
	}

	/** A veto run: where the hook vetoed, what the client saw, and whether a second request reached the server */
	FString Vetoed(const TArray<FEvent>& Seen, const FVaRestJson& Journal)
	{
		const TArray<FString> Vetoes = Marks(Seen, TEXT("veto "));
		if (Vetoes.Num() == 0)
		{
			return FString::Printf(TEXT("not exercised: the hook never vetoed (the redirect was not followed); the client %s"), *Outcome(Seen));
		}
		return FString::Printf(TEXT("%s: the client %s; a second request %s the server"), *Vetoes[0], *Outcome(Seen),
			JournalEvents(Journal, TEXT("request"), 2).Num() > 0 ? TEXT("reached") : TEXT("did not reach"));
	}

	/** The Host header of a journal's ConnIndex-th request, or "(no request)" */
	FString HostOf(const FVaRestJson& Journal, int32 ConnIndex)
	{
		if (JournalEvents(Journal, TEXT("request"), ConnIndex).Num() == 0)
		{
			return TEXT("(no request)");
		}
		return TEXT("Host: ") + JournalHeader(Journal, TEXT("Host"), ConnIndex);
	}

	/** For each reason, whether user and lws_wsi_user(wsi) were the connection's userdata (the first event of that reason) */
	FString UserByReason(const TArray<FEvent>& Seen)
	{
		const int32 Reasons[] = {
			LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER, LWS_CALLBACK_CLIENT_CONNECTION_ERROR, LWS_CALLBACK_CLIENT_ESTABLISHED,
			LWS_CALLBACK_CLOSED, LWS_CALLBACK_CLIENT_CLOSED, LWS_CALLBACK_WSI_DESTROY,
		};
		TArray<FString> Parts;
		for (const int32 Reason : Reasons)
		{
			const FEvent* First = Seen.FindByPredicate([Reason](const FEvent& Each) { return Each.Reason == Reason; });
			Parts.Add(First
				? FString::Printf(TEXT("%s user=%s wsi_user=%s"), *ReasonName(Reason), YesNo((First->Flags & UserIsConnection) != 0), YesNo((First->Flags & WsiUserIsConnection) != 0))
				: FString::Printf(TEXT("%s not seen"), *ReasonName(Reason)));
		}
		return FString::Join(Parts, TEXT("; "));
	}

	/** The claim every StatusInConnectionError case tests */
	const TCHAR* const StatusSpec = TEXT("WebSocket spec 0.3 (inferred from 4.3) and 2.2: lws_http_client_http_response(wsi) gives the upgrade's status inside CONNECTION_ERROR (documented for ESTABLISHED_CLIENT_HTTP only, libwebsockets.h:3595-3607)");

	/**
	 * Calls lws_http_client_http_response(wsi) in CONNECTION_ERROR and records what it returned. Without a header table
	 * it dereferences NULL (client.c.o 0x1294-0x12b2, review C-2): if the process ends here, the PENDING line makes the
	 * passport say so, and the case's own run (decision 10) keeps every other case's fact
	 */
	FHook StatusHook()
	{
		return [](FConn& C, lws* Wsi, int32 Reason, void* User, void* In, size_t Len) -> int32
		{
			if (Reason == LWS_CALLBACK_CLIENT_CONNECTION_ERROR && Wsi)
			{
				C.Rig->Record(C.Id, Marker, static_cast<int64>(lws_http_client_http_response(Wsi)), TEXT("status in CONNECTION_ERROR"));
			}
			return KeepDefault;
		};
	}

	/** What the call returned inside CONNECTION_ERROR, or -1 when CONNECTION_ERROR never came */
	int64 StatusMarked(const TArray<FEvent>& Seen)
	{
		const FEvent* Marked = Seen.FindByPredicate([](const FEvent& Each) { return Each.Reason == Marker; });
		return Marked ? Marked->Len : -1;
	}

	/** One case: agrees when the call returned Expected (0: no status line was parsed); unverified when CONNECTION_ERROR never came */
	void StatusFact(FAutomationTestBase& Test, const TCHAR* Id, const TArray<FEvent>& Seen, int64 Expected, const TCHAR* Evidence)
	{
		const int64 Got = StatusMarked(Seen);
		const FString Observed = Got < 0
			? FString::Printf(TEXT("CONNECTION_ERROR never came (%s)"), *CallSequence(Seen))
			: FString::Printf(TEXT("returned %lld inside CONNECTION_ERROR \"%s\"; the process survived"), Got, *ErrorOf(Seen));
		Fact(Test, Id, Got < 0 ? EVerdict::Unverified : AgreesIf(Got == Expected), Observed, StatusSpec,
			FString::Printf(TEXT("%s; expected %lld"), Evidence, Expected));
	}

	/**
	 * A case whose situation was not reached (no kill ran, no certificate check failed, the upgrade never went out):
	 * unverified with what happened instead, as Task 5's ruling reads a fact that did not measure what it names. Still
	 * a FACT after the PENDING line, so the passport never reads the case as crashed
	 */
	void NotReachedFact(FAutomationTestBase& Test, const TCHAR* Id, const FString& What, const TArray<FEvent>& Seen, const TCHAR* Evidence)
	{
		Fact(Test, Id, EVerdict::Unverified,
			FString::Printf(TEXT("not reached: %s (%s); the call returned %lld (-1: CONNECTION_ERROR never came)"), *What, *CallSequence(Seen), StatusMarked(Seen)),
			StatusSpec, Evidence);
	}

	/** hs.dns_failure's and the DnsFailure case's name (RFC 6761 6.4: .invalid never resolves) */
	const TCHAR* const UnresolvableName = TEXT("nonexistent.invalid");

	/**
	 * Controller ruling (Task 6): a host whose DNS a proxy answers (a TUN proxy in fake-ip mode resolves every name, this
	 * workstation's nonexistent.invalid to 198.18.3.46) cannot show a DNS failure, so the name is first resolved on the
	 * game thread with the call LWS 3.0 makes (getaddrinfo, from lws_client_connect_2: client-handshake.c.o 0x68d). The
	 * first address, or empty when the name did not resolve. Linux only, so the other editors compile without the
	 * headers: elsewhere bOutLookedUp is false and the connection's own events decide (TcpCompleted)
	 */
	FString ResolvedTo(const TCHAR* Name, bool& bOutLookedUp)
	{
#if PLATFORM_LINUX
		bOutLookedUp = true;
		const FTCHARToUTF8 NameUtf8(Name);
		addrinfo Hints = {};
		Hints.ai_family = AF_UNSPEC;
		Hints.ai_socktype = SOCK_STREAM;
		addrinfo* Found = nullptr;
		if (getaddrinfo(NameUtf8.Get(), nullptr, &Hints, &Found) != 0 || !Found)
		{
			return FString();
		}
		char AddressText[64] = {};
		const bool bPrinted = getnameinfo(Found->ai_addr, Found->ai_addrlen, AddressText, static_cast<socklen_t>(sizeof(AddressText)), nullptr, 0, NI_NUMERICHOST) == 0;
		freeaddrinfo(Found);
		return bPrinted ? Utf8Text(AddressText, FCStringAnsi::Strlen(AddressText)) : FString(TEXT("an address getnameinfo did not print"));
#else
		static_cast<void>(Name);
		bOutLookedUp = false;
		return FString();
#endif
	}

	/** The ruling's text for a name that resolved */
	FString ProxiedName(const FString& Address)
	{
		return FString::Printf(TEXT("unverified here: %s resolved to %s (DNS answered by a proxy); release-pass item (a host without a resolving proxy)"), UnresolvableName, *Address);
	}

	/** What the game-thread lookup found, for a fact's evidence */
	FString LookupNote(bool bLookedUp, const FString& Address)
	{
		return !bLookedUp ? FString(TEXT("not resolved first on this platform (the connection's events decide)"))
			: Address.IsEmpty() ? FString(TEXT("getaddrinfo on the game thread first: did not resolve"))
			: FString::Printf(TEXT("getaddrinfo on the game thread first: %s"), *Address);
	}

	/**
	 * Whether the TCP connect completed: LWS compiles the upgrade, and so runs APPEND_HANDSHAKE_HEADER, only to write it
	 * to the socket (client.c.o lws_client_socket_service: lws_generate_client_handshake at 0x3d2, lws_ssl_capable_write
	 * at 0x40e; Task 5's TcpConnected). For a target that must never connect (a refused port, an unresolvable name),
	 * the probe then measured something else (controller ruling, Task 6)
	 */
	bool TcpCompleted(const TArray<FEvent>& Seen)
	{
		return Seen.ContainsByPredicate([](const FEvent& Each) { return Each.Reason == LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER; });
	}

	/** The ruling's text for a name LWS connected to although the lookup did not resolve it (or did not run) */
	FString ConnectedName(const TArray<FEvent>& Seen)
	{
		return FString::Printf(TEXT("unverified here: the TCP connect to %s completed (%s), so a resolver answered for it; release-pass item (a host without a resolving proxy)"),
			UnresolvableName, *CallSequence(Seen));
	}

	/** hs.connect_refused's and the ConnectRefused case's target: loopback, which the TUN proxy does not take */
	const TCHAR* const RefusingAddress = TEXT("127.0.0.1");
	constexpr int32 RefusingPort = 1;

	/** The ruling's text for a "refused" connect that completed (something listens on the port) */
	FString NotRefused(const TArray<FEvent>& Seen)
	{
		return FString::Printf(TEXT("unverified here: the TCP connect to %s:%d completed (%s), so something listens there; release-pass item (a host where nothing listens on %s:%d)"),
			RefusingAddress, RefusingPort, *CallSequence(Seen), RefusingAddress, RefusingPort);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeSubprotocolsTest, "Probe.VaRest.Lws.Handshake.Subprotocols",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeSubprotocolsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeHandshakeTestPrivate;
	// The engine's ::FEvent (HAL/Event.h:21) is visible here too, so the directive alone makes FEvent ambiguous
	using VaRest::LwsProbe::FEvent;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	// Only "varest" is registered (FRig's protocol table); the server selects names the context does not know
	FRig Rig(Site);
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	const FString KeyBound = NewKey();
	FConnectSpec Bound;
	Bound.Path = TEXT("/ws/protocol?select=chat.v2&key=") + KeyBound;
	Bound.Protocols = TEXT("chat.v2, chat.v1");
	FConnectSpec Unbound = Bound;
	Unbound.Path = TEXT("/ws/protocol?select=chat.v2&key=") + NewKey();
	Unbound.LocalProtocolName.Reset();
	FConnectSpec Unoffered;
	Unoffered.Path = TEXT("/ws/protocol?select=evil&key=") + NewKey();
	Unoffered.Protocols = TEXT("chat.v2");
	FConnectSpec NoneSelected;
	NoneSelected.Path = TEXT("/ws/protocol?select=&key=") + NewKey();
	NoneSelected.Protocols = TEXT("chat.v2");
	TArray<FEvent> BoundSeen;
	TArray<FEvent> UnboundSeen;
	TArray<FEvent> UnofferedSeen;
	TArray<FEvent> NoneSeen;
	// A connection that never ends is noted by Visit, not failed (review M-2)
	Visit(*this, Rig, Bound, BoundSeen);
	Visit(*this, Rig, Unbound, UnboundSeen);
	Visit(*this, Rig, Unoffered, UnofferedSeen);
	Visit(*this, Rig, NoneSelected, NoneSeen);

	Fact(*this, TEXT("hs.unregistered_selected_bound"), AgreesIf(Opened(BoundSeen) && BoundNote(BoundSeen).Equals(TEXT("bound=varest"), ESearchCase::CaseSensitive)),
		Outcome(BoundSeen),
		TEXT("WebSocket spec 0.3 (inferred from 4.3) and 2.2: a server-selected subprotocol unknown to the context is accepted when local_protocol_name pre-binds the connection"),
		TEXT("offered \"chat.v2, chat.v1\", the server selected chat.v2, only \"varest\" registered, local_protocol_name = \"varest\""));
	const FString Filter = FilterNote(BoundSeen);
	Fact(*this, TEXT("hs.selected_protocol_in_filter"), AgreesIf(Filter.StartsWith(TEXT("protocol=chat.v2 "), ESearchCase::CaseSensitive)), Filter,
		TEXT("T6: the selected protocol is read with lws_hdr_copy(WSI_TOKEN_PROTOCOL) in CLIENT_FILTER_PRE_ESTABLISH"),
		TEXT("the bound connection's FILTER_PRE_ESTABLISH"));
	Fact(*this, TEXT("hs.unregistered_selected_unbound"), EVerdict::Recorded, Outcome(UnboundSeen),
		TEXT("none: the engine's way, .protocol both binds and offers (LwsWebSocket.cpp:805)"), TEXT("the same, local_protocol_name = NULL"));
	Fact(*this, TEXT("hs.unoffered_selected"), EVerdict::Recorded, Outcome(UnofferedSeen),
		TEXT("none: RFC 6455 4.1 says the client fails a connection whose selected protocol it did not offer"), TEXT("offered chat.v2, the server selected evil"));
	Fact(*this, TEXT("hs.none_selected"), EVerdict::Recorded, Outcome(NoneSeen),
		TEXT("none"), TEXT("offered chat.v2, the server selected none"));
	const FVaRestJson Journal = FetchJournal(*this, Site, KeyBound);
	if (!TestTrue(TEXT("the server's journal arrived"), Journal.IsValid()))
	{
		return false;
	}
	Fact(*this, TEXT("hs.offered_header"), EVerdict::Recorded, TEXT("Sec-WebSocket-Protocol: ") + JournalHeader(Journal, TEXT("Sec-WebSocket-Protocol")),
		TEXT("T3: .protocol is the list of protocols offered"), TEXT("what the server received for .protocol = \"chat.v2, chat.v1\""));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeOriginAndHeadersTest, "Probe.VaRest.Lws.Handshake.OriginAndHeaders",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeOriginAndHeadersTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeHandshakeTestPrivate;
	using VaRest::LwsProbe::FEvent;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	FRig Raw(Site);
	FRigOptions PlainOptions;
	PlainOptions.ContextOptions &= ~static_cast<uint32>(LWS_SERVER_OPTION_JUST_USE_RAW_ORIGIN);
	FRig Plain(Site, MoveTemp(PlainOptions));
	if (!TestTrue(TEXT("both probe contexts exist"), Raw.IsReady() && Plain.IsReady()))
	{
		return false;
	}
	const FString KeyRaw = NewKey();
	const FString KeyPlain = NewKey();
	const FString KeyNone = NewKey();
	const FString KeyFill = NewKey();
	FConnectSpec RawSpec;
	RawSpec.Path = TEXT("/ws/echo?key=") + KeyRaw;
	RawSpec.Origin = FString(TEXT("https://game.example:8443"));
	FConnectSpec PlainSpec;
	PlainSpec.Path = TEXT("/ws/echo?key=") + KeyPlain;
	PlainSpec.Origin = FString(TEXT("game.example"));
	FConnectSpec NoneSpec;
	NoneSpec.Path = TEXT("/ws/echo?key=") + KeyNone;
	TArray<FEvent> RawSeen;
	TArray<FEvent> PlainSeen;
	TArray<FEvent> NoneSeen;
	Visit(*this, Raw, RawSpec, RawSeen);
	Visit(*this, Plain, PlainSpec, PlainSeen);
	Visit(*this, Raw, NoneSpec, NoneSeen);

	// The space APPEND_HANDSHAKE_HEADER reports with the context's 32 KiB header buffer, then a header that fills it but 512 bytes
	const TArray<FEvent> Appends = OfReason(RawSeen, LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER);
	// Review I-2, the precondition hs.append_header_space and hs.append_header_user read: without it they would be written
	// from zeroed state ("len = -1", "is not"), a harness failure dressed as a library outcome
	if (!TestTrue(TEXT("the raw-origin connection reached APPEND_HANDSHAKE_HEADER"), Appends.Num() > 0))
	{
		return false;
	}
	const int64 Space = Appends.Num() > 0 ? Appends[0].Len : -1;
	const uint32 AppendFlags = Appends.Num() > 0 ? Appends[0].Flags : 0u;
	const int32 FillBytes = Space > 1024 ? static_cast<int32>(Space - 512) : 0;
	FConnectSpec FillSpec;
	FillSpec.Path = TEXT("/ws/echo?key=") + KeyFill;
	if (FillBytes > 0)
	{
		FillSpec.AppendHeaders.Add(TEXT("X-Fill: ") + FString::ChrN(FillBytes, TEXT('a')));
	}
	FillSpec.AppendHeaders.Add(TEXT("Authorization: Bearer probe-fill-token"));
	TArray<FEvent> FillSeen;
	Visit(*this, Raw, FillSpec, FillSeen);
	const FVaRestJson RawJournal = FetchJournal(*this, Site, KeyRaw);
	const FVaRestJson PlainJournal = FetchJournal(*this, Site, KeyPlain);
	const FVaRestJson NoneJournal = FetchJournal(*this, Site, KeyNone);
	const FVaRestJson FillJournal = FetchJournal(*this, Site, KeyFill);
	if (!TestTrue(TEXT("every server journal arrived"), RawJournal.IsValid() && PlainJournal.IsValid() && NoneJournal.IsValid() && FillJournal.IsValid()))
	{
		return false;
	}

	const FString RawOrigin = JournalHeader(RawJournal, TEXT("Origin"));
	Fact(*this, TEXT("hs.raw_origin"), AgreesIf(RawOrigin.Equals(TEXT("https://game.example:8443"), ESearchCase::CaseSensitive)), TEXT("Origin: ") + RawOrigin,
		TEXT("T4: with LWS_SERVER_OPTION_JUST_USE_RAW_ORIGIN, .origin is sent as given (libwebsockets.h:2618-2624)"),
		TEXT("a context with the option; .origin = \"https://game.example:8443\""));
	Fact(*this, TEXT("hs.default_origin"), EVerdict::Recorded, TEXT("Origin: ") + JournalHeader(PlainJournal, TEXT("Origin")),
		TEXT("WebSocket spec E20: without the option LWS prefixes http:// (the engine sets no raw-origin option, LwsWebSocketsManager.cpp:184)"),
		TEXT("a context without the option; .origin = \"game.example\""));
	Fact(*this, TEXT("hs.origin_omitted"), EVerdict::Recorded, TEXT("Origin: ") + JournalHeader(NoneJournal, TEXT("Origin")),
		TEXT("WebSocket spec D29: WithOrigin({}) omits it"), TEXT(".origin = NULL"));
	Fact(*this, TEXT("hs.append_header_space"), AgreesIf(Space > 30000), FString::Printf(TEXT("len = %lld bytes"), Space),
		TEXT("T5 and WebSocket spec 15: len is the space left in the 32 KiB handshake header buffer"),
		TEXT("APPEND_HANDSHAKE_HEADER with max_http_header_data2 = pt_serv_buf_size = 32 KiB"));
	Fact(*this, TEXT("hs.append_header_user"), EVerdict::Recorded,
		FString::Printf(TEXT("the user argument %s the connection's userdata; lws_wsi_user(wsi) %s"),
			(AppendFlags & UserIsConnection) != 0 ? TEXT("is") : TEXT("is not"), (AppendFlags & WsiUserIsConnection) != 0 ? TEXT("is") : TEXT("is not")),
		TEXT("libwebsockets.h:1316-1343: user is NULL and the callback goes to protocols[0]; the engine takes its socket from user there (LwsWebSocketsManager.cpp:434, :470, :477)"),
		TEXT("the first APPEND_HANDSHAKE_HEADER of a connection"));
	const FString Fill = JournalHeader(FillJournal, TEXT("X-Fill"));
	const FString Bearer = JournalHeader(FillJournal, TEXT("Authorization"));
	const bool bFilled = FillBytes > 0 && Fill.Len() == FillBytes && Bearer.Equals(TEXT("Bearer probe-fill-token"), ESearchCase::CaseSensitive) && Opened(FillSeen);
	Fact(*this, TEXT("hs.append_header_fill"), AgreesIf(bFilled),
		FString::Printf(TEXT("an X-Fill value of %d bytes arrived as %d, Authorization \"%s\"; %s"), FillBytes, Fill.Len(), *Bearer, *Outcome(FillSeen)),
		TEXT("WebSocket spec 2.2: headers appended in APPEND_HANDSHAKE_HEADER, the remaining length checked per header (T5)"),
		TEXT("two headers filling all but 512 bytes of the reported len"));
	Fact(*this, TEXT("hs.host_header"), EVerdict::Recorded, TEXT("Host: ") + JournalHeader(RawJournal, TEXT("Host")),
		TEXT("WebSocket spec 2.2: Host keeps a non-default port"), TEXT(".host = \"<address>:<port>\""));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeRefusalsTest, "Probe.VaRest.Lws.Handshake.Refusals",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeHandshakeTestPrivate;
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
	struct FRefusal
	{
		int32 Code;
		const TCHAR* Expected;
	};
	const FRefusal Cases[] = {
		{ 401, TEXT("HS: ws upgrade unauthorized") },
		{ 403, TEXT("HS: ws upgrade forbidden") },
		{ 404, TEXT("HS: ws upgrade not 101, was 404") },
		{ 429, TEXT("HS: ws upgrade not 101, was 429") },
		{ 500, TEXT("HS: ws upgrade not 101, was 500") },
		{ 503, TEXT("HS: ws upgrade not 101, was 503") },
	};
	TArray<FEvent> RefusedSeen403;
	for (const FRefusal& Case : Cases)
	{
		FConnectSpec Spec;
		Spec.Path = FString::Printf(TEXT("/ws/status/%d?key=%s"), Case.Code, *NewKey());
		TArray<FEvent> Seen;
		Visit(*this, Rig, MoveTemp(Spec), Seen);
		const FString Error = ErrorOf(Seen);
		Fact(*this, FString::Printf(TEXT("hs.refusal_%d"), Case.Code), AgreesIf(Error.Equals(Case.Expected, ESearchCase::CaseSensitive)), Error,
			FString::Printf(TEXT("T7: \"%s\" (the strings in libwebsockets.a)"), Case.Expected), FString::Printf(TEXT("the upgrade answered %d"), Case.Code));
		if (Case.Code == 403)
		{
			RefusedSeen403 = Seen;
		}
	}
	Fact(*this, TEXT("hs.refusal_sequence"), EVerdict::Recorded, CallSequence(RefusedSeen403), TEXT("none"), TEXT("the callbacks of the 403 attempt"));

	// Review M-14: which callbacks carry the connection in user. The engine takes its socket from user for every
	// connection reason it handles, WSI_DESTROY included (LwsWebSocketsManager.cpp:434, :458-461, :463-477), except the
	// certificate check, where user is the X509_STORE_CTX and the socket comes from lws_wsi_user (:447-456)
	FConnectSpec Echo;
	Echo.Path = TEXT("/ws/echo?key=") + NewKey();
	TArray<FEvent> EchoSeen;
	Visit(*this, Rig, MoveTemp(Echo), EchoSeen);
	TArray<FEvent> Both = RefusedSeen403;
	Both.Append(EchoSeen);
	Fact(*this, TEXT("hs.callback_user_by_reason"), EVerdict::Recorded, UserByReason(Both),
		TEXT("none: libwebsockets.h:1316-1343 says user is NULL in APPEND_HANDSHAKE_HEADER; W1 finds its socket per reason from this row"),
		TEXT("the first event of each reason across the 403 attempt and an echo connection closed with 1000"));

	struct FBroken
	{
		const TCHAR* Id;
		const TCHAR* Path;
		const TCHAR* Expected;
	};
	const FBroken Brokens[] = {
		{ TEXT("hs.bad_accept"), TEXT("/ws/broken?what=accept"), TEXT("HS: Accept hash wrong") },
		{ TEXT("hs.missing_upgrade"), TEXT("/ws/broken?what=upgrade"), TEXT("HS: UPGRADE missing") },
		{ TEXT("hs.missing_connection"), TEXT("/ws/broken?what=connection"), TEXT("HS: CONNECTION missing") },
	};
	for (const FBroken& Case : Brokens)
	{
		FConnectSpec Spec;
		Spec.Path = Case.Path;
		TArray<FEvent> Seen;
		Visit(*this, Rig, MoveTemp(Spec), Seen);
		const FString Error = ErrorOf(Seen);
		Fact(*this, Case.Id, AgreesIf(Error.Equals(Case.Expected, ESearchCase::CaseSensitive)), Error,
			FString::Printf(TEXT("WebSocket spec 2.2: \"%s\" is among the parser tests' strings"), Case.Expected), Case.Path);
	}

	FConnectSpec NoUpgrade;
	NoUpgrade.Path = TEXT("/ws/noupgrade");
	TArray<FEvent> NoUpgradeSeen;
	Visit(*this, Rig, MoveTemp(NoUpgrade), NoUpgradeSeen);
	Fact(*this, TEXT("hs.no_upgrade_response"), EVerdict::Recorded,
		FString::Printf(TEXT("\"%s\" (connect returned %s)"), *ErrorOf(NoUpgradeSeen), *ConnectResult(NoUpgradeSeen)),
		TEXT("WebSocket spec 2.2 and 22 A2: NoUpgradeResponse, upgrade sent and the connection closed before any status line; W0 pins the string"),
		TEXT("/ws/noupgrade reads the request and ends TCP"));

	FConnectSpec Refused;
	Refused.Address = RefusingAddress;
	Refused.Port = RefusingPort;
	TArray<FEvent> ConnectRefusedSeen;
	Visit(*this, Rig, MoveTemp(Refused), ConnectRefusedSeen);
	// Controller ruling (Task 6): a connect that completed was not refused, whatever the error says
	Fact(*this, TEXT("hs.connect_refused"), TcpCompleted(ConnectRefusedSeen) ? EVerdict::Unverified : EVerdict::Recorded,
		TcpCompleted(ConnectRefusedSeen) ? NotRefused(ConnectRefusedSeen)
			: FString::Printf(TEXT("\"%s\" (connect returned %s)"), *ErrorOf(ConnectRefusedSeen), *ConnectResult(ConnectRefusedSeen)),
		TEXT("WebSocket spec 2.2: \"Could not initialize connection\" is the engine's own text for a null connect (LwsWebSocket.cpp:813-824)"),
		TEXT("TCP to 127.0.0.1:1, where nothing listens"));

	// Controller ruling (Task 6): the name is resolved here first; a name that resolves is not probed
	const TCHAR* const DnsSpec = TEXT("WebSocket spec 3.4: a DNS failure reconnects");
	bool bLookedUp = false;
	const FString Resolved = ResolvedTo(UnresolvableName, bLookedUp);
	const FString DnsEvidence = FString::Printf(TEXT("nonexistent.invalid (RFC 6761: never resolves); %s"), *LookupNote(bLookedUp, Resolved));
	if (!Resolved.IsEmpty())
	{
		Fact(*this, TEXT("hs.dns_failure"), EVerdict::Unverified, ProxiedName(Resolved), DnsSpec, DnsEvidence);
		return true;
	}
	FConnectSpec Unknown;
	Unknown.Address = UnresolvableName;
	Unknown.Port = 80;
	Unknown.Host = UnresolvableName;
	TArray<FEvent> UnknownSeen;
	Visit(*this, Rig, MoveTemp(Unknown), UnknownSeen);
	Fact(*this, TEXT("hs.dns_failure"), TcpCompleted(UnknownSeen) ? EVerdict::Unverified : EVerdict::Recorded,
		TcpCompleted(UnknownSeen) ? ConnectedName(UnknownSeen)
			: FString::Printf(TEXT("\"%s\" (connect returned %s)"), *ErrorOf(UnknownSeen), *ConnectResult(UnknownSeen)),
		DnsSpec, DnsEvidence);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeRedirectsTest, "Probe.VaRest.Lws.Handshake.Redirects",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeRedirectsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeHandshakeTestPrivate;
	using VaRest::LwsProbe::FEvent;
	FServer Site;
	if (!FindServer(*this, Site, true))
	{
		return false;
	}
	TArray<uint8> Ca;
	if (!FetchBytes(*this, Site, TEXT("/tls/ca.pem"), Ca))
	{
		return false;
	}
	// TLS in the context, trusting the test CA, so a redirect to wss can be followed and verified
	FRigOptions Options;
	Options.Tls = ETls::LwsGlobalInit;
	Options.OnLoadExtraClientVerifyCerts = [Ca](SSL_CTX* Ctx) { AddPemToStore(Ctx, Ca); };
	FRig Rig(Site, MoveTemp(Options));
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	const FString KeyRelative = NewKey();
	const FString KeyAbsolute = NewKey();
	const FString KeyToTls = NewKey();
	const FString KeyDown = NewKey();
	const FString KeyVetoAppend = NewKey();
	const FString KeyVetoFilter = NewKey();
	FConnectSpec Relative;
	Relative.Path = RedirectPath(TEXT("/ws/echo?key=") + KeyRelative, KeyRelative);
	Relative.AppendHeaders.Add(TEXT("Authorization: Bearer redirect-probe-token"));
	Relative.Hook = TargetHook(0, false);
	FConnectSpec Absolute;
	Absolute.Path = RedirectPath(FString::Printf(TEXT("ws://%s:%d/ws/echo?key=%s"), *Site.Host, Site.Port, *KeyAbsolute), KeyAbsolute);
	Absolute.Hook = TargetHook(0, false);
	FConnectSpec ToTls;
	ToTls.Path = RedirectPath(FString::Printf(TEXT("wss://%s:%d/ws/echo?key=%s"), *Site.Host, Site.TlsPort, *KeyToTls), KeyToTls);
	ToTls.Hook = TargetHook(0, false);
	FConnectSpec Down;
	Down.bTls = true;
	Down.SslFlags = LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
	Down.Path = RedirectPath(FString::Printf(TEXT("ws://%s:%d/ws/echo?key=%s"), *Site.Host, Site.Port, *KeyDown), KeyDown);
	// Ruling Q2's two veto points, on the relative redirect: the redirected request's APPEND_HANDSHAKE_HEADER, and its FILTER_PRE_ESTABLISH
	FConnectSpec VetoAppend;
	VetoAppend.Path = RedirectPath(TEXT("/ws/echo?key=") + KeyVetoAppend, KeyVetoAppend);
	VetoAppend.Hook = TargetHook(2, false);
	FConnectSpec VetoFilter;
	VetoFilter.Path = RedirectPath(TEXT("/ws/echo?key=") + KeyVetoFilter, KeyVetoFilter);
	VetoFilter.Hook = TargetHook(0, true);
	TArray<FEvent> RelativeSeen;
	TArray<FEvent> AbsoluteSeen;
	TArray<FEvent> ToTlsSeen;
	TArray<FEvent> DownSeen;
	TArray<FEvent> VetoAppendSeen;
	TArray<FEvent> VetoFilterSeen;
	Visit(*this, Rig, Relative, RelativeSeen);
	Visit(*this, Rig, Absolute, AbsoluteSeen);
	Visit(*this, Rig, ToTls, ToTlsSeen);
	Visit(*this, Rig, Down, DownSeen);
	Visit(*this, Rig, VetoAppend, VetoAppendSeen);
	Visit(*this, Rig, VetoFilter, VetoFilterSeen);
	// Every client connection has ended or been noted: a second request that has not arrived within 3 s is not coming
	const FVaRestJson RelativeJournal = FetchJournal(*this, Site, KeyRelative, 2, 3000);
	const FVaRestJson AbsoluteJournal = FetchJournal(*this, Site, KeyAbsolute, 2, 3000);
	const FVaRestJson ToTlsJournal = FetchJournal(*this, Site, KeyToTls, 2, 3000);
	const FVaRestJson DownJournal = FetchJournal(*this, Site, KeyDown, 2, 3000);
	const FVaRestJson VetoAppendJournal = FetchJournal(*this, Site, KeyVetoAppend, 2, 3000);
	const FVaRestJson VetoFilterJournal = FetchJournal(*this, Site, KeyVetoFilter, 2, 3000);
	if (!TestTrue(TEXT("every server journal arrived"), RelativeJournal.IsValid() && AbsoluteJournal.IsValid() && ToTlsJournal.IsValid()
		&& DownJournal.IsValid() && VetoAppendJournal.IsValid() && VetoFilterJournal.IsValid()))
	{
		return false;
	}
	const FString RedirectSpec = TEXT("WebSocket spec 3.4 row \"upgrade 3xx (redirect not followed or failed)\" and ruling Q2 (2026-10-21: VaRest never follows an upgrade redirect); libwebsockets.a holds \"HS: Redirect failed\" and lws_client_reset");
	const FRedirect RelativeResult = Redirected(RelativeSeen, RelativeJournal, false);
	Fact(*this, TEXT("hs.redirect_relative"), RelativeResult.Verdict, RelativeResult.Observed, RedirectSpec,
		TEXT("302 with Location: /ws/echo?key=<k>"));
	const FRedirect AbsoluteResult = Redirected(AbsoluteSeen, AbsoluteJournal, false);
	Fact(*this, TEXT("hs.redirect_absolute"), AbsoluteResult.Verdict, AbsoluteResult.Observed, RedirectSpec,
		TEXT("302 with Location: ws://<host>:<port>/ws/echo?key=<k>"));
	// Review M-5: the header of a second request only when there was one
	const FString SecondAuthorization = JournalEvents(RelativeJournal, TEXT("request"), 2).Num() > 0
		? FString::Printf(TEXT("Authorization on the second request: %s"), *JournalHeader(RelativeJournal, TEXT("Authorization"), 2))
		: FString(TEXT("no second request"));
	Fact(*this, TEXT("hs.redirect_reappends_headers"), EVerdict::Recorded,
		FString::Printf(TEXT("APPEND_HANDSHAKE_HEADER ran %d times; %s"), OfReason(RelativeSeen, LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER).Num(), *SecondAuthorization),
		TEXT("WebSocket spec 3.3: auth headers go on every attempt; ruling Q2: they must never reach another host"),
		TEXT("the relative redirect, with Authorization appended"));
	const FRedirect ToTlsResult = Redirected(ToTlsSeen, ToTlsJournal, false);
	Fact(*this, TEXT("hs.redirect_to_tls"), ToTlsResult.Verdict, ToTlsResult.Observed, RedirectSpec,
		TEXT("ws:// answered 302 with Location: wss://<host>:<tls port>/ws/echo, the test CA trusted; followed is decided from the client's events too (review M-1)"));
	const FRedirect DownResult = Redirected(DownSeen, DownJournal, true);
	Fact(*this, TEXT("hs.redirect_downgrade"), DownResult.Verdict, DownResult.Observed,
		TEXT("none: libwebsockets.a holds \"HS: Redirect attempted SSL downgrade\""), TEXT("wss:// answered 302 with Location: ws://<host>:<port>/ws/echo"));

	// Review I-5: what W1 needs to write ruling Q2's rule from evidence
	const TArray<FString> RelativeTargets = Marks(RelativeSeen, TEXT("append "));
	const TArray<FString> AbsoluteTargets = Marks(AbsoluteSeen, TEXT("append "));
	const TArray<FString> ToTlsTargets = Marks(ToTlsSeen, TEXT("append "));
	Fact(*this, TEXT("hs.redirect_target_visible"), EVerdict::Recorded,
		FString::Printf(TEXT("relative: [%s]; absolute: [%s]; to wss: [%s]"), *FString::Join(RelativeTargets, TEXT(" | ")),
			*FString::Join(AbsoluteTargets, TEXT(" | ")), *FString::Join(ToTlsTargets, TEXT(" | "))),
		TEXT("ruling Q2: the appended-header callback adds nothing once the target differs from the original scheme+host+port; libwebsockets.h:4071-4073 lists the client stash tokens"),
		TEXT("lws_hdr_copy of _WSI_TOKEN_CLIENT_HOST, _URI and _PEER_ADDRESS in every APPEND_HANDSHAKE_HEADER; the plain redirects stay on the test server's host and port (one listener), so a changed uri shows whether the tokens follow a redirect"));
	const bool bAppendVetoed = Marks(VetoAppendSeen, TEXT("veto ")).Num() > 0;
	const bool bFilterVetoed = Marks(VetoFilterSeen, TEXT("veto ")).Num() > 0;
	Fact(*this, TEXT("hs.redirect_veto"),
		bAppendVetoed && bFilterVetoed ? AgreesIf(!Opened(VetoAppendSeen) && !Opened(VetoFilterSeen)) : EVerdict::Recorded,
		FString::Printf(TEXT("in APPEND_HANDSHAKE_HEADER: %s; in FILTER_PRE_ESTABLISH: %s"), *Vetoed(VetoAppendSeen, VetoAppendJournal), *Vetoed(VetoFilterSeen, VetoFilterJournal)),
		TEXT("ruling Q2: the socket fails a redirected connection at the first callback that can (FILTER_PRE_ESTABLISH or establish) with a redirect error; a non-zero return fails a handshake (libwebsockets.h:1301-1307, :1316-1343)"),
		TEXT("the relative redirect, the hook returning 1 in the second APPEND_HANDSHAKE_HEADER, and separately in FILTER_PRE_ESTABLISH after a second APPEND_HANDSHAKE_HEADER; the error string and the server's second request recorded"));
	Fact(*this, TEXT("hs.redirect_host_header"), EVerdict::Recorded,
		FString::Printf(TEXT("relative: %s (first request %s); absolute: %s (first request %s)"), *HostOf(RelativeJournal, 2), *HostOf(RelativeJournal, 1),
			*HostOf(AbsoluteJournal, 2), *HostOf(AbsoluteJournal, 1)),
		TEXT("WebSocket spec 2.2: Host keeps a non-default port; the review (I-5) reads lws_client_reset as given the address as host, dropping the port"),
		TEXT("the Host header of each redirect's first and second request at the server"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeStatusInErrorRefusedTest, "Probe.VaRest.Risky.Lws.StatusInConnectionError.Refused",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeStatusInErrorRefusedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeHandshakeTestPrivate;
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
	// The status-line parser's errors: the header table is still attached when CONNECTION_ERROR comes
	Pending(TEXT("hs.status_in_connection_error.refused"));
	const int32 Codes[] = { 401, 403, 404, 503 };
	TArray<FString> Parts;
	bool bAllMatch = true;
	bool bAllCame = true;
	for (const int32 Code : Codes)
	{
		FConnectSpec Spec;
		Spec.Path = FString::Printf(TEXT("/ws/status/%d?key=%s"), Code, *NewKey());
		Spec.Hook = StatusHook();
		TArray<FEvent> Seen;
		Visit(*this, Rig, MoveTemp(Spec), Seen);
		const int64 Got = StatusMarked(Seen);
		bAllMatch = bAllMatch && Got == Code;
		bAllCame = bAllCame && Got >= 0;
		Parts.Add(FString::Printf(TEXT("%d -> %lld"), Code, Got));
	}
	Fact(*this, TEXT("hs.status_in_connection_error.refused"), bAllCame ? AgreesIf(bAllMatch) : EVerdict::Unverified,
		FString::Printf(TEXT("%s; the process survived"), *FString::Join(Parts, TEXT(", "))), StatusSpec,
		TEXT("upgrades refused with 401, 403, 404, 503 (status-line parser errors); -1: CONNECTION_ERROR never came"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeStatusInErrorConnectRefusedTest, "Probe.VaRest.Risky.Lws.StatusInConnectionError.ConnectRefused",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeStatusInErrorConnectRefusedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeHandshakeTestPrivate;
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
	Pending(TEXT("hs.status_in_connection_error.connect_refused"));
	FConnectSpec Spec;
	Spec.Address = RefusingAddress;
	Spec.Port = RefusingPort;
	Spec.Hook = StatusHook();
	TArray<FEvent> Seen;
	Visit(*this, Rig, MoveTemp(Spec), Seen);
	const TCHAR* const Evidence = TEXT("TCP to 127.0.0.1:1, where nothing listens: no response at all");
	// Controller ruling (Task 6): a connect that completed did not test the refused case
	if (TcpCompleted(Seen))
	{
		Fact(*this, TEXT("hs.status_in_connection_error.connect_refused"), EVerdict::Unverified,
			FString::Printf(TEXT("%s; the call returned %lld"), *NotRefused(Seen), StatusMarked(Seen)), StatusSpec, Evidence);
		return true;
	}
	StatusFact(*this, TEXT("hs.status_in_connection_error.connect_refused"), Seen, 0, Evidence);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeStatusInErrorDnsFailureTest, "Probe.VaRest.Risky.Lws.StatusInConnectionError.DnsFailure",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeStatusInErrorDnsFailureTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeHandshakeTestPrivate;
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
	// Controller ruling (Task 6): resolved on the game thread first; a name that resolves gets its FACT, after the
	// PENDING line, without the risky call
	bool bLookedUp = false;
	const FString Resolved = ResolvedTo(UnresolvableName, bLookedUp);
	const FString Evidence = FString::Printf(TEXT("nonexistent.invalid (RFC 6761: never resolves): no TCP, no response; %s"), *LookupNote(bLookedUp, Resolved));
	Pending(TEXT("hs.status_in_connection_error.dns_failure"));
	if (!Resolved.IsEmpty())
	{
		Fact(*this, TEXT("hs.status_in_connection_error.dns_failure"), EVerdict::Unverified, ProxiedName(Resolved), StatusSpec,
			Evidence + TEXT("; the risky call was not made"));
		return true;
	}
	FConnectSpec Spec;
	Spec.Address = UnresolvableName;
	Spec.Port = 80;
	Spec.Host = UnresolvableName;
	Spec.Hook = StatusHook();
	TArray<FEvent> Seen;
	Visit(*this, Rig, MoveTemp(Spec), Seen);
	if (TcpCompleted(Seen))
	{
		Fact(*this, TEXT("hs.status_in_connection_error.dns_failure"), EVerdict::Unverified,
			FString::Printf(TEXT("%s; the call returned %lld"), *ConnectedName(Seen), StatusMarked(Seen)), StatusSpec, Evidence);
		return true;
	}
	StatusFact(*this, TEXT("hs.status_in_connection_error.dns_failure"), Seen, 0, *Evidence);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeStatusInErrorNoUpgradeTest, "Probe.VaRest.Risky.Lws.StatusInConnectionError.NoUpgrade",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeStatusInErrorNoUpgradeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeHandshakeTestPrivate;
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
	Pending(TEXT("hs.status_in_connection_error.no_upgrade"));
	FConnectSpec Spec;
	Spec.Path = TEXT("/ws/noupgrade");
	Spec.Hook = StatusHook();
	TArray<FEvent> Seen;
	Visit(*this, Rig, MoveTemp(Spec), Seen);
	const TCHAR* const Evidence = TEXT("/ws/noupgrade reads the request and ends TCP before any status line");
	// The case is "the upgrade went out and no status line came": an attempt whose connect never completed is another case
	if (!TcpCompleted(Seen))
	{
		NotReachedFact(*this, TEXT("hs.status_in_connection_error.no_upgrade"), TEXT("the upgrade never went out"), Seen, Evidence);
		return true;
	}
	StatusFact(*this, TEXT("hs.status_in_connection_error.no_upgrade"), Seen, 0, Evidence);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeStatusInErrorTlsVerifyFailureTest, "Probe.VaRest.Risky.Lws.StatusInConnectionError.TlsVerifyFailure",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeStatusInErrorTlsVerifyFailureTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeHandshakeTestPrivate;
	using VaRest::LwsProbe::FEvent;
	FServer Site;
	if (!FindServer(*this, Site, true))
	{
		return false;
	}
	// TLS in the context and no trust anchor at all (the rig disables the OS CAs and loads no extra one): the test CA is
	// untrusted, so the TLS handshake fails before any HTTP
	FRigOptions Options;
	Options.Tls = ETls::LwsGlobalInit;
	FRig Rig(Site, MoveTemp(Options));
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	Pending(TEXT("hs.status_in_connection_error.tls_verify_failure"));
	FConnectSpec Spec;
	Spec.bTls = true;
	Spec.Path = TEXT("/ws/echo?key=") + NewKey();
	Spec.Hook = StatusHook();
	TArray<FEvent> Seen;
	Visit(*this, Rig, MoveTemp(Spec), Seen);
	// Review M-3: the last check's verdict (preverify, error, depth) in the evidence, so an untrusted CA and a host-name
	// mismatch (error 62, which LWS 3.0 gives any IP-literal host: see Redirected) read apart
	const TArray<FEvent> Checks = OfReason(Seen, LWS_CALLBACK_OPENSSL_PERFORM_SERVER_CERT_VERIFICATION);
	const FString Evidence = FString::Printf(TEXT("wss to the TLS listener with its CA untrusted: the certificate check fails, no response; the last certificate check: %s"),
		Checks.Num() > 0 ? *Checks.Last().Note : TEXT("none"));
	// The case is a failed certificate check: PERFORM_SERVER_CERT_VERIFICATION with preverify_ok 0, its len (libwebsockets.h:1015-1044)
	const bool bCheckFailed = Checks.ContainsByPredicate([](const FEvent& Each) { return Each.Len == 0; });
	if (!bCheckFailed)
	{
		NotReachedFact(*this, TEXT("hs.status_in_connection_error.tls_verify_failure"), TEXT("no certificate check failed"), Seen, *Evidence);
		return true;
	}
	StatusFact(*this, TEXT("hs.status_in_connection_error.tls_verify_failure"), Seen, 0, *Evidence);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeStatusInErrorKilledPendingTest, "Probe.VaRest.Risky.Lws.StatusInConnectionError.KilledPending",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeStatusInErrorKilledPendingTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeHandshakeTestPrivate;
	using VaRest::LwsProbe::FEvent;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	// The short wait, so the kill lands within a second (review C-1)
	FRigOptions Options;
	Options.ServiceTimeoutMs = 100;
	FRig Rig(Site, MoveTemp(Options));
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	Pending(TEXT("hs.status_in_connection_error.killed_pending"));
	FConnectSpec Spec;
	Spec.Path = TEXT("/ws/hang?ms=60000&key=") + NewKey();
	Spec.Hook = StatusHook();
	const int32 Conn = Rig.Connect(MoveTemp(Spec));
	if (Rig.WaitReason(Conn, ConnectReturned))
	{
		// The upgrade is out and unanswered by now: WebSocket spec 2.2's ConnectTimeout kill
		FPlatformProcess::Sleep(1.0f);
		Rig.ExecOn(Conn, TEXT("kill"), [](FConn& C) { lws_set_timeout(C.Wsi, PENDING_TIMEOUT_USER_REASON_BASE, LWS_TO_KILL_ASYNC); });
	}
	NoteOutcome(*this, TEXT("the killed attempt ended within 25 s"), Rig.WaitEnded(Conn, 25.0));
	const TArray<FEvent> Seen = Rig.Events(Conn);
	const TCHAR* const Evidence = TEXT("an attempt to /ws/hang killed with lws_set_timeout(LWS_TO_KILL_ASYNC) 1 s after the upgrade went out: no response");
	// Task 5's rule (review minor 5): the fact speaks only for a kill that ran. "kill: no wsi" never reached LWS, and
	// without a kill LWS's own handshake timeout ends the attempt, a CONNECTION_ERROR of another case
	const int32 KillAt = Seen.IndexOfByPredicate([](const FEvent& Each) { return Each.Reason == CommandRan && Each.Note.StartsWith(TEXT("kill"), ESearchCase::CaseSensitive); });
	const FString KillNote = KillAt == INDEX_NONE ? FString(TEXT("never queued")) : Seen[KillAt].Note;
	if (!KillNote.Equals(TEXT("kill"), ESearchCase::CaseSensitive))
	{
		NotReachedFact(*this, TEXT("hs.status_in_connection_error.killed_pending"), FString::Printf(TEXT("the kill did not run (%s)"), *KillNote), Seen, Evidence);
		return true;
	}
	// Review I-1: and only for a kill that landed on a pending upgrade. ConnectReturned and the sleep do not show the
	// upgrade went out; APPEND_HANDSHAKE_HEADER before the kill command does (TcpCompleted), in the rig's service-thread order
	const int32 UpgradeAt = Seen.IndexOfByPredicate([](const FEvent& Each) { return Each.Reason == LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER; });
	if (UpgradeAt == INDEX_NONE || UpgradeAt > KillAt)
	{
		NotReachedFact(*this, TEXT("hs.status_in_connection_error.killed_pending"), TEXT("the upgrade had not gone out before the kill"), Seen, Evidence);
		return true;
	}
	StatusFact(*this, TEXT("hs.status_in_connection_error.killed_pending"), Seen, 0, Evidence);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
