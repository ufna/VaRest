// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonLibrary.h"

#include "Blueprint/BlueprintExceptionInfo.h"
#include "Misc/FileHelper.h"
#include "UObject/UnrealType.h"

bool UVaRestJsonLibrary::ParseJson(const FString& Text, FVaRestJson& Json, FVaRestJsonError& Error)
{
	TValueOrError<FVaRestJson, FVaRestJsonError> Result = FVaRestJson::Parse(FStringView(Text));
	if (Result.HasValue())
	{
		Json = Result.StealValue();
		Error = FVaRestJsonError();
		return true;
	}
	Json = FVaRestJson();
	Error = Result.StealError();
	return false;
}

FString UVaRestJsonLibrary::JsonToString(const FVaRestJson& Json, bool bPretty)
{
	return Json.ToString(bPretty ? EVaRestJsonFormat::Pretty : EVaRestJsonFormat::Condensed);
}

// Load/Save hand Path to FFileHelper as given. A relative path then starts at the working directory, which the engine
// sets to the executable's folder at startup (LaunchEngineLoop.cpp:1801-1804), hence the tooltips' advice.
bool UVaRestJsonLibrary::LoadJsonFile(const FString& Path, FVaRestJson& Json, FVaRestJsonError& Error)
{
	Json = FVaRestJson();
	Error = FVaRestJsonError();
	TArray<uint8> Bytes;
	// Silent: LoadFileToArray would log its own warning (FileHelper.cpp:47-54); the caller gets Error instead
	if (!FFileHelper::LoadFileToArray(Bytes, *Path, FILEREAD_Silent))
	{
		Error.Message = FString::Printf(TEXT("cannot read %s"), *Path);
		return false;
	}
	// Parse (one copy) rather than ParseOwned: a copy keeps the original bytes for line/column in the error
	TValueOrError<FVaRestJson, FVaRestJsonError> Result = FVaRestJson::Parse(TConstArrayView<uint8>(Bytes));
	if (!Result.HasValue())
	{
		Error = Result.StealError();
		return false;
	}
	Json = Result.StealValue();
	return true;
}

bool UVaRestJsonLibrary::SaveJsonFile(const FVaRestJson& Json, const FString& Path, bool bPretty)
{
	if (!Json.IsValid())
	{
		FFrame::KismetExecutionMessage(*FString::Printf(TEXT("Save Json File %s: the Json is invalid"), *Path), ELogVerbosity::Warning);
		return false;
	}
	TArray<uint8> Bytes;
	const TValueOrError<void, FVaRestJsonError> Written = Json.TryToUtf8(Bytes, bPretty ? EVaRestJsonFormat::Pretty : EVaRestJsonFormat::Condensed);
	if (Written.HasError())
	{
		FFrame::KismetExecutionMessage(*FString::Printf(TEXT("Save Json File %s: %s"), *Path, *Written.GetError().Message), ELogVerbosity::Warning);
		return false;
	}
	return FFileHelper::SaveArrayToFile(Bytes, *Path);
}

FVaRestJson UVaRestJsonLibrary::MakeJsonObject()
{
	return FVaRestJson::Object();
}

FVaRestJson UVaRestJsonLibrary::MakeJsonArray()
{
	return FVaRestJson::Array();
}

FVaRestJson UVaRestJsonLibrary::JsonFromBool(bool Value)
{
	return FVaRestJson::From(Value);
}

FVaRestJson UVaRestJsonLibrary::JsonFromNumber(double Value)
{
	// BP numbers are doubles; an integral one is written 3, not 3.0, as JSON.stringify writes it. Within 2^53 the
	// integer is exact.
	if (FMath::IsFinite(Value) && FMath::TruncToDouble(Value) == Value && FMath::Abs(Value) <= 9007199254740992.0)
	{
		return FVaRestJson::From(static_cast<int64>(Value));
	}
	return FVaRestJson::From(Value);
}

FVaRestJson UVaRestJsonLibrary::JsonFromString(const FString& Value)
{
	return FVaRestJson::From(FStringView(Value));
}

FVaRestJson UVaRestJsonLibrary::JsonNull()
{
	return FVaRestJson::Null();
}

EVaRestJsonType UVaRestJsonLibrary::GetJsonType(const FVaRestJson& Json)
{
	return Json.GetType();
}

bool UVaRestJsonLibrary::IsValidJson(const FVaRestJson& Json)
{
	return Json.IsValid();
}

bool UVaRestJsonLibrary::IsNullJson(const FVaRestJson& Json)
{
	return Json.IsNull();
}

FString UVaRestJsonLibrary::GetStringField(const FVaRestJson& Json, const FString& Key, const FString& Default)
{
	return Json.Get(Key).AsString(Default);
}

int32 UVaRestJsonLibrary::GetIntField(const FVaRestJson& Json, const FString& Key, int32 Default)
{
	return Json.Get(Key).AsInt(Default);
}

int64 UVaRestJsonLibrary::GetInt64Field(const FVaRestJson& Json, const FString& Key, int64 Default)
{
	return Json.Get(Key).AsInt64(Default);
}

double UVaRestJsonLibrary::GetNumberField(const FVaRestJson& Json, const FString& Key, double Default)
{
	return Json.Get(Key).AsDouble(Default);
}

bool UVaRestJsonLibrary::GetBoolField(const FVaRestJson& Json, const FString& Key, bool Default)
{
	return Json.Get(Key).AsBool(Default);
}

FVaRestJson UVaRestJsonLibrary::GetObjectField(const FVaRestJson& Json, const FString& Key, const FVaRestJson& Default)
{
	const FVaRestJson Field = Json.Get(Key);
	return Field.IsObject() ? Field : Default;
}

FVaRestJson UVaRestJsonLibrary::GetArrayField(const FVaRestJson& Json, const FString& Key, const FVaRestJson& Default)
{
	const FVaRestJson Field = Json.Get(Key);
	return Field.IsArray() ? Field : Default;
}

TArray<FString> UVaRestJsonLibrary::GetJsonKeys(const FVaRestJson& Json)
{
	return Json.GetKeys();
}

bool UVaRestJsonLibrary::HasField(const FVaRestJson& Json, const FString& Key)
{
	return Json.Has(Key);
}

bool UVaRestJsonLibrary::RemoveField(const FVaRestJson& Json, const FString& Key)
{
	FVaRestJson Target = Json; // a copy of the handle aliases the same object
	return Target.Remove(Key);
}

FVaRestJson UVaRestJsonLibrary::MergeJson(const FVaRestJson& Target, const FVaRestJson& Source)
{
	if (!Target.IsObject() || !Source.IsObject())
	{
		FFrame::KismetExecutionMessage(TEXT("Merge Json: both Target and Source must be objects"), ELogVerbosity::Warning);
		return Target;
	}
	FVaRestJson Result = Target;
	for (const FVaRestJsonMember& Member : Source.Members())
	{
		Result.Set(Member.Key, Member.Value); // copies; later members win, duplicates collapse (spec 2.2)
	}
	return Result;
}

int32 UVaRestJsonLibrary::JsonLength(const FVaRestJson& Json)
{
	return Json.Num();
}

bool UVaRestJsonLibrary::ArrayRemoveAt(const FVaRestJson& Json, int32 Index)
{
	FVaRestJson Target = Json;
	return Target.RemoveAt(Index);
}

FVaRestJson UVaRestJsonLibrary::CloneJson(const FVaRestJson& Json)
{
	return Json.Clone();
}

bool UVaRestJsonLibrary::JsonEquals(const FVaRestJson& A, const FVaRestJson& B)
{
	return A.Equals(B);
}

namespace VaRest::Json::LibraryPrivate
{
	bool IsJsonPin(const FProperty* Property)
	{
		const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
		return StructProperty && StructProperty->Struct == FVaRestJson::StaticStruct();
	}

	/** A connected Json handle stays itself (a missing one stays missing: spec 2.2); any other value is converted */
	FVaRestJson ValueAsJson(const FProperty* Property, const void* Address, EVaRestJsonNaming Naming)
	{
		if (IsJsonPin(Property))
		{
			return *static_cast<const FVaRestJson*>(Address);
		}
		FVaRestJsonConvertOptions Options;
		Options.Naming = Naming;
		return FVaRestJsonConvert::PropertyToJson(Property, Address, Options);
	}

	/**
	 * The type's default, as a fresh Blueprint temporary gets it. Not ClearValue: a Blueprint struct has no C++ struct
	 * ops (UserDefinedStruct.cpp:58-63), so ClearValue's ClearScriptStruct zeroes field by field (Class.cpp:3909-3923)
	 * and loses the struct's authored defaults, which only InitializeStruct copies from its default instance
	 * (UserDefinedStruct.cpp:331-338). InitializeValue also does a whole C-style array (UnrealType.h:1103-1120), where
	 * ClearValue does one element (UnrealType.h:984-1000).
	 */
	void ResetToDefault(const FProperty* Property, void* Address)
	{
		Property->DestroyValue(Address);
		Property->InitializeValue(Address);
	}

	/**
	 * Get Field / Get Path / Array Get. The output is reset first: a BP temporary is reused across loop iterations, so a
	 * miss must not leave the previous call's value. A Json pin receives the handle itself (reference semantics, as
	 * Get Object Field); other types convert leniently and Found says whether they did.
	 */
	bool ReadInto(const FVaRestJson& Source, const FProperty* Property, void* Address)
	{
		ResetToDefault(Property, Address);
		if (!Source.IsValid())
		{
			return false;
		}
		if (IsJsonPin(Property))
		{
			*static_cast<FVaRestJson*>(Address) = Source;
			return true;
		}
		if (Source.IsNull())
		{
			return false; // null is no value for a typed pin (as TryGet): Found false, the output its default
		}
		FVaRestJsonDecodeReport Report; // Found carries the outcome; lenient warnings stay out of the log
		if (FVaRestJsonConvert::JsonToProperty(Source, Property, Address, FVaRestJsonConvertOptions(), &Report))
		{
			return true;
		}
		// A decode can fail after storing part of the value (an element nested past MaxDepth aborts it once earlier
		// elements are in): Found false comes with the default, never half a value
		ResetToDefault(Property, Address);
		return false;
	}

	FString TypeOf(const FVaRestJson& Json)
	{
		return StaticEnum<EVaRestJsonType>()->GetNameStringByValue(static_cast<int64>(Json.GetType()));
	}

	struct FWildcard
	{
		const FProperty* Property = nullptr;
		void* Address = nullptr;
	};

	/**
	 * Steps over the wildcard parameter. From a Blueprint, Step reports the connected property in MostRecentProperty
	 * (JsonBlueprintFunctionLibrary.cpp:84-86 relies on it). Called natively through ProcessEvent there is no bytecode:
	 * StepCompiledIn takes the next property of the parameter chain (Stack.h:479-493) and StepExplicitProperty sets only
	 * the address (ScriptCore.cpp:561-585), so the property is read from the chain before the step.
	 *
	 * Every wildcard is a reference parameter (int32& or const int32&: CPF_OutParm), so the native step only looks the
	 * address up and never writes its result (ScriptCore.cpp:565-578); it still asserts a non-null result where
	 * checkSlow is compiled in (ScriptCore.cpp:563, a Debug engine), so it gets a placeholder. The VM step must get
	 * nullptr: a variable read copies the value into a non-null result (ScriptCore.cpp:2291-2294).
	 */
	FWildcard StepWildcard(FFrame& Stack)
	{
		FProperty* const ChainProperty = Stack.Code ? nullptr : CastField<FProperty>(Stack.PropertyChainForCompiledIn);
		checkf(!ChainProperty || ChainProperty->HasAnyPropertyFlags(CPF_OutParm),
			TEXT("%s: a wildcard parameter must be a reference, or the native step copies it into a placeholder"), *ChainProperty->GetName());
		uint8 Placeholder = 0;
		Stack.MostRecentProperty = nullptr;
		Stack.MostRecentPropertyAddress = nullptr;
		Stack.StepCompiledIn<FProperty>(Stack.Code ? nullptr : &Placeholder);
		FWildcard Result;
		Result.Property = Stack.MostRecentProperty ? Stack.MostRecentProperty : ChainProperty;
		Result.Address = Stack.MostRecentPropertyAddress;
		return Result;
	}

	/** The wildcard pin had no value (a broken graph): a script exception, as the engine's JSON nodes raise (JsonBlueprintFunctionLibrary.cpp:90-97) */
	void ThrowMissingValue(UObject* ActiveObject, FFrame& Stack, const TCHAR* NodeName)
	{
		const FBlueprintExceptionInfo ExceptionInfo(EBlueprintExceptionType::AccessViolation,
			FText::Format(NSLOCTEXT("VaRestJsonLibrary", "MissingValue", "{0}: the Value pin has no value"), FText::FromString(NodeName)));
		FBlueprintCoreDelegates::ThrowScriptException(ActiveObject, Stack, ExceptionInfo);
	}
}

bool UVaRestJsonLibrary::GetFieldImpl(const FVaRestJson& Json, const FString& Key, const FProperty* Property, void* Address)
{
	return VaRest::Json::LibraryPrivate::ReadInto(Json.Get(Key), Property, Address);
}

FVaRestJson UVaRestJsonLibrary::SetFieldImpl(const FVaRestJson& Json, const FString& Key, const FProperty* Property, const void* Address)
{
	using namespace VaRest::Json::LibraryPrivate;
	if (!Json.IsObject())
	{
		// No auto-vivify: the node holds a copy of the handle, so a new object would never reach the caller's variable
		FFrame::KismetExecutionMessage(*FString::Printf(TEXT("Set Field \"%s\": Json is %s, not an object (use Make Json Object)"), *Key, *TypeOf(Json)), ELogVerbosity::Warning);
		return Json;
	}
	const FVaRestJson NewValue = ValueAsJson(Property, Address, EVaRestJsonNaming::CamelCase);
	if (!NewValue.IsValid() && !IsJsonPin(Property))
	{
		return Json; // the encode failed and logged why; the field keeps its value (Set would read "missing" as remove)
	}
	FVaRestJson Target = Json;
	Target.Set(Key, NewValue);
	return Target;
}

bool UVaRestJsonLibrary::GetPathImpl(const FVaRestJson& Json, const FString& Path, const FProperty* Property, void* Address)
{
	return VaRest::Json::LibraryPrivate::ReadInto(Json.Path(Path), Property, Address);
}

bool UVaRestJsonLibrary::ArrayGetImpl(const FVaRestJson& Json, int32 Index, const FProperty* Property, void* Address)
{
	return VaRest::Json::LibraryPrivate::ReadInto(Json.At(Index), Property, Address);
}

FVaRestJson UVaRestJsonLibrary::ArrayAddImpl(const FVaRestJson& Json, const FProperty* Property, const void* Address)
{
	using namespace VaRest::Json::LibraryPrivate;
	if (!Json.IsArray())
	{
		FFrame::KismetExecutionMessage(*FString::Printf(TEXT("Array Add: Json is %s, not an array (use Make Json Array)"), *TypeOf(Json)), ELogVerbosity::Warning);
		return Json;
	}
	const FVaRestJson NewValue = ValueAsJson(Property, Address, EVaRestJsonNaming::CamelCase);
	if (!NewValue.IsValid() && !IsJsonPin(Property))
	{
		return Json; // the encode failed and logged why; nothing is appended (Add would write null)
	}
	FVaRestJson Target = Json;
	Target.Add(NewValue);
	return Target;
}

FVaRestJson UVaRestJsonLibrary::StructToJsonImpl(const FProperty* Property, const void* Address, EVaRestJsonNaming Naming)
{
	return VaRest::Json::LibraryPrivate::ValueAsJson(Property, Address, Naming);
}

bool UVaRestJsonLibrary::JsonToStructImpl(const FVaRestJson& Json, const FProperty* Property, void* Address, EVaRestJsonNaming Naming, bool bStrict)
{
	// The output is a Blueprint temporary: reused across loop iterations, and kept between events in the ubergraph's
	// persistent frame. Start from the type's defaults (ResetToDefault: a Blueprint struct's authored ones too), so
	// "missing fields keep defaults" (spec 2.4) means the type's defaults, never the previous call's values.
	VaRest::Json::LibraryPrivate::ResetToDefault(Property, Address);
	FVaRestJsonConvertOptions Options;
	Options.Naming = Naming;
	Options.bStrict = bStrict;
	FVaRestJsonDecodeReport Report;
	const bool bDecoded = FVaRestJsonConvert::JsonToProperty(Json, Property, Address, Options, &Report);
	if (Report.Errors.Num() + Report.Warnings.Num() > 0)
	{
		const FVaRestJsonError& First = Report.Errors.Num() > 0 ? Report.Errors[0] : Report.Warnings[0];
		FFrame::KismetExecutionMessage(*FString::Printf(TEXT("Json To Struct: %d issue(s); first at '%s': %s"),
			Report.Errors.Num() + Report.Warnings.Num(), *First.Path, *First.Message), ELogVerbosity::Log);
	}
	return bDecoded;
}

FString UVaRestJsonLibrary::StructToJsonStringImpl(const FProperty* Property, const void* Address, EVaRestJsonNaming Naming, bool bPretty)
{
	return StructToJsonImpl(Property, Address, Naming).ToString(bPretty ? EVaRestJsonFormat::Pretty : EVaRestJsonFormat::Condensed);
}

FVaRestJson UVaRestJsonLibrary::MakeJsonInternalObject()
{
	return FVaRestJson::Object();
}

// Thunks: parameters are read in declaration order; the wildcard's real property comes from StepWildcard
// (JsonBlueprintFunctionLibrary.cpp:79-122 does the same with StepCompiledIn alone).

DEFINE_FUNCTION(UVaRestJsonLibrary::execGetField)
{
	P_GET_STRUCT_REF(FVaRestJson, Json);
	P_GET_PROPERTY(FStrProperty, Key);
	const VaRest::Json::LibraryPrivate::FWildcard Wildcard = VaRest::Json::LibraryPrivate::StepWildcard(Stack);
	const FProperty* ValueProperty = Wildcard.Property;
	void* ValueAddress = Wildcard.Address;
	P_FINISH;
	if (!ValueProperty || !ValueAddress)
	{
		VaRest::Json::LibraryPrivate::ThrowMissingValue(P_THIS_OBJECT, Stack, TEXT("Get Field"));
		*static_cast<bool*>(RESULT_PARAM) = false;
		return;
	}
	P_NATIVE_BEGIN;
	*static_cast<bool*>(RESULT_PARAM) = GetFieldImpl(Json, Key, ValueProperty, ValueAddress);
	P_NATIVE_END;
}

DEFINE_FUNCTION(UVaRestJsonLibrary::execSetField)
{
	P_GET_STRUCT_REF(FVaRestJson, Json);
	P_GET_PROPERTY(FStrProperty, Key);
	const VaRest::Json::LibraryPrivate::FWildcard Wildcard = VaRest::Json::LibraryPrivate::StepWildcard(Stack);
	const FProperty* ValueProperty = Wildcard.Property;
	const void* ValueAddress = Wildcard.Address;
	P_FINISH;
	if (!ValueProperty || !ValueAddress)
	{
		VaRest::Json::LibraryPrivate::ThrowMissingValue(P_THIS_OBJECT, Stack, TEXT("Set Field"));
		*static_cast<FVaRestJson*>(RESULT_PARAM) = Json;
		return;
	}
	P_NATIVE_BEGIN;
	*static_cast<FVaRestJson*>(RESULT_PARAM) = SetFieldImpl(Json, Key, ValueProperty, ValueAddress);
	P_NATIVE_END;
}

DEFINE_FUNCTION(UVaRestJsonLibrary::execGetPath)
{
	P_GET_STRUCT_REF(FVaRestJson, Json);
	P_GET_PROPERTY(FStrProperty, Path);
	const VaRest::Json::LibraryPrivate::FWildcard Wildcard = VaRest::Json::LibraryPrivate::StepWildcard(Stack);
	const FProperty* ValueProperty = Wildcard.Property;
	void* ValueAddress = Wildcard.Address;
	P_FINISH;
	if (!ValueProperty || !ValueAddress)
	{
		VaRest::Json::LibraryPrivate::ThrowMissingValue(P_THIS_OBJECT, Stack, TEXT("Get Path"));
		*static_cast<bool*>(RESULT_PARAM) = false;
		return;
	}
	P_NATIVE_BEGIN;
	*static_cast<bool*>(RESULT_PARAM) = GetPathImpl(Json, Path, ValueProperty, ValueAddress);
	P_NATIVE_END;
}

DEFINE_FUNCTION(UVaRestJsonLibrary::execArrayGet)
{
	P_GET_STRUCT_REF(FVaRestJson, Json);
	P_GET_PROPERTY(FIntProperty, Index);
	const VaRest::Json::LibraryPrivate::FWildcard Wildcard = VaRest::Json::LibraryPrivate::StepWildcard(Stack);
	const FProperty* ValueProperty = Wildcard.Property;
	void* ValueAddress = Wildcard.Address;
	P_FINISH;
	if (!ValueProperty || !ValueAddress)
	{
		VaRest::Json::LibraryPrivate::ThrowMissingValue(P_THIS_OBJECT, Stack, TEXT("Array Get"));
		*static_cast<bool*>(RESULT_PARAM) = false;
		return;
	}
	P_NATIVE_BEGIN;
	*static_cast<bool*>(RESULT_PARAM) = ArrayGetImpl(Json, Index, ValueProperty, ValueAddress);
	P_NATIVE_END;
}

DEFINE_FUNCTION(UVaRestJsonLibrary::execArrayAdd)
{
	P_GET_STRUCT_REF(FVaRestJson, Json);
	const VaRest::Json::LibraryPrivate::FWildcard Wildcard = VaRest::Json::LibraryPrivate::StepWildcard(Stack);
	const FProperty* ValueProperty = Wildcard.Property;
	const void* ValueAddress = Wildcard.Address;
	P_FINISH;
	if (!ValueProperty || !ValueAddress)
	{
		VaRest::Json::LibraryPrivate::ThrowMissingValue(P_THIS_OBJECT, Stack, TEXT("Array Add"));
		*static_cast<FVaRestJson*>(RESULT_PARAM) = Json;
		return;
	}
	P_NATIVE_BEGIN;
	*static_cast<FVaRestJson*>(RESULT_PARAM) = ArrayAddImpl(Json, ValueProperty, ValueAddress);
	P_NATIVE_END;
}

DEFINE_FUNCTION(UVaRestJsonLibrary::execStructToJson)
{
	const VaRest::Json::LibraryPrivate::FWildcard Wildcard = VaRest::Json::LibraryPrivate::StepWildcard(Stack);
	const FProperty* ValueProperty = Wildcard.Property;
	const void* ValueAddress = Wildcard.Address;
	P_GET_ENUM(EVaRestJsonNaming, Naming);
	P_FINISH;
	if (!ValueProperty || !ValueAddress)
	{
		VaRest::Json::LibraryPrivate::ThrowMissingValue(P_THIS_OBJECT, Stack, TEXT("Struct To Json"));
		*static_cast<FVaRestJson*>(RESULT_PARAM) = FVaRestJson();
		return;
	}
	P_NATIVE_BEGIN;
	*static_cast<FVaRestJson*>(RESULT_PARAM) = StructToJsonImpl(ValueProperty, ValueAddress, Naming);
	P_NATIVE_END;
}

DEFINE_FUNCTION(UVaRestJsonLibrary::execJsonToStruct)
{
	P_GET_STRUCT_REF(FVaRestJson, Json);
	const VaRest::Json::LibraryPrivate::FWildcard Wildcard = VaRest::Json::LibraryPrivate::StepWildcard(Stack);
	const FProperty* ValueProperty = Wildcard.Property;
	void* ValueAddress = Wildcard.Address;
	P_GET_ENUM(EVaRestJsonNaming, Naming);
	P_GET_UBOOL(bStrict);
	P_FINISH;
	if (!ValueProperty || !ValueAddress)
	{
		VaRest::Json::LibraryPrivate::ThrowMissingValue(P_THIS_OBJECT, Stack, TEXT("Json To Struct"));
		*static_cast<bool*>(RESULT_PARAM) = false;
		return;
	}
	P_NATIVE_BEGIN;
	*static_cast<bool*>(RESULT_PARAM) = JsonToStructImpl(Json, ValueProperty, ValueAddress, Naming, bStrict);
	P_NATIVE_END;
}

DEFINE_FUNCTION(UVaRestJsonLibrary::execStructToJsonString)
{
	const VaRest::Json::LibraryPrivate::FWildcard Wildcard = VaRest::Json::LibraryPrivate::StepWildcard(Stack);
	const FProperty* ValueProperty = Wildcard.Property;
	const void* ValueAddress = Wildcard.Address;
	P_GET_ENUM(EVaRestJsonNaming, Naming);
	P_GET_UBOOL(bPretty);
	P_FINISH;
	if (!ValueProperty || !ValueAddress)
	{
		VaRest::Json::LibraryPrivate::ThrowMissingValue(P_THIS_OBJECT, Stack, TEXT("Struct To Json String"));
		*static_cast<FString*>(RESULT_PARAM) = FString();
		return;
	}
	P_NATIVE_BEGIN;
	*static_cast<FString*>(RESULT_PARAM) = StructToJsonStringImpl(ValueProperty, ValueAddress, Naming, bPretty);
	P_NATIVE_END;
}

DEFINE_FUNCTION(UVaRestJsonLibrary::execMakeJsonInternalSet)
{
	P_GET_STRUCT_REF(FVaRestJson, Json);
	P_GET_PROPERTY(FStrProperty, Key);
	const VaRest::Json::LibraryPrivate::FWildcard Wildcard = VaRest::Json::LibraryPrivate::StepWildcard(Stack);
	const FProperty* ValueProperty = Wildcard.Property;
	const void* ValueAddress = Wildcard.Address;
	P_FINISH;
	if (!ValueProperty || !ValueAddress)
	{
		VaRest::Json::LibraryPrivate::ThrowMissingValue(P_THIS_OBJECT, Stack, TEXT("Make Json"));
		*static_cast<FVaRestJson*>(RESULT_PARAM) = Json;
		return;
	}
	P_NATIVE_BEGIN;
	*static_cast<FVaRestJson*>(RESULT_PARAM) = SetFieldImpl(Json, Key, ValueProperty, ValueAddress);
	P_NATIVE_END;
}
