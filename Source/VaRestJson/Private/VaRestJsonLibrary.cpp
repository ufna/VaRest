// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonLibrary.h"

#include "Misc/FileHelper.h"

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
