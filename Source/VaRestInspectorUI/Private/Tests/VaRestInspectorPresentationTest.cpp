// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestInspector.h"
#if WITH_DEV_AUTOMATION_TESTS && VAREST_WITH_INSPECTOR
#include "VaRestInspectorBodyViewModel.h"
#include "VaRestInspectorFilter.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

namespace VaRestInspectorPresentationTest
{
FVaRestInspectorBodySource Source(const FVaRestJson& Json)
{
 FVaRestInspectorBodySource Out; Json.ToUtf8(Out.Bytes); Out.TotalBytes=Out.Bytes.Num();
 Out.Kind=EVaRestExcerptKind::Json; Out.bAvailable=true; return Out;
}
bool Settle(FAutomationTestBase& Test,FVaRestInspectorBodyViewModel& Model)
{
 const double Deadline=FPlatformTime::Seconds()+5;
 while (Model.IsPending() && FPlatformTime::Seconds()<Deadline)
 { Model.Poll(); FPlatformProcess::SleepNoStats(0.001f); }
 return Test.TestFalse(TEXT("bounded native parse work settles without GT callbacks"),Model.IsPending());
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorBodyPresentationTest,"VaRest.Inspector.UI.BodyPresentation",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorBodyPresentationTest::RunTest(const FString& Parameters)
{
 using namespace VaRestInspectorPresentationTest;
 const auto Model=FVaRestInspectorBodyViewModel::Create();
 if (!TestTrue(TEXT("GTK body presentation owner created"),Model.IsValid())) return false;
 ON_SCOPE_EXIT { Model->Shutdown(); };
 Model->SetSource(Source(FVaRestJson::Object().Set(TEXT("old"),1)));
 Model->SetSource(Source(FVaRestJson::Object().Set(TEXT("superseded"),2)));
 const FString Key=TEXT("quote\"\\key");
 const auto Latest=FVaRestJson::Object().Set(Key,FVaRestJson::Array().Add(7).Add(8));
 TestTrue(TEXT("latest source admitted during an older parse"),Model->SetSource(Source(Latest)));
 if (!Settle(*this,*Model)) return false;
 TestTrue(TEXT("latest complete JSON has pretty/tree presentation"),Model->IsPrettyJson());
 TestFalse(TEXT("stale job cannot publish old contents"),Model->Pretty().ToString().Contains(TEXT("superseded")));
 if (!TestEqual(TEXT("one JSON root"),Model->Roots().Num(),1)) return false;
 TArray<TSharedPtr<FVaRestInspectorBodyTreeNode>> Children; Model->Children(Model->Roots()[0],Children);
 if (!TestEqual(TEXT("latest root member expanded lazily"),Children.Num(),1)) return false;
 TestEqual(TEXT("tree path uses JSON escaping"),Children[0]->Path,TEXT("$[")+FVaRestJson::From(Key).ToString()+TEXT("]"));
 TArray<TSharedPtr<FVaRestInspectorBodyTreeNode>> Elements; Model->Children(Children[0],Elements);
 TestEqual(TEXT("nested array lazily expands"),Elements.Num(),2);
 Children.Reset(); Elements.Reset();

 FVaRestJson Wide=FVaRestJson::Array(); for (int32 I=0;I<600;++I) Wide.Add(I);
 Model->SetSource(Source(Wide)); if (!Settle(*this,*Model)) return false;
 Model->Children(Model->Roots()[0],Children);
 TestEqual(TEXT("tree child expansion is capped"),Children.Num(),512);
 TestTrue(TEXT("tree cap is visible to the reader"),Model->Notice().ToString().Contains(TEXT("limit"))); Children.Reset();
 auto Truncated=Source(FVaRestJson::Object().Set(TEXT("value"),7)); Truncated.bTruncated=true;
 Model->SetSource(MoveTemp(Truncated)); if (!Settle(*this,*Model)) return false;
 TestFalse(TEXT("truncated excerpts never claim complete JSON"),Model->IsPrettyJson());
 TestEqual(TEXT("truncated excerpts have no semantic tree"),Model->Roots().Num(),0);
 TestTrue(TEXT("truncated prefix is explicitly labelled"),Model->Pretty().ToString().Contains(TEXT("tree unavailable")));
 FVaRestInspectorBodySource Descriptor; Descriptor.bAvailable=true; Descriptor.Kind=EVaRestExcerptKind::Binary; Descriptor.Bytes={ '<','b','i','n','a','r','y','>' }; Descriptor.TotalBytes=5000;
 Model->SetSource(MoveTemp(Descriptor));
 TestTrue(TEXT("descriptor label bytes never appear as captured hex"),Model->Hex().ToString().Contains(TEXT("only for opted-in")));
 TestTrue(TEXT("descriptor stays visible as metadata"),Model->Raw().ToString().Contains(TEXT("5000")));
 FVaRestInspectorBodySource Binary; Binary.bAvailable=true; Binary.Kind=EVaRestExcerptKind::Binary; Binary.bBinaryCaptured=true; Binary.Bytes.Init(0xab,5000); Binary.TotalBytes=5000;
 Model->SetSource(MoveTemp(Binary));
 TestFalse(TEXT("binary display does not launch JSON parsing"),Model->IsPending());
 TestTrue(TEXT("binary hex display cap is visible"),Model->Hex().ToString().Contains(TEXT("hex display truncated")));
 TestFalse(TEXT("binary bytes are not rendered as raw text"),Model->Raw().ToString().Contains(TEXT("abab")));
 Model->SetSource(Source(Latest)); Model->Shutdown();
 TestFalse(TEXT("shutdown joins native work"),Model->IsPending());
 TestEqual(TEXT("shutdown retires tree owners on GT"),Model->Roots().Num(),0);
 TestFalse(TEXT("stopped owner refuses new source"),Model->SetSource(Source(Latest)));
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorFilterPresentationTest,"VaRest.Inspector.UI.SharedFilter",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorFilterPresentationTest::RunTest(const FString& Parameters)
{
 FVaRestInspectorFilterFields Row; Row.Id=42; Row.ScopeId=77; Row.bContextKnown=true;
 Row.Status=503; Row.Duration=.35; Row.Bytes=2048; Row.Method=TEXT("GET"); Row.Name=TEXT("Player Inventory");
 Row.Searchable=TEXT("Player Inventory retry path"); Row.Attempts=2; Row.bError=true;
 FVaRestInspectorFilter Filter;
 Filter.SetText(TEXT("\"Player Inventory\" method:GET status:5xx larger-than:1k slower-than:300ms is:error is:retried -is:socket scope:77 id:42"));
 TestTrue(TEXT("quoted AND, typed quantities and negation share the UI/console grammar"),Filter.Matches(Row));
 Filter.SetText(TEXT("status:>=500 status:<504 -method:POST")); TestTrue(TEXT("status comparisons and negative field"),Filter.Matches(Row));
 Filter.SetText(TEXT("slower-than:400ms")); TestFalse(TEXT("time quantity excludes faster records"),Filter.Matches(Row));
 Filter.SetText(TEXT("larger-than:nan")); TestFalse(TEXT("nonfinite quantity is refused"),Filter.Matches(Row));
 Filter.SetText(TEXT("scope:0")); Row.bContextKnown=false; Row.ScopeId=0;
 TestFalse(TEXT("unknown scope does not become known process scope zero"),Filter.Matches(Row));
 return true;
}
#endif
