// Copyright 2014-2020 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSubsystem.h"

#include "VaRestDefines.h"
#include "VaRestJsonObject.h"
#include "VaRestJsonValue.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Subsystems/SubsystemBlueprintLibrary.h"

UVaRestSubsystem::UVaRestSubsystem()
	: UEngineSubsystem()
{
}

void UVaRestSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	UE_LOG(LogVaRest, Log, TEXT("%s: VaRest subsystem initialized"), *VA_FUNC_LINE);
}

void UVaRestSubsystem::Deinitialize()
{
	// Do nothing for now
	Super::Deinitialize();
}

void UVaRestSubsystem::CallURL(const FString& URL, EVaRestRequestVerb Verb, EVaRestRequestContentType ContentType, UVaRestJsonObject* VaRestJson, const FVaRestCallDelegate& Callback)
{
	// Check we have valid data json
	if (VaRestJson == nullptr)
	{
		VaRestJson = ConstructVaRestJsonObject();
	}

	UVaRestRequestJSON* Request = ConstructVaRestRequest();

	Request->SetVerb(Verb);
	Request->SetContentType(ContentType);
	Request->SetRequestObject(VaRestJson);

	FVaRestCallResponse Response;
	Response.Request = Request;
	Response.Callback = Callback;

	Response.CompleteDelegateHandle = Request->OnStaticRequestComplete.AddUObject(this, &UVaRestSubsystem::OnCallComplete);
	Response.FailDelegateHandle = Request->OnStaticRequestFail.AddUObject(this, &UVaRestSubsystem::OnCallComplete);

	RequestMap.Add(Request, Response);

	Request->ResetResponseData();
	Request->ProcessURL(URL);
}

void UVaRestSubsystem::OnCallComplete(UVaRestRequestJSON* Request)
{
	if (!RequestMap.Contains(Request))
	{
		return;
	}

	const auto Response = RequestMap.Find(Request);
	Request->OnStaticRequestComplete.Remove(Response->CompleteDelegateHandle);
	Request->OnStaticRequestFail.Remove(Response->FailDelegateHandle);

	Response->Callback.ExecuteIfBound(Request);
	Response->Request = nullptr;
	RequestMap.Remove(Request);
}

UVaRestRequestJSON* UVaRestSubsystem::ConstructVaRestRequest()
{
	return NewObject<UVaRestRequestJSON>(this);
}

UVaRestRequestJSON* UVaRestSubsystem::ConstructVaRestRequestExt(EVaRestRequestVerb Verb, EVaRestRequestContentType ContentType)
{
	UVaRestRequestJSON* Request = ConstructVaRestRequest();

	Request->SetVerb(Verb);
	Request->SetContentType(ContentType);

	return Request;
}

UVaRestJsonObject* UVaRestSubsystem::ConstructVaRestJsonObject()
{
	return NewObject<UVaRestJsonObject>(this);
}

UVaRestJsonObject* UVaRestSubsystem::StaticConstructVaRestJsonObject()
{
	auto SelfSystem = CastChecked<UVaRestSubsystem>(USubsystemBlueprintLibrary::GetEngineSubsystem(UVaRestSubsystem::StaticClass()), ECastCheckedType::NullChecked);
	return SelfSystem->ConstructVaRestJsonObject();
}

UVaRestJsonValue* UVaRestSubsystem::ConstructJsonValueNumber(float Number)
{
	TSharedPtr<FJsonValue> NewVal = MakeShareable(new FJsonValueNumber(Number));

	UVaRestJsonValue* NewValue = NewObject<UVaRestJsonValue>(this);
	NewValue->SetRootValue(NewVal);

	return NewValue;
}

UVaRestJsonValue* UVaRestSubsystem::ConstructJsonValueString(const FString& StringValue)
{
	TSharedPtr<FJsonValue> NewVal = MakeShareable(new FJsonValueString(StringValue));

	UVaRestJsonValue* NewValue = NewObject<UVaRestJsonValue>(this);
	NewValue->SetRootValue(NewVal);

	return NewValue;
}

UVaRestJsonValue* UVaRestSubsystem::ConstructJsonValueBool(bool InValue)
{
	TSharedPtr<FJsonValue> NewVal = MakeShareable(new FJsonValueBoolean(InValue));

	UVaRestJsonValue* NewValue = NewObject<UVaRestJsonValue>(this);
	NewValue->SetRootValue(NewVal);

	return NewValue;
}

UVaRestJsonValue* UVaRestSubsystem::ConstructJsonValueArray(const TArray<UVaRestJsonValue*>& InArray)
{
	// Prepare data array to create new value
	TArray<TSharedPtr<FJsonValue>> ValueArray;
	for (auto InVal : InArray)
	{
		ValueArray.Add(InVal->GetRootValue());
	}

	TSharedPtr<FJsonValue> NewVal = MakeShareable(new FJsonValueArray(ValueArray));

	UVaRestJsonValue* NewValue = NewObject<UVaRestJsonValue>(this);
	NewValue->SetRootValue(NewVal);

	return NewValue;
}

UVaRestJsonValue* UVaRestSubsystem::ConstructJsonValueObject(UVaRestJsonObject* JsonObject)
{
	TSharedPtr<FJsonValue> NewVal = MakeShareable(new FJsonValueObject(JsonObject->GetRootObject()));

	UVaRestJsonValue* NewValue = NewObject<UVaRestJsonValue>(this);
	NewValue->SetRootValue(NewVal);

	return NewValue;
}

UVaRestJsonValue* UVaRestSubsystem::ConstructJsonValue(const TSharedPtr<FJsonValue>& InValue)
{
	TSharedPtr<FJsonValue> NewVal = InValue;

	UVaRestJsonValue* NewValue = NewObject<UVaRestJsonValue>(this);
	NewValue->SetRootValue(NewVal);

	return NewValue;
}

UVaRestJsonValue* UVaRestSubsystem::DecodeJsonValue(const FString& JsonString)
{
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(*JsonString);
	TSharedPtr<FJsonValue> OutJsonValue;
	if (FJsonSerializer::Deserialize(Reader, OutJsonValue))
	{
		return ConstructJsonValue(OutJsonValue);
	}

	return nullptr;
}

UVaRestJsonObject* UVaRestSubsystem::DecodeJsonObject(const FString& JsonString)
{
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(*JsonString);
	TSharedPtr<FJsonObject> OutJsonObj;
	if (FJsonSerializer::Deserialize(Reader, OutJsonObj))
	{
		auto NewJsonObj = NewObject<UVaRestJsonObject>(this);
		NewJsonObj->SetRootObject(OutJsonObj);
		return NewJsonObj;
	}

	return nullptr;
}

class UVaRestJsonObject* UVaRestSubsystem::LoadJsonFromFile(const FString& Path, const bool bIsRelativeToContentDir)
{
	auto* Json = ConstructVaRestJsonObject();

	FString JSONString;
	if (FFileHelper::LoadFileToString(JSONString, *(bIsRelativeToContentDir ? FPaths::ProjectContentDir() / Path : Path)))
	{
		if (Json->DecodeJson(JSONString))
		{
			return Json;
		}
		else
		{
			UE_LOG(LogVaRest, Error, TEXT("%s: Can't decode json from file %s"), *VA_FUNC_LINE, *Path);
		}
	}
	else
	{
		UE_LOG(LogVaRest, Error, TEXT("%s: Can't open file %s"), *VA_FUNC_LINE, *Path);
	}

	return nullptr;
}
