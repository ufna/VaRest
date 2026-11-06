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
#include "UObject/StrongObjectPtr.h"

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
	check(IsInGameThread());
	if (bCallURLClosing) return;
	bCallURLClosing = true; // Close admission and callback delivery before any retirement callout.

	TArray<TStrongObjectPtr<UVaRestRequestJSON>> Requests;
	Requests.Reserve(RequestMap.Num());
	for (const auto& Pair : RequestMap)
		if (IsValid(Pair.Key) && !Pair.Key->IsUnreachable()) Requests.Emplace(Pair.Key);

	// Every live request is pinned before reflected ownership moves away.
	auto Retiring = MoveTemp(RequestMap);
	RequestMap.Reset();
	for (auto& Pair : Retiring)
	{
		UVaRestRequestJSON* Request = Pair.Key;
		if (IsValid(Request) && !Request->IsUnreachable())
		{
			Request->OnStaticRequestComplete.Remove(Pair.Value.CompleteDelegateHandle);
			Request->OnStaticRequestFail.Remove(Pair.Value.FailDelegateHandle);
		}
		Pair.Value.Callback.Unbind();
	}
	Retiring.Empty(); // Release all callback records before cancelling any live target.
	for (const auto& Request : Requests)
		if (IsValid(Request.Get()) && !Request->IsUnreachable()) Request->Cancel();
	Super::Deinitialize();
}

void UVaRestSubsystem::CallURL(const FString& URL, EVaRestRequestVerb Verb, EVaRestRequestContentType ContentType, UVaRestJsonObject* VaRestJson, const FVaRestCallDelegate& Callback)
{
	check(IsInGameThread());
	if (!IsValid(this) || IsUnreachable() || bCallURLClosing) return;
	const TStrongObjectPtr<UVaRestSubsystem> Self(this);
	// Check we have valid data json
	if (VaRestJson == nullptr)
	{
		VaRestJson = ConstructVaRestJsonObject();
	}

	if (!IsValid(this) || IsUnreachable() || bCallURLClosing) return;
	UVaRestRequestJSON* Request = ConstructVaRestRequest();
	if (!IsValid(Request) || Request->IsUnreachable()) return;
	const TStrongObjectPtr<UVaRestRequestJSON> Kept(Request);

	Request->SetVerb(Verb);
	Request->SetContentType(ContentType);
	Request->SetRequestObject(VaRestJson);

	if (!IsValid(this) || IsUnreachable() || bCallURLClosing) return;
	FVaRestCallResponse Response;
	Response.Request = Request;
	Response.Callback = Callback;

	Response.CompleteDelegateHandle = Request->OnStaticRequestComplete.AddUObject(this, &UVaRestSubsystem::OnCallComplete);
	Response.FailDelegateHandle = Request->OnStaticRequestFail.AddUObject(this, &UVaRestSubsystem::OnCallComplete);

	if (!IsValid(this) || IsUnreachable() || bCallURLClosing)
	{
		Request->OnStaticRequestComplete.Remove(Response.CompleteDelegateHandle);
		Request->OnStaticRequestFail.Remove(Response.FailDelegateHandle);
		return;
	}
	RequestMap.Add(Request, Response);

	Request->ResetResponseData();
	if (!IsValid(this) || IsUnreachable() || bCallURLClosing) return;
	Request->ProcessURL(URL);
}

void UVaRestSubsystem::OnCallComplete(UVaRestRequestJSON* Request)
{
	check(IsInGameThread());
	if (bCallURLClosing) return; // Deinitialize owns all entries; never deliver user code while closing.
	FVaRestCallResponse Response;
	if (!RequestMap.RemoveAndCopyValue(Request, Response))
	{
		return;
	}

	Request->OnStaticRequestComplete.Remove(Response.CompleteDelegateHandle);
	Request->OnStaticRequestFail.Remove(Response.FailDelegateHandle);

	// A callback may start more CallURL requests and reallocate the map, or run
	// GC. Retire its entry before entering user code; keep this request alive
	// independently of the map while delivering the callback.
	const TStrongObjectPtr<UVaRestRequestJSON> Kept(Request);
	Response.Callback.ExecuteIfBound(Request);
}

void UVaRestSubsystem::RetireCallURLSilently(UVaRestRequestJSON* Request)
{
	check(IsInGameThread());
	if (!IsValid(this) || IsUnreachable() || bCallURLClosing || !IsValid(Request) || Request->IsUnreachable()) return;
	const TStrongObjectPtr<UVaRestRequestJSON> Kept(Request);
	FVaRestCallResponse Response;
	if (!RequestMap.RemoveAndCopyValue(Request, Response)) return;
	Request->OnStaticRequestComplete.Remove(Response.CompleteDelegateHandle);
	Request->OnStaticRequestFail.Remove(Response.FailDelegateHandle);
	Response.Callback.Unbind(); // Internal retirement only; no OnCallComplete/user callback.
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
	auto Parsed=FVaRestJson::Parse(JsonString);
	if (Parsed.HasValue())
	{
		return ConstructJsonValue(Parsed.GetValue().ToJsonValue());
	}
	const auto& Error=Parsed.GetError();UE_LOG(LogVaRest,Warning,TEXT("Legacy JSON decoding failed at %d:%d: %s"),Error.Line,Error.Column,*Error.Message);
	return nullptr;
}

UVaRestJsonObject* UVaRestSubsystem::DecodeJsonObject(const FString& JsonString)
{
	auto Parsed=FVaRestJson::Parse(JsonString);
	if (Parsed.HasValue() && Parsed.GetValue().IsObject())
	{
		auto NewJsonObj = NewObject<UVaRestJsonObject>(this);
		NewJsonObj->SetRootObject(Parsed.GetValue().ToJsonObject());
		return NewJsonObj;
	}
	if (Parsed.HasError()) { const auto& Error=Parsed.GetError();UE_LOG(LogVaRest,Warning,TEXT("Legacy JSON decoding failed at %d:%d: %s"),Error.Line,Error.Column,*Error.Message); }
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
