// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "VaRestBody.h"
#include "VaRestUrl.h"

namespace VaRest::Http::UrlTestPrivate
{
	FString AsText(TConstArrayView<uint8> Bytes)
	{
		return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
	}

	TArray<uint8> Utf8(FStringView Text)
	{
		FTCHARToUTF8 Converted(Text.GetData(), Text.Len());
		return TArray<uint8>(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPercentEncodeTest, "VaRest.Http.Url.PercentEncode",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPercentEncodeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	TestEqualSensitive(TEXT("RFC 3986 unreserved characters are kept"), PercentEncode(TEXT("AZaz09-._~")), FString(TEXT("AZaz09-._~")));
	TestEqualSensitive(TEXT("everything else is %XX of its UTF-8"), PercentEncode(TEXT("a b/c?d#e&f=g+h")), FString(TEXT("a%20b%2Fc%3Fd%23e%26f%3Dg%2Bh")));
	TestEqualSensitive(TEXT("two-byte UTF-8"), PercentEncode(TEXT("é")), FString(TEXT("%C3%A9")));
	TestEqualSensitive(TEXT("three-byte UTF-8"), PercentEncode(TEXT("日本")), FString(TEXT("%E6%97%A5%E6%9C%AC")));
	TestEqualSensitive(TEXT("a surrogate pair is one four-byte character"), PercentEncode(TEXT("\U0001F600")), FString(TEXT("%F0%9F%98%80")));
	TestEqualSensitive(TEXT("form encoding: space as '+', '*' kept, '~' encoded (WHATWG)"), FormEncode(TEXT("a b*~")), FString(TEXT("a+b*%7E")));

	TestEqualSensitive(TEXT("decode round trip"), PercentDecode(PercentEncode(TEXT("x y/é"))), FString(TEXT("x y/é")));
	TestEqualSensitive(TEXT("an invalid escape is kept as text"), PercentDecode(TEXT("%zz%4")), FString(TEXT("%zz%4")));
	TestEqualSensitive(TEXT("'+' stays '+' in a URL"), PercentDecode(TEXT("a+b")), FString(TEXT("a+b")));
	TestEqualSensitive(TEXT("... and is a space in a form"), PercentDecode(TEXT("a+b"), true), FString(TEXT("a b")));
	TestEqualSensitive(TEXT("decoded bytes that are not UTF-8 become U+FFFD"), PercentDecode(TEXT("%C3(")), FString(TEXT("�(")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpJoinTest, "VaRest.Http.Url.Join",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpJoinTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	struct FCase
	{
		const TCHAR* Base;
		const TCHAR* Path;
		const TCHAR* Expected;
	};
	// OpenAPI N14: exactly one '/' at the join, never RFC 3986 resolution (which would drop /v1 for "/pets")
	const FCase Cases[] = {
		{ TEXT("https://h/v1"), TEXT("/pets"), TEXT("https://h/v1/pets") },
		{ TEXT("https://h/v1/"), TEXT("pets"), TEXT("https://h/v1/pets") },
		{ TEXT("https://h/v1//"), TEXT("//pets"), TEXT("https://h/v1/pets") },
		{ TEXT("https://h/v1/"), TEXT("/pets/"), TEXT("https://h/v1/pets/") },
		{ TEXT("https://h"), TEXT("/"), TEXT("https://h/") },
		{ TEXT("https://h/v1"), TEXT(""), TEXT("https://h/v1") },
		{ TEXT("https://h/v1"), TEXT("https://other/x"), TEXT("https://other/x") },
		{ TEXT("https://h/v1"), TEXT("HTTP://other/x"), TEXT("HTTP://other/x") },
		{ TEXT("  https://h/v1 \t"), TEXT("  /pets \n"), TEXT("https://h/v1/pets") },
		{ TEXT("https://h/v1"), TEXT("?a=1"), TEXT("https://h/v1?a=1") },
		{ TEXT(""), TEXT("/pets"), TEXT("/pets") },
		// Decision 33 (review M22): a base URL's query stays, first
		{ TEXT("https://h/v1?key=k"), TEXT("/pets"), TEXT("https://h/v1/pets?key=k") },
		{ TEXT("https://h/v1/?key=k"), TEXT("/pets?limit=2"), TEXT("https://h/v1/pets?key=k&limit=2") },
		{ TEXT("https://h/v1?key=k"), TEXT(""), TEXT("https://h/v1?key=k") },
		{ TEXT("https://h/v1?key=k"), TEXT("?a=1"), TEXT("https://h/v1?key=k&a=1") },
		{ TEXT("https://h/v1?key=k"), TEXT("/p#top"), TEXT("https://h/v1/p?key=k#top") },
	};
	for (const FCase& Case : Cases)
	{
		TestEqualSensitive(FString::Printf(TEXT("'%s' + '%s'"), Case.Base, Case.Path), JoinUrl(Case.Base, Case.Path), FString(Case.Expected));
	}
	TestTrue(TEXT("http://h is absolute"), IsAbsoluteHttpUrl(TEXT("http://h")));
	TestTrue(TEXT("... in any case"), IsAbsoluteHttpUrl(TEXT("HttpS://h/x")));
	TestFalse(TEXT("a scheme without a host is not"), IsAbsoluteHttpUrl(TEXT("https://")));
	TestFalse(TEXT("... nor an empty host"), IsAbsoluteHttpUrl(TEXT("https:///x")));
	TestFalse(TEXT("other schemes are not http(s)"), IsAbsoluteHttpUrl(TEXT("ftp://h/x")));
	TestFalse(TEXT("a path is not"), IsAbsoluteHttpUrl(TEXT("/users/7")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPathTemplateTest, "VaRest.Http.Url.PathTemplate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPathTemplateTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using FParams = TArray<TPair<FString, FString>>;

	const FParams Params = { { TEXT("id"), TEXT("a/b?c#d e") }, { TEXT("item"), TEXT("日本") } };
	const TValueOrError<FString, FString> Expanded = ExpandPathTemplate(TEXT("/users/{id}/items/{item}"), Params);
	TestTrue(TEXT("expands"), Expanded.HasValue());
	TestEqualSensitive(TEXT("each value is one percent-encoded segment"), Expanded.GetValue(),
		FString(TEXT("/users/a%2Fb%3Fc%23d%20e/items/%E6%97%A5%E6%9C%AC")));

	const FParams Twice = { { TEXT("id"), TEXT("1") }, { TEXT("id"), TEXT("2") } };
	TestEqualSensitive(TEXT("a placeholder used twice gets the value twice; the last Path() wins"),
		ExpandPathTemplate(TEXT("/{id}/x/{id}"), Twice).GetValue(), FString(TEXT("/2/x/2")));
	TestEqualSensitive(TEXT("no placeholders, no parameters"), ExpandPathTemplate(TEXT("/plain"), FParams()).GetValue(), FString(TEXT("/plain")));

	// Decision 10: every misuse is a Config error, never a silent "/users/"
	const FParams Empty = { { TEXT("id"), TEXT("") } };
	const FParams Unknown = { { TEXT("id"), TEXT("1") }, { TEXT("idd"), TEXT("2") } };
	TestTrue(TEXT("a missing parameter is an error"), ExpandPathTemplate(TEXT("/users/{id}"), FParams()).GetError().Contains(TEXT("{id}")));
	TestTrue(TEXT("an empty parameter is an error"), ExpandPathTemplate(TEXT("/users/{id}"), Empty).GetError().Contains(TEXT("empty")));
	TestTrue(TEXT("a parameter the template lacks is an error (a typo)"), ExpandPathTemplate(TEXT("/users/{id}"), Unknown).GetError().Contains(TEXT("idd")));
	TestTrue(TEXT("an unclosed '{' is an error"), ExpandPathTemplate(TEXT("/users/{id"), FParams()).HasError());
	TestTrue(TEXT("a stray '}' is an error"), ExpandPathTemplate(TEXT("/users/id}"), FParams()).HasError());
	TestTrue(TEXT("'{}' is an error"), ExpandPathTemplate(TEXT("/users/{}"), FParams()).HasError());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpQueryTest, "VaRest.Http.Url.Query",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpQueryTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using FPairs = TArray<TPair<FString, FString>>;
	const FPairs Query = { { TEXT("a"), TEXT("1") }, { TEXT("b"), TEXT("x y&z") }, { TEXT("a"), TEXT("2") } };

	FString Url = TEXT("https://h/x");
	AppendQuery(Url, Query);
	TestEqualSensitive(TEXT("in order, repeats kept (OpenAPI N9), keys and values encoded"), Url, FString(TEXT("https://h/x?a=1&b=x%20y%26z&a=2")));

	Url = TEXT("https://h/x?k=v");
	AppendQuery(Url, FPairs{ { TEXT("a"), TEXT("1") } });
	TestEqualSensitive(TEXT("an existing query continues with '&'"), Url, FString(TEXT("https://h/x?k=v&a=1")));

	Url = TEXT("https://h/x?");
	AppendQuery(Url, FPairs{ { TEXT("a"), TEXT("1") } });
	TestEqualSensitive(TEXT("a trailing '?' is not doubled"), Url, FString(TEXT("https://h/x?a=1")));

	Url = TEXT("https://h/x#frag");
	AppendQuery(Url, FPairs{ { TEXT("a"), TEXT("1") } });
	TestEqualSensitive(TEXT("the query goes before the fragment"), Url, FString(TEXT("https://h/x?a=1#frag")));

	// OpenAPI N9's array styles (review I7, decision 34)
	FPairs Styled;
	const TArray<FString> Tags = { TEXT("a b"), TEXT("c") };
	AddQueryValues(Styled, TEXT("tag"), Tags, EVaRestQueryStyle::Repeat);
	AddQueryValues(Styled, TEXT("csv"), Tags, EVaRestQueryStyle::Csv);
	AddQueryValues(Styled, TEXT("ssv"), Tags, EVaRestQueryStyle::Ssv);
	AddQueryValues(Styled, TEXT("pipes"), Tags, EVaRestQueryStyle::Pipes);
	AddQueryValues(Styled, TEXT("none"), TArray<FString>(), EVaRestQueryStyle::Csv);
	Url = TEXT("https://h/x");
	AppendQuery(Url, Styled);
	TestEqualSensitive(TEXT("Repeat, Csv, Ssv and Pipes; joined before encoding; no values, no pair"), Url,
		FString(TEXT("https://h/x?tag=a%20b&tag=c&csv=a%20b%2Cc&ssv=a%20b%20c&pipes=a%20b%7Cc")));

	Url = TEXT("https://h/x");
	AppendQuery(Url, FPairs());
	TestEqualSensitive(TEXT("no query, no '?'"), Url, FString(TEXT("https://h/x")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpHeaderChecksTest, "VaRest.Http.Url.HeaderChecks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpHeaderChecksTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	TestTrue(TEXT("token names"), IsValidHeaderName(TEXT("X-Api-Key")) && IsValidHeaderName(TEXT("!#$%&'*+-.^_`|~09az")));
	TestFalse(TEXT("empty name"), IsValidHeaderName(TEXT("")));
	TestFalse(TEXT("space in a name"), IsValidHeaderName(TEXT("Bad Name")));
	TestFalse(TEXT("colon in a name"), IsValidHeaderName(TEXT("Bad:Name")));
	TestFalse(TEXT("CRLF in a name"), IsValidHeaderName(TEXT("Bad\r\nName")));
	TestFalse(TEXT("non-ASCII name"), IsValidHeaderName(TEXT("ключ")));
	TestTrue(TEXT("ordinary values"), IsValidHeaderValue(TEXT("Bearer abc.def; q=1, é")));
	TestFalse(TEXT("CR in a value"), IsValidHeaderValue(TEXT("a\rb")));
	TestFalse(TEXT("LF in a value (header injection)"), IsValidHeaderValue(TEXT("a\nX-Evil: 1")));
	const TCHAR WithNul[] = { TEXT('a'), TEXT('\0'), TEXT('b') };
	TestFalse(TEXT("NUL in a value"), IsValidHeaderValue(FStringView(WithNul, 3)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpFormTest, "VaRest.Http.Body.Form",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpFormTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::UrlTestPrivate;
	const FVaRestForm Form = FVaRestForm().Add(TEXT("name"), TEXT("Neo Anderson")).Add(TEXT("q"), TEXT("a&b=c")).Add(TEXT("к"), TEXT("é"));
	TestEqualSensitive(TEXT("urlencoded pairs"), AsText(Form.Encode()), FString(TEXT("name=Neo+Anderson&q=a%26b%3Dc&%D0%BA=%C3%A9")));

	const FVaRestRequestBody Body = FVaRestRequestBody::FromForm(Form);
	TestTrue(TEXT("form body kind"), Body.Kind == EVaRestBodyKind::Form);
	TestEqualSensitive(TEXT("form content type"), Body.ContentType, FString(TEXT("application/x-www-form-urlencoded")));
	TestEqual(TEXT("content length"), Body.ContentLength, static_cast<int64>(Body.Bytes.Num()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpMultipartTest, "VaRest.Http.Body.Multipart",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpMultipartTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::UrlTestPrivate;
	FVaRestMultipart Multipart(TEXT("XyZ"));
	Multipart.AddField(TEXT("title"), TEXT("hi \"there\"")).AddBytes(TEXT("blob"), TEXT("a.bin"), TEXT("application/octet-stream"), { 1, 2, 3 });
	Multipart.AddField(TEXT("we\"ird\r\n"), TEXT("v"));
	TestFalse(TEXT("no file parts"), Multipart.HasFiles());
	TestEqualSensitive(TEXT("content type names the boundary"), Multipart.GetContentType(), FString(TEXT("multipart/form-data; boundary=XyZ")));

	const TValueOrError<FVaRestRequestBody, FString> Body = FVaRestRequestBody::FromMultipart(Multipart);
	TestTrue(TEXT("builds"), Body.HasValue());
	TArray<uint8> Expected = Utf8(TEXT("--XyZ\r\nContent-Disposition: form-data; name=\"title\"\r\n\r\nhi \"there\"\r\n")
		TEXT("--XyZ\r\nContent-Disposition: form-data; name=\"blob\"; filename=\"a.bin\"\r\nContent-Type: application/octet-stream\r\n\r\n"));
	Expected.Append({ 1, 2, 3 });
	// WHATWG multipart/form-data: '"', CR and LF in a name are percent-escaped, so a name cannot end the header
	Expected.Append(Utf8(TEXT("\r\n--XyZ\r\nContent-Disposition: form-data; name=\"we%22ird%0D%0A\"\r\n\r\nv\r\n--XyZ--\r\n")));
	TestTrue(TEXT("in-memory multipart body, byte for byte"), Body.GetValue().Bytes == Expected);
	TestTrue(TEXT("kind"), Body.GetValue().Kind == EVaRestBodyKind::Multipart && !Body.GetValue().IsStreamed());
	TestEqual(TEXT("three part descriptors"), Body.GetValue().PartDescriptors.Num(), 3);
	TestEqual(TEXT("binary descriptors drop their bytes"), Body.GetValue().PartDescriptors[1].Bytes.Num(), 0);
	TestEqual(TEXT("... but keep their size"), Body.GetValue().PartDescriptors[1].Size, static_cast<int64>(3));

	FVaRestMultipart Bad(TEXT("XyZ"));
	Bad.AddBytes(TEXT("f"), TEXT("f.bin"), TEXT("text/plain\r\nX-Evil: 1"), { 1 });
	TestTrue(TEXT("a part content type with CRLF is refused"), FVaRestRequestBody::FromMultipart(Bad).HasError());

	FVaRestMultipart NulBoundary(FStringView(TEXT("ab\0cd"), 5));
	NulBoundary.AddField(TEXT("a"), TEXT("1"));
	TestTrue(TEXT("a boundary holding NUL is refused (review N-M1)"), FVaRestRequestBody::FromMultipart(NulBoundary).HasError());

	FVaRestMultipart Random;
	TestTrue(TEXT("the default boundary is random and long"), Random.GetBoundary().Len() >= 32 && Random.GetBoundary() != FVaRestMultipart().GetBoundary());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpMultipartStreamTest, "VaRest.Http.Body.MultipartStream",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpMultipartStreamTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::UrlTestPrivate;
	const FString Dir = FPaths::ProjectSavedDir() / TEXT("VaRestTests");
	const FString File = Dir / TEXT("multipart-file.bin");
	TArray<uint8> FileBytes;
	FileBytes.SetNumUninitialized(100000);
	for (int32 i = 0; i < FileBytes.Num(); ++i)
	{
		FileBytes[i] = static_cast<uint8>(i * 31 + 7);
	}
	TestTrue(TEXT("fixture written"), FFileHelper::SaveArrayToFile(FileBytes, *File));

	FVaRestMultipart Multipart(TEXT("B"));
	Multipart.AddField(TEXT("a"), TEXT("1")).AddFile(TEXT("f"), File, TEXT("image/png"));
	TestTrue(TEXT("file parts"), Multipart.HasFiles());
	TestEqualSensitive(TEXT("the file name defaults to the clean file name"), Multipart.GetParts()[1].FileName, FString(TEXT("multipart-file.bin")));

	const TValueOrError<FVaRestRequestBody, FString> Body = FVaRestRequestBody::FromMultipart(Multipart);
	TestTrue(TEXT("builds"), Body.HasValue());
	const FVaRestRequestBody& B = Body.GetValue();
	TestTrue(TEXT("streamed: memory, file, memory"), B.IsStreamed() && B.Segments.Num() == 3 && !B.Segments[1].FilePath.IsEmpty());

	TArray<uint8> Expected = Utf8(TEXT("--B\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\n1\r\n--B\r\nContent-Disposition: form-data; name=\"f\"; filename=\"multipart-file.bin\"\r\nContent-Type: image/png\r\n\r\n"));
	Expected.Append(FileBytes);
	Expected.Append(Utf8(TEXT("\r\n--B--\r\n")));
	TestEqual(TEXT("content length is the sum of the segments"), B.ContentLength, static_cast<int64>(Expected.Num()));

	FVaRestSegmentStream Stream(B.Segments);
	TestEqual(TEXT("TotalSize"), Stream.TotalSize(), static_cast<int64>(Expected.Num()));
	TArray<uint8> Read;
	Read.SetNumZeroed(Expected.Num());
	int64 Done = 0;
	while (Done < Read.Num())
	{
		// curl reads in its own block sizes and seeks to what it has sent (HttpRequestPayload.cpp:77-96)
		const int64 Take = FMath::Min<int64>(4097, Read.Num() - Done);
		Stream.Seek(Done);
		Stream.Serialize(Read.GetData() + Done, Take);
		Done += Take;
	}
	TestTrue(TEXT("streamed bytes equal the whole body"), Read == Expected && !Stream.IsError());
	Stream.Seek(0);
	TArray<uint8> Again;
	Again.SetNumZeroed(Expected.Num());
	Stream.Serialize(Again.GetData(), Again.Num());
	TestTrue(TEXT("seeking back to 0 reads it again"), Again == Expected);
	Stream.Close();

	FVaRestMultipart Missing(TEXT("B"));
	Missing.AddFile(TEXT("f"), Dir / TEXT("does-not-exist.bin"));
	TestTrue(TEXT("a missing file is an error"), !Missing.GetError().IsEmpty() && FVaRestRequestBody::FromMultipart(Missing).HasError());

	const TValueOrError<FVaRestRequestBody, FString> Upload = FVaRestRequestBody::FromFile(File, TEXT("application/octet-stream"));
	TestTrue(TEXT("a streamed file body"), Upload.HasValue() && Upload.GetValue().Kind == EVaRestBodyKind::Stream && Upload.GetValue().ContentLength == FileBytes.Num());
	TestTrue(TEXT("a missing upload file is an error"), FVaRestRequestBody::FromFile(Dir / TEXT("nope.bin"), TEXT("a/b")).HasError());
	IFileManager::Get().Delete(*File);
	return true;
}

#endif
