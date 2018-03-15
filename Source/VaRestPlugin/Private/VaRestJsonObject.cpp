// Copyright 2014 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonObject.h"
#include "VaRestJsonParser.h"
#include "VaRestJsonValue.h"
#include "VaRestPluginPrivatePCH.h"

typedef TJsonWriterFactory< TCHAR, TCondensedJsonPrintPolicy<TCHAR> > FCondensedJsonStringWriterFactory;
typedef TJsonWriter< TCHAR, TCondensedJsonPrintPolicy<TCHAR> > FCondensedJsonStringWriter;

UVaRestJsonObject::UVaRestJsonObject(const class FObjectInitializer& PCIP)
	: Super(PCIP)
{
	Reset();
}

UVaRestJsonObject* UVaRestJsonObject::ConstructJsonObject(UObject* WorldContextObject)
{
	return NewObject<UVaRestJsonObject>();
}

void UVaRestJsonObject::Reset()
{
	if (JsonObj.IsValid())
	{
		JsonObj.Reset();
	}

	JsonObj = MakeShareable(new FJsonObject());
}

TSharedPtr<FJsonObject>& UVaRestJsonObject::GetRootObject()
{
	return JsonObj;
}

void UVaRestJsonObject::SetRootObject(TSharedPtr<FJsonObject>& JsonObject)
{
	if (JsonObject.IsValid())
	{
		JsonObj = JsonObject;
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
	if (!JsonObj.IsValid())
	{
		return TEXT("");
	}

	FString OutputString;
	TSharedRef< FCondensedJsonStringWriter > Writer = FCondensedJsonStringWriterFactory::Create(&OutputString);
	FJsonSerializer::Serialize(JsonObj.ToSharedRef(), Writer);

	return OutputString;
}

FString UVaRestJsonObject::EncodeJsonToSingleString() const
{
	FString OutputString = EncodeJson();

	// Remove line terminators
	OutputString.Replace(LINE_TERMINATOR, TEXT(""));
	
	// Remove tabs
	OutputString.Replace(LINE_TERMINATOR, TEXT("\t"));

	return OutputString;
}

bool UVaRestJsonObject::DecodeJson(const FString& JsonString)
{
	DeserializeFromTCHARBytes(JsonString.GetCharArray().GetData(), JsonString.Len());
	
	if (JsonObj.IsValid())
	{
		return true;
	}

	// If we've failed to deserialize the string, we should clear our internal data
	Reset();

	UE_LOG(LogVaRest, Error, TEXT("Json decoding failed for: %s"), *JsonString);

	return false;
}


//////////////////////////////////////////////////////////////////////////
// FJsonObject API

TArray<FString> UVaRestJsonObject::GetFieldNames()
{
	TArray<FString> Result;
	
	if (!JsonObj.IsValid())
	{
		return Result;
	}
	
	JsonObj->Values.GetKeys(Result);
	
	return Result;
}

bool UVaRestJsonObject::HasField(const FString& FieldName) const
{
	if (!JsonObj.IsValid() || FieldName.IsEmpty())
	{
		return false;
	}

	return JsonObj->HasField(FieldName);
}

void UVaRestJsonObject::RemoveField(const FString& FieldName)
{
	if (!JsonObj.IsValid() || FieldName.IsEmpty())
	{
		return;
	}

	JsonObj->RemoveField(FieldName);
}

UVaRestJsonValue* UVaRestJsonObject::GetField(const FString& FieldName) const
{
	if (!JsonObj.IsValid() || FieldName.IsEmpty())
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
	if (!JsonObj.IsValid() || FieldName.IsEmpty())
	{
		return;
	}

	JsonObj->SetField(FieldName, JsonValue->GetRootValue());
}


//////////////////////////////////////////////////////////////////////////
// FJsonObject API Helpers (easy to use with simple Json objects)

float UVaRestJsonObject::GetNumberField(const FString& FieldName) const
{
	if (!JsonObj.IsValid() || !JsonObj->HasTypedField<EJson::Number>(FieldName))
	{
		UE_LOG(LogVaRest, Warning, TEXT("No field with name %s of type Number"), *FieldName);
		return 0.0f;
	}

	return JsonObj->GetNumberField(FieldName);
}

void UVaRestJsonObject::SetNumberField(const FString& FieldName, float Number)
{
	if (!JsonObj.IsValid() || FieldName.IsEmpty())
	{
		return;
	}

	JsonObj->SetNumberField(FieldName, Number);
}

int32 UVaRestJsonObject::GetIntegerField(const FString& FieldName) const
{
	if (!JsonObj.IsValid() || !JsonObj->HasTypedField<EJson::Number>(FieldName))
	{
		UE_LOG(LogVaRest, Warning, TEXT("No field with name %s of type Number"), *FieldName);
		return 0;
	}

	return JsonObj->GetIntegerField(FieldName);
}

void UVaRestJsonObject::SetIntegerField(const FString& FieldName, int32 Number)
{
	if (!JsonObj.IsValid() || FieldName.IsEmpty())
	{
		return;
	}

	JsonObj->SetNumberField(FieldName, Number);
}

FString UVaRestJsonObject::GetStringField(const FString& FieldName) const
{
	if (!JsonObj.IsValid() || !JsonObj->HasTypedField<EJson::String>(FieldName))
	{
		UE_LOG(LogVaRest, Warning, TEXT("No field with name %s of type String"), *FieldName);
		return TEXT("");
	}
		
	return JsonObj->GetStringField(FieldName);
}

void UVaRestJsonObject::SetStringField(const FString& FieldName, const FString& StringValue)
{
	if (!JsonObj.IsValid() || FieldName.IsEmpty())
	{
		return;
	}

	JsonObj->SetStringField(FieldName, StringValue);
}

bool UVaRestJsonObject::GetBoolField(const FString& FieldName) const
{
	if (!JsonObj.IsValid() || !JsonObj->HasTypedField<EJson::Boolean>(FieldName))
	{
		UE_LOG(LogVaRest, Warning, TEXT("No field with name %s of type Boolean"), *FieldName);
		return false;
	}

	return JsonObj->GetBoolField(FieldName);
}

void UVaRestJsonObject::SetBoolField(const FString& FieldName, bool InValue)
{
	if (!JsonObj.IsValid() || FieldName.IsEmpty())
	{
		return;
	}

	JsonObj->SetBoolField(FieldName, InValue);
}

TArray<UVaRestJsonValue*> UVaRestJsonObject::GetArrayField(const FString& FieldName) const
{
	TArray<UVaRestJsonValue*> OutArray;
	if (!JsonObj.IsValid() || FieldName.IsEmpty())
	{
		return OutArray;
	}

	if (!JsonObj->HasTypedField<EJson::Array>(FieldName))
	{
		UE_LOG(LogVaRest, Warning, TEXT("%s: No field with name %s of type Array"), *VA_FUNC_LINE, *FieldName);
		return OutArray;
	}

	TArray< TSharedPtr<FJsonValue> > ValArray = JsonObj->GetArrayField(FieldName);
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
	if (!JsonObj.IsValid() || FieldName.IsEmpty())
	{
		return;
	}

	TArray< TSharedPtr<FJsonValue> > ValArray;

	// Process input array and COPY original values
	for (auto InVal : InArray)
	{
		TSharedPtr<FJsonValue> JsonVal = InVal->GetRootValue();

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
	if (!JsonObj.IsValid() || !JsonObj->HasTypedField<EJson::Object>(FieldName))
	{
		UE_LOG(LogVaRest, Warning, TEXT("%s: No field with name %s of type Object"), *VA_FUNC_LINE, *FieldName);
		return nullptr;
	}

	TSharedPtr<FJsonObject> JsonObjField = JsonObj->GetObjectField(FieldName);

	UVaRestJsonObject* OutRestJsonObj = NewObject<UVaRestJsonObject>();
	OutRestJsonObj->SetRootObject(JsonObjField);

	return OutRestJsonObj;
}

void UVaRestJsonObject::SetObjectField(const FString& FieldName, UVaRestJsonObject* JsonObject)
{
	if (!JsonObj.IsValid() || FieldName.IsEmpty() || !JsonObject || !JsonObject->IsValidLowLevel())
	{
		return;
	}

	JsonObj->SetObjectField(FieldName, JsonObject->GetRootObject());
}


//////////////////////////////////////////////////////////////////////////
// Array fields helpers (uniform arrays)

TArray<float> UVaRestJsonObject::GetNumberArrayField(const FString& FieldName) const
{
	TArray<float> NumberArray;
	if (!JsonObj.IsValid() || !JsonObj->HasTypedField<EJson::Array>(FieldName) || FieldName.IsEmpty())
	{
		UE_LOG(LogVaRest, Warning, TEXT("%s: No field with name %s of type Array"), *VA_FUNC_LINE, *FieldName);
		return NumberArray;
	}

	TArray<TSharedPtr<FJsonValue> > JsonArrayValues = JsonObj->GetArrayField(FieldName);
	for (TArray<TSharedPtr<FJsonValue> >::TConstIterator It(JsonArrayValues); It; ++It)
	{
		auto Value = (*It).Get();
		if (Value->Type != EJson::Number)
		{
			UE_LOG(LogVaRest, Error, TEXT("Not Number element in array with field name %s"), *FieldName);
		}
		
		NumberArray.Add((*It)->AsNumber());
	}

	return NumberArray;
}

void UVaRestJsonObject::SetNumberArrayField(const FString& FieldName, const TArray<float>& NumberArray)
{
	if (!JsonObj.IsValid() || FieldName.IsEmpty())
	{
		return;
	}

	TArray< TSharedPtr<FJsonValue> > EntriesArray;

	for (auto Number : NumberArray)
	{
		EntriesArray.Add(MakeShareable(new FJsonValueNumber(Number)));
	}

	JsonObj->SetArrayField(FieldName, EntriesArray);
}

TArray<FString> UVaRestJsonObject::GetStringArrayField(const FString& FieldName) const
{
	TArray<FString> StringArray;
	if (!JsonObj.IsValid() || !JsonObj->HasTypedField<EJson::Array>(FieldName) || FieldName.IsEmpty())
	{
		UE_LOG(LogVaRest, Warning, TEXT("%s: No field with name %s of type Array"), *VA_FUNC_LINE, *FieldName);
		return StringArray;
	}

	TArray<TSharedPtr<FJsonValue> > JsonArrayValues = JsonObj->GetArrayField(FieldName);
	for (TArray<TSharedPtr<FJsonValue> >::TConstIterator It(JsonArrayValues); It; ++It)
	{
		auto Value = (*It).Get();
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
	if (!JsonObj.IsValid() || FieldName.IsEmpty())
	{
		return;
	}

	TArray< TSharedPtr<FJsonValue> > EntriesArray;
	for (auto String : StringArray)
	{
		EntriesArray.Add(MakeShareable(new FJsonValueString(String)));
	}

	JsonObj->SetArrayField(FieldName, EntriesArray);
}

TArray<bool> UVaRestJsonObject::GetBoolArrayField(const FString& FieldName) const
{
	TArray<bool> BoolArray;
	if (!JsonObj.IsValid() || !JsonObj->HasTypedField<EJson::Array>(FieldName) || FieldName.IsEmpty())
	{
		UE_LOG(LogVaRest, Warning, TEXT("%s: No field with name %s of type Array"), *VA_FUNC_LINE, *FieldName);
		return BoolArray;
	}

	TArray<TSharedPtr<FJsonValue> > JsonArrayValues = JsonObj->GetArrayField(FieldName);
	for (TArray<TSharedPtr<FJsonValue> >::TConstIterator It(JsonArrayValues); It; ++It)
	{
		auto Value = (*It).Get();
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
	if (!JsonObj.IsValid() || FieldName.IsEmpty())
	{
		return;
	}

	TArray< TSharedPtr<FJsonValue> > EntriesArray;
	for (auto Boolean : BoolArray)
	{
		EntriesArray.Add(MakeShareable(new FJsonValueBoolean(Boolean)));
	}

	JsonObj->SetArrayField(FieldName, EntriesArray);
}

TArray<UVaRestJsonObject*> UVaRestJsonObject::GetObjectArrayField(const FString& FieldName) const
{
	TArray<UVaRestJsonObject*> OutArray;
	if (!JsonObj.IsValid() || !JsonObj->HasTypedField<EJson::Array>(FieldName) || FieldName.IsEmpty())
	{
		UE_LOG(LogVaRest, Warning, TEXT("%s: No field with name %s of type Array"), *VA_FUNC_LINE, *FieldName);
		return OutArray;
	}

	TArray< TSharedPtr<FJsonValue> > ValArray = JsonObj->GetArrayField(FieldName);
	for (auto Value : ValArray)
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
	if (!JsonObj.IsValid() || FieldName.IsEmpty())
	{
		return;
	}

	TArray< TSharedPtr<FJsonValue> > EntriesArray;
	for (auto Value : ObjectArray)
	{
		EntriesArray.Add(MakeShareable(new FJsonValueObject(Value->GetRootObject())));
	}

	JsonObj->SetArrayField(FieldName, EntriesArray);
}

//////////////////////////////////////////////////////////////////////////
// Deserialize

int32 UVaRestJsonObject::DeserializeFromUTF8Bytes(const ANSICHAR* Bytes, int32 Size)
{
	FJSONReader Reader;

#if ENGINE_MINOR_VERSION >= 19
	// Get destLen
	int32 DestinationLength = FUTF8ToTCHAR_Convert::ConvertedLength(Bytes, Size);
	TCHAR* DestinationBuffer = new TCHAR[DestinationLength];

	// CONVERT to TCHAR string
	FUTF8ToTCHAR_Convert::Convert(DestinationBuffer, DestinationLength, Bytes, Size);

	int32 i = 0;
	while (i < DestinationLength)
	{
		if (!Reader.Read(DestinationBuffer[i++]))
		{
			break;
		}
	}
#else
	const ANSICHAR* EndByte = Bytes + Size;
	while (Bytes < EndByte)
	{
		TCHAR Char = FUTF8ToTCHAR_Convert::utf8codepoint(&Bytes);
		
		if (Char > 0xFFFF)
		{
			Char = UNICODE_BOGUS_CHAR_CODEPOINT;
		}
		
		if (!Reader.Read(Char))
		{
			break;
		}
	}
#endif
	
	SetRootObject(Reader.State.Root);
	return Reader.State.Size;
}

int32 UVaRestJsonObject::DeserializeFromTCHARBytes(const TCHAR* Bytes, int32 Size)
{
	FJSONReader Reader;
	
	int32 i = 0;
	while (i < Size)
	{
		if (!Reader.Read(Bytes[i++]))
		{
			break;
		}
	}
	
	SetRootObject(Reader.State.Root);
	return Reader.State.Size;
}

void UVaRestJsonObject::DecodeFromArchive(TUniquePtr<FArchive>& Reader)
{
	FArchive& Ar = (*Reader.Get());
	uint8 SymbolBytes[2];
	
	// Read first two bytes
	Ar << SymbolBytes[0];
	Ar << SymbolBytes[1];
	
	bool bIsIntelByteOrder = true;
	
	if(SymbolBytes[0] == 0xff && SymbolBytes[1] == 0xfe)
	{
		// Unicode Intel byte order. Less 1 for the FFFE header, additional 1 for null terminator.
		bIsIntelByteOrder = true;
	}
	else if(SymbolBytes[0] == 0xfe && SymbolBytes[1] == 0xff)
	{
		// Unicode non-Intel byte order. Less 1 for the FFFE header, additional 1 for null terminator.
		bIsIntelByteOrder = false;
	}
	
	FJSONReader JsonReader;
	TCHAR Char;
	
	while (!Ar.AtEnd())
	{
		Ar << SymbolBytes[0];
		
		if (Ar.AtEnd())
		{
			break;
		}
		
		Ar << SymbolBytes[1];
		
		if (bIsIntelByteOrder)
		{
			Char = CharCast<TCHAR>((UCS2CHAR)((uint16)SymbolBytes[0] + (uint16)SymbolBytes[1] * 256));
		}
		else
		{
			Char = CharCast<TCHAR>((UCS2CHAR)((uint16)SymbolBytes[1] + (uint16)SymbolBytes[0] * 256));
		}
		
		if (!JsonReader.Read(Char))
		{
			break;
		}
	}
	
	SetRootObject(JsonReader.State.Root);
	
	if (!Ar.Close())
	{
		UE_LOG(LogVaRest, Error, TEXT("UVaRestJsonObject::DecodeFromArchive: Error! Can't close file!"));
	}
	
}

bool UVaRestJsonObject::WriteToFile(const FString& Path)
{
	if (JsonObj.IsValid())
	{
		TUniquePtr<FArchive> FileWriter(IFileManager::Get().CreateFileWriter(*Path));
		if (!FileWriter)
		{
			return false;
		}
		
		FArchive& Ar = *FileWriter.Get();
		
		UCS2CHAR BOM = UNICODE_BOM;
		Ar.Serialize( &BOM, sizeof(UCS2CHAR) );
		
        FString Str = FString(TEXT("{"));
		WriteStringToArchive(Ar, *Str, Str.Len());
		
		int32 ElementCount = 0;
		FJSONWriter JsonWriter;
		for (auto JsonObjectValuePair : JsonObj->Values)
		{
			Str = FString(TEXT("\""));
			WriteStringToArchive(Ar, *Str, Str.Len());
			
			const TCHAR* BufferPtr = *JsonObjectValuePair.Key;
			for (int i = 0; i < JsonObjectValuePair.Key.Len(); ++i)
			{
                Str = FString(1, &BufferPtr[i]);
#if PLATFORM_WINDOWS
				WriteStringToArchive(Ar, *Str, Str.Len() - 1);
#else
				WriteStringToArchive(Ar, *Str, Str.Len());
#endif
			}
			
			Str = FString(TEXT("\""));
			WriteStringToArchive(Ar, *Str, Str.Len());
			
			Str = FString(TEXT(":"));
			WriteStringToArchive(Ar, *Str, Str.Len());
			
			++ElementCount;
			
			JsonWriter.Write(JsonObjectValuePair.Value, FileWriter.Get(), ElementCount >= JsonObj->Values.Num());
		}
		
		Str = FString(TEXT("}"));
		WriteStringToArchive(Ar, *Str, Str.Len());
		
		FileWriter->Close();
		
		return true;
	}
	else
	{
		UE_LOG(LogVaRest, Error, TEXT("UVaRestJsonObject::WriteToFile: Root object is invalid!"));
		return false;
	}
}

bool UVaRestJsonObject::WriteStringToArchive( FArchive& Ar, const TCHAR* StrPtr, int64 Len)
{
	auto Src = StringCast<UCS2CHAR>(StrPtr, Len);
	Ar.Serialize( (UCS2CHAR*)Src.Get(), Src.Length() * sizeof(UCS2CHAR) );
	
	return true;
}
