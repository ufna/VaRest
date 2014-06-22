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


//////////////////////////////////////////////////////////////////////////
// FJsonObject API

bool UVaRestJsonObject::HasField(const FString& FieldName) const
{
	return false;
}

void UVaRestJsonObject::RemoveField(const FString& FieldName)
{

}

double UVaRestJsonObject::GetNumberField(const FString& FieldName) const
{
	return 0.0f;
}

void UVaRestJsonObject::SetNumberField(const FString& FieldName, double Number)
{

}

FString UVaRestJsonObject::GetStringField(const FString& FieldName) const
{
	return TEXT("STRING");
}

void UVaRestJsonObject::SetStringField(const FString& FieldName, const FString& StringValue)
{

}

bool UVaRestJsonObject::GetBoolField(const FString& FieldName) const
{
	return false;
}

void UVaRestJsonObject::SetBoolField(const FString& FieldName, bool InValue)
{

}

UVaRestJsonObject* UVaRestJsonObject::GetObjectField(const FString& FieldName) const
{
	return NULL;
}

void UVaRestJsonObject::SetObjectField(const FString& FieldName, const UVaRestJsonObject* JsonObject)
{

}

