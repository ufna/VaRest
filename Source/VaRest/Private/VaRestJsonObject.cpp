// Copyright 2014-2019 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonObject.h"

#include "VaRestDefines.h"
#include "VaRestJsonValue.h"

#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include <atomic>

typedef TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>> FCondensedJsonStringWriterFactory;
typedef TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>> FCondensedJsonStringWriter;

UVaRestJsonObject::UVaRestJsonObject(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, JsonObj(MakeShared<FJsonObject>())
{
}

namespace
{
void WarnLegacyParserSelection()
{
	static std::atomic<bool> Warned{false};
	if (!Warned.exchange(true,std::memory_order_relaxed)) UE_LOG(LogVaRest,Warning,TEXT("VaRest 2 uses one JSON parser for legacy objects. The incremental/chunked parser switches are retained for compatibility and are ignored."));
}
}
FVaRestJson UVaRestJsonObject::ToVaRestJson() const { return FVaRestJson::FromJsonObject(JsonObj); }
UVaRestJsonObject* UVaRestJsonObject::FromVaRestJson(const FVaRestJson& Json)
{
	if (!Json.IsObject()) return nullptr;auto Native=Json.ToJsonObject();if (!Native.IsValid()) return nullptr;
	auto* Result=NewObject<UVaRestJsonObject>();Result->SetRootObject(Native);return Result;
}

void UVaRestJsonObject::Reset()
{
	JsonObj = MakeShared<FJsonObject>();
}

TSharedRef<FJsonObject>& UVaRestJsonObject::GetRootObject()
{
	return JsonObj;
}

void UVaRestJsonObject::SetRootObject(const TSharedPtr<FJsonObject>& JsonObject)
{
	if (JsonObject.IsValid())
	{
		JsonObj = JsonObject.ToSharedRef();
	}
	else
	{
		UE_LOG(LogVaRest, Error, TEXT("%s: Trying to set invalid json object as root one. Reset now."), *VA_FUNC_LINE);
		Reset();
	}
}

//////////////////////////////////////////////////////////////////////////
// Serialization

FString UVaRestJsonObject::EncodeJson() const
{
	FString OutputString;
	const auto Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(JsonObj, Writer);

	return OutputString;
}

FString UVaRestJsonObject::EncodeJsonToSingleString() const
{
	FString OutputString;
	const auto Writer = FCondensedJsonStringWriterFactory::Create(&OutputString);
	FJsonSerializer::Serialize(JsonObj, Writer);

	return OutputString;
}

bool UVaRestJsonObject::DecodeJson(const FString& JsonString, bool bUseIncrementalParser)
{
	if (bUseIncrementalParser) WarnLegacyParserSelection();
	return DeserializeFromTCHARBytes(*JsonString,JsonString.Len())>0;
}

//////////////////////////////////////////////////////////////////////////
// FJsonObject API

FString UVaRestJsonObject::GetFieldTypeString(const FString& FieldName) const
{
	if (JsonObj->HasTypedField<EJson::Null>(FieldName))
	{
		return TEXT("Null");
	}
	else if (JsonObj->HasTypedField<EJson::String>(FieldName))
	{
		return TEXT("String");
	}
	else if (JsonObj->HasTypedField<EJson::Number>(FieldName))
	{
		return TEXT("Number");
	}
	else if (JsonObj->HasTypedField<EJson::Boolean>(FieldName))
	{
		return TEXT("Boolean");
	}
	else if (JsonObj->HasTypedField<EJson::Object>(FieldName))
	{
		return TEXT("Object");
	}
	else if (JsonObj->HasTypedField<EJson::Array>(FieldName))
	{
		return TEXT("Array");
	}

	UE_LOG(LogVaRest, Warning, TEXT("Field with name %s type unknown"), *FieldName);
	return "";
}

TArray<FString> UVaRestJsonObject::GetFieldNames() const
{
	TArray<FString> Result;
	Result.Reserve(JsonObj->Values.Num());
	for (const auto& Pair : JsonObj->Values)
	{
		// UE 5.8: FJsonObject keys are UE::TSharedString, not FString
		Result.Emplace(*Pair.Key);
	}

	return Result;
}

bool UVaRestJsonObject::HasField(const FString& FieldName) const
{
	if (FieldName.IsEmpty())
	{
		return false;
	}

	return JsonObj->HasField(FieldName);
}

void UVaRestJsonObject::RemoveField(const FString& FieldName)
{
	if (FieldName.IsEmpty())
	{
		return;
	}

	JsonObj->RemoveField(FieldName);
}

UVaRestJsonValue* UVaRestJsonObject::GetField(const FString& FieldName) const
{
	if (FieldName.IsEmpty())
	{
		return nullptr;
	}

	TSharedPtr<FJsonValue> NewVal = JsonObj->TryGetField(FieldName);
	if (NewVal.IsValid())
	{
		UVaRestJsonValue* NewValue = NewObject<UVaRestJsonValue>();
		NewValue->SetRootValue(NewVal);

		return NewValue;
	}

	return nullptr;
}

void UVaRestJsonObject::SetField(const FString& FieldName, UVaRestJsonValue* JsonValue)
{
	if (FieldName.IsEmpty())
	{
		return;
	}

	JsonObj->SetField(FieldName, JsonValue->GetRootValue());
}

//////////////////////////////////////////////////////////////////////////
// FJsonObject API Helpers (easy to use with simple Json objects)

float UVaRestJsonObject::GetNumberField(const FString& FieldName) const
{
	if (!JsonObj->HasTypedField<EJson::Number>(FieldName))
	{
		UE_LOG(LogVaRest, Warning, TEXT("No field with name %s of type Number"), *FieldName);
		return 0.0f;
	}

	return JsonObj->GetNumberField(FieldName);
}

void UVaRestJsonObject::SetNumberField(const FString& FieldName, float Number)
{
	if (FieldName.IsEmpty())
	{
		return;
	}

	JsonObj->SetNumberField(FieldName, Number);
}

void UVaRestJsonObject::SetNumberFieldDouble(const FString& FieldName, double Number)
{
	if (FieldName.IsEmpty())
	{
		return;
	}

	JsonObj->SetNumberField(FieldName, Number);
}

int32 UVaRestJsonObject::GetIntegerField(const FString& FieldName) const
{
	if (!JsonObj->HasTypedField<EJson::Number>(FieldName))
	{
		UE_LOG(LogVaRest, Warning, TEXT("No field with name %s of type Number"), *FieldName);
		return 0;
	}

	return JsonObj->GetIntegerField(FieldName);
}

void UVaRestJsonObject::SetIntegerField(const FString& FieldName, int32 Number)
{
	if (FieldName.IsEmpty())
	{
		return;
	}

	JsonObj->SetNumberField(FieldName, Number);
}

int64 UVaRestJsonObject::GetInt64Field(const FString& FieldName) const
{
	if (!JsonObj->HasTypedField<EJson::Number>(FieldName))
	{
		UE_LOG(LogVaRest, Warning, TEXT("No field with name %s of type Number"), *FieldName);
		return 0;
	}

	return static_cast<int64>(JsonObj->GetNumberField(FieldName));
}

void UVaRestJsonObject::SetInt64Field(const FString& FieldName, int64 Number)
{
	if (FieldName.IsEmpty())
	{
		return;
	}

	JsonObj->SetNumberField(FieldName, Number);
}

FString UVaRestJsonObject::GetStringField(const FString& FieldName) const
{
	if (!JsonObj->HasTypedField<EJson::String>(FieldName))
	{
		UE_LOG(LogVaRest, Warning, TEXT("No field with name %s of type String"), *FieldName);
		return TEXT("");
	}

	return JsonObj->GetStringField(FieldName);
}

void UVaRestJsonObject::SetStringField(const FString& FieldName, const FString& StringValue)
{
	if (FieldName.IsEmpty())
	{
		return;
	}

	JsonObj->SetStringField(FieldName, StringValue);
}

bool UVaRestJsonObject::GetBoolField(const FString& FieldName) const
{
	if (!JsonObj->HasTypedField<EJson::Boolean>(FieldName))
	{
		UE_LOG(LogVaRest, Warning, TEXT("No field with name %s of type Boolean"), *FieldName);
		return false;
	}

	return JsonObj->GetBoolField(FieldName);
}

void UVaRestJsonObject::SetBoolField(const FString& FieldName, bool InValue)
{
	if (FieldName.IsEmpty())
	{
		return;
	}

	JsonObj->SetBoolField(FieldName, InValue);
}

TArray<UVaRestJsonValue*> UVaRestJsonObject::GetArrayField(const FString& FieldName) const
{
	TArray<UVaRestJsonValue*> OutArray;
	if (FieldName.IsEmpty())
	{
		return OutArray;
	}

	if (!JsonObj->HasTypedField<EJson::Array>(FieldName))
	{
		UE_LOG(LogVaRest, Warning, TEXT("%s: No field with name %s of type Array"), *VA_FUNC_LINE, *FieldName);
		return OutArray;
	}

	TArray<TSharedPtr<FJsonValue>> ValArray = JsonObj->GetArrayField(FieldName);
	for (auto Value : ValArray)
	{
		UVaRestJsonValue* NewValue = NewObject<UVaRestJsonValue>();
		NewValue->SetRootValue(Value);

		OutArray.Add(NewValue);
	}

	return OutArray;
}

void UVaRestJsonObject::SetArrayField(const FString& FieldName, const TArray<UVaRestJsonValue*>& InArray)
{
	if (FieldName.IsEmpty())
	{
		return;
	}

	TArray<TSharedPtr<FJsonValue>> ValArray;

	// Process input array and COPY original values
	for (auto InVal : InArray)
	{
		if (InVal == nullptr)
			continue;

		const TSharedPtr<FJsonValue> JsonVal = InVal->GetRootValue();

		switch (InVal->GetType())
		{
		case EVaJson::None:
			break;

		case EVaJson::Null:
			ValArray.Add(MakeShareable(new FJsonValueNull()));
			break;

		case EVaJson::String:
			ValArray.Add(MakeShareable(new FJsonValueString(JsonVal->AsString())));
			break;

		case EVaJson::Number:
			ValArray.Add(MakeShareable(new FJsonValueNumber(JsonVal->AsNumber())));
			break;

		case EVaJson::Boolean:
			ValArray.Add(MakeShareable(new FJsonValueBoolean(JsonVal->AsBool())));
			break;

		case EVaJson::Array:
			ValArray.Add(MakeShareable(new FJsonValueArray(JsonVal->AsArray())));
			break;

		case EVaJson::Object:
			ValArray.Add(MakeShareable(new FJsonValueObject(JsonVal->AsObject())));
			break;

		default:
			break;
		}
	}

	JsonObj->SetArrayField(FieldName, ValArray);
}

void UVaRestJsonObject::MergeJsonObject(UVaRestJsonObject* InJsonObject, bool Overwrite)
{
	if (!InJsonObject || !InJsonObject->IsValidLowLevel())
	{
		return;
	}

	TArray<FString> Keys = InJsonObject->GetFieldNames();

	for (auto Key : Keys)
	{
		if (Overwrite == false && HasField(Key))
		{
			continue;
		}

		SetField(Key, InJsonObject->GetField(Key));
	}
}

UVaRestJsonObject* UVaRestJsonObject::GetObjectField(const FString& FieldName) const
{
	if (!JsonObj->HasTypedField<EJson::Object>(FieldName))
	{
		UE_LOG(LogVaRest, Warning, TEXT("%s: No field with name %s of type Object"), *VA_FUNC_LINE, *FieldName);
		return nullptr;
	}

	const TSharedPtr<FJsonObject> JsonObjField = JsonObj->GetObjectField(FieldName);

	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();
	OutRestJsonObj->SetRootObject(JsonObjField);

	return OutRestJsonObj;
}

void UVaRestJsonObject::SetObjectField(const FString& FieldName, UVaRestJsonObject* JsonObject)
{
	if (FieldName.IsEmpty() || !JsonObject || !JsonObject->IsValidLowLevel())
	{
		return;
	}

	JsonObj->SetObjectField(FieldName, JsonObject->GetRootObject());
}

void UVaRestJsonObject::SetMapFields_string(const TMap<FString, FString>& Fields)
{
	for (auto& field : Fields)
	{
		SetStringField(field.Key, field.Value);
	}
}

void UVaRestJsonObject::SetMapFields_uint8(const TMap<FString, uint8>& Fields)
{
	SetMapFields_Impl(Fields);
}

void UVaRestJsonObject::SetMapFields_int32(const TMap<FString, int32>& Fields)
{
	SetMapFields_Impl(Fields);
}

void UVaRestJsonObject::SetMapFields_int64(const TMap<FString, int64>& Fields)
{
	SetMapFields_Impl(Fields);
}

void UVaRestJsonObject::SetMapFields_bool(const TMap<FString, bool>& Fields)
{
	SetMapFields_Impl(Fields);
}

//////////////////////////////////////////////////////////////////////////
// Array fields helpers (uniform arrays)

TArray<float> UVaRestJsonObject::GetNumberArrayField(const FString& FieldName) const
{
	return GetTypeArrayField<float>(FieldName);
}

TArray<int32> UVaRestJsonObject::GetIntegerArrayField(const FString& FieldName) const
{
	return GetTypeArrayField<int32>(FieldName);
}

void UVaRestJsonObject::SetNumberArrayField(const FString& FieldName, const TArray<float>& NumberArray)
{
	if (FieldName.IsEmpty())
	{
		return;
	}

	TArray<TSharedPtr<FJsonValue>> EntriesArray;

	for (auto Number : NumberArray)
	{
		EntriesArray.Add(MakeShareable(new FJsonValueNumber(Number)));
	}

	JsonObj->SetArrayField(FieldName, EntriesArray);
}

void UVaRestJsonObject::SetNumberArrayFieldDouble(const FString& FieldName, const TArray<double>& NumberArray)
{
	if (FieldName.IsEmpty())
	{
		return;
	}

	TArray<TSharedPtr<FJsonValue>> EntriesArray;

	for (auto Number : NumberArray)
	{
		EntriesArray.Add(MakeShareable(new FJsonValueNumber(Number)));
	}

	JsonObj->SetArrayField(FieldName, EntriesArray);
}

TArray<FString> UVaRestJsonObject::GetStringArrayField(const FString& FieldName) const
{
	TArray<FString> StringArray;
	if (!JsonObj->HasTypedField<EJson::Array>(FieldName) || FieldName.IsEmpty())
	{
		UE_LOG(LogVaRest, Warning, TEXT("%s: No field with name %s of type Array"), *VA_FUNC_LINE, *FieldName);
		return StringArray;
	}

	const TArray<TSharedPtr<FJsonValue>> JsonArrayValues = JsonObj->GetArrayField(FieldName);
	for (TArray<TSharedPtr<FJsonValue>>::TConstIterator It(JsonArrayValues); It; ++It)
	{
		const auto Value = (*It).Get();
		if (Value->Type != EJson::String)
		{
			UE_LOG(LogVaRest, Error, TEXT("Not String element in array with field name %s"), *FieldName);
		}

		StringArray.Add((*It)->AsString());
	}

	return StringArray;
}

void UVaRestJsonObject::SetStringArrayField(const FString& FieldName, const TArray<FString>& StringArray)
{
	if (FieldName.IsEmpty())
	{
		return;
	}

	TArray<TSharedPtr<FJsonValue>> EntriesArray;
	for (auto String : StringArray)
	{
		EntriesArray.Add(MakeShareable(new FJsonValueString(String)));
	}

	JsonObj->SetArrayField(FieldName, EntriesArray);
}

TArray<bool> UVaRestJsonObject::GetBoolArrayField(const FString& FieldName) const
{
	TArray<bool> BoolArray;
	if (!JsonObj->HasTypedField<EJson::Array>(FieldName) || FieldName.IsEmpty())
	{
		UE_LOG(LogVaRest, Warning, TEXT("%s: No field with name %s of type Array"), *VA_FUNC_LINE, *FieldName);
		return BoolArray;
	}

	const TArray<TSharedPtr<FJsonValue>> JsonArrayValues = JsonObj->GetArrayField(FieldName);
	for (TArray<TSharedPtr<FJsonValue>>::TConstIterator It(JsonArrayValues); It; ++It)
	{
		const auto Value = (*It).Get();
		if (Value->Type != EJson::Boolean)
		{
			UE_LOG(LogVaRest, Error, TEXT("Not Boolean element in array with field name %s"), *FieldName);
		}

		BoolArray.Add((*It)->AsBool());
	}

	return BoolArray;
}

void UVaRestJsonObject::SetBoolArrayField(const FString& FieldName, const TArray<bool>& BoolArray)
{
	if (FieldName.IsEmpty())
	{
		return;
	}

	TArray<TSharedPtr<FJsonValue>> EntriesArray;
	for (auto Boolean : BoolArray)
	{
		EntriesArray.Add(MakeShareable(new FJsonValueBoolean(Boolean)));
	}

	JsonObj->SetArrayField(FieldName, EntriesArray);
}

TArray<UVaRestJsonObject*> UVaRestJsonObject::GetObjectArrayField(const FString& FieldName) const
{
	TArray<UVaRestJsonObject*> OutArray;
	if (!JsonObj->HasTypedField<EJson::Array>(FieldName) || FieldName.IsEmpty())
	{
		UE_LOG(LogVaRest, Warning, TEXT("%s: No field with name %s of type Array"), *VA_FUNC_LINE, *FieldName);
		return OutArray;
	}

	TArray<TSharedPtr<FJsonValue>> ValArray = JsonObj->GetArrayField(FieldName);
	for (const auto& Value : ValArray)
	{
		if (Value->Type != EJson::Object)
		{
			UE_LOG(LogVaRest, Error, TEXT("Not Object element in array with field name %s"), *FieldName);
		}

		TSharedPtr<FJsonObject> NewObj = Value->AsObject();

		UVaRestJsonObject* NewJson = NewObject<UVaRestJsonObject>();
		NewJson->SetRootObject(NewObj);

		OutArray.Add(NewJson);
	}

	return OutArray;
}

void UVaRestJsonObject::SetObjectArrayField(const FString& FieldName, const TArray<UVaRestJsonObject*>& ObjectArray)
{
	if (FieldName.IsEmpty())
	{
		return;
	}

	TArray<TSharedPtr<FJsonValue>> EntriesArray;
	for (auto Value : ObjectArray)
	{
		if (Value == nullptr)
			continue;

		EntriesArray.Add(MakeShareable(new FJsonValueObject(Value->GetRootObject())));
	}

	JsonObj->SetArrayField(FieldName, EntriesArray);
}

//////////////////////////////////////////////////////////////////////////
// Deserialize

int32 UVaRestJsonObject::DeserializeFromUTF8Bytes(const ANSICHAR* Bytes, int32 Size)
{
	if (!Bytes || Size<=0) { Reset();return 0; }
	auto Parsed=FVaRestJson::Parse(MakeArrayView(reinterpret_cast<const uint8*>(Bytes),Size));
	if (Parsed.HasError())
	{
		const auto& Error=Parsed.GetError();Reset();
		UE_LOG(LogVaRest,Warning,TEXT("Legacy JSON decoding failed at %d:%d: %s"),Error.Line,Error.Column,*Error.Message);return 0;
	}
	const auto Object=Parsed.GetValue().ToJsonObject();if (!Object.IsValid()) { Reset();return 0; }
	SetRootObject(Object);return Size;
}

int32 UVaRestJsonObject::DeserializeFromTCHARBytes(const TCHAR* Bytes, int32 Size)
{
	if (!Bytes || Size<=0) { Reset();return 0; }
	auto Parsed=FVaRestJson::Parse(FStringView(Bytes,Size));
	if (Parsed.HasError())
	{
		const auto& Error=Parsed.GetError();Reset();
		UE_LOG(LogVaRest,Warning,TEXT("Legacy JSON decoding failed at %d:%d: %s"),Error.Line,Error.Column,*Error.Message);return 0;
	}
	const auto Object=Parsed.GetValue().ToJsonObject();if (!Object.IsValid()) { Reset();return 0; }
	SetRootObject(Object);return Size;
}

void UVaRestJsonObject::DecodeFromArchive(TUniquePtr<FArchive>& Reader)
{
	Reset();if (!Reader) return;
	FArchive& Ar=*Reader;const int64 Remaining=Ar.TotalSize()-Ar.Tell();
	if (Remaining<0 || Remaining>MAX_int32) { Ar.Close();return; }
	TArray<uint8> Bytes;Bytes.SetNumUninitialized(static_cast<int32>(Remaining));
	if (!Bytes.IsEmpty()) Ar.Serialize(Bytes.GetData(),Bytes.Num());
	const bool ReadOk=!Ar.IsError(),Closed=Ar.Close();if (!ReadOk || !Closed) return;
	// WriteToFile's legacy UTF-16 BOM remains supported; byte sources without a BOM keep strict UTF-8 validation.
	const bool Utf16=Bytes.Num()>=2 && ((Bytes[0]==0xff && Bytes[1]==0xfe) || (Bytes[0]==0xfe && Bytes[1]==0xff));
	if (Utf16)
	{
		if (Bytes.Num()%2) return;FString Text;FFileHelper::BufferToString(Text,Bytes.GetData(),Bytes.Num());DecodeJson(Text,false);
	}
	else DeserializeFromUTF8Bytes(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()),Bytes.Num());
}

//////////////////////////////////////////////////////////////////////////
// Serialize

bool UVaRestJsonObject::WriteToFile(const FString& Path) const
{
	TUniquePtr<FArchive> FileWriter(IFileManager::Get().CreateFileWriter(*Path));if (!FileWriter) return false;
	FArchive& Ar=*FileWriter;UCS2CHAR Bom=UNICODE_BOM;Ar.Serialize(&Bom,sizeof(Bom));
	const FString Text=EncodeJsonToSingleString();const bool Written=WriteStringToArchive(Ar,*Text,Text.Len()) && !Ar.IsError();
	const bool Closed=Ar.Close();return Written && Closed;
}

bool UVaRestJsonObject::WriteToFilePath(const FString& Path, const bool bIsRelativeToProjectDir)
{
	return WriteToFile(bIsRelativeToProjectDir ? FPaths::ProjectDir() / Path : Path);
}

bool UVaRestJsonObject::WriteStringToArchive(FArchive& Ar, const TCHAR* StrPtr, int64 Len)
{
	const auto Src = StringCast<UCS2CHAR>(StrPtr, Len);
	Ar.Serialize(const_cast<UCS2CHAR*>(Src.Get()), Src.Length() * sizeof(UCS2CHAR));

	return true;
}
