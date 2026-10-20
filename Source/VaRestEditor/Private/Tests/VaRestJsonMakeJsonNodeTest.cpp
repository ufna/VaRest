// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "K2Node_VaRestMakeJson.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/AutomationTest.h"
#include "Tests/VaRestJsonBlueprintTestUtils.h"
#include "VaRestJson.h"
#include "VaRestJsonLibrary.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonMakeJsonNodeTest, "VaRest.Json.Blueprint.MakeJsonNode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonMakeJsonNodeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Json::BlueprintTest;
	const FTestBlueprint Test = MakeBlueprint(TEXT("VaRestMakeJsonNodeTest"));
	if (!TestTrue(TEXT("test Blueprint"), Test.Blueprint && Test.Graph && Test.Entry))
	{
		return false;
	}
	FGraphNodeCreator<UK2Node_VaRestMakeJson> Creator(*Test.Graph);
	UK2Node_VaRestMakeJson* Node = Creator.CreateNode(false);
	Creator.Finalize();

	const UEdGraphPin* JsonPin = Node->GetJsonPin();
	TestTrue(TEXT("the output is a VaRest Json"), JsonPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct && JsonPin->PinType.PinSubCategoryObject == FVaRestJson::StaticStruct());
	TestEqual(TEXT("a new node has no keys"), Node->Entries.Num(), 0);

	Node->AddInputPin();
	Node->AddInputPin();
	TestEqualSensitive(TEXT("added keys are unique"), Node->Entries[0].Key + TEXT(",") + Node->Entries[1].Key, FString(TEXT("key,key2")));

	// Review Focus: keys that differ only by case are two pins and two JSON keys
	Node->Entries[0].Key = TEXT("id");
	Node->Entries[1].Key = TEXT("ID");
	Node->ReconstructNode();
	UEdGraphPin* IdPin = Node->GetValuePin(0);
	UEdGraphPin* UpperPin = Node->GetValuePin(1);
	if (!TestTrue(TEXT("\"id\" and \"ID\" are two pins"), IdPin && UpperPin && IdPin != UpperPin))
	{
		return false;
	}
	TestEqualSensitive(TEXT("a pin shows its key"), IdPin->PinFriendlyName.ToString() + TEXT(",") + UpperPin->PinFriendlyName.ToString(), FString(TEXT("id,ID")));
	TestTrue(TEXT("a new value pin is a wildcard"), IdPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard);

	UK2Node_CallFunction* Literal = AddCall(Test.Graph, UKismetSystemLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, MakeLiteralInt));
	TestTrue(TEXT("connect an int"), Link(Literal->GetReturnValuePin(), IdPin));
	TestTrue(TEXT("the pin takes the connected type"), IdPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int);
	Node->ReconstructNode();
	IdPin = Node->GetValuePin(0);
	TestTrue(TEXT("reconstruction keeps the type and the link"), IdPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int && IdPin->LinkedTo.Num() == 1);
	// The editor's Break Link(s) path: the schema notifies the pin's own node (EdGraphSchema.cpp:686-690), while
	// UEdGraphPin::BreakAllPinLinks(true) notifies only the nodes on the other end (EdGraphPin.cpp:713-718)
	GetDefault<UEdGraphSchema_K2>()->BreakPinLinks(*IdPin, true);
	TestTrue(TEXT("disconnecting makes it a wildcard again"), IdPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard);

	FString Reason;
	TestTrue(TEXT("an exec pin cannot be a value"), Node->IsConnectionDisallowed(IdPin, Test.Entry->FindPinChecked(UEdGraphSchema_K2::PN_Then), Reason));
	// Objects are never converted (IsSupported, VaRestJsonConvertNames.cpp:458-498), also as a map's values; a map pin's
	// category is its key type, the value type is in PinValueType
	FEdGraphTerminalType ObjectValue;
	ObjectValue.TerminalCategory = UEdGraphSchema_K2::PC_Object;
	ObjectValue.TerminalSubCategoryObject = UObject::StaticClass();
	FEdGraphTerminalType IntValue;
	IntValue.TerminalCategory = UEdGraphSchema_K2::PC_Int;
	UEdGraphPin* ObjectPin = Literal->CreatePin(EGPD_Output, FEdGraphPinType(UEdGraphSchema_K2::PC_Object, NAME_None, UObject::StaticClass(), EPinContainerType::None, false, FEdGraphTerminalType()), FName(TEXT("TestObject")));
	UEdGraphPin* ObjectMapPin = Literal->CreatePin(EGPD_Output, FEdGraphPinType(UEdGraphSchema_K2::PC_String, NAME_None, nullptr, EPinContainerType::Map, false, ObjectValue), FName(TEXT("TestObjectMap")));
	UEdGraphPin* IntMapPin = Literal->CreatePin(EGPD_Output, FEdGraphPinType(UEdGraphSchema_K2::PC_String, NAME_None, nullptr, EPinContainerType::Map, false, IntValue), FName(TEXT("TestIntMap")));
	TestTrue(TEXT("an object cannot be a value"), Node->IsConnectionDisallowed(IdPin, ObjectPin, Reason));
	TestTrue(TEXT("nor a map of objects"), Node->IsConnectionDisallowed(IdPin, ObjectMapPin, Reason));
	TestFalse(TEXT("a map of numbers can"), Node->IsConnectionDisallowed(IdPin, IntMapPin, Reason));
	Literal->RemovePin(ObjectPin);
	Literal->RemovePin(ObjectMapPin);
	Literal->RemovePin(IntMapPin);

	FCompilerResultsLog Log;
	Log.SetSilentMode(true);
	Node->ValidateNodeDuringCompilation(Log);
	TestEqual(TEXT("two unconnected values are two errors"), Log.NumErrors, 2);
	Node->Entries[1].Key = TEXT("id");
	FCompilerResultsLog DuplicateLog;
	DuplicateLog.SetSilentMode(true);
	Node->ValidateNodeDuringCompilation(DuplicateLog);
	TestEqual(TEXT("an exact duplicate key is one more error"), DuplicateLog.NumErrors, 3);

	// A key is printed as written, "@@" included: text after the last "@@" argument is plain (CompilerResultsLog.h:347-372)
	Node->Entries[1].Key = TEXT("a@@b");
	FCompilerResultsLog KeyTextLog;
	KeyTextLog.SetSilentMode(true);
	Node->ValidateNodeDuringCompilation(KeyTextLog);
	TestTrue(TEXT("the message names the key as written"), Messages(KeyTextLog).Contains(TEXT("connect a value for the key \"a@@b\""), ESearchCase::CaseSensitive));

	const FName FirstPinName = IdPin->PinName;
	Node->RemoveInputPin(IdPin);
	TestTrue(TEXT("Remove key drops the entry and its pin"), Node->Entries.Num() == 1 && Node->FindPin(FirstPinName) == nullptr && Node->GetValuePin(0) != nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonMakeJsonNodeCompileTest, "VaRest.Json.Blueprint.MakeJsonNodeCompile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonMakeJsonNodeCompileTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Json::BlueprintTest;
	const FTestBlueprint Test = MakeBlueprint(TEXT("VaRestMakeJsonNodeCompileTest"));
	const FName OutName(TEXT("Out"));
	if (!TestTrue(TEXT("test Blueprint"), Test.Blueprint && Test.Graph && Test.Entry && AddVariable(Test.Blueprint, OutName, UEdGraphSchema_K2::PC_String)))
	{
		return false;
	}
	// Run: Out = To String (Json) (Make Json {id: 5})
	FGraphNodeCreator<UK2Node_VaRestMakeJson> Creator(*Test.Graph);
	UK2Node_VaRestMakeJson* Node = Creator.CreateNode(false);
	Creator.Finalize();
	Node->AddInputPin();
	Node->Entries[0].Key = TEXT("id");
	Node->ReconstructNode();
	UK2Node_CallFunction* Literal = AddCall(Test.Graph, UKismetSystemLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, MakeLiteralInt));
	Literal->FindPinChecked(FName(TEXT("Value")))->DefaultValue = TEXT("5");
	UK2Node_CallFunction* ToText = AddCall(Test.Graph, UVaRestJsonLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, JsonToString));
	UK2Node_VariableSet* SetOut = AddVariableSet(Test.Graph, OutName);
	if (!TestTrue(TEXT("wire the graph"), Link(Literal->GetReturnValuePin(), Node->GetValuePin(0)) && Link(Node->GetJsonPin(), ToText->FindPinChecked(FName(TEXT("Json"))))
		&& Link(ToText->GetReturnValuePin(), SetOut->FindPinChecked(OutName)) && Link(Test.Entry->FindPinChecked(UEdGraphSchema_K2::PN_Then), SetOut->GetExecPin())))
	{
		return false;
	}

	// The compiler validates nodes after expanding them, when the value links are gone: a connected node must not
	// report "connect a value" then
	FCompilerResultsLog Results;
	const bool bCompiled = Compile(Test.Blueprint, Results);
	if (!TestTrue(FString::Printf(TEXT("a connected node compiles cleanly: %s"), *Messages(Results)), bCompiled))
	{
		return false;
	}
	UObject* Instance = Run(Test.Blueprint);
	const FStrProperty* OutProperty = FindFProperty<FStrProperty>(Test.Blueprint->GeneratedClass, OutName);
	if (TestTrue(TEXT("Run ran"), Instance && OutProperty))
	{
		TestEqualSensitive(TEXT("the expansion builds the object"), OutProperty->GetPropertyValue_InContainer(Instance), FString(TEXT("{\"id\":5}")));
	}

	// Errors are reported once, while the links are still there (ExpandNode), not again after the expansion
	GetDefault<UEdGraphSchema_K2>()->BreakPinLinks(*Node->GetValuePin(0), true);
	FCompilerResultsLog Unconnected;
	const bool bUnconnectedCompiled = Compile(Test.Blueprint, Unconnected);
	TestFalse(TEXT("an unconnected value fails the compile"), bUnconnectedCompiled);
	TestEqual(FString::Printf(TEXT("... with one error: %s"), *Messages(Unconnected)), Unconnected.NumErrors, 1);
	TestTrue(TEXT("... naming the key"), Messages(Unconnected).Contains(TEXT("connect a value for the key \"id\""), ESearchCase::CaseSensitive));
	return true;
}

#endif
