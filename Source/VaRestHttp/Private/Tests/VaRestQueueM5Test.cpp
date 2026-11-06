// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "VaRestJson.h"
#include "VaRestQueueStore.h"

namespace VaRest::Http::QueueM5Test
{
// Literal v1 wire fixtures pinned BEFORE changing EncodeQueueRecord to v2.
// Derived from the original v1 schema/source; native execution remains UNRUN.
struct FPin
{
	EVaRestBodyKind Kind;
	const UTF8CHAR* Text;
};
const FPin V1[] = {
	{EVaRestBodyKind::None, UTF8TEXT(R"json({"version":1,"queueId":"00000000000000000000000000000001","sequence":"1","runId":"00000000000000000000000000000002","originalRequestId":"12","createdUtc":"2026-10-23T12:00:00.000Z","scope":"0000000000000000000000000000000000000000000000000000000000000000","verb":"GET","url":"https://api.test/items","authMode":2,"idempotent":false,"idempotencyKey":"","tag":"","bodyKind":0,"contentType":"","body":"","bodySha256":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","headers":[]})json")},
	{EVaRestBodyKind::Json, UTF8TEXT(R"json({"version":1,"queueId":"00000000000000000000000000000001","sequence":"1","runId":"00000000000000000000000000000002","originalRequestId":"12","createdUtc":"2026-10-23T12:00:00.000Z","scope":"0000000000000000000000000000000000000000000000000000000000000000","verb":"GET","url":"https://api.test/items","authMode":2,"idempotent":false,"idempotencyKey":"","tag":"","bodyKind":1,"contentType":"application/json","body":"e30=","bodySha256":"44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a","headers":[["Content-Type","application/json"]]})json")},
	{EVaRestBodyKind::Text, UTF8TEXT(R"json({"version":1,"queueId":"00000000000000000000000000000001","sequence":"1","runId":"00000000000000000000000000000002","originalRequestId":"12","createdUtc":"2026-10-23T12:00:00.000Z","scope":"0000000000000000000000000000000000000000000000000000000000000000","verb":"GET","url":"https://api.test/items","authMode":2,"idempotent":false,"idempotencyKey":"","tag":"","bodyKind":2,"contentType":"text/plain","body":"eA==","bodySha256":"2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881","headers":[["Content-Type","text/plain"]]})json")},
	{EVaRestBodyKind::Form, UTF8TEXT(R"json({"version":1,"queueId":"00000000000000000000000000000001","sequence":"1","runId":"00000000000000000000000000000002","originalRequestId":"12","createdUtc":"2026-10-23T12:00:00.000Z","scope":"0000000000000000000000000000000000000000000000000000000000000000","verb":"GET","url":"https://api.test/items","authMode":2,"idempotent":false,"idempotencyKey":"","tag":"","bodyKind":3,"contentType":"application/x-www-form-urlencoded","body":"az12","bodySha256":"9246d2c0e0f213ae2b86ac78a432a55edfd31d07a072331d58763c08d5292212","headers":[["Content-Type","application/x-www-form-urlencoded"]]})json")},
	{EVaRestBodyKind::Multipart, UTF8TEXT(R"json({"version":1,"queueId":"00000000000000000000000000000001","sequence":"1","runId":"00000000000000000000000000000002","originalRequestId":"12","createdUtc":"2026-10-23T12:00:00.000Z","scope":"0000000000000000000000000000000000000000000000000000000000000000","verb":"GET","url":"https://api.test/items","authMode":2,"idempotent":false,"idempotencyKey":"","tag":"","bodyKind":4,"contentType":"multipart/form-data; boundary=bound","body":"LS1ib3VuZA0KQ29udGVudC1EaXNwb3NpdGlvbjogZm9ybS1kYXRhOyBuYW1lPSJrIg0KDQp2DQotLWJvdW5kLS0NCg==","bodySha256":"c552792ebd4a382c27d7a1640d8a06dceee57f008244afb4b676f18419354e08","headers":[["Content-Type","multipart/form-data; boundary=bound"]]})json")},
	{EVaRestBodyKind::Binary, UTF8TEXT(R"json({"version":1,"queueId":"00000000000000000000000000000001","sequence":"1","runId":"00000000000000000000000000000002","originalRequestId":"12","createdUtc":"2026-10-23T12:00:00.000Z","scope":"0000000000000000000000000000000000000000000000000000000000000000","verb":"GET","url":"https://api.test/items","authMode":2,"idempotent":false,"idempotencyKey":"","tag":"","bodyKind":5,"contentType":"application/octet-stream","body":"AP8B","bodySha256":"47ffa3ea45a70b8a41c2c0825df323c00a8b7a01c1ea06083cc41dddcc001123","headers":[["Content-Type","application/octet-stream"]]})json")},
};
static_assert(static_cast<uint8>(EVaRestBodyKind::None) == 0 && static_cast<uint8>(EVaRestBodyKind::Json) == 1 && static_cast<uint8>(EVaRestBodyKind::Text) == 2 && static_cast<uint8>(EVaRestBodyKind::Form) == 3 && static_cast<uint8>(EVaRestBodyKind::Multipart) == 4 && static_cast<uint8>(EVaRestBodyKind::Binary) == 5,
	"Queue v1 body-kind numeric values are persistent wire pins");
} // namespace VaRest::Http::QueueM5Test

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestQueueM5V1PinsTest, "VaRest.Http.QueueStore.M5V1Pins",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestQueueM5V1PinsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	for (const auto& Pin : QueueM5Test::V1)
	{
		const FUtf8StringView Wire(Pin.Text);
		const auto Read = DecodeQueueRecord(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Wire.GetData()), Wire.Len()),
			TEXT("https://api.test"), FVaRestOfflinePolicy());
		if (!TestTrue(TEXT("literal old v1 record remains readable"), Read.HasValue()))
			return false;
		TestEqual(TEXT("v1 native body kind keeps its numeric meaning"), Read.GetValue().Body.Kind, Pin.Kind);
		TestTrue(TEXT("v1 has no fabricated endpoint"), Read.GetValue().Endpoint.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestQueueM5V2SchemaTest, "VaRest.Http.QueueStore.M5V2Schema",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestQueueM5V2SchemaTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	const FUtf8StringView OldWire(QueueM5Test::V1[0].Text);
	auto Old = DecodeQueueRecord(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(OldWire.GetData()), OldWire.Len()), TEXT("https://api.test"), FVaRestOfflinePolicy());
	if (!TestTrue(TEXT("real legacy record supplies the fixture"), Old.HasValue()))
		return false;
	FQueueItem Item = Old.StealValue();
	Item.Endpoint = TEXT("ListCASE_17");
	const auto Bytes = EncodeQueueRecord(Item);
	auto Parsed = FVaRestJson::Parse(TConstArrayView<uint8>(Bytes));
	if (!TestTrue(TEXT("new record is complete JSON"), Parsed.HasValue()))
		return false;
	const FVaRestJson Original = Parsed.StealValue();
	TestTrue(TEXT("writer emits exact v2 field count"), Original.Num() == 19 && Original[TEXT("version")].AsInt64() == 2);
	auto Read = DecodeQueueRecord(Bytes, TEXT("https://api.test"), FVaRestOfflinePolicy());
	if (!TestTrue(TEXT("v2 endpoint round trip"), Read.HasValue()))
		return false;
	TestEqualSensitive(TEXT("complete case/suffix text survives persistence"), Read.GetValue().Endpoint, Item.Endpoint);
	const auto Reject = [&](FVaRestJson Record, const TCHAR* Label) {
		TArray<uint8> Wire;
		if (!TestTrue(TEXT("negative fixture serializes"), Record.TryToUtf8(Wire).HasValue()))
			return;
		TestTrue(Label, DecodeQueueRecord(Wire, TEXT("https://api.test"), FVaRestOfflinePolicy()).HasError());
	};
	auto Record = Original.Clone();
	Record.Remove(TEXT("endpoint"));
	Reject(Record, TEXT("v2 requires endpoint even when it is empty"));
	Record = Original.Clone();
	Record.Set(TEXT("version"), 1);
	Reject(Record, TEXT("v1 refuses a v2 extra member"));
	Record = Original.Clone();
	Record.Set(TEXT("endpoint"), 7);
	Reject(Record, TEXT("endpoint must be a string"));
	Record = Original.Clone();
	Record.Set(TEXT("extra"), true);
	Reject(Record, TEXT("unknown v2 member is refused"));
	Record = Original.Clone();
	Record.Set(TEXT("endpoint"), FString::ChrN(257, TEXT('x')));
	Reject(Record, TEXT("endpoint over the plain bound is refused"));
	Record = Original.Clone();
	Record.Set(TEXT("endpoint"), TEXT("bad\nendpoint"));
	Reject(Record, TEXT("endpoint controls are refused"));
	TArray<uint8> Duplicate = Bytes;
	Duplicate.Pop(EAllowShrinking::No);
	const FUtf8StringView Suffix(UTF8TEXT(",\"endpoint\":\"again\"}"));
	Duplicate.Append(reinterpret_cast<const uint8*>(Suffix.GetData()), Suffix.Len());
	TestTrue(TEXT("duplicate v2 endpoint cannot replace the captured identity"), DecodeQueueRecord(Duplicate, TEXT("https://api.test"), FVaRestOfflinePolicy()).HasError());
	return true;
}
#endif
