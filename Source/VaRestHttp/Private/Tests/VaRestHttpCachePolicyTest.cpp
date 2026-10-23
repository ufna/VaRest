// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestCache.h"
#include "VaRestCachePolicy.h"
#include "VaRestIdentity.h"

#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpCachePolicyDirectivesTest, "VaRest.Http.CachePolicy.Directives",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpCachePolicyDirectivesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	FVaRestHeaders Headers;
	Headers.Set(TEXT("Cache-Control"), TEXT("private, max-age=60, no-cache"));
	const FDateTime Utc(2026, 10, 23, 12, 0, 0);
	auto Meta = ReadCacheMetadata(Headers, 300, 100, Utc);
	TestTrue(TEXT("no-cache stores but never fresh"), !Meta.bNoStore && Meta.bNoCache && !Meta.IsFresh(100));
	Headers.Set(TEXT("Cache-Control"), TEXT("public, MAX-AGE=60"));
	Headers.Set(TEXT("Age"), TEXT("10"));
	Meta = ReadCacheMetadata(Headers, 300, 100, Utc);
	TestTrue(TEXT("remaining lifetime"), Meta.IsFresh(149.999));
	TestFalse(TEXT("boundary expires"), Meta.IsFresh(150));
	Headers.Set(TEXT("Cache-Control"), TEXT("max-age=60, max-age=70"));
	TestFalse(TEXT("conflicting ages require validation"), ReadCacheMetadata(Headers, 300, 100, Utc).IsFresh(100));
	Headers.Set(TEXT("Cache-Control"), TEXT("no-store, max-age=3600"));
	TestTrue(TEXT("no-store wins"), ReadCacheMetadata(Headers, 300, 100, Utc).bNoStore);
	double Delta = -1;
	TestFalse(TEXT("overflow is not immortal"), ReadDeltaSeconds(TEXT("9999999999999999999999999999"), Delta));
	TestFalse(TEXT("negative invalid"), ReadDeltaSeconds(TEXT("-1"), Delta));
	Headers.Set(TEXT("Vary"), TEXT("*"));
	TestFalse(TEXT("wildcard uncacheable"), CacheHeadersEligible(FVaRestHeaders(), Headers));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpCachePolicyDefaultsTest, "VaRest.Http.CachePolicy.Defaults",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpCachePolicyDefaultsTest::RunTest(const FString& Parameters)
{
	const FVaRestCachePolicy Disabled;
	TestTrue(TEXT("disabled by default"), Disabled.Mode == EVaRestCacheMode::None);
	TestEqual(TEXT("8 MiB memory"), Disabled.MemoryBytes, int64(8 * 1024 * 1024));
	TestEqual(TEXT("64 MiB disk"), Disabled.DiskBytes, int64(64 * 1024 * 1024));
	TestEqual(TEXT("no fallback TTL"), Disabled.DefaultTtlSeconds, 0.0);
	TestFalse(TEXT("stale fallback opt-in"), Disabled.bServeStaleOnError);
	TestTrue(TEXT("memory factory"), FVaRestCachePolicy::Memory().Mode == EVaRestCacheMode::Memory);
	TestTrue(TEXT("disk factory"), FVaRestCachePolicy::MemoryAndDisk().Mode == EVaRestCacheMode::MemoryAndDisk);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpCachePolicyNumericTest, "VaRest.Http.CachePolicy.Numeric",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpCachePolicyNumericTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	struct FRow
	{
		const TCHAR* Text;
		bool bValid;
		double Seconds;
	};
	const FRow Rows[] = {
		{TEXT("0"), true, 0}, {TEXT("00060"), true, 60}, {TEXT("315360000"), true, 315360000},
		{TEXT("315360001"), false, 0}, {TEXT("9999999999999999999999999999"), false, 0},
		{TEXT(""), false, 0}, {TEXT("-1"), false, 0}, {TEXT("+1"), false, 0},
		{TEXT("1.0"), false, 0}, {TEXT("1e2"), false, 0}, {TEXT(" 1"), false, 0},
		{TEXT("1 "), false, 0}, {TEXT("1x"), false, 0}, {TEXT("\"1\""), false, 0},
		{TEXT("\u0661"), false, 0}};
	for (const FRow& Row : Rows)
	{
		double Seconds = -123;
		const FString Label = FString::Printf(TEXT("delta [%s]"), Row.Text);
		TestEqual(*Label, ReadDeltaSeconds(Row.Text, Seconds), Row.bValid);
		TestEqual(*(Label + TEXT(" value/failure preserves output")), Seconds, Row.bValid ? Row.Seconds : -123.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpCachePolicyFreshnessTest, "VaRest.Http.CachePolicy.Freshness",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpCachePolicyFreshnessTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	const FDateTime Utc(2026, 10, 23, 12, 0, 0);
	struct FRow
	{
		const TCHAR* Label;
		const TCHAR* Control;
		const TCHAR* Date;
		const TCHAR* Expires;
		const TCHAR* Age;
		double DefaultTtl;
		double Lifetime;
		double InitialAge;
	};
	const FRow Rows[] = {
		{TEXT("empty zero default"), nullptr, nullptr, nullptr, nullptr, 0, 0, 0},
		{TEXT("five second default"), TEXT(""), nullptr, nullptr, nullptr, 5, 5, 0},
		{TEXT("negative default clamps"), nullptr, nullptr, nullptr, nullptr, -5, 0, 0},
		{TEXT("large default clamps"), nullptr, nullptr, nullptr, nullptr, 1e12, 315360000, 0},
		{TEXT("NaN default"), nullptr, nullptr, nullptr, nullptr, std::numeric_limits<double>::quiet_NaN(), 0, 0},
		{TEXT("infinite default"), nullptr, nullptr, nullptr, nullptr, std::numeric_limits<double>::infinity(), 0, 0},
		{TEXT("expires and Age leave fifteen"), nullptr, TEXT("Fri, 23 Oct 2026 12:00:00 GMT"), TEXT("Fri, 23 Oct 2026 12:00:20 GMT"), TEXT("5"), 99, 20, 5},
		{TEXT("future Date has zero apparent age"), TEXT("max-age=20"), TEXT("Fri, 23 Oct 2026 12:01:00 GMT"), nullptr, nullptr, 99, 20, 0},
		{TEXT("malformed Date uses receive UTC"), nullptr, TEXT("broken"), TEXT("Fri, 23 Oct 2026 12:00:20 GMT"), nullptr, 99, 20, 0},
		{TEXT("absent Date uses receive UTC"), nullptr, nullptr, TEXT("Fri, 23 Oct 2026 12:00:20 GMT"), nullptr, 99, 20, 0},
		{TEXT("bad Expires uses default"), nullptr, TEXT("broken"), TEXT("broken"), nullptr, 5, 5, 0},
		{TEXT("past Expires zero lifetime"), nullptr, nullptr, TEXT("Fri, 23 Oct 2026 11:59:59 GMT"), nullptr, 99, 0, 0},
		{TEXT("max-age precedes expires"), TEXT("max-age=10"), nullptr, TEXT("Fri, 23 Oct 2026 12:00:20 GMT"), nullptr, 99, 10, 0},
		{TEXT("apparent age dominates Age"), TEXT("max-age=20"), TEXT("Fri, 23 Oct 2026 11:59:50 GMT"), nullptr, TEXT("5"), 99, 20, 10},
		{TEXT("Age dominates apparent age"), TEXT("max-age=20"), TEXT("Fri, 23 Oct 2026 11:59:55 GMT"), nullptr, TEXT("10"), 99, 20, 10},
		{TEXT("Age equals lifetime is stale"), TEXT("max-age=20"), nullptr, nullptr, TEXT("20"), 99, 20, 20},
		{TEXT("invalid Age stale"), TEXT("max-age=20"), nullptr, nullptr, TEXT("-1"), 99, 0, 0},
		{TEXT("folded Age is invalid"), TEXT("max-age=20"), nullptr, nullptr, TEXT("1, 2"), 99, 0, 0},
		{TEXT("empty Age invalid"), TEXT("max-age=20"), nullptr, nullptr, TEXT(""), 99, 0, 0},
		{TEXT("overflow Age stale"), TEXT("max-age=20"), nullptr, nullptr, TEXT("315360001"), 99, 0, 0},
		{TEXT("OWS Age"), TEXT("max-age=20"), nullptr, nullptr, TEXT(" \t5\t "), 99, 20, 5}};
	for (const FRow& Row : Rows)
	{
		FVaRestHeaders Headers;
		if (Row.Control)
		{
			Headers.Add(TEXT("Cache-Control"), Row.Control);
		}
		if (Row.Date)
		{
			Headers.Add(TEXT("Date"), Row.Date);
		}
		if (Row.Expires)
		{
			Headers.Add(TEXT("Expires"), Row.Expires);
		}
		if (Row.Age)
		{
			Headers.Add(TEXT("Age"), Row.Age);
		}
		const auto Meta = ReadCacheMetadata(Headers, Row.DefaultTtl, 100, Utc);
		const FString Label(Row.Label);
		TestEqual(*(Label + TEXT(" lifetime")), Meta.Lifetime, Row.Lifetime);
		TestEqual(*(Label + TEXT(" initial age")), Meta.InitialAge, Row.InitialAge);
		TestEqual(*(Label + TEXT(" stored monotonic")), Meta.StoredAt, 100.0);
		TestTrue(*(Label + TEXT(" stored UTC")), Meta.StoredUtc == Utc);
		if (Row.Lifetime > Row.InitialAge)
		{
			const double Boundary = 100 + Row.Lifetime - Row.InitialAge;
			TestTrue(*(Label + TEXT(" before expiry")), Meta.IsFresh(Boundary - 0.001));
			TestFalse(*(Label + TEXT(" equality expiry stale")), Meta.IsFresh(Boundary));
			TestFalse(*(Label + TEXT(" after expiry")), Meta.IsFresh(Boundary + 0.001));
			TestTrue(*(Label + TEXT(" future stored timestamp cannot add age")), Meta.IsFresh(99));
		}
		else
		{
			TestFalse(*(Label + TEXT(" immediately stale")), Meta.IsFresh(100));
		}
	}
	struct FControlRow
	{
		const TCHAR* Text;
		double Lifetime;
	};
	const FControlRow Controls[] = {
		{TEXT("PUBLIC, MAX-AGE = \"60\""), 60}, {TEXT("max-age=00060,max-age=60"), 60},
		{TEXT("max-age=60, max-age=70"), 0}, {TEXT("max-age=70,max-age=60"), 0},
		{TEXT("max-age=60,max-age=bad,max-age=60"), 0}, {TEXT("max-age=bad,max-age=60"), 0},
		{TEXT("max-age"), 0}, {TEXT("max-age="), 0}, {TEXT("max-age=-1"), 0},
		{TEXT("max-age=\"60"), 0}, {TEXT("max-age=60\""), 0}, {TEXT("max-age=\"\""), 0},
		{TEXT("max-age=\" 60 \""), 0}, {TEXT("max-age=\"60\"tail"), 0}, {TEXT("max-age=1.5"), 0},
		{TEXT("max-age=315360001"), 0}, {TEXT("max-age=315360000"), 315360000},
		{TEXT("private, public, s-maxage=999"), 5}, {TEXT(""), 5}};
	for (const FControlRow& Row : Controls)
	{
		FVaRestHeaders Headers;
		Headers.Set(TEXT("Cache-Control"), Row.Text);
		Headers.Set(TEXT("Expires"), TEXT("Fri, 23 Oct 2026 12:00:05 GMT"));
		TestEqual(*FString::Printf(TEXT("directive [%s] lifetime"), Row.Text), ReadCacheMetadata(Headers, 5, 100, Utc).Lifetime, Row.Lifetime);
	}
	FVaRestHeaders Repeated;
	Repeated.Add(TEXT("cache-control"), TEXT("max-age=60"));
	Repeated.Add(TEXT("CACHE-CONTROL"), TEXT("MAX-AGE=\"60\", MUST-REVALIDATE"));
	Repeated.Add(TEXT("Age"), TEXT("5"));
	Repeated.Add(TEXT("aGe"), TEXT("10"));
	auto Meta = ReadCacheMetadata(Repeated, 99, 100, Utc);
	TestEqual(TEXT("every control line parsed"), Meta.Lifetime, 60.0);
	TestEqual(TEXT("largest repeated Age"), Meta.InitialAge, 10.0);
	TestTrue(TEXT("must-revalidate retains fresh entries"), Meta.IsFresh(100));
	TestFalse(TEXT("must-revalidate denies stale"), Meta.CanServeStale());
	Repeated.Add(TEXT("Cache-Control"), TEXT("max-age=70"));
	TestEqual(TEXT("conflict across header lines"), ReadCacheMetadata(Repeated, 99, 100, Utc).Lifetime, 0.0);
	Repeated.Set(TEXT("Cache-Control"), TEXT("max-age=60"));
	Repeated.Add(TEXT("Age"), TEXT("bad"));
	TestFalse(TEXT("invalid later Age forces stale"), ReadCacheMetadata(Repeated, 99, 100, Utc).IsFresh(100));
	Repeated.Set(TEXT("Age"), TEXT("bad"));
	Repeated.Add(TEXT("Age"), TEXT("5"));
	TestFalse(TEXT("later valid Age cannot undo invalid"), ReadCacheMetadata(Repeated, 99, 100, Utc).IsFresh(100));
	Repeated.Set(TEXT("Age"), TEXT("0"));
	Repeated.Set(TEXT("Cache-Control"), TEXT("max-age=60, no-cache"));
	Repeated.Set(TEXT("ETag"), TEXT("\"v1\""));
	Repeated.Set(TEXT("Last-Modified"), TEXT("Fri, 23 Oct 2026 11:00:00 GMT"));
	Meta = ReadCacheMetadata(Repeated, 99, 100, Utc);
	TestTrue(TEXT("no-cache with validator stored"), !Meta.bNoStore && Meta.bNoCache);
	TestFalse(TEXT("no-cache never fresh"), Meta.IsFresh(100));
	TestFalse(TEXT("no-cache denies stale"), Meta.CanServeStale());
	TestEqual(TEXT("ETag captured"), Meta.ETag, Repeated.Get(TEXT("ETag")));
	TestEqual(TEXT("Last-Modified captured"), Meta.LastModified, Repeated.Get(TEXT("Last-Modified")));
	Repeated.Set(TEXT("Cache-Control"), TEXT("max-age=60, no-store"));
	Meta = ReadCacheMetadata(Repeated, 99, 100, Utc);
	TestTrue(TEXT("no-store flag"), Meta.bNoStore);
	TestFalse(TEXT("no-store cannot be fresh"), Meta.IsFresh(100));
	TestFalse(TEXT("no-store cannot serve stale"), Meta.CanServeStale());
	Repeated.Set(TEXT("Cache-Control"), TEXT("max-age=60"));
	TestTrue(TEXT("ordinary entry may serve stale"), ReadCacheMetadata(Repeated, 99, 100, Utc).CanServeStale());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpCachePolicyEligibilityTest, "VaRest.Http.CachePolicy.Eligibility",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpCachePolicyEligibilityTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	TestTrue(TEXT("plain eligible"), CacheHeadersEligible({}, {}));
	for (const TCHAR* Name : {TEXT("Range"), TEXT("if-range"), TEXT("IF-NONE-MATCH"), TEXT("If-Modified-Since")})
	{
		FVaRestHeaders Request;
		Request.Add(Name, TEXT(""));
		TestFalse(*FString::Printf(TEXT("request %s bypasses even empty"), Name), CacheHeadersEligible(Request, {}));
	}
	FVaRestHeaders Request;
	Request.Add(TEXT("Cache-Control"), TEXT("public"));
	Request.Add(TEXT("cache-control"), TEXT(" \tNO-STORE = 1"));
	TestFalse(TEXT("request no-store on later line"), CacheHeadersEligible(Request, {}));
	Request.Set(TEXT("Cache-Control"), TEXT("no-cache, max-age=0"));
	TestTrue(TEXT("request validation directives are stage concern"), CacheHeadersEligible(Request, {}));
	Request.Set(TEXT("Accept-Language"), TEXT("en"));
	Request.Set(TEXT("Accept"), TEXT(""));
	for (const TCHAR* Value : {TEXT("*"), TEXT("Accept Language"), TEXT("Unknown-Transport-Default"), TEXT("Accept,"), TEXT("")})
	{
		FVaRestHeaders Response;
		Response.Add(TEXT("Vary"), Value);
		TestFalse(*FString::Printf(TEXT("Vary [%s] ineligible"), Value), CacheHeadersEligible(Request, Response));
	}
	FVaRestHeaders Response;
	Response.Add(TEXT("VaRy"), TEXT(" AcCePt-LaNgUaGe, ACCEPT "));
	Response.Add(TEXT("vary"), TEXT("accept-language"));
	TestTrue(TEXT("all known Vary eligible"), CacheHeadersEligible(Request, Response));
	const auto Meta = ReadCacheMetadata(Response, 60, 100, FDateTime(2026, 10, 23));
	TestEqual(TEXT("lowercase unique Vary"), Meta.Vary.Num(), 2);
	TestTrue(TEXT("lowercase language"), Meta.Vary.Contains(TEXT("accept-language")));
	TestTrue(TEXT("lowercase accept"), Meta.Vary.Contains(TEXT("accept")));
	Response.Add(TEXT("VARY"), TEXT("*"));
	TestFalse(TEXT("wildcard on later line"), CacheHeadersEligible(Request, Response));
	TestTrue(TEXT("wildcard metadata no-store"), ReadCacheMetadata(Response, 60, 100, FDateTime(2026, 10, 23)).bNoStore);
	Response.Set(TEXT("Vary"), TEXT("invalid name"));
	TestTrue(TEXT("invalid metadata no-store"), ReadCacheMetadata(Response, 60, 100, FDateTime(2026, 10, 23)).bNoStore);
	for (const TCHAR* Name : {TEXT("set-cookie"), TEXT("WWW-AUTHENTICATE")})
	{
		FVaRestHeaders Forbidden;
		Forbidden.Add(Name, TEXT(""));
		TestFalse(*FString::Printf(TEXT("response %s ineligible"), Name), CacheHeadersEligible({}, Forbidden));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpCachePolicyVariationTest, "VaRest.Http.CachePolicy.Variation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpCachePolicyVariationTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	FVaRestHeaders English;
	English.Add(TEXT("Accept-Language"), TEXT("en"));
	English.Add(TEXT("Accept"), TEXT("application/json"));
	FVaRestHeaders French;
	French.Add(TEXT("accept-language"), TEXT("fr"));
	French.Add(TEXT("accept"), TEXT("application/json"));
	FVaRestHeaders Vary;
	Vary.Add(TEXT("Vary"), TEXT("AcCePt-LaNgUaGe"));
	TestTrue(TEXT("mixed case Vary eligible"), CacheHeadersEligible(English, Vary));
	const FString Url(TEXT("https://example.test/items?a=1&b=2"));
	const FString Key = CacheKey(TEXT("GET"), Url, TEXT("anonymous"), English, {});
	TestTrue(TEXT("Vary language has separate Task1 key"), Key != CacheKey(TEXT("GET"), Url, TEXT("anonymous"), French, {}));
	FVaRestHeaders Reordered;
	Reordered.Add(TEXT("accept"), TEXT("application/json"));
	Reordered.Add(TEXT("ACCEPT-LANGUAGE"), TEXT("en"));
	TestEqual(TEXT("insertion order and names canonical"), Key, CacheKey(TEXT("GET"), Url, TEXT("anonymous"), Reordered, {}));
	TestEqual(TEXT("fragment ignored"), Key, CacheKey(TEXT("GET"), Url + TEXT("#fragment"), TEXT("anonymous"), English, {}));
	TestTrue(TEXT("query order preserved"), Key != CacheKey(TEXT("GET"), TEXT("https://example.test/items?b=2&a=1"), TEXT("anonymous"), English, {}));
	TestTrue(TEXT("auth A/B separates same URL"), CacheKey(TEXT("GET"), Url, TEXT("auth-A"), English, {}) != CacheKey(TEXT("GET"), Url, TEXT("auth-B"), English, {}));
	FVaRestHeaders ExplicitAuthA;
	ExplicitAuthA.Set(TEXT("Authorization"), TEXT("Bearer A"));
	FVaRestHeaders ExplicitAuthB;
	ExplicitAuthB.Set(TEXT("Authorization"), TEXT("Bearer B"));
	TestTrue(TEXT("explicit auth varies anonymous key"), CacheKey(TEXT("GET"), Url, TEXT("anonymous"), ExplicitAuthA, {}) != CacheKey(TEXT("GET"), Url, TEXT("anonymous"), ExplicitAuthB, {}));
	TestTrue(TEXT("HEAD distinct from GET"), Key != CacheKey(TEXT("HEAD"), Url, TEXT("anonymous"), English, {}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpCachePolicyMergeTest, "VaRest.Http.CachePolicy.MergeNotModified",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpCachePolicyMergeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	FVaRestHeaders HeadStored;
	HeadStored.Set(TEXT("Content-Length"), TEXT("7123"));
	HeadStored.Set(TEXT("ETag"), TEXT("\"head-v1\""));
	FVaRestHeaders NotModified;
	NotModified.Set(TEXT("Cache-Control"), TEXT("max-age=60"));
	const auto HeadMerged = MergeNotModified(HeadStored, NotModified, 0, true);
	TestEqual(TEXT("HEAD describes GET representation length"), HeadMerged.Get(TEXT("Content-Length")), FString(TEXT("7123")));
	const auto GetMerged = MergeNotModified(HeadStored, NotModified, 3, false);
	TestEqual(TEXT("GET retains actual stored body length"), GetMerged.Get(TEXT("Content-Length")), FString(TEXT("3")));
	TestEqual(TEXT("304 retains ETag unless supplied"), GetMerged.Get(TEXT("ETag")), FString(TEXT("\"head-v1\"")));
	NotModified.Set(TEXT("Content-Length"), TEXT("9999"));
	TestEqual(TEXT("fresh HEAD length cannot replace stored representation"), MergeNotModified(HeadStored, NotModified, 0, true).Get(TEXT("Content-Length")), FString(TEXT("7123")));
	TestFalse(TEXT("HEAD without stored length cannot invent empty-body length"), MergeNotModified({}, NotModified, 0, true).Contains(TEXT("Content-Length")));

	FVaRestHeaders Stored;
	Stored.Add(TEXT("Cache-Control"), TEXT("max-age=0"));
	Stored.Add(TEXT("CACHE-CONTROL"), TEXT("private"));
	Stored.Add(TEXT("ETag"), TEXT("\"v1\""));
	Stored.Add(TEXT("Last-Modified"), TEXT("Fri, 23 Oct 2026 11:00:00 GMT"));
	Stored.Add(TEXT("Content-Length"), TEXT("999"));
	Stored.Add(TEXT("Warning"), TEXT(" 110 stale"));
	Stored.Add(TEXT("Warning"), TEXT("199 misc"));
	Stored.Add(TEXT("Warning"), TEXT("299 keep"));
	Stored.Add(TEXT("X-Retained"), TEXT("original"));
	Stored.Add(TEXT("Connection"), TEXT("X-Old-Hop"));
	Stored.Add(TEXT("X-New-Hop"), TEXT("stored nominated by fresh"));
	FVaRestHeaders Fresh;
	Fresh.Add(TEXT("cache-control"), TEXT("max-age=60"));
	Fresh.Add(TEXT("Cache-Control"), TEXT("must-revalidate"));
	Fresh.Add(TEXT("etag"), TEXT("\"v2\""));
	Fresh.Add(TEXT("Content-Length"), TEXT("0"));
	Fresh.Add(TEXT("connection"), TEXT(" x-new-hop "));
	Fresh.Add(TEXT("X-Old-Hop"), TEXT("fresh nominated by stored"));
	for (const TCHAR* Name : {TEXT("Set-Cookie"), TEXT("Authorization"), TEXT("X-Session"), TEXT("X-Api-Key"),
			 TEXT("Keep-Alive"), TEXT("Proxy-Authenticate"), TEXT("Proxy-Authorization"), TEXT("TE"),
			 TEXT("Trailer"), TEXT("Transfer-Encoding"), TEXT("Upgrade")})
	{
		Stored.Add(Name, TEXT("stored forbidden"));
		Fresh.Add(Name, TEXT("fresh forbidden"));
	}
	const TArray<uint8> Body = {0, 1, 255};
	const auto Merged = MergeNotModified(Stored, Fresh, Body.Num(), false);
	TestEqual(TEXT("GET stale and wire lengths replaced"), Merged.Get(TEXT("Content-Length")), FString(TEXT("3")));
	TestEqual(TEXT("fresh ETag replaces stored"), Merged.Get(TEXT("ETag")), FString(TEXT("\"v2\"")));
	TestEqual(TEXT("Last-Modified retained"), Merged.Get(TEXT("Last-Modified")), Stored.Get(TEXT("Last-Modified")));
	TestEqual(TEXT("other metadata retained"), Merged.Get(TEXT("X-Retained")), FString(TEXT("original")));
	TArray<FString> Controls;
	TArray<FString> Warnings;
	for (const auto& Pair : Merged)
	{
		if (Pair.Key.Equals(TEXT("Cache-Control"), ESearchCase::IgnoreCase))
		{
			Controls.Add(Pair.Value);
		}
		if (Pair.Key.Equals(TEXT("Warning"), ESearchCase::IgnoreCase))
		{
			Warnings.Add(Pair.Value);
		}
	}
	if (TestEqual(TEXT("both fresh control occurrences retained"), Controls.Num(), 2))
	{
		TestEqual(TEXT("first replacement control"), Controls[0], FString(TEXT("max-age=60")));
		TestEqual(TEXT("second replacement control"), Controls[1], FString(TEXT("must-revalidate")));
	}
	if (TestEqual(TEXT("only nonstale Warning retained"), Warnings.Num(), 1))
	{
		TestEqual(TEXT("299 Warning retained"), Warnings[0], FString(TEXT("299 keep")));
	}
	for (const TCHAR* Name : {TEXT("Set-Cookie"), TEXT("Authorization"), TEXT("X-Session"), TEXT("X-Api-Key"),
			 TEXT("Connection"), TEXT("Keep-Alive"), TEXT("Proxy-Authenticate"), TEXT("Proxy-Authorization"), TEXT("TE"),
			 TEXT("Trailer"), TEXT("Transfer-Encoding"), TEXT("Upgrade"), TEXT("X-Old-Hop"), TEXT("X-New-Hop")})
	{
		TestFalse(*FString::Printf(TEXT("merge omits %s from either side"), Name), Merged.Contains(Name));
	}
	TestEqual(TEXT("inputs unchanged stored length"), Stored.Get(TEXT("Content-Length")), FString(TEXT("999")));
	TestEqual(TEXT("inputs unchanged fresh length"), Fresh.Get(TEXT("Content-Length")), FString(TEXT("0")));
	return true;
}

#endif
