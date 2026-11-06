// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "VaRestJsonConvert.h"
#include "VaRestJsonConvertTestTypes.h"
#include "VaRestJsonKeyHash.h"

namespace VaRestJsonCandidatesTest
{
TArray<uint8> Bytes(const ANSICHAR* Text)
{
 return TArray<uint8>(reinterpret_cast<const uint8*>(Text),FCStringAnsi::Strlen(Text));
}
void Reports(FAutomationTestBase& Test,const FVaRestJsonDecodeReport& Dom,const FVaRestJsonDecodeReport& Stream)
{
 Test.TestEqual(TEXT("same budget refusal"),Stream.bBudgetExceeded,Dom.bBudgetExceeded);
 const auto Compare=[&](const auto& Left,const auto& Right)
 {
  Test.TestEqual(TEXT("same issue count"),Right.Num(),Left.Num());
  for (int32 I=0;I<FMath::Min(Left.Num(),Right.Num());++I)
   Test.TestEqual(TEXT("same issue path and message"),Right[I].ToString(),Left[I].ToString());
 };
 Compare(Dom.Errors,Stream.Errors); Compare(Dom.Warnings,Stream.Warnings);
}
template<class T> bool Decode(FAutomationTestBase& Test,const TArray<uint8>& Input,T& Dom,T& Stream,bool ExpectedStreaming,const FVaRestJsonConvertOptions& Options={})
{
	const TArray<uint8> OriginalInput=Input;
 FVaRestJsonDecodeReport A,B; bool Used=false;
 const bool DomResult=FVaRestJsonConvert::Utf8ToStruct(Input,T::StaticStruct(),&Dom,Options,&A);
 const bool StreamResult=FVaRestJsonConvert::Utf8ToStructStreaming(Input,T::StaticStruct(),&Stream,Options,&B,&Used);
 Test.TestEqual(TEXT("same conversion result"),StreamResult,DomResult);
 Test.TestEqual(TEXT("expected streaming or pre-write fallback path"),Used,ExpectedStreaming);
 Test.TestTrue(TEXT("both parsers preserve caller-owned UTF-8 bytes"),Input==OriginalInput);
 Reports(Test,A,B);
 Test.TestEqual(TEXT("same final native values including partial refusal writes"),
  FVaRestJsonConvert::StructToString(T::StaticStruct(),&Stream),FVaRestJsonConvert::StructToString(T::StaticStruct(),&Dom));
 return StreamResult;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonSipHashVectorsTest,"VaRest.Json.Candidates.SipHash24",
 EAutomationTestFlags_ApplicationContextMask|EAutomationTestFlags::EngineFilter)
bool FVaRestJsonSipHashVectorsTest::RunTest(const FString& Parameters)
{
 // Known-answer vectors: little-endian key bytes 00..0f, message bytes 00..N-1.
 // Covers empty input, every tail size, two complete words and unaligned input.
 const uint64 Expected[]={
  0x726fdb47dd0e0e31ULL,0x74f839c593dc67fdULL,0x0d6c8009d9a94f5aULL,0x85676696d7fb7e2dULL,
  0xcf2794e0277187b7ULL,0x18765564cd99a68dULL,0xcbc9466e58fee3ceULL,0xab0200f58b01d137ULL,
  0x93f5f5799a932462ULL,0x9e0082df0ba9e4b0ULL,0x7a5dbbc594ddb9f3ULL,0xf4b32f46226bada7ULL,
  0x751e8fbc860ee5fbULL,0x14ea5627c0843d90ULL,0xf723ca908e7af2eeULL,0xa129ca6149be45e5ULL,
  0x3f2acc7f57c29bdbULL};
 alignas(8) uint8 Message[18]{};
 for (int32 I=0;I<17;++I) Message[I+1]=uint8(I);
 for (int32 Length=0;Length<UE_ARRAY_COUNT(Expected);++Length)
 {
  const uint64 Actual=VaRest::Json::KeyHash::SipHash24(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Message+1),Length),
   0x0706050403020100ULL,0x0f0e0d0c0b0a0908ULL);
  TestEqual(*FString::Printf(TEXT("SipHash-2-4 vector length %d"),Length),Actual,Expected[Length]);
 }
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonStreamingParityTest,"VaRest.Json.Candidates.StreamingParity",
 EAutomationTestFlags_ApplicationContextMask|EAutomationTestFlags::EngineFilter)
bool FVaRestJsonStreamingParityTest::RunTest(const FString& Parameters)
{
 using namespace VaRestJsonCandidatesTest;
 struct FCase { const ANSICHAR* Text; bool Stream; bool Strict=false; };
 const FCase Cases[]={
  {"{\"id\":7,\"name\":\"text\\u0000tail\"}",true},
  {"{\"name\":\"last\",\"id\":7}",false},
  {"{\"id\":1,\"id\":2,\"name\":\"last\"}",false},
  {"{\"ID\":1,\"id\":2,\"name\":\"last\"}",false},
  {"{\"ignore\":{\"nested\":[1,{\"id\":400}]},\"id\":3,\"name\":\"last\"}",true},
  {"null",true},{"[]",true},
  {"{\"id\":7,\"name\":",false},
  {"{\"id\":7,\"name\":1e309}",false},
  {"{\"id\":7,\"name\":1.7976931348623157e308}",true},
  {"{\"id\":\"7\",\"name\":42}",true,true},
  {"{\"id\":\"7\",\"name\":42}",true,false}};
 for (const auto& Case:Cases)
 {
  FVaRestJsonTestInner Dom,Stream; Dom.Id=Stream.Id=91; Dom.Name=Stream.Name=TEXT("keep");
  FVaRestJsonConvertOptions Options; Options.bStrict=Case.Strict;
  Decode(*this,Bytes(Case.Text),Dom,Stream,Case.Stream,Options);
 }
 FVaRestJsonBenchResponse Source; Source.Ok=true; Source.Data.Count=32;
 for (int32 I=0;I<32;++I)
 {
  auto& Item=Source.Data.Items.AddDefaulted_GetRef(); Item.Id=I; Item.Uid=MAX_int64-I;
  Item.Name=FString::Printf(TEXT("item %d"),I); Item.Desc=TEXT("escaped \"value\" \\ and \nline"); Item.Price=1.25+I;
  Item.Active=true; Item.Owner.Id=I+10; Item.Owner.Nick=TEXT("owner"); Item.Tags={TEXT("one"),TEXT("two")};
  Item.Stats.Add(TEXT("health"),I); Item.Stats.Add(TEXT("score"),I+1);
 }
 const auto Input=FVaRestJsonConvert::ToUtf8(Source);
 FVaRestJsonBenchResponse Dom,Stream;
 TestTrue(TEXT("nested ordinary structs, arrays and scalar maps use SAX with DOM parity"),Decode(*this,Input,Dom,Stream,true));
 TestEqual(TEXT("all nested array elements converted"),Stream.Data.Items.Num(),32);
 // The same map property occurs repeatedly, with empty/mismatched values between healthy entries.
 // A reused scratch entry must not retain keys, values or diagnostics from the previous destination.
 const auto RepeatedMaps=Bytes("{\"data\":{\"items\":["
  "{\"name\":\"first\\u0000suffix\",\"stats\":{\"retained\":71}},"
  "{\"name\":\"second\",\"stats\":{}},"
  "{\"name\":\"third\",\"stats\":{\"bad\":[],\"fresh\":9}},"
  "{\"name\":\"fourth\",\"stats\":{\"retained\":null}},"
  "{\"name\":\"last\",\"stats\":{\"A\":5,\"a\":6,\"nul\\u0000x\":7}}]}}");
 for (bool Strict:{false,true})
 {
  FVaRestJsonBenchResponse MapDom,MapStream;
  FVaRestJsonConvertOptions Options; Options.bStrict=Strict;
  TestEqual(TEXT("repeated map result retains strict/lenient semantics"),
   Decode(*this,RepeatedMaps,MapDom,MapStream,true,Options),!Strict);
  if (!TestEqual(TEXT("all repeated map destinations survive"),MapStream.Data.Items.Num(),5)) continue;
  TestEqual(TEXT("moved string retains its counted null suffix"),MapStream.Data.Items[0].Name.Len(),12);
  TestEqual(TEXT("first map keeps its own value"),MapStream.Data.Items[0].Stats.FindRef(TEXT("retained")),71);
  TestEqual(TEXT("empty next map has no previous entry"),MapStream.Data.Items[1].Stats.Num(),0);
  TestEqual(TEXT("mismatch does not add stale map value"),MapStream.Data.Items[2].Stats.Num(),1);
  TestEqual(TEXT("healthy entry after mismatch is decoded"),MapStream.Data.Items[2].Stats.FindRef(TEXT("fresh")),9);
  TestEqual(TEXT("null mismatch does not reuse an earlier value"),MapStream.Data.Items[3].Stats.Num(),0);
  TestEqual(TEXT("native case-equal keys still keep the last value"),MapStream.Data.Items[4].Stats.FindRef(TEXT("A")),6);
  TestEqual(TEXT("counted null key remains diagnosed before any move"),MapStream.Data.Items[4].Stats.Num(),Strict?1:2);
 }
 FVaRestJsonTestTree RawDom,RawStream;
 TestTrue(TEXT("raw JSON layouts retain the DOM decoder"),Decode(*this,Bytes("{\"children\":[{\"free\":[1,2]}]}"),RawDom,RawStream,false));
 FVaRestJsonTestInner BudgetDom,BudgetStream; BudgetDom.Id=BudgetStream.Id=91;
 FVaRestJsonConvertOptions Budget; Budget.MaxValues=1;
 TestFalse(TEXT("preflight budget refusal retains DOM semantics"),Decode(*this,Bytes("{\"id\":7,\"name\":\"value\"}"),BudgetDom,BudgetStream,false,Budget));
 return true;
}
#endif
