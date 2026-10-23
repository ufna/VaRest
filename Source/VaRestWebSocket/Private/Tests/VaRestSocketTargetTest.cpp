// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestSocketTarget.h"

namespace VaRest::WebSocket::VaRestSocketTargetTestPrivate
{
	TValueOrError<FSocketTarget, FTargetError> Prepare(const TCHAR* Url, bool bShipping = false)
	{
		FVaRestSocketConnectRequest Request;
		Request.Url = Url;
		return PrepareTarget(Request, bShipping);
	}

	FString BlockText(const FSocketTarget& Target)
	{
		return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Target.HeaderBlock.GetData()), Target.HeaderBlock.Num());
	}

	/**
	 * The value, or null after a failing TestTrue (review spec I-4): GetValue() on an error is a check failure
	 * (ValueOrError.h:156-158, TVariant.h:132-137) that would end the run instead of failing the row. Keep Made alive
	 * while the pointer is used.
	 */
	const FSocketTarget* ValueOf(FAutomationTestBase& Test, const FString& What, const TValueOrError<FSocketTarget, FTargetError>& Made)
	{
		return Test.TestTrue(What, Made.HasValue()) ? &Made.GetValue() : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketTargetValidTest, "VaRest.WebSocket.Target.Valid",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketTargetValidTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket;
	using namespace VaRest::WebSocket::VaRestSocketTargetTestPrivate;
	struct FRow
	{
		const TCHAR* Url;
		const TCHAR* Host;
		int32 Port;
		const TCHAR* HostHeader;
		const TCHAR* Path;
		bool bTls;
		bool bIp;
		const TCHAR* Origin;
		const TCHAR* FilterUrl;
	};
	const FRow Rows[] = {
		{ TEXT("ws://example.com/chat"), TEXT("example.com"), 80, TEXT("example.com"), TEXT("/chat"), false, false, TEXT("http://example.com"), TEXT("ws://example.com") },
		{ TEXT("wss://example.com"), TEXT("example.com"), 443, TEXT("example.com"), TEXT("/"), true, false, TEXT("https://example.com"), TEXT("wss://example.com") },
		{ TEXT("wss://example.com:8443/a?b=c"), TEXT("example.com"), 8443, TEXT("example.com:8443"), TEXT("/a?b=c"), true, false, TEXT("https://example.com:8443"), TEXT("wss://example.com:8443") },
		{ TEXT("ws://127.0.0.1:41234/ws/echo?key=K"), TEXT("127.0.0.1"), 41234, TEXT("127.0.0.1:41234"), TEXT("/ws/echo?key=K"), false, true, TEXT("http://127.0.0.1:41234"), TEXT("ws://127.0.0.1:41234") },
		{ TEXT("ws://[::1]:8080/x"), TEXT("::1"), 8080, TEXT("[::1]:8080"), TEXT("/x"), false, true, TEXT("http://[::1]:8080"), TEXT("ws://[::1]:8080") },
		{ TEXT("WS://Example.com:80/"), TEXT("Example.com"), 80, TEXT("Example.com"), TEXT("/"), false, false, TEXT("http://Example.com"), TEXT("ws://Example.com") },
		{ TEXT("ws://example.com?x=1"), TEXT("example.com"), 80, TEXT("example.com"), TEXT("/?x=1"), false, false, TEXT("http://example.com"), TEXT("ws://example.com") },
		{ TEXT("ws://example.com/\u043F?q=a b"), TEXT("example.com"), 80, TEXT("example.com"), TEXT("/%D0%BF?q=a%20b"), false, false, TEXT("http://example.com"), TEXT("ws://example.com") },
		{ TEXT("wss+insecure://127.0.0.1:9/"), TEXT("127.0.0.1"), 9, TEXT("127.0.0.1:9"), TEXT("/"), true, true, TEXT("https://127.0.0.1:9"), TEXT("wss://127.0.0.1:9") },
		{ TEXT("WSS://example.com"), TEXT("example.com"), 443, TEXT("example.com"), TEXT("/"), true, false, TEXT("https://example.com"), TEXT("wss://example.com") },
		{ TEXT("ws://example.com/a\r\nX"), TEXT("example.com"), 80, TEXT("example.com"), TEXT("/a%0D%0AX"), false, false, TEXT("http://example.com"), TEXT("ws://example.com") },
		{ TEXT("ws://[::]:1/"), TEXT("::"), 1, TEXT("[::]:1"), TEXT("/"), false, true, TEXT("http://[::]:1"), TEXT("ws://[::]:1") },
		{ TEXT("ws://[1:2:3:4:5:6:7:8]/"), TEXT("1:2:3:4:5:6:7:8"), 80, TEXT("[1:2:3:4:5:6:7:8]"), TEXT("/"), false, true, TEXT("http://[1:2:3:4:5:6:7:8]"), TEXT("ws://[1:2:3:4:5:6:7:8]") },
		{ TEXT("ws://[::ffff:1.2.3.4]/"), TEXT("::ffff:1.2.3.4"), 80, TEXT("[::ffff:1.2.3.4]"), TEXT("/"), false, true, TEXT("http://[::ffff:1.2.3.4]"), TEXT("ws://[::ffff:1.2.3.4]") },
		{ TEXT("ws://[1::1.2.3.4]/"), TEXT("1::1.2.3.4"), 80, TEXT("[1::1.2.3.4]"), TEXT("/"), false, true, TEXT("http://[1::1.2.3.4]"), TEXT("ws://[1::1.2.3.4]") },
		{ TEXT("ws://[fe80::1]/"), TEXT("fe80::1"), 80, TEXT("[fe80::1]"), TEXT("/"), false, true, TEXT("http://[fe80::1]"), TEXT("ws://[fe80::1]") },
		{ TEXT("ws://[2001:db8::8a2e:370:7334]/"), TEXT("2001:db8::8a2e:370:7334"), 80, TEXT("[2001:db8::8a2e:370:7334]"), TEXT("/"), false, true, TEXT("http://[2001:db8::8a2e:370:7334]"), TEXT("ws://[2001:db8::8a2e:370:7334]") },
	};
	for (const FRow& Row : Rows)
	{
		const TValueOrError<FSocketTarget, FTargetError> Made = Prepare(Row.Url);
		if (!TestTrue(FString::Printf(TEXT("%s is taken"), Row.Url), Made.HasValue()))
		{
			continue;
		}
		const FSocketTarget& Target = Made.GetValue();
		TestEqualSensitive(FString::Printf(TEXT("%s: host"), Row.Url), Target.Host, FString(Row.Host));
		TestEqual(FString::Printf(TEXT("%s: port"), Row.Url), Target.Port, Row.Port);
		TestEqualSensitive(FString::Printf(TEXT("%s: Host header"), Row.Url), Target.HostHeader, FString(Row.HostHeader));
		TestEqualSensitive(FString::Printf(TEXT("%s: path and query"), Row.Url), Target.PathAndQuery, FString(Row.Path));
		TestEqual(FString::Printf(TEXT("%s: TLS"), Row.Url), Target.bTls, Row.bTls);
		TestEqual(FString::Printf(TEXT("%s: IP literal"), Row.Url), Target.bHostIsIp, Row.bIp);
		TestEqualSensitive(FString::Printf(TEXT("%s: default Origin (D29)"), Row.Url), Target.Origin, FString(Row.Origin));
		TestEqualSensitive(FString::Printf(TEXT("%s: what the URL filter sees"), Row.Url), Target.FilterUrl, FString(Row.FilterUrl));
	}
	const TValueOrError<FSocketTarget, FTargetError> Insecure = Prepare(TEXT("wss+insecure://127.0.0.1:9/"));
	if (const FSocketTarget* Target = ValueOf(*this, TEXT("wss+insecure is taken"), Insecure))
	{
		TestTrue(TEXT("wss+insecure is insecure outside Shipping"), Target->bInsecure);
	}
	const TValueOrError<FSocketTarget, FTargetError> Checked = Prepare(TEXT("wss://example.com/"));
	if (const FSocketTarget* Target = ValueOf(*this, TEXT("wss is taken"), Checked))
	{
		TestFalse(TEXT("wss is not"), Target->bInsecure);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketTargetInvalidTest, "VaRest.WebSocket.Target.Invalid",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketTargetInvalidTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket;
	using namespace VaRest::WebSocket::VaRestSocketTargetTestPrivate;
	const TCHAR* const Refused[] = {
		TEXT("example.com/ws"),             // no scheme
		TEXT("http://example.com/"),        // the socket layer maps http(s), not the transport (spec 3.2)
		TEXT("ftp://example.com/"),
		TEXT("ws://"),                      // no host
		TEXT("ws://:80/"),
		TEXT("ws://user:pw@example.com/"),  // user info
		TEXT("ws://example.com/#frag"),     // a fragment
		TEXT("ws://exa mple.com/"),
		TEXT("ws://\u043F\u0440\u0438\u043C\u0435\u0440.\u0440\u0444/"),   // IDN needs punycode
		TEXT("ws://example.com:0/"),
		TEXT("ws://example.com:65536/"),
		TEXT("ws://example.com:http/"),
		TEXT("ws://example.com:/"),
		TEXT("ws://[::1/"),
		TEXT("ws://[abc]/"),                // brackets hold IPv6 only
		TEXT("ws://[::1]x/"),
		TEXT("ws://::1/"),                  // an IPv6 address needs brackets
		TEXT("ws://exa\r\nmple.com/"),      // CR/LF in the host
		TEXT("ws://[:]/"),                  // RFC 4291 shape (review M-1)
		TEXT("ws://[:::]/"),
		TEXT("ws://[1.2.3.4:]/"),
		TEXT("ws://[1::2::3]/"),
		TEXT("ws://[1:2:3:4:5:6:7]/"),      // too few groups without ::
		TEXT("ws://[1:2:3:4:5:6:7:8:9]/"),
		TEXT("ws://[12345::]/"),            // a group of five digits
		TEXT("ws://[::1.2.3]/"),
		TEXT("ws://a..b/"),                 // host shape (review M-2)
		TEXT("ws://.example.com/"),
		TEXT("ws://example.com./"),
		TEXT("ws://01.1.1.1/"),             // ambiguous octal/decimal
		TEXT("ws://1.2.3/"),                // inet_aton reads these as numbers (task 4 residual)
		TEXT("ws://2130706433/"),
		TEXT("ws://0x7f.0.0.1/"),
		TEXT("ws://0x7f000001/"),
		TEXT("ws://0X7F.1/"),
		TEXT("ws://[::ffff:999.0.0.1]/"),   // a dotted tail past 255
		TEXT("ws://[fe80::1%25eth0]/"),     // a zone id is never sent
	};
	for (const TCHAR* Url : Refused)
	{
		const TValueOrError<FSocketTarget, FTargetError> Made = Prepare(Url);
		TestTrue(FString::Printf(TEXT("%s is refused as BadUrl"), Url), Made.HasError() && Made.GetError().Failure == EVaRestSocketFailure::BadUrl);
	}
	const FString LongLabel = FString::Printf(TEXT("ws://%s.com/"), *FString::ChrN(64, TEXT('a')));
	TestTrue(TEXT("a 64-character label is refused"), Prepare(*LongLabel).HasError());
	const FString OkLabel = FString::Printf(TEXT("ws://%s.com/"), *FString::ChrN(63, TEXT('a')));
	TestTrue(TEXT("a 63-character label is taken"), Prepare(*OkLabel).HasValue());
	FString LongHost;
	for (int32 Index = 0; Index < 5; ++Index)
	{
		LongHost += FString::ChrN(55, TEXT('a')) + TEXT(".");
	}
	LongHost += TEXT("com");
	const FString TooLong = FString::Printf(TEXT("ws://%s/"), *LongHost);
	TestTrue(TEXT("a host over 253 characters is refused"), LongHost.Len() > 253 && Prepare(*TooLong).HasError());
	// Spec 2.2, 15: wss+insecure and bAllowSelfSignedTls do nothing in Shipping
	TestTrue(TEXT("wss+insecure is refused under Shipping rules"), Prepare(TEXT("wss+insecure://127.0.0.1:9/"), true).HasError());
	FVaRestSocketConnectRequest SelfSigned;
	SelfSigned.Url = TEXT("wss://127.0.0.1:9/");
	SelfSigned.bAllowSelfSignedTls = true;
	const TValueOrError<FSocketTarget, FTargetError> Development = PrepareTarget(SelfSigned, false);
	if (const FSocketTarget* Target = ValueOf(*this, TEXT("bAllowSelfSignedTls is taken outside Shipping"), Development))
	{
		TestTrue(TEXT("bAllowSelfSignedTls makes wss insecure outside Shipping"), Target->bInsecure);
	}
	const TValueOrError<FSocketTarget, FTargetError> Shipping = PrepareTarget(SelfSigned, true);
	if (const FSocketTarget* Target = ValueOf(*this, TEXT("bAllowSelfSignedTls is taken under Shipping rules"), Shipping))
	{
		TestFalse(TEXT("... and is ignored under Shipping rules"), Target->bInsecure);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketTargetHandshakeTest, "VaRest.WebSocket.Target.Handshake",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketTargetHandshakeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket;
	using namespace VaRest::WebSocket::VaRestSocketTargetTestPrivate;
	FVaRestSocketConnectRequest Request;
	Request.Url = TEXT("wss://example.com/");
	// Origin (spec 2.2, D29): given, omitted, refused
	Request.Origin = FString(TEXT("https://game.example"));
	const TValueOrError<FSocketTarget, FTargetError> Given = PrepareTarget(Request);
	if (const FSocketTarget* Target = ValueOf(*this, TEXT("an Origin is taken"), Given))
	{
		TestEqualSensitive(TEXT("an Origin is sent as given"), Target->Origin, FString(TEXT("https://game.example")));
	}
	Request.Origin = FString();
	const TValueOrError<FSocketTarget, FTargetError> Omitted = PrepareTarget(Request);
	if (const FSocketTarget* Target = ValueOf(*this, TEXT("an empty Origin is taken"), Omitted))
	{
		TestTrue(TEXT("an empty Origin omits it"), Target->Origin.IsEmpty());
	}
	Request.Origin = FString(TEXT("https://bad origin"));
	TestTrue(TEXT("an Origin with a space is BadHandshake"), PrepareTarget(Request).HasError() && PrepareTarget(Request).GetError().Failure == EVaRestSocketFailure::BadHandshake);
	Request.Origin.Reset();

	// Subprotocols: tokens, each once, case-sensitive, joined with ", " (plan decision 14)
	Request.Protocols = { TEXT("chat.v2"), TEXT("chat.v1"), TEXT("Chat.v1") };
	const TValueOrError<FSocketTarget, FTargetError> Offered = PrepareTarget(Request);
	if (const FSocketTarget* Target = ValueOf(*this, TEXT("three distinct protocols are taken"), Offered))
	{
		TestEqualSensitive(TEXT("protocols are joined with \", \""), Target->Protocols, FString(TEXT("chat.v2, chat.v1, Chat.v1")));
	}
	Request.Protocols = { TEXT("chat v2") };
	TestTrue(TEXT("a protocol that is not a token is BadHandshake"), PrepareTarget(Request).HasError());
	Request.Protocols = { TEXT("base64url.bearer.authorization.k8s.io.SECRETTOKEN"), TEXT("base64url.bearer.authorization.k8s.io.SECRETTOKEN") };
	const TValueOrError<FSocketTarget, FTargetError> Twice = PrepareTarget(Request);
	TestTrue(TEXT("a protocol offered twice is BadHandshake"), Twice.HasError());
	if (Twice.HasError())
	{
		// The message reaches logs and a subprotocol can carry a bearer token (review I-2)
		TestFalse(TEXT("... and its message holds no part of the subprotocol"), Twice.GetError().Message.Contains(TEXT("SECRETTOKEN"), ESearchCase::CaseSensitive) || Twice.GetError().Message.Contains(TEXT("bearer"), ESearchCase::CaseSensitive));
	}
	Request.Protocols.Reset();

	// Headers (spec 2.2, D23)
	Request.Headers = { { TEXT("X-A"), TEXT("1") }, { TEXT("Authorization"), TEXT("Bearer t\tx") } };
	const TValueOrError<FSocketTarget, FTargetError> Lines = PrepareTarget(Request);
	if (const FSocketTarget* Target = ValueOf(*this, TEXT("tab and space in values are taken"), Lines))
	{
		TestEqualSensitive(TEXT("headers become CRLF lines in order"), BlockText(*Target), FString(TEXT("X-A: 1\r\nAuthorization: Bearer t\tx\r\n")));
	}
	const TPair<FString, FString> Bad[] = {
		{ TEXT("Origin"), TEXT("x") },                       // VaRest's own: set Origin
		{ TEXT("sec-websocket-protocol"), TEXT("x") },       // any case
		{ TEXT("Host"), TEXT("x") },
		{ TEXT("X-B"), TEXT("a\r\nX-C: b") },                // header injection
		{ TEXT("X-D"), TEXT("caf\u00E9") },                  // StringCast<ANSICHAR> would mangle it (review item 21)
		{ TEXT("sec-websocket-foo"), TEXT("x") },            // the whole Sec-WebSocket- prefix (RFC 6455 4.1)
		{ TEXT("Sec-WebSocket-Origin"), TEXT("x") },
		{ TEXT("SEC-WEBSOCKET-KEY"), TEXT("x") },
		{ TEXT("X bad"), TEXT("v") },                        // not a token
	};
	for (const TPair<FString, FString>& Header : Bad)
	{
		Request.Headers = { Header };
		const TValueOrError<FSocketTarget, FTargetError> Made = PrepareTarget(Request);
		TestTrue(FString::Printf(TEXT("header \"%s\" is BadHandshake"), *Header.Key), Made.HasError() && Made.GetError().Failure == EVaRestSocketFailure::BadHandshake);
	}
	Request.Headers = { { TEXT("X-Big"), FString::ChrN(33000, TEXT('a')) } };
	const TValueOrError<FSocketTarget, FTargetError> Big = PrepareTarget(Request);
	TestTrue(TEXT("headers over 32 KiB are BadHandshake before any connect"), Big.HasError() && Big.GetError().Message.Contains(TEXT("32768"), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketTargetAllowlistTest, "VaRest.WebSocket.Target.Allowlist",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketTargetAllowlistTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket;
	// URLRequestFilter.cpp:87-165: no map allows everything; a scheme without a section is refused; a section's domains are matched
	const TArray<TPair<FString, TArray<FString>>> None;
	TestTrue(TEXT("an empty filter allows everything"), IsAllowedByFilter(TEXT("ws://127.0.0.1:9"), &None));
	TArray<TPair<FString, TArray<FString>>> OnlyExample;
	OnlyExample.Emplace(FString(TEXT("ws")), TArray<FString>{ FString(TEXT("example.com")) });
	TestFalse(TEXT("a domain not listed is refused"), IsAllowedByFilter(TEXT("ws://127.0.0.1:9"), &OnlyExample));
	TestTrue(TEXT("a listed one is allowed"), IsAllowedByFilter(TEXT("ws://example.com"), &OnlyExample));
	// URLRequestFilter.cpp:148,159: domain matching is IgnoreCase, so Out.Host keeps the caller's case and FilterUrl needs no lowering
	TArray<TPair<FString, TArray<FString>>> OnlyWss;
	OnlyWss.Emplace(FString(TEXT("wss")), TArray<FString>{ FString(TEXT("example.com")) });
	TestTrue(TEXT("an allowlist entry matches the host whatever its case"), IsAllowedByFilter(TEXT("wss://Example.COM"), &OnlyWss));
	TestFalse(TEXT("a scheme without a section is refused (plan decision 15)"), IsAllowedByFilter(TEXT("wss://example.com"), &OnlyExample));
	TestTrue(TEXT("IPv4 literals are recognised"), IsIpv4Literal(TEXT("127.0.0.1")) && IsIpv4Literal(TEXT("0.0.0.0")) && IsIpv4Literal(TEXT("255.255.255.255")));
	TestFalse(TEXT("... and only they"), IsIpv4Literal(TEXT("01.1.1.1")) || IsIpv4Literal(TEXT("256.0.0.1")) || IsIpv4Literal(TEXT("1.2.3")) || IsIpv4Literal(TEXT("1.2.3.4.5"))
		|| IsIpv4Literal(TEXT("a.b.c.d")) || IsIpv4Literal(TEXT("1..2.3")) || IsIpv4Literal(TEXT("1234.1.1.1")));
	return true;
}


#endif
