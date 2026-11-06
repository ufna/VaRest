// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "VaRestJson.h"
#include "VaRestJsonObject.h"
#include "VaRestJsonValue.h"
#include "VaRestRequestJSON.h"
#include "Misc/AutomationTest.h"
#include "Containers/StringConv.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLegacyJsonCopiesTest,"VaRest.Compat.Legacy.JsonCopies",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestLegacyJsonCopiesTest::RunTest(const FString& Parameters)
{
 const TCHAR Units[]={TEXT('x'),TCHAR(0),TEXT('y')};
 const FString Counted=FString::ConstructFromPtrSize(Units,UE_ARRAY_COUNT(Units));
 auto Native=FVaRestJson::Object().Set(TEXT("child"),FVaRestJson::Object().Set(TEXT("n"),1))
  .Set(TEXT("n"),5).Set(TEXT("A"),2).Set(TEXT("a"),3).Set(TEXT("s"),Counted).Set(TEXT("big"),int64(9007199254740993LL));
 TStrongObjectPtr<UVaRestJsonObject> Legacy(UVaRestJsonObject::FromVaRestJson(Native));
 if (!TestNotNull(TEXT("create an actual legacy UObject from a v2 object"),Legacy.Get())) return false;
 Native.Set(TEXT("n"),99);
 TestEqual(TEXT("legacy copy is independent of subsequent v2 mutation"),Legacy->GetIntegerField(TEXT("n")),5);
 TestEqual(TEXT("legacy case-insensitive storage retains the last case-colliding value"),Legacy->GetIntegerField(TEXT("A")),3);
 TestEqual(TEXT("both legacy key spellings select that same value"),Legacy->GetIntegerField(TEXT("a")),3);
 TestEqual(TEXT("legacy string retains interior NUL and suffix"),Legacy->GetStringField(TEXT("s")),Counted);
 const auto Snapshot=Legacy->ToVaRestJson();
 TestEqual(TEXT("native copy bridge retains the exact int64 spelling"),Snapshot.Get(TEXT("big")).AsInt64(),int64(9007199254740993LL));
 TStrongObjectPtr<UVaRestJsonObject> Child(Legacy->GetObjectField(TEXT("child")));
 if (!TestNotNull(TEXT("legacy nested object view"),Child.Get())) return false;
 Child->SetIntegerField(TEXT("n"),17);
 TestEqual(TEXT("legacy nested alias semantics remain intact"),Legacy->ToVaRestJson().Get(TEXT("child")).Get(TEXT("n")).AsInt(),17);
 TestEqual(TEXT("v2 snapshot owns its nested values"),Snapshot.Get(TEXT("child")).Get(TEXT("n")).AsInt(),1);
 TestEqual(TEXT("original v2 source owns its nested values"),Native.Get(TEXT("child")).Get(TEXT("n")).AsInt(),1);
 TStrongObjectPtr<UVaRestJsonValue> Value(UVaRestJsonValue::FromVaRestJson(FVaRestJson::Array().Add(7).Add(Counted)));
 if (!TestNotNull(TEXT("actual legacy value wrapper"),Value.Get())) return false;
 const auto ValueSnapshot=Value->ToVaRestJson(); Value->Reset();
 TestEqual(TEXT("value snapshot survives legacy reset"),ValueSnapshot[0].AsInt(),7);
 TestEqual(TEXT("value snapshot retains counted string"),ValueSnapshot[1].AsString(),Counted);
 TestNull(TEXT("object bridge refuses a nonobject"),UVaRestJsonObject::FromVaRestJson(FVaRestJson::Array()));
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLegacyJsonKeyCopiesTest,"VaRest.Compat.Legacy.CountedKeyCopies",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestLegacyJsonKeyCopiesTest::RunTest(const FString& Parameters)
{
 const TCHAR KeyUnits[]={TEXT('a'),TCHAR(0),TEXT('x')};
 const FString Key=FString::ConstructFromPtrSize(KeyUnits,UE_ARRAY_COUNT(KeyUnits));
 auto Native=FVaRestJson::Object().Set(TEXT("a"),1).Set(FStringView(Key),2)
  .Set(TEXT("child"),FVaRestJson::Object().Set(TEXT("a"),3).Set(FStringView(Key),4));
 TStrongObjectPtr<UVaRestJsonObject> Legacy(UVaRestJsonObject::FromVaRestJson(Native));
 if (!TestNotNull(TEXT("counted-key object migration result"),Legacy.Get())) return false;
 Native.Set(FStringView(Key),12); Native.Get(TEXT("child")).Set(FStringView(Key),14);
 auto Copy=Legacy->ToVaRestJson();
 TestEqual(TEXT("object copy keeps the real top-level prefix key"),Copy.Get(TEXT("a")).AsInt(),1);
 TestEqual(TEXT("object copy keeps top-level NUL key distinct and isolated from v2 mutation"),Copy.Get(FStringView(Key)).AsInt(),2);
 TestEqual(TEXT("object copy keeps the real nested prefix key"),Copy.Get(TEXT("child")).Get(TEXT("a")).AsInt(),3);
 TestEqual(TEXT("object copy keeps nested NUL key distinct and isolated from v2 mutation"),Copy.Get(TEXT("child")).Get(FStringView(Key)).AsInt(),4);
 TStrongObjectPtr<UVaRestJsonObject> Child(Legacy->GetObjectField(TEXT("child")));
 if (!TestNotNull(TEXT("owned legacy nested object view"),Child.Get())) return false;
 Legacy->SetIntegerField(Key,20); Child->SetIntegerField(Key,40);
 const auto Changed=Legacy->ToVaRestJson();
 TestEqual(TEXT("engine mutation reaches the top-level counted key"),Changed.Get(FStringView(Key)).AsInt(),20);
 TestEqual(TEXT("engine mutation reaches the nested counted key"),Changed.Get(TEXT("child")).Get(FStringView(Key)).AsInt(),40);
 TestEqual(TEXT("engine mutation does not overwrite the real top-level prefix"),Changed.Get(TEXT("a")).AsInt(),1);
 TestEqual(TEXT("engine mutation does not overwrite the real nested prefix"),Changed.Get(TEXT("child")).Get(TEXT("a")).AsInt(),3);
 TestEqual(TEXT("engine mutation leaves the earlier top-level v2 copy independent"),Copy.Get(FStringView(Key)).AsInt(),2);
 TestEqual(TEXT("engine mutation leaves the earlier nested v2 copy independent"),Copy.Get(TEXT("child")).Get(FStringView(Key)).AsInt(),4);
 Copy.Set(FStringView(Key),22); Copy.Get(TEXT("child")).Set(FStringView(Key),44);
 TestEqual(TEXT("mutating the copied top-level handle leaves engine storage independent"),Legacy->GetIntegerField(Key),20);
 TestEqual(TEXT("mutating the copied nested handle leaves engine storage independent"),Child->GetIntegerField(Key),40);
 TestEqual(TEXT("original top-level v2 source remains independent of engine and copied mutations"),Native.Get(FStringView(Key)).AsInt(),12);
 TestEqual(TEXT("original nested v2 source remains independent of engine and copied mutations"),Native.Get(TEXT("child")).Get(FStringView(Key)).AsInt(),14);

 auto ValueSource=FVaRestJson::Object().Set(TEXT("a"),5).Set(FStringView(Key),6)
  .Set(TEXT("child"),FVaRestJson::Object().Set(TEXT("a"),7).Set(FStringView(Key),8));
 TStrongObjectPtr<UVaRestJsonValue> Value(UVaRestJsonValue::FromVaRestJson(ValueSource));
 if (!TestNotNull(TEXT("counted-key value migration result"),Value.Get())) return false;
 ValueSource.Set(FStringView(Key),60); ValueSource.Get(TEXT("child")).Set(FStringView(Key),80);
 auto ValueCopy=Value->ToVaRestJson();
 TestEqual(TEXT("value copy keeps the real top-level prefix"),ValueCopy.Get(TEXT("a")).AsInt(),5);
 TestEqual(TEXT("value copy preserves its distinct top-level NUL key"),ValueCopy.Get(FStringView(Key)).AsInt(),6);
 TestEqual(TEXT("value copy keeps the real nested prefix"),ValueCopy.Get(TEXT("child")).Get(TEXT("a")).AsInt(),7);
 TestEqual(TEXT("value copy preserves its distinct nested NUL key"),ValueCopy.Get(TEXT("child")).Get(FStringView(Key)).AsInt(),8);
 TStrongObjectPtr<UVaRestJsonObject> ValueObject(Value->AsObject());
 if (!TestNotNull(TEXT("owned object view of the legacy value"),ValueObject.Get())) return false;
 TStrongObjectPtr<UVaRestJsonObject> ValueChild(ValueObject->GetObjectField(TEXT("child")));
 if (!TestNotNull(TEXT("owned nested view of the legacy value"),ValueChild.Get())) return false;
 ValueObject->SetIntegerField(Key,61); ValueChild->SetIntegerField(Key,81);
 const auto ChangedValue=Value->ToVaRestJson();
 TestEqual(TEXT("value root observes engine top-level mutation"),ChangedValue.Get(FStringView(Key)).AsInt(),61);
 TestEqual(TEXT("value root observes engine nested mutation"),ChangedValue.Get(TEXT("child")).Get(FStringView(Key)).AsInt(),81);
 TestEqual(TEXT("value top-level prefix survives engine mutation"),ChangedValue.Get(TEXT("a")).AsInt(),5);
 TestEqual(TEXT("value nested prefix survives engine mutation"),ChangedValue.Get(TEXT("child")).Get(TEXT("a")).AsInt(),7);
 TestEqual(TEXT("value top-level copy is independent of engine mutation"),ValueCopy.Get(FStringView(Key)).AsInt(),6);
 TestEqual(TEXT("value nested copy is independent of engine mutation"),ValueCopy.Get(TEXT("child")).Get(FStringView(Key)).AsInt(),8);
 ValueCopy.Set(FStringView(Key),62); ValueCopy.Get(TEXT("child")).Set(FStringView(Key),82);
 TestEqual(TEXT("value copied top-level mutation leaves engine storage independent"),ValueObject->GetIntegerField(Key),61);
 TestEqual(TEXT("value copied nested mutation leaves engine storage independent"),ValueChild->GetIntegerField(Key),81);
 TestEqual(TEXT("value original top-level source remains independent"),ValueSource.Get(FStringView(Key)).AsInt(),60);
 TestEqual(TEXT("value original nested source remains independent"),ValueSource.Get(TEXT("child")).Get(FStringView(Key)).AsInt(),80);
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLegacyJsonParserTest,"VaRest.Compat.Legacy.CountedParser",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestLegacyJsonParserTest::RunTest(const FString& Parameters)
{
 TStrongObjectPtr<UVaRestJsonObject> Legacy(NewObject<UVaRestJsonObject>());
 const FString Prefix=TEXT("{\"label\":\"Кот\",\"s\":\"x\\u0000y\",\"n\":7}");
 const FTCHARToUTF8 Utf8(*Prefix,Prefix.Len());
 TArray<uint8> Input(reinterpret_cast<const uint8*>(Utf8.Get()),Utf8.Length());
 const int32 ByteCount=Input.Num(); const uint8 Trailer[]={TEXT('b'),TEXT('a'),TEXT('d')}; Input.Append(Trailer,UE_ARRAY_COUNT(Trailer));
 const auto Original=Input;
 TestEqual(TEXT("UTF8 parser consumes exactly the supplied byte prefix"),Legacy->DeserializeFromUTF8Bytes(reinterpret_cast<const ANSICHAR*>(Input.GetData()),ByteCount),ByteCount);
 TestTrue(TEXT("legacy parser preserves caller bytes"),Input==Original);
 TestEqual(TEXT("UTF8 multibyte value"),Legacy->GetStringField(TEXT("label")),FString(TEXT("Кот")));
 TestEqual(TEXT("counted NUL suffix survives legacy parsing"),Legacy->GetStringField(TEXT("s")).Len(),3);
 const FString Wide=Prefix+TEXT("bad");
 TestEqual(TEXT("TCHAR parser consumes exactly the supplied character prefix"),Legacy->DeserializeFromTCHARBytes(*Wide,Prefix.Len()),Prefix.Len());
 TestTrue(TEXT("legacy string parser uses the unified parser"),Legacy->DecodeJson(Prefix,false));
 AddExpectedMessagePlain(TEXT("Legacy JSON decoding failed"),ELogVerbosity::Warning,EAutomationExpectedMessageFlags::Contains,1);
 const ANSICHAR Invalid[]="{\"n\":";
 TestEqual(TEXT("malformed input refuses the decode"),Legacy->DeserializeFromUTF8Bytes(Invalid,UE_ARRAY_COUNT(Invalid)-1),0);
 TestEqual(TEXT("failed parse resets prior fields"),Legacy->GetFieldNames().Num(),0);
 TestFalse(TEXT("legacy object parser refuses an array root"),Legacy->DecodeJson(TEXT("[1,2]"),false));
 TestEqual(TEXT("array root keeps an empty legacy object"),Legacy->GetFieldNames().Num(),0);
 TestEqual(TEXT("empty input refuses without reading a pointer"),Legacy->DeserializeFromUTF8Bytes(nullptr,0),0);
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLegacyJsonReflectionTest,"VaRest.Compat.Legacy.Reflection",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestLegacyJsonReflectionTest::RunTest(const FString& Parameters)
{
 struct FEntry { UClass* Class; const TCHAR* Function; };
 const FEntry Entries[]={
  {UVaRestJsonObject::StaticClass(),TEXT("DecodeJson")},{UVaRestJsonObject::StaticClass(),TEXT("SetArrayField")},
  {UVaRestJsonValue::StaticClass(),TEXT("AsString")},{UVaRestJsonValue::StaticClass(),TEXT("AsInt64")},
  {UVaRestRequestJSON::StaticClass(),TEXT("ProcessURL")},{UVaRestRequestJSON::StaticClass(),TEXT("ApplyURL")}};
 for (const auto& Entry:Entries)
 {
  TestEqual(TEXT("legacy reflected package identity remains loadable"),Entry.Class->GetOutermost()->GetName(),FString(TEXT("/Script/VaRest")));
  const auto* Function=Entry.Class->FindFunctionByName(Entry.Function);
  if (!TestNotNull(Entry.Function,Function)) return false;
  TestTrue(TEXT("old function remains native and Blueprint callable"),Function->HasAllFunctionFlags(FUNC_Native|FUNC_BlueprintCallable));
  TestTrue(TEXT("old function carries migration deprecation metadata"),Function->HasMetaData(TEXT("DeprecatedFunction")));
  TestEqual(TEXT("old function remains hidden from new BP placement"),Function->GetMetaData(TEXT("BlueprintInternalUseOnly")),FString(TEXT("true")));
 }
 // Real reflected thunk marshalling, using owned parameter storage rather than a direct C++ bridge call.
 const auto Native=FVaRestJson::Object().Set(TEXT("n"),17);
 UFunction* From=UVaRestJsonObject::StaticClass()->FindFunctionByName(TEXT("FromVaRestJson"));
 if (!TestNotNull(TEXT("v2-to-legacy migration UFunction"),From)) return false;
 const auto* Input=FindFProperty<FStructProperty>(From,TEXT("Json"));
 const auto* Return=FindFProperty<FObjectProperty>(From,TEXT("ReturnValue"));
 if (!TestTrue(TEXT("actual reflected migration parameter types"),Input && Input->Struct==FVaRestJson::StaticStruct() && Return)) return false;
 FStructOnScope FromStorage(From);
 Input->CopyCompleteValue(Input->ContainerPtrToValuePtr<void>(FromStorage.GetStructMemory()),&Native);
 GetMutableDefault<UVaRestJsonObject>()->ProcessEvent(From,FromStorage.GetStructMemory());
 TStrongObjectPtr<UVaRestJsonObject> Legacy(Cast<UVaRestJsonObject>(Return->GetObjectPropertyValue_InContainer(FromStorage.GetStructMemory())));
 if (!TestNotNull(TEXT("migration thunk returns a real owned legacy UObject"),Legacy.Get())) return false;
 UFunction* To=Legacy->FindFunction(TEXT("ToVaRestJson"));
 if (!TestNotNull(TEXT("legacy-to-v2 migration UFunction"),To)) return false;
 const auto* Result=FindFProperty<FStructProperty>(To,TEXT("ReturnValue"));
 if (!TestTrue(TEXT("migration returns the current native JSON type"),Result && Result->Struct==FVaRestJson::StaticStruct())) return false;
 FStructOnScope ToStorage(To); Legacy->ProcessEvent(To,ToStorage.GetStructMemory());
 const FVaRestJson Copy=*Result->ContainerPtrToValuePtr<FVaRestJson>(ToStorage.GetStructMemory());
 Legacy->SetIntegerField(TEXT("n"),99);
 TestEqual(TEXT("reflected migration output owns its independent value"),Copy.Get(TEXT("n")).AsInt(),17);
 return true;
}
#endif
