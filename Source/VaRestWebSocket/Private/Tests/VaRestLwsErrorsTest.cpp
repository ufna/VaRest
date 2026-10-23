// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestLwsErrors.h"

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
		{ 19.5, EVaRestSocketFailure::ConnectTimeout, TEXT("20 s connect stage") },     // LWS's fixed TCP-connect timer (review conc M-1)
		{ 20.1, EVaRestSocketFailure::ConnectTimeout, TEXT("20 s connect stage") },
		{ 60.3, EVaRestSocketFailure::ConnectTimeout, TEXT("timeout_secs") },           // the context's backstop
	};
	for (const FEmptyRow& Row : EmptyRows)
	{
		FString Detail;
		const FConnectionErrorClass Class = ClassifyEmptyConnectionError(Row.Took, Detail);
		TestTrue(FString::Printf(TEXT("an empty string after %.1f s is %s (got %s)"), Row.Took, LexToString(Row.Failure), LexToString(Class.Failure)), Class.Failure == Row.Failure);
		TestTrue(FString::Printf(TEXT("... its detail names it (%s)"), *Detail), Detail.Contains(Row.DetailHas, ESearchCase::CaseSensitive));
	}
	return true;
}

#endif
