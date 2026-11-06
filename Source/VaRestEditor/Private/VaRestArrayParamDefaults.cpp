// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestArrayParamDefaults.h"
#include "VaRestJson.h"
#include "UObject/Class.h"

bool VaRest::Http::Editor::ArrayParameterDefaults(const FVaRestTypeRef& Type,const FString& Text,TArray<FString>& Elements,FString& Error)
{
	check(IsInGameThread());Elements.Reset();Error.Reset();
	FVaRestApiValidationContext Validation;
	if (Type.Container!=EVaRestTypeContainer::Array || !Type.Validate(Validation,TEXTVIEW("Default"),true)) { Error=TEXT("an array parameter requires a valid scalar element type");return false; }
	if (Text.IsEmpty()) return true;
	FVaRestJsonParseOptions Options;Options.MaxInputBytes=64ll*1024*1024;auto Parsed=FVaRestJson::Parse(Text,Options);
	if (Parsed.HasError()) { Error=Parsed.GetError().ToString();return false; }
	const auto Values=Parsed.StealValue();if (!Values.IsArray()) { Error=TEXT("an array default must be a JSON array");return false; }
	TArray<FString> Result;
	for (const auto& Value:Values.Elements())
	{
		FString Default;bool Valid=false;
		switch (Type.Kind)
		{
		case EVaRestTypeKind::String:Valid=Value.IsString();if (Valid) Default=Value.AsString();break;
		case EVaRestTypeKind::Bool:Valid=Value.IsBool();if (Valid) Default=Value.AsBool() ? TEXT("true") : TEXT("false");break;
		case EVaRestTypeKind::Int:Valid=Value.TryGet<int32>().IsSet();if (Valid) Default=Value.ToString();break;
		case EVaRestTypeKind::Int64:Valid=Value.TryGet<int64>().IsSet();if (Valid) Default=Value.ToString();break;
		case EVaRestTypeKind::Double:Valid=Value.IsNumber();if (Valid) Default=Value.ToString();break;
		case EVaRestTypeKind::Enum:
		{
			const auto* Enum=Cast<UEnum>(Type.Type);const int32 Index=Enum && Value.IsString() ? Enum->GetIndexByNameString(Value.AsString()) : INDEX_NONE;
			Valid=Index!=INDEX_NONE;if (Valid) Default=Enum->GetNameStringByIndex(Index);break;
		}
		default:break;
		}
		for (int32 Index=0;Index<Default.Len();++Index) if (!Default[Index]) Valid=false;
		if (!Valid) { Error=FString::Printf(TEXT("array default element %d is not a valid scalar K2 literal of its declared type (enum defaults use internal names)"),Result.Num());return false; }
		Result.Add(MoveTemp(Default));
	}
	Elements=MoveTemp(Result);return true;
}
