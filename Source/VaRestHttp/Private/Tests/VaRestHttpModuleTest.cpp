// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "VaRestHttpSettings.h"
#include "VaRestHttpTypes.h"
#include "VaRestSha256.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpModuleLoadsTest, "VaRest.Http.Module.Loads",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpModuleLoadsTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("VaRestHttp is loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("VaRestHttp")));
	const UVaRestHttpSettings* Settings = GetDefault<UVaRestHttpSettings>();
	if (!TestNotNull(TEXT("the settings default object exists"), Settings))
	{
		return false;
	}
	TestTrue(TEXT("default log level is Basic (spec 4.5)"), Settings->LogLevel == EVaRestLogLevel::Basic);
	TestEqual(TEXT("default max concurrent is 16 (spec 4.6)"), Settings->MaxConcurrent, 16);
	TestEqual(TEXT("total timeout 0: the engine default"), Settings->Timeout, 0.f);
	TestEqual(TEXT("activity timeout 0: the engine default"), Settings->ActivityTimeout, 0.f);
	TestEqual(TEXT("JSON responses are capped at 32 MiB by default (controller ruling, M2 final review rec. 4)"), Settings->MaxJsonBytes, 32ll * 1024 * 1024);
	TestEqual(TEXT("Project Settings > Plugins"), Settings->GetCategoryName(), FName(TEXT("Plugins")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpHeadersTest, "VaRest.Http.Types.Headers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpHeadersTest::RunTest(const FString& Parameters)
{
	FVaRestHeaders H;
	H.Set(TEXT("Content-Type"), TEXT("text/plain"));
	H.Add(TEXT("Set-Cookie"), TEXT("a=1"));
	H.Add(TEXT("set-cookie"), TEXT("b=2"));
	H.Set(TEXT("X-Trace"), TEXT("1"));
	TestEqual(TEXT("repeated names are kept"), H.Num(), 4);
	TestEqualSensitive(TEXT("lookup ignores case"), H.Get(TEXT("CONTENT-TYPE")), FString(TEXT("text/plain")));
	const FString* FirstCookie = H.Find(TEXT("Set-Cookie"));
	if (!TestNotNull(TEXT("Find finds a repeated name"), FirstCookie))
	{
		return false;
	}
	TestEqualSensitive(TEXT("Find returns the first of a repeated name"), *FirstCookie, FString(TEXT("a=1")));

	H.Set(TEXT("SET-COOKIE"), TEXT("c=3"));
	if (!TestEqual(TEXT("Set replaces every header of that name"), H.Num(), 3))
	{
		return false;
	}
	TestEqualSensitive(TEXT("... at the first one's position"), H.GetPairs()[1].Value, FString(TEXT("c=3")));
	TestEqualSensitive(TEXT("... with the name as given"), H.GetPairs()[1].Key, FString(TEXT("SET-COOKIE")));

	TestTrue(TEXT("Remove reports a removal"), H.Remove(TEXT("x-trace")));
	TestFalse(TEXT("... and a miss"), H.Remove(TEXT("x-trace")));
	TestTrue(TEXT("Get on a miss is empty"), H.Get(TEXT("nope")).IsEmpty() && H.Find(TEXT("nope")) == nullptr);

	const TArray<FString> Lines = { TEXT("Content-Length: 12"), TEXT("ETag:  \"v1\" "), TEXT("garbage"), TEXT("Set-Cookie: a=1"), TEXT("Set-Cookie: b=2") };
	const FVaRestHeaders Parsed = FVaRestHeaders::FromLines(Lines);
	TestEqual(TEXT("engine lines parse; a line without ':' is skipped"), Parsed.Num(), 4);
	TestEqualSensitive(TEXT("values are trimmed"), Parsed.Get(TEXT("etag")), FString(TEXT("\"v1\"")));
	int32 Cookies = 0;
	for (const FVaRestHeaders::FPair& Pair : Parsed)
	{
		Cookies += FStringView(Pair.Key).Equals(TEXTVIEW("set-cookie"), ESearchCase::IgnoreCase) ? 1 : 0;
	}
	TestEqual(TEXT("both Set-Cookie lines survive"), Cookies, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpErrorTypesTest, "VaRest.Http.Types.Errors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpErrorTypesTest::RunTest(const FString& Parameters)
{
	const FString NotFound = FVaRestError::DefaultMessage(EVaRestErrorKind::Http, 404);
	TestTrue(TEXT("HTTP message names the status"), NotFound.Contains(TEXT("404")) && NotFound.Contains(TEXT("Not Found")));
	TestFalse(TEXT("every kind but None has a message"), FVaRestError::DefaultMessage(EVaRestErrorKind::Decode, 0).IsEmpty());

	for (int32 Status : { 408, 425, 429, 500, 502, 503, 504 })
	{
		TestTrue(FString::Printf(TEXT("%d is retryable (spec 4.1)"), Status), FVaRestError::IsRetryableStatus(Status));
	}
	for (int32 Status : { 200, 400, 401, 403, 404, 409, 501 })
	{
		TestFalse(FString::Printf(TEXT("%d is not retryable"), Status), FVaRestError::IsRetryableStatus(Status));
	}

	FVaRestError E;
	TestFalse(TEXT("a default error is no error"), E.IsError());
	E.Kind = EVaRestErrorKind::Decode;
	E.Message = TEXT("expected number");
	E.Path = TEXT("items[3].level");
	E.Url = TEXT("https://api.test/x?token=***");
	TestEqualSensitive(TEXT("Describe"), E.Describe(), FString(TEXT("Decode: expected number at items[3].level (https://api.test/x?token=***)")));
	E.Kind = EVaRestErrorKind::Http;
	E.HttpStatus = 503;
	E.Code = TEXT("E503");
	E.Path.Reset();
	E.Url.Reset();
	TestEqualSensitive(TEXT("Describe with status and code"), E.Describe(), FString(TEXT("Http 503 [E503]: expected number")));

	using VaRest::Http::IsJsonContentType;
	TestTrue(TEXT("application/json"), IsJsonContentType(TEXT("application/json")));
	TestTrue(TEXT("case and parameters ignored"), IsJsonContentType(TEXT(" Application/JSON ; charset=utf-8")));
	TestTrue(TEXT("+json (RFC 6839)"), IsJsonContentType(TEXT("application/problem+json")));
	TestFalse(TEXT("text/plain is not JSON"), IsJsonContentType(TEXT("text/plain")));
	TestFalse(TEXT("text/json is not a registered JSON type"), IsJsonContentType(TEXT("text/json")));
	TestFalse(TEXT("empty is unknown, not JSON"), IsJsonContentType(TEXT("")));
	TestTrue(TEXT("2xx"), VaRest::Http::IsSuccessStatus(204) && !VaRest::Http::IsSuccessStatus(304) && !VaRest::Http::IsSuccessStatus(199));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSha256Test, "VaRest.Http.Sha256.Vectors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSha256Test::RunTest(const FString& Parameters)
{
	using VaRest::Http::FSha256;
	auto Hex = [](const char* Text) -> FString
	{
		return FSha256::Hash(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text))).ToHex();
	};
	// FIPS 180-2 appendix B and the NIST short-message vectors; the 56-byte one fills the first block past the padding point
	TestEqualSensitive(TEXT("empty"), Hex(""), FString(TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")));
	TestEqualSensitive(TEXT("abc"), Hex("abc"), FString(TEXT("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")));
	TestEqualSensitive(TEXT("448 bits"), Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
		FString(TEXT("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1")));
	TestEqualSensitive(TEXT("896 bits"), Hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
		FString(TEXT("cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1")));

	TArray<uint8> Million;
	Million.Init('a', 1000000);
	TestEqualSensitive(TEXT("a million 'a'"), FSha256::Hash(Million).ToHex(), FString(TEXT("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0")));

	// Incremental updates in uneven pieces give the one-shot digest
	FSha256 Pieces;
	int64 Done = 0;
	int32 Step = 1;
	while (Done < Million.Num())
	{
		const int64 Take = FMath::Min<int64>(Step, Million.Num() - Done);
		Pieces.Update(Million.GetData() + Done, Take);
		Done += Take;
		Step = Step * 7 % 97 + 1;
	}
	TestEqualSensitive(TEXT("incremental"), Pieces.Finalize().ToHex(), FString(TEXT("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0")));

	// Padding boundaries (FIPS 180-4 5.1.1): 55 bytes is the longest message whose padding fits the first block, 56 and 57
	// need a second, 64 fills a block exactly, 119/120 are the same edge one block later. Digests from Python hashlib.
	struct FBoundary { int32 Length; const TCHAR* Digest; };
	const FBoundary Boundaries[] = {
		{ 55, TEXT("9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318") },
		{ 56, TEXT("b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a") },
		{ 57, TEXT("f13b2d724659eb3bf47f2dd6af1accc87b81f09f59f2b75e5c0bed6589dfe8c6") },
		{ 63, TEXT("7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34") },
		{ 64, TEXT("ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb") },
		{ 65, TEXT("635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0") },
		{ 119, TEXT("31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb") },
		{ 120, TEXT("2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c") },
	};
	for (const FBoundary& Case : Boundaries)
	{
		TArray<uint8> Input;
		Input.Init('a', Case.Length);
		TestEqualSensitive(*FString::Printf(TEXT("%d bytes of 'a'"), Case.Length), FSha256::Hash(Input).ToHex(), FString(Case.Digest));
	}

	// Every split of the 119-byte message into two Update calls lands the buffer at a different fill level
	TArray<uint8> Edge;
	Edge.Init('a', 119);
	for (int32 Split = 0; Split <= 119; ++Split)
	{
		FSha256 Halves;
		Halves.Update(Edge.GetData(), Split);
		Halves.Update(Edge.GetData() + Split, 119 - Split);
		TestEqualSensitive(*FString::Printf(TEXT("119 bytes split at %d"), Split), Halves.Finalize().ToHex(),
			FString(TEXT("31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb")));
	}
	return true;
}

#endif
