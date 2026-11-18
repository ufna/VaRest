// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestM5EndpointContract.h"
#include "UObject/EnumProperty.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"
#include "VaRestApi.h"
#include "VaRestJson.h"
#include "VaRestJsonConvert.h"
namespace VaRestM5Endpoint
{
FString WireBody() { return TEXT("{\"ExactFlag\":false,\"Wire.Score\":7,\"sanitizedLabel\":\"alpha\",\"nullCount\":null,\"nullable\":\"kept\",\"bytes\":\"AQID\",\"nullState\":null,\"mapped\":\"wire-ONE\",\"pretty\":\"Pretty State Two\",\"states\":[\"wire-ONE\",\"Pretty State Two\"],\"stateMap\":{\"one\":\"wire-ONE\",\"pretty\":\"Pretty State Two\"},\"nested\":{\"Nested.Flag\":true,\"Nested.Value\":9},\"scalars\":[1,2,3],\"scalarMap\":{\"x\":4,\"y\":5},\"stamp\":\"2026-10-25T12:34:56.000Z\"}"); }
static FString HealthyBody() { return TEXT("{\"ExactFlag\":false,\"Wire.Score\":7,\"sanitizedLabel\":\"alpha\",\"decodeOnly\":\"client\",\"ignored\":\"attempt\",\"nullable\":null,\"bytes\":\"AQID\",\"mapped\":\"wire-ONE\",\"pretty\":\"Pretty State Two\",\"states\":[1,2],\"stateMap\":{\"one\":1,\"pretty\":2},\"nested\":{\"Nested.Flag\":true,\"Nested.Value\":9},\"scalars\":[1,2,3],\"scalarMap\":{\"x\":4,\"y\":5},\"stamp\":\"2026-10-25T12:34:56.000Z\"}"); }
FString ReplyBody(int32 Case)
{
    switch (Case)
    {
    case 0: return TEXT("{\"ExactFlag\":false,\"Wire.Score\":7,\"sanitizedLabel\":\"alpha\",\"nullable\":null,\"bytes\":\"AQID\",\"mapped\":\"wire-ONE\",\"pretty\":\"Pretty State Two\",\"states\":[\"wire-ONE\",\"Pretty State Two\"],\"stateMap\":{\"one\":\"wire-ONE\",\"pretty\":\"Pretty State Two\"},\"nested\":{\"Nested.Flag\":true,\"Nested.Value\":9},\"scalars\":[1,2,3],\"scalarMap\":{\"x\":4,\"y\":5},\"stamp\":\"2026-10-25T12:34:56.000Z\",\"decodeOnly\":\"server\",\"ignored\":\"tamper\"}");
    case 1: return TEXT("{\"ExactFlag\":false,\"Wire.Score\":7,\"sanitizedLabel\":\"alpha\",\"nullable\":null,\"bytes\":\"AQID\",\"mapped\":\"wire-ONE\",\"pretty\":\"Pretty State Two\",\"states\":[\"wire-ONE\",\"Pretty State Two\"],\"stateMap\":{\"one\":\"wire-ONE\",\"pretty\":\"Pretty State Two\"},\"nested\":{\"Nested.Flag\":true,\"Nested.Value\":9},\"scalars\":[1,2,3],\"scalarMap\":{\"x\":4,\"y\":5},\"stamp\":\"2026-10-25T12:34:56.000Z\",\"decodeOnly\":\"server\",\"ignored\":\"tamper\"}");
    case 2: return TEXT("[3,4,5]");
    case 3: return TEXT("{\"left\":7,\"right\":8}");
    case 4: return TEXT("\"wire-ONE\"");
    case 5: return TEXT("\"2026-10-25T12:34:56.000Z\"");
    case 6: return TEXT("{\"ExactFlag\":false,\"Wire.Score\":7,\"sanitizedLabel\":\"alpha\",\"nullable\":null,\"bytes\":\"AQID\",\"mapped\":\"wire-ONE\",\"pretty\":\"Pretty State Two\",\"states\":[\"wire-ONE\",\"Pretty State Two\"],\"stateMap\":{\"one\":\"wire-ONE\",\"pretty\":\"Pretty State Two\"},\"nested\":{\"Nested.Flag\":true,\"Nested.Value\":9},\"scalars\":[1,2,3],\"scalarMap\":{\"x\":4,\"y\":5},\"stamp\":\"2026-10-25T12:34:56.000Z\",\"decodeOnly\":\"server\",\"ignored\":\"tamper\"}");
    case 7: return TEXT("42");
    default: return {};
    }
}
FProperty* RuleProperty(const UVaRestApi* Api, UScriptStruct* Root, const TCHAR* Wire)
{
    for (const auto& Type : Api->JsonTypes) if (Type.Type.Get() == Root)
        for (const auto& Rule : Type.Fields) if (Rule.Json.Equals(Wire, ESearchCase::CaseSensitive))
            return FindFProperty<FProperty>(Root, Root->FindPropertyNameFromGuid(Rule.Field));
    return nullptr;
}
static const FNumericProperty* EnumStorage(const FProperty* Property)
{
    if (const auto* Enum = CastField<FEnumProperty>(Property)) return Enum->GetUnderlyingProperty();
    return CastField<FNumericProperty>(Property);
}
bool PreparePayload(UVaRestApi* Api, UScriptStruct* Root, void* Memory, FString& Error)
{
    check(IsInGameThread());
    const auto Fail=[&](const TCHAR* Detail) { Error=Detail; return false; };
    if (!Api || !Root || !Memory || !Root->GetPathName().Equals(RootPath, ESearchCase::CaseSensitive)) return Fail(TEXT("Missing owned A root payload"));
    auto Input=FVaRestJson::Parse(FStringView(HealthyBody()));
    if (!Input.HasValue()) return Fail(TEXT("Malformed healthy literal"));
    Root->DestroyStruct(Memory); Root->InitializeStruct(Memory);
    FVaRestJsonDecodeReport Report;
    if (!FVaRestJsonConvert::JsonToStruct(Input.GetValue(),Root,Memory,FVaRestJsonConvertOptions(),&Report)
        || Report.Warnings.Num() || Report.Errors.Num() || Report.bBudgetExceeded) return Fail(TEXT("Healthy default decode diagnostic"));
    auto* Array=CastField<FArrayProperty>(RuleProperty(Api,Root,TEXT("states")));
    auto* Map=CastField<FMapProperty>(RuleProperty(Api,Root,TEXT("stateMap")));
    auto* Key=Map ? CastField<FStrProperty>(Map->KeyProp) : nullptr;
    const auto* ArrayValue=Array ? EnumStorage(Array->Inner) : nullptr;
    const auto* MapValue=Map ? EnumStorage(Map->ValueProp) : nullptr;
    if (!Array || !Map || !Key || !ArrayValue || !MapValue) return Fail(TEXT("Real enum container layout missing"));
    FScriptArrayHelper A(Array,Array->ContainerPtrToValuePtr<void>(Memory));
    FScriptMapHelper M(Map,Map->ContainerPtrToValuePtr<void>(Memory));
    if (A.Num()!=2 || M.Num()!=2) return Fail(TEXT("Healthy container count differs"));
    const int32 AI=A.AddValue(); ArrayValue->SetIntPropertyValue(A.GetRawPtr(AI),int64(0));
    const int32 MI=M.AddDefaultValue_Invalid_NeedsRehash();
    Key->SetPropertyValue(M.GetKeyPtr(MI),FString(TEXT("unset"))); MapValue->SetIntPropertyValue(M.GetValuePtr(MI),int64(0)); M.Rehash();
    int32 Found=0;
    for (FScriptMapHelper::FIterator It(M);It;++It)
        if (Key->GetPropertyValue(M.GetKeyPtr(It)).Equals(TEXT("unset"),ESearchCase::CaseSensitive) && MapValue->GetSignedIntPropertyValue(M.GetValuePtr(It))==0) ++Found;
    if (AI!=2 || A.Num()!=3 || ArrayValue->GetSignedIntPropertyValue(A.GetRawPtr(2))!=0 || M.Num()!=3 || Found!=1)
        return Fail(TEXT("Unset owned storage insertion failed"));
    if (!FVaRestJsonConvert::StructToString(Root,Memory).Equals(WireBody(),ESearchCase::CaseSensitive)) return Fail(TEXT("Exact A wire bytes differ"));
    Error.Reset(); return true;
}
bool VerifyDefaultConversion(UVaRestApi* Api, UScriptStruct* Root, FString& Error)
{
    if (!Api || !Root) { Error=TEXT("A API/root unavailable"); return false; }
    FStructOnScope Value(Root);
    const auto Defaults=FVaRestJsonConvert::StructToJson(Root,Value.GetStructMemory());
    if (Defaults[TEXT("Wire.Score")].AsInt()!=42 || Defaults[TEXT("omitCount")].IsValid() || !Defaults[TEXT("nullCount")].IsNull()
        || Defaults[TEXT("state")].IsValid() || !Defaults[TEXT("nullState")].IsNull() || Defaults[TEXT("decodeOnly")].IsValid() || Defaults[TEXT("ignored")].IsValid())
    { Error=TEXT("A authored defaults/presence/unset default route differs"); return false; }
    return PreparePayload(Api,Root,Value.GetStructMemory(),Error);
}
}
