// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS
#include "VaRestMemoryCache.h"
#include "Misc/AutomationTest.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestMemoryLruTest, "VaRest.Http.CacheStore.MemoryLru",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestMemoryLruTest::RunTest(const FString& Parameters)
{
using namespace VaRest::Http;
FMemoryCache Cache;
FCacheEntry A; A.Key = TEXT("a"); A.Body = {1,2,3};
FCacheEntry B; B.Key = TEXT("b"); B.Body = {4,5,6};
FCacheEntry C; C.Key = TEXT("c"); C.Body = {7,8,9};
const int64 One = A.Charge();
Cache.Put(A, One * 2);
Cache.Put(B, One * 2);
TestTrue(TEXT("A touch"), Cache.Find(TEXT("a")).IsValid());
Cache.Put(C, One * 2);
TestFalse(TEXT("least recently used B evicted"), Cache.Find(TEXT("b")).IsValid());
const auto Found = Cache.Find(TEXT("a"));
if (!TestTrue(TEXT("A survives"), Found.IsValid())) return false;
auto Response = Found->Answer();
if (!TestEqual(TEXT("answer length"),Response.Body.Num(),3)) return false;
Response.Body[0] = 99;
TestTrue(TEXT("cache provenance"),Response.bFromCache);
TestEqual(TEXT("no network download"),Response.BytesDown,int64(0));
TestEqual(TEXT("answer bytes are independent"), Found->Body[0], static_cast<uint8>(1));
TestTrue(TEXT("charge bounded"), Cache.Bytes() <= One * 2);
Cache.Clear();
TestEqual(TEXT("clear count"), Cache.Num(), 0);
TestEqual(TEXT("clear charge"), Cache.Bytes(), static_cast<int64>(0));
FCacheEntry Empty; Empty.Key=TEXT("empty");
for (int32 Index=0;Index<4100;++Index) { Empty.Key=FString::FromInt(Index); Cache.Put(Empty,MAX_int64); }
TestEqual(TEXT("empty body count bound"),Cache.Num(),4096);
Cache.Remove(TEXT("4099"));
TestEqual(TEXT("remove count"),Cache.Num(),4095);
Cache.Clear();
TestEqual(TEXT("empty charge cleared"),Cache.Bytes(),int64(0));
return true;
}
#endif
