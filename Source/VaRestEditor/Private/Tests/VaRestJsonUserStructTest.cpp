// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Misc/AutomationTest.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/Package.h"
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
}

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

#endif
