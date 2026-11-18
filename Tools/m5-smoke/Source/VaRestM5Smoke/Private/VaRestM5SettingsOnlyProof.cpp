// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestM5SettingsOnlyProof.h"
#include "UObject/EnumProperty.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"
#include "VaRestApi.h"
#include "VaRestJson.h"
#include "VaRestJsonConvert.h"
#include "VaRestJsonRules.h"

namespace VaRestM5SettingsOnly
{
namespace
{
FProperty* RuleProperty(const UVaRestApi* Api, UScriptStruct* Root, const TCHAR* Wire)
{
    for (const FVaRestApiJsonTypeRules& Type : Api->JsonTypes)
    {
        if (Type.Type.Get() != Root) continue;
        for (const FVaRestJsonFieldRule& Rule : Type.Fields)
        {
            if (!Rule.Json.Equals(Wire, ESearchCase::CaseSensitive)) continue;
            const FName Name = Root->FindPropertyNameFromGuid(Rule.Field);
            return Name.IsNone() ? nullptr : FindFProperty<FProperty>(Root, Name);
        }
    }
    return nullptr;
}
const FNumericProperty* EnumStorage(const FProperty* Property, UEnum*& Enum)
{
    if (const FEnumProperty* Typed = CastField<FEnumProperty>(Property))
    {
        Enum = Typed->GetEnum();
        return Typed->GetUnderlyingProperty();
    }
    const FNumericProperty* Numeric = CastField<FNumericProperty>(Property);
    Enum = Numeric ? Numeric->GetIntPropertyEnum() : nullptr;
    return Numeric;
}
}

bool VerifyDefaultConversion(UVaRestApi* Api, UScriptStruct* Root, FString& Error)
{
    check(IsInGameThread());
    const auto Fail = [&Error](const TCHAR* Message) { Error = Message; return false; };
    if (!Api || !Root || !Api->GetPathName().Equals(ApiPath, ESearchCase::CaseSensitive) || !Root->GetPathName().Equals(RootPath, ESearchCase::CaseSensitive))
        return Fail(TEXT("No-load API/root identity is missing or differs"));
    if (Api->Endpoints.Num() != 0 || Api->Sockets.Num() != 0 || Api->JsonTypes.Num() != 3)
        return Fail(TEXT("B must contain only its three settings-only rule/type declarations and no endpoint/socket"));
    if (FVaRestJsonRules::GetGeneration(Api) == 0 || !Root->ArePropertyGuidsAvailable())
        return Fail(TEXT("Production API PostLoad rule registration or actual property-GUID table is absent"));

    // This is the first converter call in the runtime entry. It supplies no Options or Rules.
    FStructOnScope Defaults(Root);
    const FVaRestJson Initial = FVaRestJsonConvert::StructToJson(Root, Defaults.GetStructMemory());
    if (!Initial.IsObject() || Initial[TEXT("omitCount")].IsValid() || !Initial[TEXT("nullCount")].IsNull()
        || Initial[TEXT("decodeOnly")].IsValid() || Initial[TEXT("ignored")].IsValid()
        || Initial[TEXT("state")].IsValid() || !Initial[TEXT("nullState")].IsNull()
        || !Initial[TEXT("ExactFlag")].IsBool() || Initial[TEXT("ExactFlag")].AsBool()
        || Initial[TEXT("Wire.Score")].AsInt() != 42 || Initial[TEXT("nullable")].AsString() != TEXT("kept"))
        return Fail(TEXT("First standalone default conversion lost authored defaults, exact GUID rules, presence or unset semantics"));

    const auto Input = FVaRestJson::Parse(FUtf8StringView(UTF8TEXT(
        "{\"ExactFlag\":false,\"Wire.Score\":7,\"sanitizedLabel\":\"alpha\","
        "\"decodeOnly\":\"server\",\"ignored\":\"attempt\",\"nullable\":null,\"bytes\":\"AQID\","
        "\"mapped\":\"wire-ONE\",\"pretty\":\"Pretty State Two\",\"states\":[1,2],"
        "\"stateMap\":{\"one\":1,\"pretty\":2},"
        "\"nested\":{\"Nested.Flag\":true,\"Nested.Value\":9},\"scalars\":[1,2,3],"
        "\"scalarMap\":{\"x\":4,\"y\":5},\"stamp\":\"2026-10-25T12:34:56.000Z\"}")));
    const auto Expected = FVaRestJson::Parse(FUtf8StringView(UTF8TEXT(
        "{\"ExactFlag\":false,\"Wire.Score\":7,\"sanitizedLabel\":\"alpha\",\"nullCount\":null,"
        "\"nullable\":\"kept\",\"bytes\":\"AQID\",\"mapped\":\"wire-ONE\",\"nullState\":null,"
        "\"pretty\":\"Pretty State Two\",\"states\":[\"wire-ONE\",\"Pretty State Two\"],"
        "\"stateMap\":{\"one\":\"wire-ONE\",\"pretty\":\"Pretty State Two\"},"
        "\"nested\":{\"Nested.Flag\":true,\"Nested.Value\":9},\"scalars\":[1,2,3],"
        "\"scalarMap\":{\"x\":4,\"y\":5},\"stamp\":\"2026-10-25T12:34:56.000Z\"}")));
    if (!Input.HasValue() || !Expected.HasValue()) return Fail(TEXT("Fixture literal JSON is malformed"));
    FStructOnScope Instance(Root);
    FVaRestJsonDecodeReport Report;
    // Unmodified default options solely to supply OutReport. Rules stays null, as in ordinary standalone calls.
    if (!FVaRestJsonConvert::JsonToStruct(Input.GetValue(), Root, Instance.GetStructMemory(), FVaRestJsonConvertOptions(), &Report)
        || Report.Errors.Num() || Report.Warnings.Num() || Report.bBudgetExceeded)
        return Fail(TEXT("Real standalone default-options decode failed or produced a diagnostic"));

    // The healthy decoder must never receive the forbidden unset enumerator. After its zero-diagnostic
    // admission, insert owned unset values through the real reflected container/storage APIs for encode-only proof.
    FArrayProperty* States = CastField<FArrayProperty>(RuleProperty(Api, Root, TEXT("states")));
    FMapProperty* StateMap = CastField<FMapProperty>(RuleProperty(Api, Root, TEXT("stateMap")));
    FStrProperty* StateMapKey = StateMap ? CastField<FStrProperty>(StateMap->KeyProp) : nullptr;
    UEnum* ArrayEnum = nullptr;
    UEnum* MapEnum = nullptr;
    const FNumericProperty* ArrayStorage = States ? EnumStorage(States->Inner, ArrayEnum) : nullptr;
    const FNumericProperty* MapStorage = StateMap ? EnumStorage(StateMap->ValueProp, MapEnum) : nullptr;
    if (!States || !StateMap || !StateMapKey || !ArrayStorage || !MapStorage || !ArrayEnum
        || ArrayEnum != MapEnum || !ArrayEnum->GetPathName().Equals(EnumPath, ESearchCase::CaseSensitive)
        || !ArrayEnum->IsValidEnumValue(0))
        return Fail(TEXT("Real enum array/map reflection or numeric storage is unavailable"));
    FScriptArrayHelper Array(States, States->ContainerPtrToValuePtr<void>(Instance.GetStructMemory()));
    FScriptMapHelper Map(StateMap, StateMap->ContainerPtrToValuePtr<void>(Instance.GetStructMemory()));
    if (Array.Num() != 2 || Map.Num() != 2 || ArrayStorage->GetSignedIntPropertyValue(Array.GetRawPtr(0)) != 1
        || ArrayStorage->GetSignedIntPropertyValue(Array.GetRawPtr(1)) != 2)
        return Fail(TEXT("Healthy decode did not store both legitimate non-unset container values"));
    const int32 UnsetArrayIndex = Array.AddValue();
    ArrayStorage->SetIntPropertyValue(Array.GetRawPtr(UnsetArrayIndex), int64(0));
    const int32 UnsetMapIndex = Map.AddDefaultValue_Invalid_NeedsRehash();
    StateMapKey->SetPropertyValue(Map.GetKeyPtr(UnsetMapIndex), FString(TEXT("unset")));
    MapStorage->SetIntPropertyValue(Map.GetValuePtr(UnsetMapIndex), int64(0));
    Map.Rehash();
    int32 StoredUnsetMapEntries = 0;
    for (FScriptMapHelper::FIterator It(Map); It; ++It)
    {
        if (StateMapKey->GetPropertyValue(Map.GetKeyPtr(It)).Equals(TEXT("unset"), ESearchCase::CaseSensitive)
            && MapStorage->GetSignedIntPropertyValue(Map.GetValuePtr(It)) == 0)
            ++StoredUnsetMapEntries;
    }
    if (UnsetArrayIndex != 2 || Array.Num() != 3 || ArrayStorage->GetSignedIntPropertyValue(Array.GetRawPtr(2)) != 0
        || Map.Num() != 3 || StoredUnsetMapEntries != 1)
        return Fail(TEXT("Owned reflected unset array/map entries were not physically stored before encoding"));

    const FVaRestJson Actual = FVaRestJsonConvert::StructToJson(Root, Instance.GetStructMemory());
    if (!Actual.Equals(Expected.GetValue()))
    {
        Error = FString::Printf(TEXT("Default-options roundtrip mismatch: %s"), *Actual.ToString());
        return false;
    }
    const FStrProperty* DecodeOnly = CastField<FStrProperty>(RuleProperty(Api, Root, TEXT("decodeOnly")));
    const FStrProperty* Ignored = CastField<FStrProperty>(RuleProperty(Api, Root, TEXT("ignored")));
    if (!DecodeOnly || !Ignored || !DecodeOnly->GetPropertyValue_InContainer(Instance.GetStructMemory()).Equals(TEXT("server"), ESearchCase::CaseSensitive)
        || !Ignored->GetPropertyValue_InContainer(Instance.GetStructMemory()).Equals(TEXT("kept"), ESearchCase::CaseSensitive))
        return Fail(TEXT("DecodeOnly/Ignore storage semantics differ despite encoded omissions"));
    // Standalone scalar, array, map, nested and DateTime property paths also use untouched defaults.
    for (const TCHAR* Key : { TEXT("mapped"), TEXT("pretty"), TEXT("states"), TEXT("stateMap"),
        TEXT("scalars"), TEXT("scalarMap"), TEXT("nested"), TEXT("stamp") })
    {
        FProperty* Property = RuleProperty(Api, Root, Key);
        if (!Property || !FVaRestJsonConvert::PropertyToJson(Property,
            Property->ContainerPtrToValuePtr<void>(Instance.GetStructMemory())).Equals(Expected.GetValue()[Key]))
            return Fail(TEXT("Standalone scalar/array/map/nested/date-time default-options conversion differs"));
    }
    Error.Reset();
    return true;
}
}
