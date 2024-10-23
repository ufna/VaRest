// Copyright 2014-2019 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonValue.h"

#include "VaRestDefines.h"
#include "VaRestJsonObject.h"


namespace JsonValueUtils
{
	static const TMap<EJson, EVaJson> JsonToVaJsonMap = {
		{EJson::None, EVaJson::None},
		{EJson::Null, EVaJson::Null},
		{EJson::String, EVaJson::String},
		{EJson::Number, EVaJson::Number},
		{EJson::Boolean, EVaJson::Boolean},
		{EJson::Array, EVaJson::Array},
		{EJson::Object, EVaJson::Object}
	};

	static const TMap<EJson, FString> JsonToStringMap = {
		{EJson::None, TEXT("None")},
		{EJson::Null, TEXT("Null")},
		{EJson::String, TEXT("String")},
		{EJson::Number, TEXT("Number")},
		{EJson::Boolean, TEXT("Boolean")},
		{EJson::Array, TEXT("Array")},
		{EJson::Object, TEXT("Object")}
	};
}


UVaRestJsonValue::UVaRestJsonValue(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UVaRestJsonValue::Reset()
{
	JsonVal = nullptr;
}

TSharedPtr<FJsonValue>& UVaRestJsonValue::GetRootValue()
{
	return JsonVal;
}

void UVaRestJsonValue::SetRootValue(TSharedPtr<FJsonValue>& JsonValue)
{
	JsonVal = JsonValue;
}

//////////////////////////////////////////////////////////////////////////
// FJsonValue API

EVaJson UVaRestJsonValue::GetType() const
{
	if (!JsonVal.IsValid())
	{
		return EVaJson::None;
	}

	if (JsonValueUtils::JsonToVaJsonMap.Contains(JsonVal->Type))
	{
		return JsonValueUtils::JsonToVaJsonMap[JsonVal->Type];
	}

	return EVaJson::None;
}

FString UVaRestJsonValue::GetTypeString() const
{
	if (!JsonVal.IsValid())
	{
		return TEXT("None");
	}

	if (JsonValueUtils::JsonToStringMap.Contains(JsonVal->Type))
	{
		return JsonValueUtils::JsonToStringMap[JsonVal->Type];
	}

	return TEXT("None");
}

bool UVaRestJsonValue::IsNull() const
{
	if (!JsonVal.IsValid())
	{
		return true;
	}

	return JsonVal->IsNull();
}

float UVaRestJsonValue::AsNumber() const
{
	if (!JsonVal.IsValid())
	{
		ErrorMessage(TEXT("Number"));
		return 0.f;
	}

	return static_cast<float>(JsonVal->AsNumber());
}

int32 UVaRestJsonValue::AsInt32() const
{
	if (!JsonVal.IsValid())
	{
		ErrorMessage(TEXT("Number"));
		return 0.f;
	}

	return static_cast<int32>(JsonVal->AsNumber());
}

int64 UVaRestJsonValue::AsInt64() const
{
	if (!JsonVal.IsValid())
	{
		ErrorMessage(TEXT("Number"));
		return 0.f;
	}

	return static_cast<int64>(JsonVal->AsNumber());
}

FString UVaRestJsonValue::AsString() const
{
	if (!JsonVal.IsValid())
	{
		ErrorMessage(TEXT("String"));
		return FString();
	}

	return JsonVal->AsString();
}

bool UVaRestJsonValue::AsBool() const
{
	if (!JsonVal.IsValid())
	{
		ErrorMessage(TEXT("Boolean"));
		return false;
	}

	return JsonVal->AsBool();
}

TArray<UVaRestJsonValue*> UVaRestJsonValue::AsArray() const
{
	TArray<UVaRestJsonValue*> OutArray;

	if (!JsonVal.IsValid())
	{
		ErrorMessage(TEXT("Array"));
		return OutArray;
	}

	TArray<TSharedPtr<FJsonValue>> ValArray = JsonVal->AsArray();
	for (auto Value : ValArray)
	{
		UVaRestJsonValue* NewValue = NewObject<UVaRestJsonValue>();
		NewValue->SetRootValue(Value);

		OutArray.Add(NewValue);
	}

	return OutArray;
}

UVaRestJsonObject* UVaRestJsonValue::AsObject()
{
	if (!JsonVal.IsValid())
	{
		ErrorMessage(TEXT("Object"));
		return nullptr;
	}

	const TSharedPtr<FJsonObject> NewObj = JsonVal->AsObject();

	UVaRestJsonObject* JsonObj = NewObject<UVaRestJsonObject>();
	JsonObj->SetRootObject(NewObj);

	return JsonObj;
}

//////////////////////////////////////////////////////////////////////////
// Helpers

void UVaRestJsonValue::ErrorMessage(const FString& InType) const
{
	UE_LOG(LogVaRest, Error, TEXT("Json Value of type '%s' used as a '%s'."), *GetTypeString(), *InType);
}
