// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestLwsErrors.h"

#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsErrorsTest, "VaRest.WebSocket.Lws.ConnectError",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsErrorsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::Lws;
	struct FRow
	{
		const TCHAR* Text;
		EVaRestSocketFailure Failure;
		int32 Status;
	};
	// Spec 2.2's strings and the rest of the archive's (strings lib/Unix/x86_64-unknown-linux-gnu/Release/libwebsockets.a)
	const FRow Rows[] = {
		{ TEXT("HS: ws upgrade unauthorized"), EVaRestSocketFailure::UpgradeRefused, 401 },          // hs.refusal_401
		{ TEXT("HS: ws upgrade forbidden"), EVaRestSocketFailure::UpgradeRefused, 403 },             // hs.refusal_403
		{ TEXT("HS: ws upgrade not 101, was 404"), EVaRestSocketFailure::UpgradeRefused, 404 },      // hs.refusal_404
		{ TEXT("HS: ws upgrade not 101, was 429"), EVaRestSocketFailure::UpgradeRefused, 429 },
		{ TEXT("HS: ws upgrade not 101, was 503"), EVaRestSocketFailure::UpgradeRefused, 503 },
		{ TEXT("HS: ws upgrade not 101, was 302"), EVaRestSocketFailure::Redirect, 302 },            // a 3xx LWS did not follow
		{ TEXT("HS: ws upgrade not 101, was 000"), EVaRestSocketFailure::Unknown, 0 },
		{ TEXT("HS: ws upgrade not 101, was 099"), EVaRestSocketFailure::Unknown, 0 },
		{ TEXT("HS: ws upgrade not 101, was 600"), EVaRestSocketFailure::Unknown, 0 },
		{ TEXT("HS: ws upgrade not 101, was 999"), EVaRestSocketFailure::Unknown, 0 },
		{ TEXT("HS: ws upgrade not 101, was 42"), EVaRestSocketFailure::Unknown, 0 },
		{ TEXT("HS: ws upgrade not 101, was "), EVaRestSocketFailure::Unknown, 0 },
		{ TEXT("HS: ws upgrade not 101, was 99999999999"), EVaRestSocketFailure::Unknown, 0 },
		{ TEXT("HS: ws upgrade not 101, was 404 x"), EVaRestSocketFailure::Unknown, 0 },
		{ TEXT("HS: ws upgrade not 101, was +40"), EVaRestSocketFailure::Unknown, 0 },
		{ TEXT("HS: ws upgrade not 101, was -10"), EVaRestSocketFailure::Unknown, 0 },
		{ TEXT("HS: ws upgrade not 101, was 100"), EVaRestSocketFailure::UpgradeRefused, 100 },
		{ TEXT("HS: ws upgrade not 101, was 599"), EVaRestSocketFailure::UpgradeRefused, 599 },
		{ TEXT("HS: ws upgrade not 101, was abc"), EVaRestSocketFailure::Unknown, 0 },
		{ TEXT("HS: Redirect failed"), EVaRestSocketFailure::Redirect, 0 },
		{ TEXT("HS: Redirect attempted SSL downgrade"), EVaRestSocketFailure::Redirect, 0 },         // hs.redirect_downgrade
		{ TEXT("HS: Redirect code but no Location"), EVaRestSocketFailure::Redirect, 0 },
		{ TEXT("HS: PROTOCOL malformed"), EVaRestSocketFailure::ProtocolMismatch, 0 },               // hs.unoffered_selected
		{ TEXT("HS: Cannot match protocol"), EVaRestSocketFailure::ProtocolMismatch, 0 },
		{ TEXT("HS: ACCEPT missing"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: Accept hash wrong"), EVaRestSocketFailure::BadUpgradeResponse, 0 },              // hs.bad_accept
		{ TEXT("HS: UPGRADE missing"), EVaRestSocketFailure::BadUpgradeResponse, 0 },                // hs.missing_upgrade
		{ TEXT("HS: UPGRADE malformed"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: Upgrade to something other than websocket"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: CONNECTION missing"), EVaRestSocketFailure::BadUpgradeResponse, 0 },             // hs.missing_connection
		{ TEXT("HS: :status missing"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: URI missing"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: URI did not parse"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: h2 / ws upgrade unsupported"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: disallowed by client filter"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: Rejected by filter cb"), EVaRestSocketFailure::BadUpgradeResponse, 0 },          // hs.redirect_veto (VaRest's own refusal wins there)
		{ TEXT("HS: Rejected at CLIENT_ESTABLISHED"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: disallowed at ESTABLISHED"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("read failed"), EVaRestSocketFailure::NoUpgradeResponse, 0 },                          // hs.no_upgrade_response (22 A2)
		{ TEXT("connect failed"), EVaRestSocketFailure::ConnectFailed, 0 },                           // hs.connect_refused
		{ TEXT("client connect failed"), EVaRestSocketFailure::ConnectFailed, 0 },                    // tls.untrusted_rejected (VaRest's TLS evidence wins there)
		{ TEXT("getaddrinfo failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("socket connect failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("lws_ssl_client_connect1 failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("bio_create failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("proxy write failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("ERROR reading from proxy socket"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("ERROR proxy: HTTP/1.1 403 Forbidden"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("set socket opts failed"), EVaRestSocketFailure::ConnectFailed, 0 },                   // review spec M-3: the archive's other client strings
		{ TEXT("insert wsi failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("POLLOUT set failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("first service failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("HS: SO_SNDBUF failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT(""), EVaRestSocketFailure::Unknown, 0 },                                               // timeout.default_secs, killed_pending: FConn decides by elapsed time (review conc M-2)
		{ TEXT("Timed out waiting SSL"), EVaRestSocketFailure::ConnectTimeout, 0 },
		{ TEXT("HS: OOM"), EVaRestSocketFailure::Unknown, 0 },
		{ TEXT("something LWS 3.0 does not say"), EVaRestSocketFailure::Unknown, 0 },
		{ TEXT("Read failed"), EVaRestSocketFailure::Unknown, 0 },                                    // case matters
	};
	for (const FRow& Row : Rows)
	{
		const FConnectionErrorClass Class = ClassifyConnectionError(Row.Text);
		TestTrue(FString::Printf(TEXT("\"%s\" is %s (got %s)"), Row.Text, LexToString(Row.Failure), LexToString(Class.Failure)), Class.Failure == Row.Failure);
		TestEqual(FString::Printf(TEXT("\"%s\": status"), Row.Text), Class.HttpStatus, Row.Status);
	}
	// LWS may count a trailing NUL or newline in len
	const TCHAR WithNul[] = { TEXT('r'), TEXT('e'), TEXT('a'), TEXT('d'), TEXT(' '), TEXT('f'), TEXT('a'), TEXT('i'), TEXT('l'), TEXT('e'), TEXT('d'), 0 };
	TestTrue(TEXT("a trailing NUL is ignored"), ClassifyConnectionError(FStringView(WithNul, 12)).Failure == EVaRestSocketFailure::NoUpgradeResponse);
	TestTrue(TEXT("a trailing newline is ignored"), ClassifyConnectionError(TEXT("connect failed\n")).Failure == EVaRestSocketFailure::ConnectFailed);
	// Review conc M-2: an empty string is LWS's 20 s connect stage, timeout_secs, or a kill VaRest did not make; the
	// classifier only says it was empty
	TestTrue(TEXT("an empty string is flagged empty"), ClassifyConnectionError(TEXT("")).bEmpty);
	const TCHAR JustNul[] = { 0 };
	TestTrue(TEXT("... and so is a lone NUL"), ClassifyConnectionError(FStringView(JustNul, 1)).bEmpty);
	TestFalse(TEXT("a real string is not"), ClassifyConnectionError(TEXT("read failed")).bEmpty);
	// ... and the connection splits it by the time the attempt took (re-review: conc-M-2's elapsed branch)
	struct FEmptyRow
	{
		double Took;
		EVaRestSocketFailure Failure;
		const TCHAR* DetailHas;
	};
	const FEmptyRow EmptyRows[] = {
		{ 0.2, EVaRestSocketFailure::Unknown, TEXT("empty CONNECTION_ERROR") },          // a kill VaRest did not make
		{ 19.4, EVaRestSocketFailure::Unknown, TEXT("empty CONNECTION_ERROR") },
		{ 19.499, EVaRestSocketFailure::Unknown, TEXT("empty CONNECTION_ERROR") },     // just under the 19.5 s tolerance edge
		{ 19.5, EVaRestSocketFailure::ConnectTimeout, TEXT("20 s connect stage") },     // LWS's fixed TCP-connect timer (review conc M-1)
		{ 19.999, EVaRestSocketFailure::ConnectTimeout, TEXT("20 s connect stage") },
		{ 20.0, EVaRestSocketFailure::ConnectTimeout, TEXT("20 s connect stage") },
		{ 20.001, EVaRestSocketFailure::ConnectTimeout, TEXT("20 s connect stage") },
		{ 20.1, EVaRestSocketFailure::ConnectTimeout, TEXT("20 s connect stage") },
		{ -1.0, EVaRestSocketFailure::Unknown, TEXT("empty CONNECTION_ERROR") },        // a clock step back
		{ std::numeric_limits<double>::quiet_NaN(), EVaRestSocketFailure::Unknown, TEXT("empty CONNECTION_ERROR") },
		{ 60.3, EVaRestSocketFailure::ConnectTimeout, TEXT("timeout_secs") },           // the context's backstop
		{ 25.0, EVaRestSocketFailure::ConnectTimeout, TEXT("timeout_secs") },
	};
	for (const FEmptyRow& Row : EmptyRows)
	{
		FString Detail;
		const FConnectionErrorClass Class = ClassifyEmptyConnectionError(Row.Took, Detail);
		TestTrue(FString::Printf(TEXT("an empty string after %.1f s is %s (got %s)"), Row.Took, LexToString(Row.Failure), LexToString(Class.Failure)), Class.Failure == Row.Failure);
		TestTrue(FString::Printf(TEXT("... its detail names it (%s)"), *Detail), Detail.Contains(Row.DetailHas, ESearchCase::CaseSensitive));
		// Each detail names one cause only, so the 20 s and timeout_secs rows cannot pass the same way
		const bool bStageRow = FCString::Strstr(Row.DetailHas, TEXT("20 s connect stage")) != nullptr;
		const bool bSecsRow = FCString::Strstr(Row.DetailHas, TEXT("timeout_secs")) != nullptr;
		TestFalse(FString::Printf(TEXT("... and names no other cause (%s)"), *Detail),
			(bStageRow && Detail.Contains(TEXT("timeout_secs"), ESearchCase::CaseSensitive)) || (bSecsRow && Detail.Contains(TEXT("20 s connect stage"), ESearchCase::CaseSensitive)));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsErrorDetailTest, "VaRest.WebSocket.Lws.ConnectErrorDetail",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsErrorDetailTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::Lws;
	// Task 5 review M-4: an Unknown keeps libwebsockets' string in Detail -- there it is the only clue
	for (const TCHAR* UnknownText : { TEXT("HS: EXT: unknown ext"), TEXT("a string a later libwebsockets may add") })
	{
		const FConnectionErrorEnd Ended = DescribeConnectionError(UnknownText, 0.5);
		TestTrue(FString::Printf(TEXT("\"%s\" is Unknown, By Transport"), UnknownText), Ended.Failure == EVaRestSocketFailure::Unknown && Ended.By == EVaRestSocketCloseBy::Transport);
		TestEqualSensitive(TEXT("... and its Detail quotes the string"), Ended.Detail, FString::Printf(TEXT("libwebsockets: \"%s\""), UnknownText));
	}
	const FConnectionErrorEnd Refused = DescribeConnectionError(TEXT("HS: ws upgrade not 101, was 404"), 0.1);
	TestTrue(TEXT("a known string keeps its class and status"), Refused.Failure == EVaRestSocketFailure::UpgradeRefused && Refused.HttpStatus == 404);
	TestEqualSensitive(TEXT("... and its string"), Refused.Detail, FString(TEXT("libwebsockets: \"HS: ws upgrade not 101, was 404\"")));
	TestEqualSensitive(TEXT("trailing CR, LF and NUL are trimmed, not shown"), CleanConnectionErrorText(FStringView(TEXT("read failed\r\n\0"), 14)), FString(TEXT("read failed")));

	// The proxy's own reply: control characters never reach a Detail or a log line, and its length is capped
	const FString ProxyReply = FString(TEXT("ERROR proxy: HTTP/1.1 407 Proxy\r\nX-Evil: 1\x1b[31m\x7f\x85")) + FString::ChrN(1000, TEXT('z'));
	const FConnectionErrorEnd Proxied = DescribeConnectionError(ProxyReply, 0.3);
	TestTrue(TEXT("a proxy's reply is ConnectFailed"), Proxied.Failure == EVaRestSocketFailure::ConnectFailed && Proxied.By == EVaRestSocketCloseBy::Transport);
	TestTrue(FString::Printf(TEXT("... its controls shown as '?' (%s)"), *Proxied.Detail.Left(80)),
		Proxied.Detail.StartsWith(TEXT("libwebsockets: \"ERROR proxy: HTTP/1.1 407 Proxy??X-Evil: 1?[31m??zzz"), ESearchCase::CaseSensitive));
	TestFalse(TEXT("... no control character left"), Proxied.Detail.Contains(TEXT("\r"), ESearchCase::CaseSensitive) || Proxied.Detail.Contains(TEXT("\n"), ESearchCase::CaseSensitive)
		|| Proxied.Detail.Contains(TEXT("\x1b"), ESearchCase::CaseSensitive));
	// "libwebsockets: \"" + the capped text + "...\""
	TestEqual(TEXT("... cut to MaxLwsErrorTextChars, with \"...\""), Proxied.Detail.Len(), 16 + MaxLwsErrorTextChars + 3 + 1);
	TestTrue(TEXT("... the cut is marked"), Proxied.Detail.EndsWith(TEXT("zzz...\""), ESearchCase::CaseSensitive));

	// The cap's edges, and never half a surrogate pair (TCHAR is UTF-16)
	TestEqualSensitive(TEXT("exactly the cap: kept whole"), CleanConnectionErrorText(FString::ChrN(MaxLwsErrorTextChars, TEXT('a'))), FString::ChrN(MaxLwsErrorTextChars, TEXT('a')));
	TestEqualSensitive(TEXT("one over: cut, marked"), CleanConnectionErrorText(FString::ChrN(MaxLwsErrorTextChars + 1, TEXT('a'))), FString::ChrN(MaxLwsErrorTextChars, TEXT('a')) + TEXT("..."));
	const FString Emoji = FString::ChrN(MaxLwsErrorTextChars - 1, TEXT('a')) + FString(UTF8_TO_TCHAR("\xF0\x9F\x98\x80")) + TEXT("b");
	TestEqualSensitive(TEXT("a pair across the cap goes whole"), CleanConnectionErrorText(Emoji), FString::ChrN(MaxLwsErrorTextChars - 1, TEXT('a')) + TEXT("..."));

	// An empty string: which timer, by the time taken; By Timeout only for LWS's own timer
	const FConnectionErrorEnd Stage = DescribeConnectionError(FStringView(), 20.0);
	TestTrue(FString::Printf(TEXT("an empty string after 20 s is ConnectTimeout, By Timeout (%s)"), *Stage.Detail),
		Stage.Failure == EVaRestSocketFailure::ConnectTimeout && Stage.By == EVaRestSocketCloseBy::Timeout && Stage.Detail.Contains(TEXT("20 s connect stage"), ESearchCase::CaseSensitive));
	const FConnectionErrorEnd Early = DescribeConnectionError(TEXT(" \r\n"), 0.4);
	TestTrue(FString::Printf(TEXT("an empty one after 0.4 s is Unknown, By Transport (%s)"), *Early.Detail),
		Early.Failure == EVaRestSocketFailure::Unknown && Early.By == EVaRestSocketCloseBy::Transport && Early.Detail.Contains(TEXT("empty CONNECTION_ERROR"), ESearchCase::CaseSensitive));
	return true;
}

#endif
