// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "UObject/StructOnScope.h"
#include "VaRestJsonConvertTestTypes.h"
#include "VaRestJsonLibrary.h"

namespace VaRestJsonLibraryWildcardTest
{
	FVaRestJson ParseText(const UTF8CHAR* Text)
	{
		return FVaRestJson::Parse(FUtf8StringView(Text)).StealValue();
	}

	/** A field of a test struct as the (property, address) pair a thunk receives */
	struct FSlot
	{
		const FProperty* Property;
		void* Address;
	};

	template <typename StructType>
	FSlot SlotOf(StructType& Container, const TCHAR* Name)
	{
		const FProperty* Property = FindFProperty<FProperty>(StructType::StaticStruct(), FName(Name));
		return FSlot{ Property, Property->ContainerPtrToValuePtr<void>(&Container) };
	}

	/** Calls a UVaRestJsonLibrary UFUNCTION the way native code reaches a thunk: ProcessEvent on the CDO with a parameter block */
	struct FCall
	{
		UFunction* Function;
		FStructOnScope Params;

		explicit FCall(const TCHAR* Name)
			: Function(UVaRestJsonLibrary::StaticClass()->FindFunctionByName(FName(Name)))
			, Params(Function)
		{
		}

		template <typename T>
		T& Param(const TCHAR* Name)
		{
			return *FindFProperty<FProperty>(Function, FName(Name))->ContainerPtrToValuePtr<T>(Params.GetStructMemory());
		}

		void Invoke()
		{
			GetMutableDefault<UVaRestJsonLibrary>()->ProcessEvent(Function, Params.GetStructMemory());
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonLibraryWildcardImplTest, "VaRest.Json.Library.WildcardImpl",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonLibraryWildcardImplTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonLibraryWildcardTest;
	using L = UVaRestJsonLibrary;
	FVaRestJsonTestAll All;
	const FSlot Int = SlotOf(All, TEXT("Int32Value"));
	const FSlot Items = SlotOf(All, TEXT("Items"));
	const FSlot Json = SlotOf(All, TEXT("Extra"));
	const FSlot Inner = SlotOf(All, TEXT("Inner"));
	const FVaRestJson Source = ParseText(UTF8TEXT("{\"n\":42,\"items\":[{\"id\":1,\"name\":\"a\"},{\"id\":2}],\"o\":{\"x\":true},\"s\":\"str\",\"z\":null}"));

	TestTrue(TEXT("Get Field int"), L::GetFieldImpl(Source, TEXT("n"), Int.Property, Int.Address) && All.Int32Value == 42);
	// Review Focus: a miss resets the output, so a BP loop never sees the previous iteration's value
	TestFalse(TEXT("a missing key is not found"), L::GetFieldImpl(Source, TEXT("missing"), Int.Property, Int.Address));
	TestEqual(TEXT("... and resets the output"), All.Int32Value, 0);
	All.Int32Value = 5;
	TestFalse(TEXT("a value of another type is not found"), L::GetFieldImpl(Source, TEXT("s"), Int.Property, Int.Address));
	TestEqual(TEXT("... and resets the output too"), All.Int32Value, 0);
	TestFalse(TEXT("null into a typed pin is not found"), L::GetFieldImpl(Source, TEXT("z"), Inner.Property, Inner.Address));
	TestTrue(TEXT("Get Field array of structs"), L::GetFieldImpl(Source, TEXT("items"), Items.Property, Items.Address) && All.Items.Num() == 2 && All.Items[1].Id == 2);
	TestTrue(TEXT("Get Field into a Json pin"), L::GetFieldImpl(Source, TEXT("o"), Json.Property, Json.Address) && All.Extra[TEXT("x")].AsBool());
	TestTrue(TEXT("... is a handle into the same document"), All.Extra.GetDocumentInternal() == Source.GetDocumentInternal());
	TestTrue(TEXT("Get Path"), L::GetPathImpl(Source, TEXT("items[1].id"), Int.Property, Int.Address) && All.Int32Value == 2);
	TestTrue(TEXT("Array Get"), L::ArrayGetImpl(Source[TEXT("items")], 0, Inner.Property, Inner.Address) && All.Inner.Id == 1 && All.Inner.Name.Equals(TEXT("a"), ESearchCase::CaseSensitive));

	// The reset covers a whole C-style array member (a C++ caller's property): ClearValue would do only its first element
	const FSlot Fixed = SlotOf(All, TEXT("FixedArray"));
	All.FixedArray[0] = 1;
	All.FixedArray[1] = 2;
	All.FixedArray[2] = 3;
	TestFalse(TEXT("a miss into a fixed array"), L::GetFieldImpl(Source, TEXT("missing"), Fixed.Property, Fixed.Address));
	TestTrue(TEXT("... resets every element"), All.FixedArray[0] == 0 && All.FixedArray[1] == 0 && All.FixedArray[2] == 0);

	// A decode that fails part-way leaves the default too: an element nested past MaxDepth aborts it after the earlier
	// elements were stored, and Found false must not come with half a value
	FVaRestJsonTestTree Tree;
	const FSlot Children = SlotOf(Tree, TEXT("Children"));
	FVaRestJson Deep = FVaRestJson::Object();
	Deep.Set(TEXT("children"), FVaRestJson::Array(1, FVaRestJson::Array()));
	FVaRestJson Cursor = Deep[TEXT("children")].At(1);
	for (int32 Level = 0; Level < VaRest::Json::MaxDepth; ++Level)
	{
		Cursor.Add(FVaRestJson::Array()); // built through Add: only Set/Add can nest past the limit
		Cursor = Cursor.At(0);
	}
	TestFalse(TEXT("a value nested past the depth limit is not found"), L::GetFieldImpl(Deep, TEXT("children"), Children.Property, Children.Address));
	TestEqual(TEXT("... and the elements decoded before the failure are gone"), Tree.Children.Num(), 0);

	FVaRestJson Target = FVaRestJson::Object();
	All.Int32Value = 7;
	L::SetFieldImpl(Target, TEXT("n"), Int.Property, Int.Address);
	L::SetFieldImpl(Target, TEXT("items"), Items.Property, Items.Address);
	All.Extra = FVaRestJson();
	L::SetFieldImpl(Target, TEXT("gone"), Json.Property, Json.Address); // a missing handle is undefined: no key
	TestEqualSensitive(TEXT("Set Field of an int, an array of structs, a missing handle"), Target.ToString(),
		FString(TEXT("{\"n\":7,\"items\":[{\"id\":1,\"name\":\"a\"},{\"id\":2,\"name\":\"\"}]}")));

	FVaRestJson List = FVaRestJson::Array();
	L::ArrayAddImpl(List, Int.Property, Int.Address);
	L::ArrayAddImpl(List, Json.Property, Json.Address); // a missing handle in an array is null (spec 2.2)
	TestEqualSensitive(TEXT("Array Add"), List.ToString(), FString(TEXT("[7,null]")));

	AddExpectedMessagePlain(TEXT("not an object"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	TestFalse(TEXT("Set Field on an invalid handle does not auto-vivify"), L::SetFieldImpl(FVaRestJson(), TEXT("n"), Int.Property, Int.Address).IsValid());

	All.Inner.Id = 3;
	All.Inner.Name = TEXT("x");
	TestEqualSensitive(TEXT("Struct To Json"), L::StructToJsonImpl(Inner.Property, Inner.Address, EVaRestJsonNaming::SnakeCase).ToString(), FString(TEXT("{\"id\":3,\"name\":\"x\"}")));
	TestEqualSensitive(TEXT("Struct To Json String"), L::StructToJsonStringImpl(Inner.Property, Inner.Address, EVaRestJsonNaming::PascalCase, false), FString(TEXT("{\"Id\":3,\"Name\":\"x\"}")));
	All.Inner.Id = 3;
	All.Inner.Name = TEXT("stale");
	TestTrue(TEXT("Json To Struct"), L::JsonToStructImpl(ParseText(UTF8TEXT("{\"id\":9}")), Inner.Property, Inner.Address, EVaRestJsonNaming::CamelCase, false));
	// Review Focus: the Value output is a reused BP temporary; a field the JSON lacks gets the default, not the last value
	TestTrue(TEXT("... starts from defaults, not from the previous call (a reused BP temporary)"), All.Inner.Id == 9 && All.Inner.Name.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonLibraryThunksTest, "VaRest.Json.Library.Thunks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonLibraryThunksTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonLibraryWildcardTest;
	// Native calls through ProcessEvent read each parameter in declaration order (the int32 placeholder is the
	// "connected" type here), so this pins every thunk's parameter order before a Blueprint ever calls it.
	const FVaRestJson Target = FVaRestJson::Object();
	{
		FCall Call(TEXT("SetField"));
		Call.Param<FVaRestJson>(TEXT("Json")) = Target;
		Call.Param<FString>(TEXT("Key")) = TEXT("n");
		Call.Param<int32>(TEXT("Value")) = 42;
		Call.Invoke();
		TestEqual(TEXT("SetField wrote through the handle"), Target[TEXT("n")].AsInt(), 42);
		TestTrue(TEXT("... and returned it"), Call.Param<FVaRestJson>(TEXT("ReturnValue")).IsSameNode(Target));
	}
	{
		FCall Call(TEXT("GetField"));
		Call.Param<FVaRestJson>(TEXT("Json")) = Target;
		Call.Param<FString>(TEXT("Key")) = TEXT("n");
		Call.Param<int32>(TEXT("Value")) = -1;
		Call.Invoke();
		TestTrue(TEXT("GetField"), Call.Param<bool>(TEXT("ReturnValue")) && Call.Param<int32>(TEXT("Value")) == 42);
	}
	{
		FCall Call(TEXT("GetPath"));
		Call.Param<FVaRestJson>(TEXT("Json")) = Target;
		Call.Param<FString>(TEXT("Path")) = TEXT("n");
		Call.Invoke();
		TestTrue(TEXT("GetPath"), Call.Param<bool>(TEXT("ReturnValue")) && Call.Param<int32>(TEXT("Value")) == 42);
	}
	const FVaRestJson List = FVaRestJson::Array(5, 6);
	{
		FCall Call(TEXT("ArrayGet"));
		Call.Param<FVaRestJson>(TEXT("Json")) = List;
		Call.Param<int32>(TEXT("Index")) = 1;
		Call.Invoke();
		TestTrue(TEXT("ArrayGet"), Call.Param<bool>(TEXT("ReturnValue")) && Call.Param<int32>(TEXT("Value")) == 6);
	}
	{
		FCall Call(TEXT("ArrayAdd"));
		Call.Param<FVaRestJson>(TEXT("Json")) = List;
		Call.Param<int32>(TEXT("Value")) = 7;
		Call.Invoke();
		TestEqualSensitive(TEXT("ArrayAdd"), List.ToString(), FString(TEXT("[5,6,7]")));
	}
	{
		FCall Call(TEXT("StructToJson"));
		Call.Param<int32>(TEXT("Value")) = 9;
		Call.Invoke();
		TestEqual(TEXT("StructToJson of any value"), Call.Param<FVaRestJson>(TEXT("ReturnValue")).AsInt(), 9);
	}
	{
		FCall Call(TEXT("StructToJsonString"));
		Call.Param<int32>(TEXT("Value")) = 5;
		Call.Invoke();
		TestEqualSensitive(TEXT("StructToJsonString"), Call.Param<FString>(TEXT("ReturnValue")), FString(TEXT("5")));
	}
	{
		FCall Call(TEXT("JsonToStruct"));
		Call.Param<FVaRestJson>(TEXT("Json")) = FVaRestJson::From(3);
		Call.Invoke();
		TestTrue(TEXT("JsonToStruct"), Call.Param<bool>(TEXT("ReturnValue")) && Call.Param<int32>(TEXT("Value")) == 3);
	}
	{
		FCall Call(TEXT("MakeJsonInternalSet"));
		Call.Param<FVaRestJson>(TEXT("Json")) = FVaRestJson::Object();
		Call.Param<FString>(TEXT("Key")) = TEXT("k");
		Call.Param<int32>(TEXT("Value")) = 1;
		Call.Invoke();
		TestEqualSensitive(TEXT("MakeJsonInternalSet"), Call.Param<FVaRestJson>(TEXT("ReturnValue")).ToString(), FString(TEXT("{\"k\":1}")));
	}
	return true;
}

#endif
