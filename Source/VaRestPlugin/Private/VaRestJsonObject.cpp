// Copyright 2014 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestPluginPrivatePCH.h"

UVaRestJsonObject::UVaRestJsonObject(const class FPostConstructInitializeProperties& PCIP)
	: Super(PCIP)
{
	Reset();
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
	JsonObj = JsonObject;
}


//////////////////////////////////////////////////////////////////////////
// FJsonObject API

bool UVaRestJsonObject::HasField(const FString& FieldName) const
{
	if (!JsonObj.IsValid())
	{
		return false;
	}

	return JsonObj->HasField(FieldName);
}

void UVaRestJsonObject::RemoveField(const FString& FieldName)
{
	if (!JsonObj.IsValid())
	{
		return;
	}

	JsonObj->RemoveField(FieldName);
}

double UVaRestJsonObject::GetNumberField(const FString& FieldName) const
{
	if (!JsonObj.IsValid())
	{
		return 0.0f;
	}

	return JsonObj->GetNumberField(FieldName);
}

void UVaRestJsonObject::SetNumberField(const FString& FieldName, double Number)
{
	if (!JsonObj.IsValid())
	{
		return;
	}

	JsonObj->SetNumberField(FieldName, Number);
}

FString UVaRestJsonObject::GetStringField(const FString& FieldName) const
{
	if (!JsonObj.IsValid())
	{
		return TEXT("");
	}
	
	return JsonObj->GetStringField(FieldName);
}

void UVaRestJsonObject::SetStringField(const FString& FieldName, const FString& StringValue)
{
	if (!JsonObj.IsValid())
	{
		return;
	}

	JsonObj->SetStringField(FieldName, StringValue);
}

bool UVaRestJsonObject::GetBoolField(const FString& FieldName) const
{
	if (!JsonObj.IsValid())
	{
		return false;
	}

	return JsonObj->GetBoolField(FieldName);
}

void UVaRestJsonObject::SetBoolField(const FString& FieldName, bool InValue)
{
	if (!JsonObj.IsValid())
	{
		return;
	}

	JsonObj->SetBoolField(FieldName, InValue);
}

UVaRestJsonObject* UVaRestJsonObject::GetObjectField(const FString& FieldName) const
{
	if (!JsonObj.IsValid())
	{
		return NULL;
	}

	TSharedPtr<FJsonObject> JsonObjField = JsonObj->GetObjectField(FieldName);

	UVaRestJsonObject* OutRestJsonObj = (UVaRestJsonObject*)StaticConstructObject(UVaRestJsonObject::StaticClass());
	OutRestJsonObj->SetRootObject(JsonObjField);

	return OutRestJsonObj;
}

void UVaRestJsonObject::SetObjectField(const FString& FieldName, const UVaRestJsonObject* JsonObject)
{
	if (!JsonObj.IsValid())
	{
		return;
	}
}

