// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/UserDefinedEnum.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/StructOnScope.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "VaRestJson.h"
#include "VaRestJsonConvert.h"
#include "VaRestJsonLibrary.h"

namespace VaRestJsonUserStructTest
{
FEdGraphPinType PinType(FName Category)
{
	return FEdGraphPinType(Category, NAME_None, nullptr, EPinContainerType::None, false, FEdGraphTerminalType());
}

/** A transient Blueprint struct: the bool every new struct starts with, renamed to BoolName, then an int named IntName */
UUserDefinedStruct* MakeStruct(const TCHAR* BoolName, const TCHAR* IntName)
{
	UUserDefinedStruct* Struct = FStructureEditorUtils::CreateUserDefinedStruct(GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UUserDefinedStruct::StaticClass(), FName(TEXT("VaRestJsonTestStruct"))), RF_Transient);
	if (!Struct)
	{
		return nullptr;
	}
	FStructureEditorUtils::AddVariable(Struct, PinType(UEdGraphSchema_K2::PC_Int));
	const FGuid BoolGuid = FStructureEditorUtils::GetVarDesc(Struct)[0].VarGuid;
	const FGuid IntGuid = FStructureEditorUtils::GetVarDesc(Struct)[1].VarGuid;
	FStructureEditorUtils::RenameVariable(Struct, BoolGuid, BoolName);
	FStructureEditorUtils::RenameVariable(Struct, IntGuid, IntName);
	return Struct;
}

template <typename PropertyType>
PropertyType* FirstField(const UUserDefinedStruct* Struct)
{
	TFieldIterator<PropertyType> It(Struct);
	return It ? *It : nullptr;
}
} // namespace VaRestJsonUserStructTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonUserStructNamesTest, "VaRest.Json.Convert.UserStruct.Names",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonUserStructNamesTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonUserStructTest;
	UUserDefinedStruct* Struct = MakeStruct(TEXT("Is Alive"), TEXT("Player Score"));
	if (!TestNotNull(TEXT("Blueprint structs are enabled (BaseEditor.ini [UserDefinedStructure])"), Struct))
	{
		return false;
	}
	FBoolProperty* Alive = FirstField<FBoolProperty>(Struct);
	FIntProperty* Score = FirstField<FIntProperty>(Struct);
	if (!TestTrue(TEXT("both members exist"), Alive && Score))
	{
		return false;
	}
	TestTrue(TEXT("the property name is <Name>_<id>_<GUID>"), Score->GetName().StartsWith(TEXT("PlayerScore_"), ESearchCase::CaseSensitive) && Score->GetName().Len() > 32 + 3);
	// The divergence the converter avoids: the editor's authored name keeps the space, a cooked game's does not
	TestEqualSensitive(TEXT("editor GetAuthoredName is the friendly name"), Score->GetAuthoredName(), FString(TEXT("Player Score")));

	FStructOnScope Instance(Struct);
	FVaRestJsonConvertOptions AsIs;
	AsIs.Naming = EVaRestJsonNaming::AsIs;
	TestEqualSensitive(TEXT("AsIs writes the cooked name in the editor too"),
		FVaRestJsonConvert::StructToString(Struct, Instance.GetStructMemory(), AsIs), FString(TEXT("{\"IsAlive\":false,\"PlayerScore\":0}")));
	TestEqualSensitive(TEXT("camelCase"),
		FVaRestJsonConvert::StructToString(Struct, Instance.GetStructMemory()), FString(TEXT("{\"isAlive\":false,\"playerScore\":0}")));

	const FVaRestJson Body = FVaRestJson::Parse(FUtf8StringView(UTF8TEXT("{\"is_alive\":true,\"PlayerScore\":5}"))).StealValue();
	FVaRestJsonDecodeReport Report;
	TestTrue(TEXT("decode by other spellings"), FVaRestJsonConvert::JsonToStruct(Body, Struct, Instance.GetStructMemory(), FVaRestJsonConvertOptions(), &Report));
	TestTrue(TEXT("... fills both members"), Alive->GetPropertyValue_InContainer(Instance.GetStructMemory()) && Score->GetPropertyValue_InContainer(Instance.GetStructMemory()) == 5);
	TestEqual(TEXT("... without warnings"), Report.Warnings.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonUserStructCacheTest, "VaRest.Json.Convert.UserStruct.CacheInvalidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonUserStructCacheTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonUserStructTest;
	UUserDefinedStruct* Struct = MakeStruct(TEXT("Alive"), TEXT("Score"));
	if (!TestNotNull(TEXT("Blueprint struct"), Struct))
	{
		return false;
	}
	FVaRestJsonConvert::ResetCache();
	{
		FStructOnScope Before(Struct);
		TestEqualSensitive(TEXT("before the change"), FVaRestJsonConvert::StructToString(Struct, Before.GetStructMemory()), FString(TEXT("{\"alive\":false,\"score\":0}")));
	}
	TestTrue(TEXT("the field map is cached"), FVaRestJsonConvert::NumCachedFieldMapsInternal() > 0);

	TestTrue(TEXT("rename Score to Points"), FStructureEditorUtils::RenameVariable(Struct, FStructureEditorUtils::GetVarDesc(Struct)[1].VarGuid, TEXT("Points")));
	// The recompile destroyed the old properties: a map that survived would point at freed memory, so stop before using it
	if (!TestEqual(TEXT("the rename dropped the cached maps"), FVaRestJsonConvert::NumCachedFieldMapsInternal(), 0))
	{
		return false;
	}
	{
		FStructOnScope Renamed(Struct);
		TestEqualSensitive(TEXT("after a rename"), FVaRestJsonConvert::StructToString(Struct, Renamed.GetStructMemory()), FString(TEXT("{\"alive\":false,\"points\":0}")));
	}

	TestTrue(TEXT("add a string member"), FStructureEditorUtils::AddVariable(Struct, PinType(UEdGraphSchema_K2::PC_String)));
	if (!TestEqual(TEXT("adding a member dropped the cached maps"), FVaRestJsonConvert::NumCachedFieldMapsInternal(), 0))
	{
		return false;
	}
	FStructOnScope Added(Struct);
	// A new member is MemberVar_<id>_<GUID>; the cooked algorithm drops the id too
	TestEqualSensitive(TEXT("after adding a member"), FVaRestJsonConvert::StructToString(Struct, Added.GetStructMemory()),
		FString(TEXT("{\"alive\":false,\"points\":0,\"memberVar\":\"\"}")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonUserStructDefaultsTest, "VaRest.Json.Convert.UserStruct.Defaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonUserStructDefaultsTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonUserStructTest;
	// Json To Struct and Get Field reset their output to the type's default. For a Blueprint struct that is its authored
	// defaults, which only UUserDefinedStruct::InitializeStruct copies from the default instance (UserDefinedStruct.cpp:321-338);
	// the struct has no C++ struct ops (:58-63), so ClearValue would zero the fields instead.
	UUserDefinedStruct* Stats = MakeStruct(TEXT("Alive"), TEXT("Score"));
	if (!TestNotNull(TEXT("Blueprint struct"), Stats))
	{
		return false;
	}
	TestTrue(TEXT("Score defaults to 42"), FStructureEditorUtils::ChangeVariableDefaultValue(Stats, FStructureEditorUtils::GetVarDesc(Stats)[1].VarGuid, TEXT("42")));

	// A Blueprint node's output of that type is an FStructProperty of the struct; another Blueprint struct's member is one
	UUserDefinedStruct* Holder = FStructureEditorUtils::CreateUserDefinedStruct(GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UUserDefinedStruct::StaticClass(), FName(TEXT("VaRestJsonTestHolder"))), RF_Transient);
	if (!TestNotNull(TEXT("holder struct"), Holder))
	{
		return false;
	}
	TestTrue(TEXT("add a Stats member"), FStructureEditorUtils::AddVariable(Holder,
											 FEdGraphPinType(UEdGraphSchema_K2::PC_Struct, NAME_None, Stats, EPinContainerType::None, false, FEdGraphTerminalType())));
	FStructProperty* StatsProperty = FirstField<FStructProperty>(Holder);
	FBoolProperty* Alive = FirstField<FBoolProperty>(Stats);
	FIntProperty* Score = FirstField<FIntProperty>(Stats);
	if (!TestTrue(TEXT("the members exist"), StatsProperty && StatsProperty->Struct == Stats && Alive && Score))
	{
		return false;
	}
	FStructOnScope Instance(Holder);
	void* Value = StatsProperty->ContainerPtrToValuePtr<void>(Instance.GetStructMemory());
	if (!TestEqual(TEXT("a new value has the authored default"), Score->GetPropertyValue_InContainer(Value), 42))
	{
		return false; // the default did not take: the checks below would prove nothing
	}

	// The output of an earlier call (a reused Blueprint temporary) holds another value
	Score->SetPropertyValue_InContainer(Value, 7);
	const FVaRestJson NoScore = FVaRestJson::Parse(FUtf8StringView(UTF8TEXT("{\"alive\":true}"))).StealValue();
	TestTrue(TEXT("Json To Struct"), UVaRestJsonLibrary::JsonToStructImpl(NoScore, StatsProperty, Value, EVaRestJsonNaming::CamelCase, false));
	TestTrue(TEXT("... fills the field the JSON has"), Alive->GetPropertyValue_InContainer(Value));
	TestEqual(TEXT("... and gives the missing one its authored default"), Score->GetPropertyValue_InContainer(Value), 42);

	Score->SetPropertyValue_InContainer(Value, 7);
	TestFalse(TEXT("Get Field of a missing key"), UVaRestJsonLibrary::GetFieldImpl(NoScore, TEXT("stats"), StatsProperty, Value));
	TestFalse(TEXT("... resets the output: Alive"), Alive->GetPropertyValue_InContainer(Value));
	TestEqual(TEXT("... resets the output: Score to its authored default"), Score->GetPropertyValue_InContainer(Value), 42);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonUserStructRulesTest, "VaRest.Json.Rules.UserStructGuidRecompile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonUserStructRulesTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonUserStructTest;
	TStrongObjectPtr<UUserDefinedStruct> Struct(MakeStruct(TEXT("Alive"), TEXT("Score")));
	TStrongObjectPtr<UObject> Owner(NewObject<UEnum>());
	if (!TestNotNull(TEXT("real Blueprint struct"), Struct.Get()))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		FVaRestJsonRules::UnregisterOwner(Owner.Get());
	};
	FIntProperty* Score = FirstField<FIntProperty>(Struct.Get());
	if (!TestNotNull(TEXT("real integer property"), Score))
	{
		return false;
	}
	const FGuid Guid = FStructureEditorUtils::GetVarDesc(Struct.Get())[1].VarGuid;
	TestTrue(TEXT("this fixture uses the primary property-GUID API"), Struct->ArePropertyGuidsAvailable());
	TestEqual(TEXT("primary API identifies the original field"), Struct->FindPropertyGuidFromName(Score->GetFName()), Guid);
	FVaRestJsonFieldRule Rule;
	Rule.Field = Guid;
	Rule.Json = TEXT("ExactWireName");
	Rule.Presence = EVaRestJsonPresence::OmitWhenDefault;
	const TArray<FVaRestJsonFieldRule> Rules{Rule};
	TestTrue(TEXT("register rules once"), FVaRestJsonRules::RegisterFields(Owner.Get(), Struct.Get(), Rules));
	const uint64 Generation = FVaRestJsonRules::GetGeneration(Owner.Get());
	{
		FStructOnScope Before(Struct.Get());
		Score->SetPropertyValue_InContainer(Before.GetStructMemory(), 5);
		TestEqual(TEXT("registered wire key before rename"), FVaRestJsonConvert::StructToJson(Struct.Get(), Before.GetStructMemory())[TEXT("ExactWireName")].AsInt(), 5);
	} // release the old-layout instance before the real editor compiler invalidates its properties
	TestTrue(TEXT("rename and recompile the real Blueprint field"), FStructureEditorUtils::RenameVariable(Struct.Get(), Guid, TEXT("Points")));
	TestEqual(TEXT("rename requires no rules re-registration"), FVaRestJsonRules::GetGeneration(Owner.Get()), Generation);
	FIntProperty* Points = FirstField<FIntProperty>(Struct.Get());
	if (!TestNotNull(TEXT("new-layout property"), Points))
	{
		return false;
	}
	TestEqual(TEXT("primary GUID survives the recompile"), Struct->FindPropertyGuidFromName(Points->GetFName()), Guid);
	{
		FStructOnScope After(Struct.Get());
		Points->SetPropertyValue_InContainer(After.GetStructMemory(), 9);
		TestEqual(TEXT("wire key survives field rename"), FVaRestJsonConvert::StructToJson(Struct.Get(), After.GetStructMemory())[TEXT("ExactWireName")].AsInt(), 9);
	}
	TestTrue(TEXT("change authored default through the real editor"), FStructureEditorUtils::ChangeVariableDefaultValue(Struct.Get(), Guid, TEXT("42")));
	{
		FStructOnScope Authored(Struct.Get());
		const auto Json = FVaRestJsonConvert::StructToJson(Struct.Get(), Authored.GetStructMemory());
		TestFalse(TEXT("presence uses the authored default after recompile"), Json[TEXT("ExactWireName")].IsValid());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonEnumMapMissingRulesTest, "VaRest.Json.Rules.EnumMapMissingRulesWarning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonEnumMapMissingRulesTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonUserStructTest;
	TStrongObjectPtr<UEnum> Enum(FEnumEditorUtils::CreateUserDefinedEnum(GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UEnum::StaticClass(), TEXT("VaRestMissingRulesEnum")), RF_Public | RF_Transient));
	if (Enum.Get())
	{
		FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(CastChecked<UUserDefinedEnum>(Enum.Get()));
	}
	TStrongObjectPtr<UUserDefinedStruct> Holder(MakeStruct(TEXT("Alive"), TEXT("Score")));
	TStrongObjectPtr<UObject> Owner(NewObject<UEnum>());
	if (!TestTrue(TEXT("real enum/struct fixtures exist"), Enum.Get() && Holder.Get() && Enum->NumEnums() > 1))
	{
		return false;
	}
	FEdGraphPinType MapType = PinType(UEdGraphSchema_K2::PC_Byte);
	MapType.PinSubCategoryObject = Enum.Get();
	MapType.ContainerType = EPinContainerType::Map;
	MapType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Int;
	if (!TestTrue(TEXT("compile a genuine enum-key map property"), FStructureEditorUtils::AddVariable(Holder.Get(), MapType)))
	{
		return false;
	}
	FMapProperty* Map = FirstField<FMapProperty>(Holder.Get());
	if (!TestNotNull(TEXT("the compiled map property exists"), Map))
	{
		return false;
	}
	const FEnumProperty* EnumKey = CastField<FEnumProperty>(Map->KeyProp);
	const FNumericProperty* Underlying = EnumKey ? EnumKey->GetUnderlyingProperty() : CastField<FNumericProperty>(Map->KeyProp);
	if (!TestNotNull(TEXT("the enum key has real integer storage"), Underlying))
	{
		return false;
	}
	FStructOnScope Instance(Holder.Get());
	void* Address = Map->ContainerPtrToValuePtr<void>(Instance.GetStructMemory());
	FScriptMapHelper Helper(Map, Address);
	void* Key = Map->KeyProp->AllocateAndInitializeValue();
	ON_SCOPE_EXIT
	{
		Map->KeyProp->DestroyAndFreeValue(Key);
		FVaRestJsonRules::UnregisterOwner(Owner.Get());
	};
	Underlying->SetIntPropertyValue(Key, Enum->GetValueByIndex(0));
	const int32 Value = 7;
	Helper.AddPair(Key, &Value);
	TestTrue(TEXT("register the known transient content folder"), FVaRestJsonRules::RegisterTypesFolder(Owner.Get(), TEXT("/Engine")));
#if !UE_BUILD_SHIPPING
	AddExpectedMessagePlain(TEXT("No JSON rules registered for /Engine/Transient.VaRestMissingRulesEnum"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

#endif
	TestTrue(TEXT("map-key-only use still encodes"), FVaRestJsonConvert::PropertyToJson(Map, Address).IsObject());
	TestTrue(TEXT("repeat use does not emit another warning"), FVaRestJsonConvert::PropertyToJson(Map, Address).IsObject());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonAuthoredEnumWireTest, "VaRest.Json.Rules.AuthoredEnumWireNamespace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonAuthoredEnumWireTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonUserStructTest;
	TStrongObjectPtr<UUserDefinedEnum> Enum(Cast<UUserDefinedEnum>(FEnumEditorUtils::CreateUserDefinedEnum(GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UUserDefinedEnum::StaticClass(), TEXT("VaRestAuthoredWireEnum")), RF_Public | RF_Transient)));
	if (!TestNotNull(TEXT("create the real authored-name enum"), Enum.Get()))
		return false;
	FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Enum.Get());
	FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Enum.Get());
	if (!TestTrue(TEXT("two canonical values exist before MAX"), Enum->NumEnums() >= 3))
		return false;
	TStrongObjectPtr<UUserDefinedStruct> Holder(MakeStruct(TEXT("Alive"), TEXT("Score")));
	if (!TestNotNull(TEXT("create the real enum holder"), Holder.Get()))
		return false;
	FEdGraphPinType ScalarType = PinType(UEdGraphSchema_K2::PC_Byte);
	ScalarType.PinSubCategoryObject = Enum.Get();
	if (!TestTrue(TEXT("compile an actual enum scalar"), FStructureEditorUtils::AddVariable(Holder.Get(), ScalarType)))
		return false;
	if (!TestTrue(TEXT("give the scalar a distinct cooked wire name"), FStructureEditorUtils::RenameVariable(Holder.Get(),
		FStructureEditorUtils::GetVarDesc(Holder.Get()).Last().VarGuid, TEXT("EnumValue"))))
		return false;
	FEdGraphPinType MapType = ScalarType;
	MapType.ContainerType = EPinContainerType::Map;
	MapType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Int;
	if (!TestTrue(TEXT("compile an actual enum-key map"), FStructureEditorUtils::AddVariable(Holder.Get(), MapType)))
		return false;
	if (!TestTrue(TEXT("give the map a distinct cooked wire name"), FStructureEditorUtils::RenameVariable(Holder.Get(),
		FStructureEditorUtils::GetVarDesc(Holder.Get()).Last().VarGuid, TEXT("EnumMap"))))
		return false;
	FProperty* Scalar = nullptr;
	for (TFieldIterator<FProperty> It(Holder.Get()); It; ++It)
	{
		if (const FEnumProperty* E = CastField<FEnumProperty>(*It); E && E->GetEnum() == Enum.Get())
			Scalar = *It;
		if (const FByteProperty* B = CastField<FByteProperty>(*It); B && B->Enum == Enum.Get())
			Scalar = *It;
	}
	FMapProperty* Map = FirstField<FMapProperty>(Holder.Get());
	if (!TestTrue(TEXT("real compiled scalar and map properties are present"), Scalar && Map))
		return false;
	const auto Underlying = [](const FProperty* Property) -> const FNumericProperty* {
		if (const FEnumProperty* E = CastField<FEnumProperty>(Property))
			return E->GetUnderlyingProperty();
		return CastField<FNumericProperty>(Property);
	};
	const FNumericProperty* ScalarValue = Underlying(Scalar);
	const FNumericProperty* KeyValue = Underlying(Map->KeyProp);
	if (!TestTrue(TEXT("actual scalar/key numeric storage is available"), ScalarValue && KeyValue))
		return false;
	FStructOnScope Instance(Holder.Get());
	void* ScalarAddress = Scalar->ContainerPtrToValuePtr<void>(Instance.GetStructMemory());
	void* MapAddress = Map->ContainerPtrToValuePtr<void>(Instance.GetStructMemory());
	void* Key = Map->KeyProp->AllocateAndInitializeValue();
	TStrongObjectPtr<UObject> Owner(NewObject<UEnum>());
	ON_SCOPE_EXIT
	{
		Map->KeyProp->DestroyAndFreeValue(Key);
		FVaRestJsonRules::UnregisterOwner(Owner.Get());
	};
	const int64 A = Enum->GetValueByIndex(0), B = Enum->GetValueByIndex(1);
	const FName NativeB = Enum->GetNameByIndex(1);
	const FName DisplayA(*Enum->GetNameStringByIndex(0)), DisplayB(*Enum->GetNameStringByIndex(1));
	FVaRestJsonEnumRule Rule;
	Rule.Enumerator = NativeB;
	Rule.Json.AppendChar(TCHAR(0xFFFD));
	TArray<FVaRestJsonEnumRule> Rules{Rule};
	FString Malformed;
	Malformed.AppendChar(TCHAR(0xD800));
	Enum->DisplayNameMap.Add(DisplayA, FText::AsCultureInvariant(Malformed));
	Enum->DisplayNameMap.Add(DisplayB, FText::AsCultureInvariant(TEXT("second")));
	if (!TestTrue(TEXT("fixture changes the actual counted malformed authored source"), VaRest::Json::EqualWireText(Enum->GetAuthoredNameStringByIndex(0), Malformed)))
		return false;
	FString Why;
	TestFalse(TEXT("malformed unmapped authored fallback is refused before registration"), FVaRestJsonRules::RegisterEnum(Owner.Get(), Enum.Get(), Rules, &Why));
	Enum->DisplayNameMap.Add(DisplayA, FText::AsCultureInvariant(TEXT("first")));
	if (!TestTrue(TEXT("fixture restores the actual well-formed authored source"), VaRest::Json::EqualWireText(Enum->GetAuthoredNameStringByIndex(0), TEXT("first"))))
		return false;
	TestTrue(TEXT("well-formed effective namespace registers"), FVaRestJsonRules::RegisterEnum(Owner.Get(), Enum.Get(), Rules, &Why));
	FVaRestJsonConvertOptions Options;
	Options.Rules = FVaRestJsonRules::Snapshot(Owner.Get());
	Options.bStrict = true;
	const uint64 Generation = FVaRestJsonRules::GetGeneration(Owner.Get());
	Enum->DisplayNameMap.Add(DisplayA, FText::AsCultureInvariant(Malformed));
	if (!TestTrue(TEXT("the retained snapshot sees the actual changed authored source"), VaRest::Json::EqualWireText(Enum->GetAuthoredNameStringByIndex(0), Malformed)))
		return false;
	ScalarValue->SetIntPropertyValue(ScalarAddress, A);
	KeyValue->SetIntPropertyValue(Key, A);
	const int32 Payload = 7;
	FScriptMapHelper MapValues(Map, MapAddress);
	MapValues.AddPair(Key, &Payload);
	AddExpectedErrorPlain(TEXT("effective enum wire names"), EAutomationExpectedErrorFlags::Contains, 2);
	TestFalse(TEXT("a retained snapshot revalidates scalar authored names"), FVaRestJsonConvert::PropertyToJson(Scalar, ScalarAddress, Options).IsValid());
	TestFalse(TEXT("singleton map also refuses repaired authored-name alias"), FVaRestJsonConvert::PropertyToJson(Map, MapAddress, Options).IsValid());
	TArray<uint8> Bytes{99, 100};
	FVaRestJsonError Error;
	TestFalse(TEXT("full UTF-8 walk refuses the changed effective namespace"), FVaRestJsonConvert::StructToUtf8(Holder.Get(), Instance.GetStructMemory(), Bytes, Options, EVaRestJsonFormat::Pretty, &Error));
	TestTrue(TEXT("failed encode preserves all prior caller bytes"), Bytes == TArray<uint8>({99, 100}));
	TestEqual(TEXT("display-only mutation did not replace the captured rules"), FVaRestJsonRules::GetGeneration(Owner.Get()), Generation);
	FVaRestJsonDecodeReport Refusal;
	TestFalse(TEXT("decode also refuses a changed unstable authored namespace"), FVaRestJsonConvert::JsonToProperty(FVaRestJson::From(Rule.Json), Scalar, ScalarAddress, Options, &Refusal));
	TestEqual(TEXT("refused scalar decode preserves the native value"), ScalarValue->GetSignedIntPropertyValue(ScalarAddress), A);
	TestFalse(TEXT("map decode refuses before clearing its old entry"), FVaRestJsonConvert::JsonToProperty(FVaRestJson::Object().Set(Rule.Json, Payload), Map, MapAddress, Options, &Refusal));
	TestTrue(TEXT("refused map decode preserves its old native key"), MapValues.Num() == 1 && KeyValue->GetSignedIntPropertyValue(MapValues.GetKeyPtr(0)) == A);

	const TCHAR FallbackUnits[] = {TEXT('X'), TCHAR(0), TEXT('a')};
	const TCHAR MappedUnits[] = {TEXT('X'), TCHAR(0), TEXT('b')};
	const FString Fallback = FString::ConstructFromPtrSize(FallbackUnits, UE_ARRAY_COUNT(FallbackUnits));
	const FString Mapped = FString::ConstructFromPtrSize(MappedUnits, UE_ARRAY_COUNT(MappedUnits));
	if (!TestTrue(TEXT("both competing wire strings contain the counted interior NUL"), Fallback.Len() == 3 && Mapped.Len() == 3 && Fallback[1] == 0 && Mapped[1] == 0))
		return false;
	Enum->DisplayNameMap.Add(DisplayA, FText::AsCultureInvariant(Fallback));
	if (!TestTrue(TEXT("fixture authored fallback preserves every code unit after NUL"), Enum->GetAuthoredNameStringByIndex(0).Len() == 3 && VaRest::Json::EqualWireText(Enum->GetAuthoredNameStringByIndex(0), Fallback)))
		return false;
	Rules[0].Json = Mapped;
	TestTrue(TEXT("well-formed names differing after NUL remain distinct"), FVaRestJsonRules::RegisterEnum(Owner.Get(), Enum.Get(), Rules, &Why));
	Options.Rules = FVaRestJsonRules::Snapshot(Owner.Get());
	ScalarValue->SetIntPropertyValue(ScalarAddress, B);
	MapValues.EmptyValues();
	KeyValue->SetIntPropertyValue(Key, B);
	MapValues.AddPair(Key, &Payload);
	Bytes.Reset();
	TestTrue(TEXT("serialize the scalar and singleton map including embedded NUL"), FVaRestJsonConvert::StructToUtf8(Holder.Get(), Instance.GetStructMemory(), Bytes, Options, EVaRestJsonFormat::Condensed, &Error));
	FStructOnScope Decoded(Holder.Get());
	TestTrue(TEXT("strict actual UTF-8 parse and property decode preserve mapped identity"), FVaRestJsonConvert::Utf8ToStruct(Bytes, Holder.Get(), Decoded.GetStructMemory(), Options));
	TestEqual(TEXT("the earlier authored fallback cannot steal the mapped scalar"), ScalarValue->GetSignedIntPropertyValue(Scalar->ContainerPtrToValuePtr<void>(Decoded.GetStructMemory())), B);
	FScriptMapHelper DecodedMap(Map, Map->ContainerPtrToValuePtr<void>(Decoded.GetStructMemory()));
	const bool MapIdentityRetained = DecodedMap.Num() == 1 && KeyValue->GetSignedIntPropertyValue(DecodedMap.GetKeyPtr(0)) == B;
	if (!MapIdentityRetained)
	{
		const FString Wire = FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
		const int64 Actual = DecodedMap.Num() ? KeyValue->GetSignedIntPropertyValue(DecodedMap.GetKeyPtr(0)) : MIN_int64;
		const FVaRestJson EncodedMap = FVaRestJsonConvert::PropertyToJson(Map, MapAddress, Options);
		AddInfo(FString::Printf(TEXT("Enum map diagnostics: count=%d expected=%lld actual=%lld source-key=%lld map=%s struct=%s"), DecodedMap.Num(), B, Actual,
			KeyValue->GetSignedIntPropertyValue(MapValues.GetKeyPtr(0)), *EncodedMap.ToString(), *Wire));
	}
	TestTrue(TEXT("the singleton map key retains the mapped enum"), MapIdentityRetained);
	Options.bStrict = false;
	const TCHAR DifferentUnits[] = {TEXT('x'), TCHAR(0), TEXT('c')};
	FString Different = FString::ConstructFromPtrSize(DifferentUnits, UE_ARRAY_COUNT(DifferentUnits));
	if (!TestTrue(TEXT("lenient input contains the counted interior NUL"), Different.Len() == 3 && Different[1] == 0))
		return false;
	FVaRestJsonDecodeReport LenientReport;
	FVaRestJsonConvert::JsonToProperty(FVaRestJson::From(Different), Scalar, ScalarAddress, Options, &LenientReport);
	TestEqual(TEXT("lenient comparison still examines the suffix after NUL"), ScalarValue->GetSignedIntPropertyValue(ScalarAddress), B);
	Different[2] = TEXT('A');
	TestTrue(TEXT("genuine counted case-only authored fallback remains supported"), FVaRestJsonConvert::JsonToProperty(FVaRestJson::From(Different), Scalar, ScalarAddress, Options, &LenientReport));
	TestEqual(TEXT("case-only fallback selects its actual native value"), ScalarValue->GetSignedIntPropertyValue(ScalarAddress), A);
	FVaRestJsonRules::UnregisterOwner(Owner.Get());
	Options.Rules = FVaRestJsonRules::DefaultSnapshot();
	Options.bStrict = true;
	TestTrue(TEXT("ordinary no-rule authored name remains supported"), FVaRestJsonConvert::JsonToProperty(FVaRestJson::From(TEXT("second")), Scalar, ScalarAddress, Options));
	TestEqual(TEXT("ordinary no-rule fallback selects the same native value"), ScalarValue->GetSignedIntPropertyValue(ScalarAddress), B);
	TestTrue(TEXT("no-rule counted NUL fallback is also exact"), FVaRestJsonConvert::JsonToProperty(FVaRestJson::From(Fallback), Scalar, ScalarAddress, Options));
	TestEqual(TEXT("full counted fallback selects A"), ScalarValue->GetSignedIntPropertyValue(ScalarAddress), A);
	return true;
}

#endif
