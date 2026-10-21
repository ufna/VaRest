// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "VaRestRedactionPipe.h"
#include "VaRestRedactor.h"
#include "VaRestSha256.h"

namespace VaRest::Http::RedactorTestPrivate
{
	TConstArrayView<uint8> Bytes(const char* Text)
	{
		return TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text));
	}

	const TCHAR* const Bearer = TEXT("canary-bearer-0123456789");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRedactUrlTest, "VaRest.Http.Redactor.Url",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRedactUrlTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::RedactorTestPrivate;
	const FVaRestRedactor Redactor(FVaRestRedactRules(), { Bearer });

	// Inspector 2.2 layers 1 and 5: userinfo, secret query keys (presigned URLs, OAuth code), fragments (implicit flow)
	TestEqualSensitive(TEXT("every URL secret class"),
		Redactor.RedactUrl(TEXT("https://user:pa55word@api.test/v1/items?token=canary-1&page=2&X-Amz-Signature=canary-2&code=canary-3#access_token=canary-4&state=ok")),
		FString(TEXT("https://***@api.test/v1/items?token=***&page=2&X-Amz-Signature=***&code=***#access_token=***&state=ok")));
	TestEqualSensitive(TEXT("keys are matched decoded; the written key stays as sent"),
		Redactor.RedactUrl(TEXT("https://h/x?%74oken=canary&a+b=1")), FString(TEXT("https://h/x?%74oken=***&a+b=1")));
	TestEqualSensitive(TEXT("a URL without secrets is unchanged"), Redactor.RedactUrl(TEXT("https://h/x/y?page=2")), FString(TEXT("https://h/x/y?page=2")));
	// Layer 3: a known secret in a path segment, which no key rule names (Inspector restore-the-bug R13)
	TestEqualSensitive(TEXT("a known secret in the path is scrubbed"),
		Redactor.RedactUrl(FString::Printf(TEXT("https://h/u/%s/x"), Bearer)), FString(TEXT("https://h/u/***/x")));
	TestEqualSensitive(TEXT("a relative URL works too"), Redactor.RedactUrl(TEXT("/users?password=x")), FString(TEXT("/users?password=***")));
	// Review C2: a "://" inside the query is not a scheme
	TestEqualSensitive(TEXT("a relative URL carrying a URL in its query"), Redactor.RedactUrl(TEXT("/cb?code=canary-r&redirect_uri=https://app.test/x")),
		FString(TEXT("/cb?code=***&redirect_uri=https://app.test/x")));
	TestEqualSensitive(TEXT("a protocol-relative URL's userinfo"), Redactor.RedactUrl(TEXT("//u:canary-u@h/x")), FString(TEXT("//***@h/x")));
	// Review M6: a secret percent-encoded by Path()/Query() (base64 +/=) is scrubbed in that form too
	const FVaRestRedactor Encoded(FVaRestRedactRules(), { TEXT("canary+b64/value==") });
	TestEqualSensitive(TEXT("a percent-encoded known secret"), Encoded.RedactUrl(TEXT("https://h/u/canary%2Bb64%2Fvalue%3D%3D/x")), FString(TEXT("https://h/u/***/x")));
	// Review I2: VaRest's own form encoding (space '+', '~' %7E, '*' kept) and lower-case hex are needles too
	const FVaRestRedactor Odd(FVaRestRedactRules(), { TEXT("canary tok~en*1"), TEXT("canary+b64/value==") });
	TestEqualSensitive(TEXT("a form-encoded known secret in a query value"), Odd.RedactUrl(TEXT("https://h/x?note=canary+tok%7Een*1&a=2")), FString(TEXT("https://h/x?note=***&a=2")));
	TestEqualSensitive(TEXT("a percent-encoded known secret with a space"), Odd.RedactUrl(TEXT("https://h/u/canary%20tok~en%2A1/x")), FString(TEXT("https://h/u/***/x")));
	TestEqualSensitive(TEXT("lower-case hex digits"), Odd.RedactUrl(TEXT("https://h/u/canary%2bb64%2fvalue%3d%3d/x")), FString(TEXT("https://h/u/***/x")));
	// Review M1: a hash router's query inside the fragment
	TestEqualSensitive(TEXT("a query inside a fragment"), Redactor.RedactUrl(TEXT("https://h/#/callback?code=X&state=Y")), FString(TEXT("https://h/#/callback?code=***&state=Y")));
	// Review M2: ';' as a pair separator and path parameters
	TestEqualSensitive(TEXT("a semicolon-separated pair"), Redactor.RedactUrl(TEXT("https://h/x?a=1;token=X")), FString(TEXT("https://h/x?a=1;token=***")));
	TestEqualSensitive(TEXT("';' never ends a secret value"), Redactor.RedactUrl(TEXT("https://h/x?password=ab;cd")), FString(TEXT("https://h/x?password=***")));
	TestEqualSensitive(TEXT("a pair after '&' is separate"), Redactor.RedactUrl(TEXT("https://h/x?a=1;token=X&b=2")), FString(TEXT("https://h/x?a=1;token=***&b=2")));
	TestEqualSensitive(TEXT("a query inside a query value"), Redactor.RedactUrl(TEXT("https://h/x?next=/x?code=X&s=1")), FString(TEXT("https://h/x?next=/x?code=***&s=1")));
	TestEqualSensitive(TEXT("a path parameter run"), Redactor.RedactUrl(TEXT("/p;jsessionid=X;y=1/q")), FString(TEXT("/p;jsessionid=***/q")));
	TestEqualSensitive(TEXT("a path parameter"), Redactor.RedactUrl(TEXT("/p;jsessionid=X")), FString(TEXT("/p;jsessionid=***")));
	TestEqualSensitive(TEXT("a plain path parameter is kept"), Redactor.RedactUrl(TEXT("/p;v=2/q?a=1")), FString(TEXT("/p;v=2/q?a=1")));
	// Review M3: leading whitespace, and userinfo without a scheme
	TestEqualSensitive(TEXT("leading space"), Redactor.RedactUrl(TEXT(" https://u:p@h/x")), FString(TEXT(" https://***@h/x")));
	TestEqualSensitive(TEXT("userinfo without a scheme"), Redactor.RedactUrl(TEXT("u:pw@h/x?a=1")), FString(TEXT("***@h/x?a=1")));
	TestEqualSensitive(TEXT("an '@' after the first '/' is not userinfo"), Redactor.RedactUrl(TEXT("/a@b")), FString(TEXT("/a@b")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRedactHeadersTest, "VaRest.Http.Redactor.Headers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRedactHeadersTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::RedactorTestPrivate;
	const FVaRestRedactor Redactor(FVaRestRedactRules(), { Bearer });
	FVaRestHeaders In;
	In.Set(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), Bearer));
	In.Add(TEXT("Cookie"), TEXT("sid=canary-c"));
	In.Add(TEXT("Set-Cookie"), TEXT("sid=canary-s"));
	In.Add(TEXT("X-Api-Key"), TEXT("canary-k"));
	In.Add(TEXT("X-CSRF-Token"), TEXT("canary-x"));
	In.Add(TEXT("Idempotency-Key"), TEXT("3f2c"));
	In.Add(TEXT("Content-Type"), TEXT("application/json"));
	In.Add(TEXT("Location"), TEXT("https://app.test/cb?code=canary-code&state=1"));
	In.Add(TEXT("X-Echo"), FString::Printf(TEXT("seen %s"), Bearer));
	const FVaRestHeaders Out = Redactor.RedactHeaders(In);
	TestEqual(TEXT("names and order are kept"), Out.Num(), In.Num());
	for (const TCHAR* Name : { TEXT("Authorization"), TEXT("Cookie"), TEXT("Set-Cookie"), TEXT("X-Api-Key"), TEXT("X-CSRF-Token") })
	{
		TestEqualSensitive(FString::Printf(TEXT("%s is ***"), Name), Out.Get(Name), FString(TEXT("***")));
	}
	TestEqualSensitive(TEXT("Idempotency-Key is on the allowlist"), Out.Get(TEXT("Idempotency-Key")), FString(TEXT("3f2c")));
	TestEqualSensitive(TEXT("plain headers are kept"), Out.Get(TEXT("Content-Type")), FString(TEXT("application/json")));
	TestEqualSensitive(TEXT("Location is redacted as a URL (layer 5)"), Out.Get(TEXT("Location")), FString(TEXT("https://app.test/cb?code=***&state=1")));
	TestEqualSensitive(TEXT("a known secret in any value is scrubbed (layer 3)"), Out.Get(TEXT("X-Echo")), FString(TEXT("seen ***")));
	FVaRestHeaders Relative;
	Relative.Set(TEXT("Location"), TEXT("/cb?code=canary-rel&next=https://app.test/done"));
	TestEqualSensitive(TEXT("a relative Location with a URL in its query (review C2)"), Redactor.RedactHeaders(Relative).Get(TEXT("Location")),
		FString(TEXT("/cb?code=***&next=https://app.test/done")));
	// Review M9: other headers that carry URLs
	FVaRestHeaders Urls;
	Urls.Add(TEXT("Referer"), TEXT("https://app.test/p?token=canary-ref&a=1"));
	Urls.Add(TEXT("Refresh"), TEXT("0; url=https://app.test/cb?code=canary-rf"));
	Urls.Add(TEXT("Link"), TEXT("<https://api.test/x?page=2&sig=canary-l1>; rel=\"next\", </y?token=canary-l2>; rel=\"prev\""));
	const FVaRestHeaders UrlsOut = Redactor.RedactHeaders(Urls);
	TestEqualSensitive(TEXT("Referer is a URL"), UrlsOut.Get(TEXT("Referer")), FString(TEXT("https://app.test/p?token=***&a=1")));
	TestEqualSensitive(TEXT("Refresh's url="), UrlsOut.Get(TEXT("Refresh")), FString(TEXT("0; url=https://app.test/cb?code=***")));
	TestEqualSensitive(TEXT("every Link target"), UrlsOut.Get(TEXT("Link")),
		FString(TEXT("<https://api.test/x?page=2&sig=***>; rel=\"next\", </y?token=***>; rel=\"prev\"")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRedactBodiesTest, "VaRest.Http.Redactor.Bodies",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRedactBodiesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::RedactorTestPrivate;
	const FVaRestRedactor Redactor(FVaRestRedactRules(), { Bearer });
	auto Run = [&Redactor](FStringView Type, const char* Body, int32 Cap = 4096)
	{
		const TConstArrayView<uint8> View = Bytes(Body);
		return Redactor.RedactBody(Type, View, View.Num(), Cap);
	};

	FVaRestTraceExcerpt E = Run(TEXT("application/json; charset=utf-8"), R"({"user":"neo","password":"canary-5"})");
	TestTrue(TEXT("JSON kind"), E.Kind == EVaRestExcerptKind::Json);
	TestEqualSensitive(TEXT("JSON keys redacted"), E.ToText(), FString(TEXT(R"({"user":"neo","password":"***"})")));
	TestTrue(TEXT("... redacted, whole"), E.bRedacted && !E.bTruncated && E.StopReason.IsEmpty() && E.TotalBytes == 36);

	TestTrue(TEXT("+json is JSON"), Run(TEXT("application/problem+json"), R"({"token":"x"})").ToText().Equals(TEXT(R"({"token":"***"})"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("text/* starting with '{' is sniffed as JSON"), Run(TEXT("text/plain"), R"(  {"secret":1})").Kind == EVaRestExcerptKind::Json);
	TestTrue(TEXT("no type starting with '[' is sniffed as JSON"), Run(TEXT(""), R"([{"jwt":"a.b.c"}])").ToText().Equals(TEXT(R"([{"jwt":"***"}])"), ESearchCase::CaseSensitive));

	const FVaRestTraceExcerpt Cut = Run(TEXT("application/json"), R"({"a":"0123456789","password":"canary-cut"})", 22);
	TestTrue(TEXT("a capped JSON body is truncated with reason cap"), Cut.bTruncated && Cut.StopReason == TEXT("cap"));
	TestFalse(TEXT("... and no canary"), Cut.ToText().Contains(TEXT("canary")));
	// Review M4: the cap falls inside the sensitive value ("can" is the part before the cut)
	const FVaRestTraceExcerpt Inside = Run(TEXT("application/json"), R"({"a":"0123456789","password":"canary-cut"})", 33);
	TestTrue(TEXT("a cap inside a secret value is a cap stop"), Inside.bTruncated && Inside.StopReason == TEXT("cap"));
	TestFalse(TEXT("... with none of the value"), Inside.ToText().Contains(TEXT("can")));

	const FVaRestTraceExcerpt Form = Run(TEXT("application/x-www-form-urlencoded"), "a=1&password=canary-6&b=2");
	TestTrue(TEXT("form kind"), Form.Kind == EVaRestExcerptKind::Form);
	TestEqualSensitive(TEXT("form pairs by key"), Form.ToText(), FString(TEXT("a=1&password=***&b=2")));
	TestEqualSensitive(TEXT("';' does not end a form secret"), Run(TEXT("application/x-www-form-urlencoded"), "a=1&password=ab;cd&b=2").ToText(), FString(TEXT("a=1&password=***&b=2")));
	const TConstArrayView<uint8> FormBody = Bytes("a=1&b=22&c=333");
	TestEqualSensitive(TEXT("a capped form drops its cut last pair"),
		Redactor.RedactBody(TEXT("application/x-www-form-urlencoded"), FormBody.Left(10), FormBody.Num(), 10).ToText(), FString(TEXT("a=1&b=22")));

	const FVaRestTraceExcerpt Text = Run(TEXT("text/plain"), "hello canary-bearer-0123456789 world");
	TestEqualSensitive(TEXT("text is value-scrubbed"), Text.ToText(), FString(TEXT("hello *** world")));
	TestTrue(TEXT("... kind Text, redacted"), Text.Kind == EVaRestExcerptKind::Text && Text.bRedacted);
	const TConstArrayView<uint8> Utf8Text = Bytes("ab\xC3\xA9");
	TestEqualSensitive(TEXT("text is cut at a character boundary"), Redactor.RedactBody(TEXT("text/plain"), Utf8Text, 4, 3).ToText(), FString(TEXT("ab")));

	// Review I1: a known secret across the cap shows no part of itself, wherever the cap falls
	const FString Hello = FString::Printf(TEXT("hello %s world"), Bearer);
	const FTCHARToUTF8 HelloUtf8(*Hello);
	const TConstArrayView<uint8> HelloBytes(reinterpret_cast<const uint8*>(HelloUtf8.Get()), HelloUtf8.Length());
	const FString Secret(Bearer);
	for (int32 CapAt = 1; CapAt < HelloBytes.Num(); ++CapAt)
	{
		const FString Excerpt = Redactor.RedactBody(TEXT("text/plain"), HelloBytes.Left(CapAt), HelloBytes.Num(), CapAt).ToText();
		for (int32 Length = 1; Length < Secret.Len(); ++Length)
		{
			if (Excerpt.EndsWith(Secret.Left(Length), ESearchCase::CaseSensitive))
			{
				AddError(FString::Printf(TEXT("cap %d leaves %d bytes of the secret: '%s'"), CapAt, Length, *Excerpt));
				break;
			}
		}
	}
	TestEqualSensitive(TEXT("a cap inside the secret drops its beginning"), Redactor.RedactBody(TEXT("text/plain"), HelloBytes.Left(16), HelloBytes.Num(), 16).ToText(), FString(TEXT("hello ")));
	// Review M5: overlapping known secrets are one range
	const FVaRestRedactor Overlap(FVaRestRedactRules(), { TEXT("AAAABBBB"), TEXT("BBBBCCCC") });
	TestEqualSensitive(TEXT("overlapping secrets in text"), Overlap.ScrubText(TEXT("x AAAABBBBCCCC y")), FString(TEXT("x *** y")));
	TestEqualSensitive(TEXT("... in a body"), Overlap.RedactBody(TEXT("text/plain"), Bytes("x AAAABBBBCCCC y"), 16, 4096).ToText(), FString(TEXT("x *** y")));
	// Review I2: a form-encoded known secret in a form body
	const FVaRestRedactor Odd(FVaRestRedactRules(), { TEXT("canary tok~en*1") });
	TestEqualSensitive(TEXT("a form-encoded known secret in a form body"), Odd.RedactBody(TEXT("application/x-www-form-urlencoded"), Bytes("note=canary+tok%7Een*1&b=2"), 26, 4096).ToText(), FString(TEXT("note=***&b=2")));
	// Review M7: GetSecrets() is what was passed in
	TestEqual(TEXT("GetSecrets is the input"), Odd.GetSecrets().Num(), 1);

	// Layer 4: binary is never copied; protobuf stores strings verbatim
	const uint8 Proto[] = { 0x0A, 0x08, 'p', 'a', 's', 's', 'w', 'o', 'r', 'd', 0x12, 0x02, 0x01, 0x02 };
	const FVaRestTraceExcerpt Binary = Redactor.RedactBody(TEXT("application/x-protobuf"), TConstArrayView<uint8>(Proto, 14), 14, 4096);
	TestTrue(TEXT("binary is a descriptor"), Binary.Kind == EVaRestExcerptKind::Binary && !Binary.ToText().Contains(TEXT("password")));
	TestEqualSensitive(TEXT("... with the SHA-256 prefix of the whole body"), Binary.Sha256Prefix, VaRest::Http::FSha256::Hash(TConstArrayView<uint8>(Proto, 14)).ToHex().Left(8));
	TestTrue(TEXT("... named in the text"), Binary.ToText().Contains(TEXT("14 bytes")) && Binary.ToText().Contains(Binary.Sha256Prefix));
	TestTrue(TEXT("a partial binary body gets no hash"), Redactor.RedactBody(TEXT("image/png"), TConstArrayView<uint8>(Proto, 4), 14, 4096).Sha256Prefix.IsEmpty());
	// Review I4: the protobuf above is valid UTF-8 without NUL; without a Content-Type it must still be a descriptor
	TestTrue(TEXT("untyped non-JSON is binary (layer 4)"), Redactor.RedactBody(TEXT(""), TConstArrayView<uint8>(Proto, 14), 14, 4096).Kind == EVaRestExcerptKind::Binary);
	// Review M5: the parser skips a UTF-8 BOM; the sniffing and the excerpt do too
	TestEqualSensitive(TEXT("a JSON body with a BOM"), Run(TEXT("application/json"), "\xEF\xBB\xBF{\"token\":\"canary-bom\"}").ToText(), FString(TEXT(R"({"token":"***"})")));
	// Review M6: a JSON string escapes '"' and '\\': the known secret is scrubbed in its escaped form too
	const FVaRestRedactor Escaping(FVaRestRedactRules(), { TEXT("can\"ary\\quote") });
	const TConstArrayView<uint8> Echo = Bytes(R"({"echo":"can\"ary\\quote"})");
	TestFalse(TEXT("a JSON-escaped known secret"), Escaping.RedactBody(TEXT("application/json"), Echo, Echo.Num(), 4096).ToText().Contains(TEXT("ary")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRedactPartsTest, "VaRest.Http.Redactor.MultipartAndStream",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRedactPartsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::RedactorTestPrivate;
	const FVaRestRedactor Redactor(FVaRestRedactRules(), { Bearer });
	FVaRestMultipart Multipart(TEXT("B"));
	Multipart.AddField(TEXT("name"), TEXT("neo")).AddField(TEXT("password"), TEXT("canary-7")).AddField(TEXT("note"), FString::Printf(TEXT("t=%s"), Bearer));
	Multipart.AddBytes(TEXT("avatar"), TEXT("a.png"), TEXT("image/png"), TArray<uint8>{ 1, 2, 3 });
	const TValueOrError<FVaRestRequestBody, FString> Built = FVaRestRequestBody::FromMultipart(Multipart);
	if (!TestTrue(TEXT("the multipart body is built"), Built.HasValue()))
	{
		return false;
	}
	const FVaRestRequestBody& Body = Built.GetValue();
	const FString Text = Redactor.DescribeMultipart(Body.PartDescriptors, Body.ContentLength, 4096).ToText();
	TestTrue(TEXT("a text part by name"), Text.Contains(TEXT("part \"name\": neo")));
	TestTrue(TEXT("a secret part name (layer 1)"), Text.Contains(TEXT("part \"password\": ***")));
	TestTrue(TEXT("a known secret in a text part (layer 3)"), Text.Contains(TEXT("part \"note\": t=***")));
	TestTrue(TEXT("a binary part as a descriptor"), Text.Contains(TEXT("part \"avatar\" filename=\"a.png\" image/png 3 B")));
	TestFalse(TEXT("no canary"), Text.Contains(TEXT("canary")));

	const FString Inside = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("up.bin"));
	const FVaRestTraceExcerpt Stream = Redactor.DescribeStream(Inside, TEXT("application/zip"), 99);
	TestTrue(TEXT("a streamed upload is a descriptor"), Stream.Kind == EVaRestExcerptKind::Stream);
	TestTrue(TEXT("... with the path relative to the project"), Stream.ToText().Contains(TEXT("Saved/up.bin")) && !Stream.ToText().Contains(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir())));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSecretSetTest, "VaRest.Http.Redactor.SecretSet",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSecretSetTest::RunTest(const FString& Parameters)
{
	FVaRestSecretSet Set;
	Set.Set(TEXT("bearer"), TEXT("token-v1-abcdef"), 0.0);
	Set.Set(TEXT("short"), TEXT("1234567"), 0.0);
	TestEqual(TEXT("values shorter than 8 bytes are not scrubbed (they would hit ordinary text)"), Set.GetActive(0.0).Num(), 1);
	Set.Set(TEXT("bearer"), TEXT("token-v2-abcdef-longer"), 100.0);
	TArray<FString> Active = Set.GetActive(100.0);
	if (!TestEqual(TEXT("after a rotation the old value stays known (Inspector D21)"), Active.Num(), 2))
	{
		return false;
	}
	TestEqualSensitive(TEXT("longest first, so a value containing another is replaced whole"), Active[0], FString(TEXT("token-v2-abcdef-longer")));
	TestEqual(TEXT("next expiry is 10 minutes after the rotation"), Set.NextExpiry(100.0), 700.0);
	TestEqual(TEXT("... and then it is forgotten"), Set.GetActive(700.0).Num(), 1);
	TestTrue(TEXT("an expired value is no longer pending (review C1)"), Set.NextExpiry(700.0) < 0.0);
	TestTrue(TEXT("... nor one a hair before its expiry: clocks round (review N-2)"), Set.NextExpiry(700.0 - 1e-7) < 0.0);
	{
		// Review M4: Prune forgets what expired and keeps the rest (GetActive at time 0 would list a value that is still stored)
		FVaRestSecretSet Pruned;
		Pruned.Set(TEXT("bearer"), TEXT("token-v1-abcdef"), 0.0);
		Pruned.Set(TEXT("bearer"), TEXT("token-v2-abcdef"), 100.0);
		Pruned.Prune(100.0);
		TestEqual(TEXT("Prune keeps a live previous value"), Pruned.GetActive(0.0).Num(), 2);
		Pruned.Prune(700.0);
		TestEqual(TEXT("Prune forgets an expired one"), Pruned.GetActive(0.0).Num(), 1);
	}
	{
		FVaRestSecretSet Cased;
		Cased.Set(TEXT("a"), TEXT("TokenValue-1"), 0.0);
		Cased.Set(TEXT("b"), TEXT("tokenvalue-1"), 0.0);
		TestEqual(TEXT("secrets that differ only in case are two secrets"), Cased.GetActive(0.0).Num(), 2);
	}
	for (int32 i = 0; i < 6; ++i)
	{
		Set.Set(TEXT("bearer"), FString::Printf(TEXT("rotated-token-%d"), i), 200.0 + i);
	}
	TestEqual(TEXT("current + at most 4 previous values"), Set.GetActive(206.0).Num(), 1 + FVaRestSecretSet::MaxPrevious);
	Set.ClearAll();
	TestEqual(TEXT("logout forgets everything"), Set.GetActive(206.0).Num(), 0);
	TestTrue(TEXT("... and nothing expires"), Set.NextExpiry(206.0) < 0.0);

	// Review M9: a process-wide counter -- let an earlier test's body log lines (the redaction pipe) land first
	if (UE::Tasks::FPipe* Pipe = VaRest::Http::GetRedactionPipe())
	{
		Pipe->WaitUntilEmpty();
	}
	const uint64 Before = FVaRestTrace::Counters().RedactorCalls;
	const FVaRestRedactor Redactor;
	(void)Redactor.RedactUrl(TEXT("https://h/x"));
	(void)Redactor.ScrubText(TEXT("x"));
	TestEqual(TEXT("each redactor call is counted (IN-11)"), FVaRestTrace::Counters().RedactorCalls.load(), Before + 2);
	const FVaRestRedactor WithSecret(FVaRestRedactRules(), { VaRest::Http::RedactorTestPrivate::Bearer });
	TestEqualSensitive(TEXT("ScrubText replaces a known secret (an error message)"), WithSecret.ScrubText(FString::Printf(TEXT("failed for %s: 401"), VaRest::Http::RedactorTestPrivate::Bearer)), FString(TEXT("failed for ***: 401")));
	return true;
}

#endif
