// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "K2Node_MacroInstance.h"
#include "K2Node_VaRestMakeJson.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/AutomationTest.h"
#include "Tests/VaRestJsonBlueprintTestUtils.h"
#include "VaRestJson.h"
#include "VaRestJsonLibrary.h"

namespace VaRestJsonBlueprintTest
{
	UK2Node_CallFunction* AddLiteralInt(UEdGraph* Graph, const TCHAR* Value)
	{
		UK2Node_CallFunction* Node = VaRest::Json::BlueprintTest::AddCall(Graph, UKismetSystemLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, MakeLiteralInt));
		Node->FindPinChecked(FName(TEXT("Value")))->DefaultValue = Value;
		return Node;
	}

	UK2Node_CallFunction* AddLibraryCall(UEdGraph* Graph, FName FunctionName)
	{
		return VaRest::Json::BlueprintTest::AddCall(Graph, UVaRestJsonLibrary::StaticClass(), FunctionName);
	}

	FString ReadString(UObject* Instance, const TCHAR* Name)
	{
		const FStrProperty* Property = FindFProperty<FStrProperty>(Instance->GetClass(), FName(Name));
		return Property ? Property->GetPropertyValue_InContainer(Instance) : FString(TEXT("<no property>"));
	}

	int32 ReadInt(UObject* Instance, const TCHAR* Name)
	{
		const FIntProperty* Property = FindFProperty<FIntProperty>(Instance->GetClass(), FName(Name));
		return Property ? Property->GetPropertyValue_InContainer(Instance) : -1;
	}

	FVector ReadVector(UObject* Instance, const TCHAR* Name)
	{
		const FStructProperty* Property = FindFProperty<FStructProperty>(Instance->GetClass(), FName(Name));
		return Property && Property->Struct == TBaseStructure<FVector>::Get() ? *Property->ContainerPtrToValuePtr<FVector>(Instance) : FVector(-999.0);
	}

	TArray<FVector> ReadVectors(UObject* Instance, const TCHAR* Name)
	{
		const FArrayProperty* Property = FindFProperty<FArrayProperty>(Instance->GetClass(), FName(Name));
		const FStructProperty* Inner = Property ? CastField<FStructProperty>(Property->Inner) : nullptr;
		return Inner && Inner->Struct == TBaseStructure<FVector>::Get() ? *Property->ContainerPtrToValuePtr<TArray<FVector>>(Instance) : TArray<FVector>();
	}

	FEdGraphPinType VectorType(EPinContainerType Container = EPinContainerType::None)
	{
		return FEdGraphPinType(UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FVector>::Get(), Container, false, FEdGraphTerminalType());
	}

	UEdGraphPin* PinOf(UEdGraphNode* Node, const TCHAR* Name)
	{
		return Node->FindPinChecked(FName(Name));
	}

	/** A library call with one input given as a pin default (a Key, a Path, an Index) */
	UK2Node_CallFunction* AddLibraryCall(UEdGraph* Graph, FName FunctionName, const TCHAR* PinName, const TCHAR* Value)
	{
		UK2Node_CallFunction* Node = AddLibraryCall(Graph, FunctionName);
		PinOf(Node, PinName)->DefaultValue = Value;
		return Node;
	}

	/** Links each node's then pin to the next node's exec pin */
	bool ExecChain(std::initializer_list<UK2Node*> Nodes)
	{
		const UK2Node* Previous = nullptr;
		for (const UK2Node* Node : Nodes)
		{
			if (!Node || (Previous && !VaRest::Json::BlueprintTest::Link(Previous->GetThenPin(), Node->GetExecPin())))
			{
				return false;
			}
			Previous = Node;
		}
		return Nodes.size() > 1;
	}

	/**
	 * A node of one of the engine's standard macros (For Loop, ...), at the path the editor's own key bindings use
	 * (BaseEditorPerProjectUserSettings.ini:808), loaded as UBlueprintGraphEditor::AddMacroNode does
	 * (BlueprintGraphEditor.cpp:808-809). The macro is set before the pins are made: they are its tunnel's pins
	 * (K2Node_MacroInstance.cpp:109-142).
	 */
	UK2Node_MacroInstance* AddStandardMacro(UEdGraph* Graph, const TCHAR* MacroName)
	{
		UEdGraph* Macro = TSoftObjectPtr<UEdGraph>(FSoftObjectPath(FString::Printf(TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros:%s"), MacroName))).LoadSynchronous();
		if (!Macro)
		{
			return nullptr;
		}
		FGraphNodeCreator<UK2Node_MacroInstance> Creator(*Graph);
		UK2Node_MacroInstance* Node = Creator.CreateNode(false);
		Node->SetMacroGraph(Macro);
		Creator.Finalize();
		return Node;
	}

	/** A macro pin by name; a miss lists the node's pins, so a renamed engine pin is a failure that says what is there */
	UEdGraphPin* MacroPin(FAutomationTestBase& Test, UEdGraphNode* Node, const TCHAR* Name)
	{
		UEdGraphPin* Found = Node->FindPin(FName(Name));
		if (!Found)
		{
			FString Names;
			for (const UEdGraphPin* Each : Node->Pins)
			{
				Names += Each->PinName.ToString() + TEXT(" ");
			}
			Test.AddError(FString::Printf(TEXT("%s has no pin \"%s\"; its pins: %s"), *Node->GetName(), Name, *Names));
		}
		return Found;
	}

	/** Deep JSON equality (FVaRestJson::Equals: keys case-sensitive, member order ignored, 1 == 1.0); the message shows the actual text */
	void TestJson(FAutomationTestBase& Test, const TCHAR* What, const FString& Actual, const TCHAR* Expected)
	{
		const TValueOrError<FVaRestJson, FVaRestJsonError> ActualJson = FVaRestJson::Parse(FStringView(Actual));
		const TValueOrError<FVaRestJson, FVaRestJsonError> ExpectedJson = FVaRestJson::Parse(FStringView(Expected));
		Test.TestTrue(FString::Printf(TEXT("%s: got %s"), What, *Actual), ActualJson.HasValue() && ExpectedJson.HasValue() && ActualJson.GetValue().Equals(ExpectedJson.GetValue()));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonMakeJsonSmokeTest, "VaRest.Json.Blueprint.MakeJsonSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonMakeJsonSmokeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Json::BlueprintTest;
	using namespace VaRestJsonBlueprintTest;
	const FTestBlueprint Test = MakeBlueprint(TEXT("VaRestMakeJsonSmoke"));
	if (!TestTrue(TEXT("test Blueprint"), Test.Blueprint && Test.Graph && Test.Entry) || !TestTrue(TEXT("Result variable"), AddVariable(Test.Blueprint, FName(TEXT("Result")), UEdGraphSchema_K2::PC_String)))
	{
		return false;
	}

	// Run: Result = To String(Make Json { id: 1, ID: 2, name: "Neo" })
	FGraphNodeCreator<UK2Node_VaRestMakeJson> Creator(*Test.Graph);
	UK2Node_VaRestMakeJson* Make = Creator.CreateNode(false);
	Creator.Finalize();
	Make->AddInputPin();
	Make->AddInputPin();
	Make->AddInputPin();
	Make->Entries[0].Key = TEXT("id");
	Make->Entries[1].Key = TEXT("ID"); // Review Focus: differs from "id" only by case
	Make->Entries[2].Key = TEXT("name");
	Make->ReconstructNode();

	UK2Node_CallFunction* Name = AddCall(Test.Graph, UKismetSystemLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, MakeLiteralString));
	Name->FindPinChecked(FName(TEXT("Value")))->DefaultValue = TEXT("Neo");
	UK2Node_CallFunction* ToText = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, JsonToString));
	UK2Node_VariableSet* SetResult = AddVariableSet(Test.Graph, FName(TEXT("Result")));

	TestTrue(TEXT("link id"), Link(AddLiteralInt(Test.Graph, TEXT("1"))->GetReturnValuePin(), Make->GetValuePin(0)));
	TestTrue(TEXT("link ID"), Link(AddLiteralInt(Test.Graph, TEXT("2"))->GetReturnValuePin(), Make->GetValuePin(1)));
	TestTrue(TEXT("link name"), Link(Name->GetReturnValuePin(), Make->GetValuePin(2)));
	TestTrue(TEXT("link Make Json -> To String"), Link(Make->GetJsonPin(), ToText->FindPinChecked(FName(TEXT("Json")))));
	TestTrue(TEXT("link To String -> Result"), Link(ToText->GetReturnValuePin(), SetResult->FindPinChecked(FName(TEXT("Result")))));
	TestTrue(TEXT("link exec"), Link(Test.Entry->FindPinChecked(UEdGraphSchema_K2::PN_Then), SetResult->GetExecPin()));

	FCompilerResultsLog Results;
	if (!TestTrue(FString::Printf(TEXT("compiles with zero errors:\n%s"), *Messages(Results)), Compile(Test.Blueprint, Results)))
	{
		return false;
	}
	TestEqual(TEXT("... and zero warnings"), Results.NumWarnings, 0);
	UObject* Instance = Run(Test.Blueprint);
	if (!TestNotNull(TEXT("Run exists and ran"), Instance))
	{
		return false;
	}
	TestEqualSensitive(TEXT("both case variants reach the JSON, in order"), ReadString(Instance, TEXT("Result")), FString(TEXT("{\"id\":1,\"ID\":2,\"name\":\"Neo\"}")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonWildcardSmokeTest, "VaRest.Json.Blueprint.WildcardSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonWildcardSmokeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Json::BlueprintTest;
	using namespace VaRestJsonBlueprintTest;
	const FTestBlueprint Test = MakeBlueprint(TEXT("VaRestWildcardSmoke"));
	if (!TestTrue(TEXT("test Blueprint"), Test.Blueprint && Test.Graph && Test.Entry)
		|| !TestTrue(TEXT("variables"), AddVariable(Test.Blueprint, FName(TEXT("Result")), UEdGraphSchema_K2::PC_String) && AddVariable(Test.Blueprint, FName(TEXT("Number")), UEdGraphSchema_K2::PC_Int)))
	{
		return false;
	}

	// Run: J = Make Json Object; Set Field(J, "n", 42); Set Field(J, "v", Make Vector(1, 2, 3)); Number = Get Field(J, "n"); Result = To String(J)
	UK2Node_CallFunction* MakeObject = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, MakeJsonObject));
	UK2Node_CallFunction* SetNumberField = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, SetField));
	UK2Node_CallFunction* SetVectorField = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, SetField));
	UK2Node_CallFunction* GetNumberField = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, GetField));
	UK2Node_CallFunction* ToText = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, JsonToString));
	UK2Node_CallFunction* Vector = AddCall(Test.Graph, UKismetMathLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, MakeVector));
	UK2Node_VariableSet* SetNumber = AddVariableSet(Test.Graph, FName(TEXT("Number")));
	UK2Node_VariableSet* SetResult = AddVariableSet(Test.Graph, FName(TEXT("Result")));
	Vector->FindPinChecked(FName(TEXT("X")))->DefaultValue = TEXT("1");
	Vector->FindPinChecked(FName(TEXT("Y")))->DefaultValue = TEXT("2");
	Vector->FindPinChecked(FName(TEXT("Z")))->DefaultValue = TEXT("3");
	SetNumberField->FindPinChecked(FName(TEXT("Key")))->DefaultValue = TEXT("n");
	SetVectorField->FindPinChecked(FName(TEXT("Key")))->DefaultValue = TEXT("v");
	GetNumberField->FindPinChecked(FName(TEXT("Key")))->DefaultValue = TEXT("n");

	UEdGraphPin* Json = MakeObject->GetReturnValuePin();
	TestTrue(TEXT("data links"),
		Link(Json, SetNumberField->FindPinChecked(FName(TEXT("Json"))))
		&& Link(AddLiteralInt(Test.Graph, TEXT("42"))->GetReturnValuePin(), SetNumberField->FindPinChecked(FName(TEXT("Value"))))
		&& Link(Json, SetVectorField->FindPinChecked(FName(TEXT("Json"))))
		&& Link(Vector->GetReturnValuePin(), SetVectorField->FindPinChecked(FName(TEXT("Value"))))
		&& Link(Json, GetNumberField->FindPinChecked(FName(TEXT("Json"))))
		&& Link(GetNumberField->FindPinChecked(FName(TEXT("Value"))), SetNumber->FindPinChecked(FName(TEXT("Number"))))
		&& Link(Json, ToText->FindPinChecked(FName(TEXT("Json"))))
		&& Link(ToText->GetReturnValuePin(), SetResult->FindPinChecked(FName(TEXT("Result")))));
	TestTrue(TEXT("wildcards took the connected types"),
		SetNumberField->FindPinChecked(FName(TEXT("Value")))->PinType.PinCategory == UEdGraphSchema_K2::PC_Int
		&& SetVectorField->FindPinChecked(FName(TEXT("Value")))->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct
		&& GetNumberField->FindPinChecked(FName(TEXT("Value")))->PinType.PinCategory == UEdGraphSchema_K2::PC_Int);
	TestTrue(TEXT("exec chain"),
		Link(Test.Entry->FindPinChecked(UEdGraphSchema_K2::PN_Then), MakeObject->GetExecPin())
		&& Link(MakeObject->GetThenPin(), SetNumberField->GetExecPin())
		&& Link(SetNumberField->GetThenPin(), SetVectorField->GetExecPin())
		&& Link(SetVectorField->GetThenPin(), SetNumber->GetExecPin())
		&& Link(SetNumber->GetThenPin(), SetResult->GetExecPin()));

	FCompilerResultsLog Results;
	if (!TestTrue(FString::Printf(TEXT("compiles with zero errors:\n%s"), *Messages(Results)), Compile(Test.Blueprint, Results)))
	{
		return false;
	}
	TestEqual(TEXT("... and zero warnings"), Results.NumWarnings, 0);
	UObject* Instance = Run(Test.Blueprint);
	if (!TestNotNull(TEXT("Run exists and ran"), Instance))
	{
		return false;
	}
	TestEqual(TEXT("Get Field read the int back through the VM"), ReadInt(Instance, TEXT("Number")), 42);
	const TValueOrError<FVaRestJson, FVaRestJsonError> Parsed = FVaRestJson::Parse(FStringView(ReadString(Instance, TEXT("Result"))));
	if (!TestTrue(TEXT("Result is JSON"), Parsed.HasValue()))
	{
		return false;
	}
	const FVaRestJson& Out = Parsed.GetValue();
	TestTrue(TEXT("Set Field wrote an int and a struct (camelCase fields)"),
		Out[TEXT("n")].AsInt() == 42 && Out.Path(TEXT("v.x")).AsDouble() == 1.0 && Out.Path(TEXT("v.y")).AsDouble() == 2.0 && Out.Path(TEXT("v.z")).AsDouble() == 3.0);
	TestEqual(TEXT("nothing else"), Out.Num(), 2);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonWildcardTypesSmokeTest, "VaRest.Json.Blueprint.WildcardTypesSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonWildcardTypesSmokeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Json::BlueprintTest;
	using namespace VaRestJsonBlueprintTest;
	const FTestBlueprint Test = MakeBlueprint(TEXT("VaRestWildcardTypesSmoke"));
	const FName PlayerName(TEXT("PlayerName")), Pos(TEXT("Pos")), Points(TEXT("Points")), Deep(TEXT("Deep")), Second(TEXT("Second")), Spawn(TEXT("Spawn"));
	const FName Encoded(TEXT("Encoded")), Written(TEXT("Written"));
	if (!TestTrue(TEXT("test Blueprint"), Test.Blueprint && Test.Graph && Test.Entry)
		|| !TestTrue(TEXT("variables"), AddVariable(Test.Blueprint, PlayerName, UEdGraphSchema_K2::PC_String) && AddVariable(Test.Blueprint, Pos, VectorType())
			&& AddVariable(Test.Blueprint, Points, VectorType(EPinContainerType::Array)) && AddVariable(Test.Blueprint, Deep, UEdGraphSchema_K2::PC_Int)
			&& AddVariable(Test.Blueprint, Second, VectorType()) && AddVariable(Test.Blueprint, Spawn, VectorType())
			&& AddVariable(Test.Blueprint, Encoded, UEdGraphSchema_K2::PC_String) && AddVariable(Test.Blueprint, Written, UEdGraphSchema_K2::PC_String)))
	{
		return false;
	}

	// Read, every wildcard typed by what it is connected to:
	//   Doc = Parse Json(text); PlayerName = Get Field(Doc, "name") (string); Pos = Get Field(Doc, "pos") (struct);
	//   Points = Get Field(Doc, "points") (array of structs); Deep = Get Path(Doc, "deep.list[2]") (int);
	//   Second = Array Get(Get Path(Doc, "points") (a Json handle), 1) (struct); Spawn = Json To Struct(Get Object Field(Doc, "spawn"))
	UK2Node_CallFunction* Parse = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, ParseJson), TEXT("Text"),
		TEXT("{\"name\":\"Neo\",\"pos\":{\"x\":1,\"y\":2,\"z\":3},\"points\":[{\"x\":4,\"y\":5,\"z\":6},{\"x\":7,\"y\":8,\"z\":9}],")
		TEXT("\"deep\":{\"list\":[10,20,30]},\"spawn\":{\"x\":-1,\"y\":0.5,\"z\":1000}}"));
	UEdGraphPin* Doc = PinOf(Parse, TEXT("Json"));
	UK2Node_CallFunction* GetName = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, GetField), TEXT("Key"), TEXT("name"));
	UK2Node_CallFunction* GetPos = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, GetField), TEXT("Key"), TEXT("pos"));
	UK2Node_CallFunction* GetPoints = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, GetField), TEXT("Key"), TEXT("points"));
	UK2Node_CallFunction* GetDeep = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, GetPath), TEXT("Path"), TEXT("deep.list[2]"));
	UK2Node_CallFunction* GetPointsJson = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, GetPath), TEXT("Path"), TEXT("points"));
	UK2Node_CallFunction* GetSecond = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, ArrayGet), TEXT("Index"), TEXT("1"));
	UK2Node_CallFunction* GetSpawnJson = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, GetObjectField), TEXT("Key"), TEXT("spawn"));
	UK2Node_CallFunction* Decode = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, JsonToStruct));
	UK2Node_VariableSet* SetName = AddVariableSet(Test.Graph, PlayerName);
	UK2Node_VariableSet* SetPos = AddVariableSet(Test.Graph, Pos);
	UK2Node_VariableSet* SetPoints = AddVariableSet(Test.Graph, Points);
	UK2Node_VariableSet* SetDeep = AddVariableSet(Test.Graph, Deep);
	UK2Node_VariableSet* SetSecond = AddVariableSet(Test.Graph, Second);
	UK2Node_VariableSet* SetSpawn = AddVariableSet(Test.Graph, Spawn);
	TestTrue(TEXT("read links"),
		Link(Doc, PinOf(GetName, TEXT("Json"))) && Link(PinOf(GetName, TEXT("Value")), PinOf(SetName, TEXT("PlayerName")))
		&& Link(Doc, PinOf(GetPos, TEXT("Json"))) && Link(PinOf(GetPos, TEXT("Value")), PinOf(SetPos, TEXT("Pos")))
		&& Link(Doc, PinOf(GetPoints, TEXT("Json"))) && Link(PinOf(GetPoints, TEXT("Value")), PinOf(SetPoints, TEXT("Points")))
		&& Link(Doc, PinOf(GetDeep, TEXT("Json"))) && Link(PinOf(GetDeep, TEXT("Value")), PinOf(SetDeep, TEXT("Deep")))
		&& Link(Doc, PinOf(GetPointsJson, TEXT("Json"))) && Link(PinOf(GetPointsJson, TEXT("Value")), PinOf(GetSecond, TEXT("Json")))
		&& Link(PinOf(GetSecond, TEXT("Value")), PinOf(SetSecond, TEXT("Second")))
		&& Link(Doc, PinOf(GetSpawnJson, TEXT("Json"))) && Link(GetSpawnJson->GetReturnValuePin(), PinOf(Decode, TEXT("Json")))
		&& Link(PinOf(Decode, TEXT("Value")), PinOf(SetSpawn, TEXT("Spawn"))));
	const FEdGraphPinType& PointsType = PinOf(GetPoints, TEXT("Value"))->PinType;
	const FEdGraphPinType& HandleType = PinOf(GetPointsJson, TEXT("Value"))->PinType;
	TestTrue(TEXT("Get Field took an array of vectors"), PointsType.IsArray() && PointsType.PinSubCategoryObject == TBaseStructure<FVector>::Get());
	TestTrue(TEXT("Get Path took a Json handle"), !HandleType.IsContainer() && HandleType.PinSubCategoryObject == FVaRestJson::StaticStruct());
	TestTrue(TEXT("Json To Struct took a vector"), PinOf(Decode, TEXT("Value"))->PinType.PinSubCategoryObject == TBaseStructure<FVector>::Get());

	// Write the variables back: Out = Make Json Object; Set Field(Out, "name", PlayerName); Set Field(Out, "points", Points);
	//   List = Make Json Array; Array Add(List, Second); Array Add(List, PlayerName); Set Field(Out, "list", List) (a Json value);
	//   Set Field(Out, "spawn", Struct To Json(Spawn)); Written = To String(Out); Encoded = Struct To Json String(Pos)
	UK2Node_CallFunction* MakeObject = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, MakeJsonObject));
	UK2Node_CallFunction* SetNameField = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, SetField), TEXT("Key"), TEXT("name"));
	UK2Node_CallFunction* SetPointsField = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, SetField), TEXT("Key"), TEXT("points"));
	UK2Node_CallFunction* MakeList = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, MakeJsonArray));
	UK2Node_CallFunction* AddSecond = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, ArrayAdd));
	UK2Node_CallFunction* AddName = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, ArrayAdd));
	UK2Node_CallFunction* SetListField = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, SetField), TEXT("Key"), TEXT("list"));
	UK2Node_CallFunction* Encode = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, StructToJson));
	UK2Node_CallFunction* SetSpawnField = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, SetField), TEXT("Key"), TEXT("spawn"));
	UK2Node_CallFunction* ToText = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, JsonToString));
	UK2Node_CallFunction* EncodeText = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, StructToJsonString));
	UK2Node_VariableSet* SetWritten = AddVariableSet(Test.Graph, Written);
	UK2Node_VariableSet* SetEncoded = AddVariableSet(Test.Graph, Encoded);
	UEdGraphPin* Out = MakeObject->GetReturnValuePin();
	UEdGraphPin* List = MakeList->GetReturnValuePin();
	TestTrue(TEXT("write links"),
		Link(Out, PinOf(SetNameField, TEXT("Json"))) && Link(PinOf(AddVariableGet(Test.Graph, PlayerName), TEXT("PlayerName")), PinOf(SetNameField, TEXT("Value")))
		&& Link(Out, PinOf(SetPointsField, TEXT("Json"))) && Link(PinOf(AddVariableGet(Test.Graph, Points), TEXT("Points")), PinOf(SetPointsField, TEXT("Value")))
		&& Link(List, PinOf(AddSecond, TEXT("Json"))) && Link(PinOf(AddVariableGet(Test.Graph, Second), TEXT("Second")), PinOf(AddSecond, TEXT("Value")))
		&& Link(List, PinOf(AddName, TEXT("Json"))) && Link(PinOf(AddVariableGet(Test.Graph, PlayerName), TEXT("PlayerName")), PinOf(AddName, TEXT("Value")))
		&& Link(Out, PinOf(SetListField, TEXT("Json"))) && Link(List, PinOf(SetListField, TEXT("Value")))
		&& Link(PinOf(AddVariableGet(Test.Graph, Spawn), TEXT("Spawn")), PinOf(Encode, TEXT("Value")))
		&& Link(Out, PinOf(SetSpawnField, TEXT("Json"))) && Link(Encode->GetReturnValuePin(), PinOf(SetSpawnField, TEXT("Value")))
		&& Link(Out, PinOf(ToText, TEXT("Json"))) && Link(ToText->GetReturnValuePin(), PinOf(SetWritten, TEXT("Written")))
		&& Link(PinOf(AddVariableGet(Test.Graph, Pos), TEXT("Pos")), PinOf(EncodeText, TEXT("Value"))) && Link(EncodeText->GetReturnValuePin(), PinOf(SetEncoded, TEXT("Encoded"))));
	TestTrue(TEXT("exec chain"), ExecChain({ Test.Entry, Parse, SetName, SetPos, SetPoints, SetDeep, SetSecond, Decode, SetSpawn,
		MakeObject, SetNameField, SetPointsField, MakeList, AddSecond, AddName, SetListField, Encode, SetSpawnField, SetWritten, SetEncoded }));

	FCompilerResultsLog Results;
	if (!TestTrue(FString::Printf(TEXT("compiles with zero errors:\n%s"), *Messages(Results)), Compile(Test.Blueprint, Results)))
	{
		return false;
	}
	TestEqual(FString::Printf(TEXT("... and zero warnings:\n%s"), *Messages(Results)), Results.NumWarnings, 0);
	UObject* Instance = Run(Test.Blueprint);
	if (!TestNotNull(TEXT("Run exists and ran"), Instance))
	{
		return false;
	}
	TestEqualSensitive(TEXT("Get Field into a string"), ReadString(Instance, TEXT("PlayerName")), FString(TEXT("Neo")));
	TestEqual(TEXT("Get Field into a struct"), ReadVector(Instance, TEXT("Pos")), FVector(1.0, 2.0, 3.0));
	const TArray<FVector> ReadPoints = ReadVectors(Instance, TEXT("Points"));
	if (TestEqual(TEXT("Get Field into an array of structs"), ReadPoints.Num(), 2))
	{
		TestEqual(TEXT("... first"), ReadPoints[0], FVector(4.0, 5.0, 6.0));
		TestEqual(TEXT("... second"), ReadPoints[1], FVector(7.0, 8.0, 9.0));
	}
	TestEqual(TEXT("Get Path into an int"), ReadInt(Instance, TEXT("Deep")), 30);
	TestEqual(TEXT("Get Path into a Json handle, then Array Get into a struct"), ReadVector(Instance, TEXT("Second")), FVector(7.0, 8.0, 9.0));
	TestEqual(TEXT("Json To Struct"), ReadVector(Instance, TEXT("Spawn")), FVector(-1.0, 0.5, 1000.0));
	TestJson(*this, TEXT("Set Field, Array Add and Struct To Json wrote them back"), ReadString(Instance, TEXT("Written")),
		TEXT("{\"name\":\"Neo\",\"points\":[{\"x\":4,\"y\":5,\"z\":6},{\"x\":7,\"y\":8,\"z\":9}],\"list\":[{\"x\":7,\"y\":8,\"z\":9},\"Neo\"],")
		TEXT("\"spawn\":{\"x\":-1,\"y\":0.5,\"z\":1000}}"));
	TestJson(*this, TEXT("Struct To Json String"), ReadString(Instance, TEXT("Encoded")), TEXT("{\"x\":1,\"y\":2,\"z\":3}"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonGetFieldLoopSmokeTest, "VaRest.Json.Blueprint.GetFieldLoopSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonGetFieldLoopSmokeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Json::BlueprintTest;
	using namespace VaRestJsonBlueprintTest;
	const FTestBlueprint Test = MakeBlueprint(TEXT("VaRestGetFieldLoopSmoke"));
	const FName Lower(TEXT("Lower")), Upper(TEXT("Upper")), LowerText(TEXT("LowerText")), UpperText(TEXT("UpperText")), FoundText(TEXT("FoundText"));
	if (!TestTrue(TEXT("test Blueprint"), Test.Blueprint && Test.Graph && Test.Entry)
		|| !TestTrue(TEXT("variables"), AddVariable(Test.Blueprint, Lower, UEdGraphSchema_K2::PC_Int) && AddVariable(Test.Blueprint, Upper, UEdGraphSchema_K2::PC_Int)
			&& AddVariable(Test.Blueprint, LowerText, UEdGraphSchema_K2::PC_String) && AddVariable(Test.Blueprint, UpperText, UEdGraphSchema_K2::PC_String)
			&& AddVariable(Test.Blueprint, FoundText, UEdGraphSchema_K2::PC_String)))
	{
		return false;
	}

	// Run: Items = Parse Json([{"id":1,"ID":2},{"ID":4},{"id":3}]); Lowers, Uppers, Founds = Make Json Array;
	//   For Loop I in 0..2 { Item = Array Get(Items, I); Lower = Get Field(Item, "id"); Array Add(Lowers, Lower);
	//     Upper = Get Field(Item, "ID"); Array Add(Uppers, Upper); Array Add(Founds, Found of Get Field(Item, "id")) }
	//   Completed: LowerText, UpperText, FoundText = To String(Lowers, Uppers, Founds)
	// Review Focus: the pure Get Field runs again before every node that reads it (KismetCompiler.cpp:2816), into the
	// same output temporary each iteration, so a miss after a hit ("id" on the second item, "ID" on the third) must
	// reset the value, not repeat the last one. "id" and "ID" are two keys.
	UK2Node_MacroInstance* Loop = AddStandardMacro(Test.Graph, TEXT("ForLoop"));
	if (!TestNotNull(TEXT("the engine's For Loop macro"), Loop))
	{
		return false;
	}
	UEdGraphPin* LoopExec = Loop->GetExecPin();
	UEdGraphPin* LoopFirst = MacroPin(*this, Loop, TEXT("FirstIndex"));
	UEdGraphPin* LoopLast = MacroPin(*this, Loop, TEXT("LastIndex"));
	UEdGraphPin* LoopBody = MacroPin(*this, Loop, TEXT("LoopBody"));
	UEdGraphPin* LoopIndex = MacroPin(*this, Loop, TEXT("Index"));
	UEdGraphPin* LoopCompleted = MacroPin(*this, Loop, TEXT("Completed"));
	if (!LoopExec || !LoopFirst || !LoopLast || !LoopBody || !LoopIndex || !LoopCompleted)
	{
		return false;
	}
	LoopFirst->DefaultValue = TEXT("0");
	LoopLast->DefaultValue = TEXT("2");

	UK2Node_CallFunction* Parse = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, ParseJson), TEXT("Text"),
		TEXT("[{\"id\":1,\"ID\":2},{\"ID\":4},{\"id\":3}]"));
	UK2Node_CallFunction* MakeLowers = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, MakeJsonArray));
	UK2Node_CallFunction* MakeUppers = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, MakeJsonArray));
	UK2Node_CallFunction* MakeFounds = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, MakeJsonArray));
	UK2Node_CallFunction* GetItem = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, ArrayGet));
	UK2Node_CallFunction* GetLower = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, GetField), TEXT("Key"), TEXT("id"));
	UK2Node_CallFunction* GetUpper = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, GetField), TEXT("Key"), TEXT("ID"));
	UK2Node_CallFunction* AddLower = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, ArrayAdd));
	UK2Node_CallFunction* AddUpper = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, ArrayAdd));
	UK2Node_CallFunction* AddFound = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, ArrayAdd));
	UK2Node_CallFunction* LowersText = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, JsonToString));
	UK2Node_CallFunction* UppersText = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, JsonToString));
	UK2Node_CallFunction* FoundsText = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, JsonToString));
	UK2Node_VariableSet* SetLower = AddVariableSet(Test.Graph, Lower);
	UK2Node_VariableSet* SetUpper = AddVariableSet(Test.Graph, Upper);
	UK2Node_VariableSet* SetLowerText = AddVariableSet(Test.Graph, LowerText);
	UK2Node_VariableSet* SetUpperText = AddVariableSet(Test.Graph, UpperText);
	UK2Node_VariableSet* SetFoundText = AddVariableSet(Test.Graph, FoundText);
	UEdGraphPin* Item = PinOf(GetItem, TEXT("Value"));
	TestTrue(TEXT("data links"),
		Link(PinOf(Parse, TEXT("Json")), PinOf(GetItem, TEXT("Json"))) && Link(LoopIndex, PinOf(GetItem, TEXT("Index")))
		&& Link(Item, PinOf(GetLower, TEXT("Json"))) && Link(Item, PinOf(GetUpper, TEXT("Json")))
		&& Link(PinOf(GetLower, TEXT("Value")), PinOf(SetLower, TEXT("Lower"))) && Link(PinOf(GetUpper, TEXT("Value")), PinOf(SetUpper, TEXT("Upper")))
		&& Link(MakeLowers->GetReturnValuePin(), PinOf(AddLower, TEXT("Json"))) && Link(PinOf(AddVariableGet(Test.Graph, Lower), TEXT("Lower")), PinOf(AddLower, TEXT("Value")))
		&& Link(MakeUppers->GetReturnValuePin(), PinOf(AddUpper, TEXT("Json"))) && Link(PinOf(AddVariableGet(Test.Graph, Upper), TEXT("Upper")), PinOf(AddUpper, TEXT("Value")))
		&& Link(MakeFounds->GetReturnValuePin(), PinOf(AddFound, TEXT("Json"))) && Link(GetLower->GetReturnValuePin(), PinOf(AddFound, TEXT("Value")))
		&& Link(MakeLowers->GetReturnValuePin(), PinOf(LowersText, TEXT("Json"))) && Link(LowersText->GetReturnValuePin(), PinOf(SetLowerText, TEXT("LowerText")))
		&& Link(MakeUppers->GetReturnValuePin(), PinOf(UppersText, TEXT("Json"))) && Link(UppersText->GetReturnValuePin(), PinOf(SetUpperText, TEXT("UpperText")))
		&& Link(MakeFounds->GetReturnValuePin(), PinOf(FoundsText, TEXT("Json"))) && Link(FoundsText->GetReturnValuePin(), PinOf(SetFoundText, TEXT("FoundText"))));
	TestTrue(TEXT("Array Get took a Json handle"), Item->PinType.PinSubCategoryObject == FVaRestJson::StaticStruct());
	TestTrue(TEXT("exec: before the loop"), ExecChain({ Test.Entry, Parse, MakeLowers, MakeUppers, MakeFounds }) && Link(MakeFounds->GetThenPin(), LoopExec));
	TestTrue(TEXT("exec: the loop body"), Link(LoopBody, SetLower->GetExecPin()) && ExecChain({ SetLower, AddLower, SetUpper, AddUpper, AddFound }));
	TestTrue(TEXT("exec: after the loop"), Link(LoopCompleted, SetLowerText->GetExecPin()) && ExecChain({ SetLowerText, SetUpperText, SetFoundText }));

	FCompilerResultsLog Results;
	if (!TestTrue(FString::Printf(TEXT("compiles with zero errors:\n%s"), *Messages(Results)), Compile(Test.Blueprint, Results)))
	{
		return false;
	}
	TestEqual(FString::Printf(TEXT("... and zero warnings:\n%s"), *Messages(Results)), Results.NumWarnings, 0);
	UObject* Instance = Run(Test.Blueprint);
	if (!TestNotNull(TEXT("Run exists and ran"), Instance))
	{
		return false;
	}
	TestEqualSensitive(TEXT("\"id\" per item: a miss after a hit is 0, not the last hit"), ReadString(Instance, TEXT("LowerText")), FString(TEXT("[1,0,3]")));
	TestEqualSensitive(TEXT("\"ID\" per item: a different key from \"id\""), ReadString(Instance, TEXT("UpperText")), FString(TEXT("[2,4,0]")));
	TestEqualSensitive(TEXT("Found per item"), ReadString(Instance, TEXT("FoundText")), FString(TEXT("[true,false,true]")));
	return true;
}

#endif
