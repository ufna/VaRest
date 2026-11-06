// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "HAL/Event.h"
#include "HAL/PlatformTime.h"
#include "HAL/Thread.h"
#include "Misc/Paths.h"
#include "Tasks/Task.h"
#include "VaRestHttpTesting.h"
#include "VaRestUrl.h"
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

	/** The straightforward per-candidate logic the linear scan replaced: quadratic, and the ground truth for it */
	int32 ReferenceScan(const FVaRestRedactRules& Rules, FStringView Piece, EVaRestRedactKeyKind FirstKind)
	{
		for (int32 Start = 0; Start < Piece.Len(); ++Start)
		{
			if (Start > 0 && Piece[Start - 1] != TEXT('?') && Piece[Start - 1] != TEXT(';') && Piece[Start - 1] != TEXT('#'))
			{
				continue;
			}
			int32 Equals = INDEX_NONE;
			if (Piece.RightChop(Start).FindChar(TEXT('='), Equals)
				&& Rules.ShouldRedact(FStringView(VaRest::Http::PercentDecode(Piece.Mid(Start, Equals), true)), Start == 0 ? FirstKind : EVaRestRedactKeyKind::Query))
			{
				return Start + Equals;
			}
		}
		return INDEX_NONE;
	}

	/** The form excerpt the reference scan gives: the pairs of AppendRedactedPairs, written with ReferenceScan */
	FString ReferenceForm(const FVaRestRedactRules& Rules, FStringView Body)
	{
		FString Out;
		int32 Start = 0;
		while (Start <= Body.Len())
		{
			int32 End = Start;
			while (End < Body.Len() && Body[End] != TEXT('&'))
			{
				++End;
			}
			const FStringView Piece = Body.Mid(Start, End - Start);
			int32 Equals = ReferenceScan(Rules, Piece, EVaRestRedactKeyKind::Form);
			int32 FirstEquals = INDEX_NONE;
			int32 Percent = INDEX_NONE;
			if (Equals == INDEX_NONE && Piece.FindChar(TEXT('='), FirstEquals) && Piece.RightChop(FirstEquals + 1).FindChar(TEXT('%'), Percent))
			{
				const FString Decoded = VaRest::Http::PercentDecode(Piece.RightChop(FirstEquals + 1), true);
				int32 Question = INDEX_NONE;
				if (Decoded.FindChar(TEXT('?'), Question))
				{
					// The decoded tail is a query: every '&'-pair of it is scanned (review round 4)
					TArray<FString> TailPairs;
					FString(FStringView(Decoded).RightChop(Question + 1)).ParseIntoArray(TailPairs, TEXT("&"), /*InCullEmpty*/ false);
					for (const FString& TailPair : TailPairs)
					{
						if (ReferenceScan(Rules, TailPair, EVaRestRedactKeyKind::Query) != INDEX_NONE)
						{
							Equals = FirstEquals;
						}
					}
				}
			}
			if (Equals != INDEX_NONE)
			{
				Out += Piece.Left(Equals);
				Out += TEXT("=***");
			}
			else
			{
				Out += Piece;
			}
			if (End < Body.Len())
			{
				Out.AppendChar(TEXT('&'));
			}
			Start = End + 1;
		}
		return Out;
	}
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRedactPartCutTest, "VaRest.Http.Redactor.MultipartCut",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRedactPartCutTest::RunTest(const FString& Parameters)
{
	// Task 14 fix round 1: a text part read only up to the cap plus the longest needle, or copied short by the log stage
	// (review M3), can end in the front of a known secret. The scrubs before it shorten the text, so that front can land
	// inside the cap; it must be dropped, as RedactBody drops a cut body's tail.
	const FString Secret(TEXT("SECRET-abcdefghijklmnop")); // 23 bytes; its percent, form and JSON forms are the same
	const FVaRestRedactor Redactor(FVaRestRedactRules(), { Secret });
	TestEqual(TEXT("the longest needle"), Redactor.GetMaxNeedleBytes(), 23);
	auto Field = [](const FString& Value, int32 Kept)
	{
		FVaRestMultipartPart Part;
		Part.Name = TEXT("f");
		Part.bField = true;
		const FTCHARToUTF8 Utf8(*Value);
		Part.Size = Utf8.Length();
		Part.Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), FMath::Min(Kept, Utf8.Length()));
		return Part;
	};

	// Cut by the reach (64 + 23 = 87 bytes), 10 bytes into the third copy; the first two become ******, 40 bytes fewer
	const FString Long = Secret + Secret + FString::ChrN(31, TEXT('x')) + Secret;
	const TArray<FVaRestMultipartPart> LongParts = { Field(Long, MAX_int32) };
	const FVaRestTraceExcerpt Reached = Redactor.DescribeMultipart(LongParts, Long.Len(), 64);
	TestFalse(TEXT("a part cut at the reach keeps no front of a secret"), Reached.ToText().Contains(TEXT("SECRET")));
	TestTrue(TEXT("... and is truncated"), Reached.bTruncated);
	TestTrue(TEXT("... the rest is there"), Reached.ToText().StartsWith(TEXT("part \"f\": ******xxx")));

	// A caller's copy cut 10 bytes into the second copy: Bytes shorter than Size
	const FString Short = Secret + TEXT("xx") + Secret;
	const TArray<FVaRestMultipartPart> CopiedParts = { Field(Short, 35) };
	const FVaRestTraceExcerpt Copied = Redactor.DescribeMultipart(CopiedParts, Short.Len(), 4096);
	TestEqualSensitive(TEXT("a part copied short keeps no front of a secret"), Copied.ToText(), FString(TEXT("part \"f\": ***xx")));
	TestTrue(TEXT("... and is truncated"), Copied.bTruncated);

	const TArray<FVaRestMultipartPart> WholeParts = { Field(Short, MAX_int32) };
	const FVaRestTraceExcerpt Whole = Redactor.DescribeMultipart(WholeParts, Short.Len(), 4096);
	TestEqualSensitive(TEXT("a whole part is scrubbed as before"), Whole.ToText(), FString(TEXT("part \"f\": ***xx***")));
	TestFalse(TEXT("... and not truncated"), Whole.bTruncated);
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
	VaRest::Http::WaitForRedactionPipe();
	const uint64 Before = FVaRestTrace::Counters().RedactorCalls;
	const FVaRestRedactor Redactor;
	(void)Redactor.RedactUrl(TEXT("https://h/x"));
	(void)Redactor.ScrubText(TEXT("x"));
	TestEqual(TEXT("each redactor call is counted (IN-11)"), FVaRestTrace::Counters().RedactorCalls.load(), Before + 2);
	const FVaRestRedactor WithSecret(FVaRestRedactRules(), { VaRest::Http::RedactorTestPrivate::Bearer });
	TestEqualSensitive(TEXT("ScrubText replaces a known secret (an error message)"), WithSecret.ScrubText(FString::Printf(TEXT("failed for %s: 401"), VaRest::Http::RedactorTestPrivate::Bearer)), FString(TEXT("failed for ***: 401")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRedactScanTest, "VaRest.Http.Redactor.CandidateScan",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRedactScanTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::RedactorTestPrivate;
	FVaRestRedactRules Rules;
	Rules.AddKey(TEXT("playerpin"));
	const FVaRestRedactor Redactor(Rules, {});

	// Review round 3 rulings: encoded nested queries, a query inside a form value, a second fragment
	TestEqualSensitive(TEXT("an encoded nested code"), Redactor.RedactUrl(TEXT("https://h/x?next=%2Fx%3Fcode%3DX")), FString(TEXT("https://h/x?next=***")));
	TestEqualSensitive(TEXT("an encoded nested access_token"), Redactor.RedactUrl(TEXT("https://h/x?next=%2Fx%3Faccess_token%3DX")), FString(TEXT("https://h/x?next=***")));
	TestEqualSensitive(TEXT("an encoded value without a secret is kept"), Redactor.RedactUrl(TEXT("https://h/x?next=%2Fhome")), FString(TEXT("https://h/x?next=%2Fhome")));
	TestEqualSensitive(TEXT("an encoded nested query without a secret is kept"), Redactor.RedactUrl(TEXT("https://h/x?next=%2Fx%3Fpage%3D2")), FString(TEXT("https://h/x?next=%2Fx%3Fpage%3D2")));
	// Review round 4: Query() encodes '&' as %26, so an OAuth callback's second pair hides behind its first
	TestEqualSensitive(TEXT("an encoded nested code after another pair"), Redactor.RedactUrl(TEXT("https://h/x?next=%2Fcb%3Fstate%3Dabc%26code%3DX")), FString(TEXT("https://h/x?next=***")));
	TestEqualSensitive(TEXT("an encoded nested access_token after another pair"), Redactor.RedactUrl(TEXT("https://h/x?next=%2Fx%3Fa%3D1%26access_token%3DX")), FString(TEXT("https://h/x?next=***")));
	TestEqualSensitive(TEXT("encoded nested pairs without a secret are kept"), Redactor.RedactUrl(TEXT("https://h/x?next=%2Fcb%3Fstate%3Dabc%26x%3D1")), FString(TEXT("https://h/x?next=%2Fcb%3Fstate%3Dabc%26x%3D1")));
	const FVaRestTraceExcerpt Nested = Redactor.RedactBody(TEXT("application/x-www-form-urlencoded"), Bytes("next=/x?code=X"), 14, 4096);
	TestEqualSensitive(TEXT("a '?code=' in a form value is a query key"), Nested.ToText(), FString(TEXT("next=/x?code=***")));
	TestEqualSensitive(TEXT("a second '#' in a fragment starts a key"), Redactor.RedactUrl(TEXT("https://h/x#/cb#code=X")), FString(TEXT("https://h/x#/cb#code=***")));
	// Task 5 re-review 3, minor 1: two candidates reach one '=' ("token;c" and "c"); only the longest names a secret, so a
	// scan that judged the shortest alone would leak X. The seeded strings below never produced this shape.
	const FVaRestTraceExcerpt Longest = Redactor.RedactBody(TEXT("application/x-www-form-urlencoded"), Bytes("a=b;token;c=X"), 13, 4096);
	TestEqualSensitive(TEXT("the longest candidate of a group is judged (form)"), Longest.ToText(), FString(TEXT("a=b;token;c=***")));
	TestEqualSensitive(TEXT("the longest candidate of a group is judged (query)"), Redactor.RedactUrl(TEXT("https://h/x?a=b;token;c=X")), FString(TEXT("https://h/x?a=b;token;c=***")));

	// The linear scan gives what the per-candidate reference gave, on seeded pseudo-random pieces over the separators, the
	// encodings and the letters of the rules' keys
	const TCHAR Alphabet[] = { 'a', 'b', 'c', '?', ';', '=', '&', '%', '2', 'F', '3', 'D', 't', 'o', 'k', 'e', 'n', 'p', 'w', 'd', 's', 'i', 'g', '#' };
	FRandomStream Random(20261021);
	int32 Mismatches = 0;
	int32 Redacting = 0;
	for (int32 Case = 0; Case < 3000; ++Case)
	{
		FString Text;
		const int32 Length = Random.RandRange(1, 24);
		for (int32 i = 0; i < Length; ++i)
		{
			Text.AppendChar(Alphabet[Random.RandRange(0, UE_ARRAY_COUNT(Alphabet) - 1)]);
		}
		// Splice whole keys in too, so the interesting candidates are common
		if (Random.RandRange(0, 2) == 0)
		{
			const TCHAR* const Keys[] = { TEXT("token="), TEXT("code="), TEXT("password="), TEXT("sig="), TEXT("key="), TEXT("tokentype="), TEXT("playerPin="), TEXT("a?code="), TEXT(";token=") };
			Text.InsertAt(Random.RandRange(0, Text.Len()), Keys[Random.RandRange(0, UE_ARRAY_COUNT(Keys) - 1)]);
		}
		const FTCHARToUTF8 Utf8(*Text);
		const TConstArrayView<uint8> Body(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		const FString Expected = ReferenceForm(Rules, Text);
		const FString Actual = Redactor.RedactBody(TEXT("application/x-www-form-urlencoded"), Body, Body.Num(), 4096).ToText();
		Redacting += Expected != Text ? 1 : 0;
		if (!Expected.Equals(Actual, ESearchCase::CaseSensitive))
		{
			if (++Mismatches <= 5)
			{
				AddError(FString::Printf(TEXT("'%s': reference '%s', linear '%s'"), *Text, *Expected, *Actual));
			}
		}
	}
	TestEqual(TEXT("the linear scan equals the reference on every string"), Mismatches, 0);
	TestTrue(TEXT("... and the strings redact often enough to mean something"), Redacting > 300);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRedactBoundTest, "VaRest.Http.Redactor.LinearBound",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRedactBoundTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::RedactorTestPrivate;
	// A smoke check against quadratic blowup (a server can send 100 KiB of ';' in a Location), not a benchmark: the bound is generous
	const FVaRestRedactor Redactor;
	const int32 Size = 64 * 1024;
	auto Repeat = [Size](const TCHAR* Unit, int32 UnitLength)
	{
		FString Text;
		Text.Reserve(Size);
		while (Text.Len() < Size)
		{
			Text.Append(Unit, UnitLength);
		}
		return Text;
	};
	auto Seconds = [](TFunctionRef<void()> Work)
	{
		const double Begin = FPlatformTime::Seconds();
		Work();
		return FPlatformTime::Seconds() - Begin;
	};
	const FString Semicolons = Repeat(TEXT(";"), 1);
	const FString Questions = Repeat(TEXT("?"), 1);
	const FString Encoded = Repeat(TEXT("%3F"), 3);
	const FString Hashes = Repeat(TEXT("#"), 1);
	const double Bound = 0.05;

	TestTrue(TEXT("64 KiB of ';' then '=' in a URL query"), Seconds([&] { (void)Redactor.RedactUrl(FString(TEXT("https://h/x?")) + Semicolons + TEXT("=")); }) < Bound);
	TestTrue(TEXT("... in a path run"), Seconds([&] { (void)Redactor.RedactUrl(FString(TEXT("https://h/x")) + Semicolons + TEXT("=")); }) < Bound);
	TestTrue(TEXT("... of '?'"), Seconds([&] { (void)Redactor.RedactUrl(FString(TEXT("https://h/x?")) + Questions + TEXT("=")); }) < Bound);
	TestTrue(TEXT("... of '#' in a fragment"), Seconds([&] { (void)Redactor.RedactUrl(FString(TEXT("https://h/x#")) + Hashes + TEXT("=")); }) < Bound);
	TestTrue(TEXT("... of '%3F' in an encoded value"), Seconds([&] { (void)Redactor.RedactUrl(FString(TEXT("https://h/x?next=")) + Encoded); }) < Bound);
	TestTrue(TEXT("... of '%3F' then '=' in an encoded value"), Seconds([&] { (void)Redactor.RedactUrl(FString(TEXT("https://h/x?next=")) + Encoded + TEXT("%3D")); }) < Bound);
	// A value that decodes to '?' then 64 KiB of "a=1&" (16 K nested pairs): the decoded tail is split once, not rescanned per pair
	FString NestedPairs(TEXT("%3F"));
	NestedPairs.Reserve(2 * Size + 3);
	while (NestedPairs.Len() < 2 * Size + 3)
	{
		NestedPairs += TEXT("a%3D1%26");
	}
	TestTrue(TEXT("... of encoded '&'-pairs in a nested query"), Seconds([&] { (void)Redactor.RedactUrl(FString(TEXT("https://h/x?next=")) + NestedPairs); }) < Bound);
	const FTCHARToUTF8 FormUtf8(*(Semicolons + TEXT("=")));
	const TConstArrayView<uint8> FormBody(reinterpret_cast<const uint8*>(FormUtf8.Get()), FormUtf8.Length());
	TestTrue(TEXT("... in a form body"), Seconds([&] { (void)Redactor.RedactBody(TEXT("application/x-www-form-urlencoded"), FormBody, FormBody.Num(), 1 << 20); }) < Bound);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRedactorLaunchTest, "VaRest.Http.Redactor.LaunchFromAnyThread",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRedactorLaunchTest::RunTest(const FString& Parameters)
{
	// Plan decision 16 (carry (a), review M-10), a smoke test: workers launch while the game thread stops the pipe; a launch
	// either runs its body or reports false, none is lost. The stop drains the queued bodies for longer than the workers
	// take to finish, so no launch meets the restart (review I-2): the Gate.* tests below force the interleavings.
	struct FCounts
	{
		std::atomic<int32> Ran{ 0 };
		std::atomic<int32> Accepted{ 0 };
		std::atomic<int32> Refused{ 0 };
		std::atomic<int32> WorkersDone{ 0 };
	};
	TSharedRef<FCounts, ESPMode::ThreadSafe> Counts = MakeShared<FCounts, ESPMode::ThreadSafe>();
	constexpr int32 Workers = 8;
	constexpr int32 PerWorker = 2000;
	for (int32 Worker = 0; Worker < Workers; ++Worker)
	{
		UE::Tasks::Launch(TEXT("VaRest.Test.LaunchRedaction"), [Counts]
		{
			for (int32 Index = 0; Index < PerWorker; ++Index)
			{
				if (VaRest::Http::LaunchRedaction(TEXT("VaRest.Test.Body"), [Counts] { ++Counts->Ran; }))
				{
					++Counts->Accepted;
				}
				else
				{
					++Counts->Refused;
				}
			}
			++Counts->WorkersDone;
		});
	}
	VaRest::Http::Testing::PumpUntil([Counts] { return Counts->Accepted.load() + Counts->Refused.load() > Workers * PerWorker / 4; });
	VaRest::Http::StopRedactionPipe();
	VaRest::Http::StartRedactionPipe();
	if (!TestTrue(TEXT("the workers finish"), VaRest::Http::Testing::PumpUntil([Counts] { return Counts->WorkersDone.load() == Workers; }, 30.0)))
	{
		return false;
	}
	VaRest::Http::WaitForRedactionPipe();
	TestEqual(TEXT("every launch reported"), Counts->Accepted.load() + Counts->Refused.load(), Workers * PerWorker);
	TestEqual(TEXT("every accepted body ran, no refused one did"), Counts->Ran.load(), Counts->Accepted.load());
	TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> bAfter = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	TestTrue(TEXT("the restarted pipe takes work"), VaRest::Http::LaunchRedaction(TEXT("VaRest.Test.After"), [bAfter] { *bAfter = true; }));
	VaRest::Http::WaitForRedactionPipe();
	TestTrue(TEXT("... and runs it"), bAfter->load());
	return true;
}

namespace VaRest::Http::RedactorTestPrivate
{
	/**
	 * Review I-2: one launcher W on its own thread and the game thread's Stop, on a gate of the test's own, in an order
	 * the gate's test hook forces. Every wait is bounded, and a broken gate is made to fail an assertion without ever
	 * reaching a deleted pipe: Stop's last hook lets W's launch return before the pipe is taken out.
	 */
	struct FGateScript
	{
		enum class EOrder : uint8
		{
			/** W read the flag clear; the stop begins and drains before W counts itself in */
			StopBetweenReads,
			/** W counted itself in and holds the pipe; the stop begins */
			StopWhileHolding,
		};

		explicit FGateScript(EOrder InOrder)
			: Order(InOrder)
		{
		}

		static constexpr uint32 WaitMs = 5000;

		static void OnHook(void* Context, FRedactionGate::EHookPoint Point)
		{
			FGateScript& Script = *static_cast<FGateScript*>(Context);
			switch (Point)
			{
			case FRedactionGate::EHookPoint::AfterUncountedRead: // W
				if (Script.Order == EOrder::StopBetweenReads)
				{
					Script.Reached->Trigger();
					Script.Release->Wait(WaitMs);
				}
				break;
			case FRedactionGate::EHookPoint::HoldingPipe: // W
				Script.bHeldPastDrain = Script.bDrained.load();
				if (Script.Order == EOrder::StopWhileHolding)
				{
					Script.Reached->Trigger();
					Script.Release->Wait(WaitMs);
				}
				break;
			case FRedactionGate::EHookPoint::StopWaitingForCount: // the game thread, inside Stop
				// Released only by a stop that waits for it; the stop reads the count again after W's launch returned
				Script.bStopWaited = true;
				Script.Release->Trigger();
				Script.Returned->Wait(WaitMs);
				break;
			case FRedactionGate::EHookPoint::StopAfterDrain: // the game thread, inside Stop
				Script.bDrained = true;
				Script.bReturnedAtDrain = Script.bLaunchReturned.load();
				Script.Release->Trigger();
				Script.Returned->Wait(WaitMs);
				break;
			}
		}

		const EOrder Order;
		FRedactionGate Gate;
		FEventRef Reached{ EEventMode::ManualReset };
		FEventRef Release{ EEventMode::ManualReset };
		FEventRef Returned{ EEventMode::ManualReset };
		std::atomic<bool> bDrained{ false };
		std::atomic<bool> bHeldPastDrain{ false };
		std::atomic<bool> bStopWaited{ false };
		std::atomic<bool> bReturnedAtDrain{ false };
		std::atomic<bool> bLaunchReturned{ false };
		std::atomic<bool> bAccepted{ false };
		std::atomic<int32> Ran{ 0 };
	};

	using FGateScriptRef = TSharedRef<FGateScript, ESPMode::ThreadSafe>;

	/** Starts the gate, runs W's one launch against the game thread's Stop in Script's order, joins W; false when W never reached its point */
	bool RunGateScript(const FGateScriptRef& Script)
	{
		Script->Gate.SetTestHook(&FGateScript::OnHook, &Script.Get());
		Script->Gate.Start();
		FThread Launcher(TEXT("VaRest.Test.GateLauncher"), [Script]
		{
			Script->bAccepted = Script->Gate.Launch(TEXT("VaRest.Test.GateBody"), [Script] { ++Script->Ran; });
			Script->bLaunchReturned = true;
			Script->Returned->Trigger();
		});
		const bool bReached = Script->Reached->Wait(FGateScript::WaitMs);
		Script->Gate.Stop();
		Launcher.Join();
		Script->Gate.SetTestHook(nullptr, nullptr);
		return bReached;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRedactorGateBetweenReadsTest, "VaRest.Http.Redactor.Gate.StopBetweenReads",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRedactorGateBetweenReadsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::RedactorTestPrivate;
	// Review I-2, plan decision 16: a launch that read the flag clear just before a stop, and counts itself in only after
	// the stop has seen no launch inside, must be refused by its second read -- it must not take the pipe being deleted
	const FGateScriptRef Script = MakeShared<FGateScript, ESPMode::ThreadSafe>(FGateScript::EOrder::StopBetweenReads);
	TestTrue(TEXT("W read the flag clear before the stop began"), RunGateScript(Script));
	TestFalse(TEXT("the stop had no launch inside to wait for"), Script->bStopWaited.load());
	TestFalse(TEXT("the launch is refused"), Script->bAccepted.load());
	TestFalse(TEXT("... and never held the pipe past the stop's drain"), Script->bHeldPastDrain.load());
	TestEqual(TEXT("... nor ran its body"), Script->Ran.load(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRedactorGateWhileHoldingTest, "VaRest.Http.Redactor.Gate.StopWhileHolding",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRedactorGateWhileHoldingTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::RedactorTestPrivate;
	// Review I-2, plan decision 16: a stop that begins while a launch holds the pipe must wait for that launch to return
	// before it takes the pipe out, and the launched body runs before the delete
	const FGateScriptRef Script = MakeShared<FGateScript, ESPMode::ThreadSafe>(FGateScript::EOrder::StopWhileHolding);
	TestTrue(TEXT("W held the pipe before the stop began"), RunGateScript(Script));
	TestTrue(TEXT("the stop waited for the launch inside"), Script->bStopWaited.load());
	TestTrue(TEXT("... and drained only after it returned"), Script->bReturnedAtDrain.load());
	TestTrue(TEXT("the launch is accepted"), Script->bAccepted.load());
	TestFalse(TEXT("... and held the pipe before the drain"), Script->bHeldPastDrain.load());
	TestEqual(TEXT("its body ran before the pipe went"), Script->Ran.load(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRedactBinaryCaptureTest, "VaRest.Http.Redactor.BinaryCapture",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestHttpRedactBinaryCaptureTest::RunTest(const FString& Parameters)
{
	const FVaRestRedactor Redactor(FVaRestRedactRules(), {TEXT("Binary-Known-Secret")});
	const TArray<uint8> Raw{0, 0xff, 0x89, 'x'};
	const auto Default = Redactor.RedactBody(TEXT("application/octet-stream"), Raw, Raw.Num(), 4096);
	TestTrue(TEXT("existing overload retains a descriptor"), !Default.bBinaryCaptured && Default.ToText().StartsWith(TEXT("<binary: ")));
	const auto Captured = Redactor.RedactBody(TEXT("application/octet-stream"), Raw, Raw.Num(), 0, 3);
	TestTrue(TEXT("independent binary cap preserves NUL and invalid UTF-8"), Captured.bBinaryCaptured && Captured.Bytes && *Captured.Bytes == TArray<uint8>{0, 0xff, 0x89});
	TestTrue(TEXT("binary cap is explicit and truncated"), Captured.bTruncated && Captured.StopReason == TEXT("cap") && Captured.TotalBytes == 4);
	TestEqual(TEXT("text projection is hex, never a binary UTF-8 decode"), Captured.ToText(), FString(TEXT("00ff89")));
	TestFalse(TEXT("negative binary cap keeps descriptor mode"), Redactor.RedactBody(TEXT("image/png"), Raw, Raw.Num(), 4096, -1).bBinaryCaptured);
	const FTCHARToUTF8 Secret(TEXT("Binary-Known-Secret"));
	TArray<uint8> Payload{0, 0xff}; Payload.Append(reinterpret_cast<const uint8*>(Secret.Get()), Secret.Length()); Payload.Add(0x80);
	const auto Whole = Redactor.RedactBody(TEXT("application/x-protobuf"), Payload, Payload.Num(), 1, 256);
	TestTrue(TEXT("known value scrub changes only its byte span"), Whole.bRedacted && !Whole.bTruncated && Whole.Bytes && *Whole.Bytes == TArray<uint8>{0, 0xff, '*', '*', '*', 0x80});
	const auto Cut = Redactor.RedactBody(TEXT("application/x-protobuf"), Payload, Payload.Num(), 1, 8);
	TestTrue(TEXT("a cap through a known secret removes its entire partial tail"), Cut.bRedacted && Cut.bTruncated && Cut.Bytes && *Cut.Bytes == TArray<uint8>{0, 0xff});
	const char Json[] = "{\"password\":\"canary\",\"ok\":7}";
	const auto JsonBytes = TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Json), sizeof(Json)-1);
	const auto Text = Redactor.RedactBody(TEXT("application/json"), JsonBytes, JsonBytes.Num(), 4096, 1);
	TestTrue(TEXT("binary cap does not change JSON cap or key redaction"), !Text.bBinaryCaptured && Text.Kind == EVaRestExcerptKind::Json && !Text.ToText().Contains(TEXT("canary")) && Text.ToText().Contains(TEXT("\"ok\":7")));
	const auto Empty = Redactor.RedactBody(TEXT("image/png"), {}, 0, 0, 4);
	TestTrue(TEXT("empty binary capture has an explicit marker and no descriptor bytes"), Empty.bBinaryCaptured && !Empty.bTruncated && Empty.Bytes && Empty.Bytes->IsEmpty());
	return true;
}

#endif
