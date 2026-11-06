// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiEnums.h"

FVaRestOpenApiEnumPlan FVaRestOpenApiEnumPlanner::Build(const FVaRestJson& StringValues,bool bNeedsUnset,bool bEnumsAsString)
{
	FVaRestOpenApiEnumPlan Result;
	const auto Fallback=[&](const TCHAR* Reason) { Result.Values.Reset();Result.bHasUnset=false;Result.bStringFallback=true;Result.StringFallbackReason=Reason;return Result; };
	if (bEnumsAsString) return Fallback(TEXT("Enums as String is enabled."));
	if (!StringValues.IsArray() || StringValues.Num()<2) return Fallback(TEXT("An enum needs at least two string values; const/single-value schemas use a string default."));
	if (StringValues.Num()>254-(bNeedsUnset ? 1 : 0)) return Fallback(TEXT("The enum exceeds the 254-value Blueprint limit including unset."));
	TSet<FString> DisplayNames; // engine display-name uniqueness is deliberately case-insensitive
	for (const auto& Item:StringValues.Elements())
	{
		if (!Item.IsString()) return Fallback(TEXT("Non-string enum values retain their scalar JSON type."));
		const FString Wire=Item.AsString();
		if (Wire.TrimStartAndEnd().IsEmpty()) return Fallback(TEXT("An empty/whitespace wire value cannot be an enum display name."));
		for (TCHAR Char:Wire) if (!Char) return Fallback(TEXT("A wire value containing a null character cannot be an enum display name."));
		if (DisplayNames.Contains(Wire)) return Fallback(TEXT("Wire values collide under Blueprint enum display-name comparison."));
		DisplayNames.Add(Wire);
	}
	if (bNeedsUnset)
	{
		FString Display=TEXT("(unset)");
		for (int32 Suffix=2;DisplayNames.Contains(Display);++Suffix) Display=TEXT("(unset)_")+LexToString(Suffix);
		Result.Values.Add({TEXT("NewEnumerator0"),MoveTemp(Display),{},true});Result.bHasUnset=true;
	}
	for (const auto& Item:StringValues.Elements())
	{
		FVaRestOpenApiEnumValue Value;Value.InternalName=TEXT("NewEnumerator")+LexToString(Result.Values.Num());
		Value.DisplayName=Item.AsString();Value.WireValue=Value.DisplayName;Result.Values.Add(MoveTemp(Value));
	}
	return Result;
}
TOptional<FString> FVaRestOpenApiEnumPlan::DefaultInternalName(const FVaRestJson& Default,bool bUnsetWhenAbsent,bool bAllowNull) const
{
	if (bStringFallback || Values.IsEmpty()) return {};
	if (!Default.IsValid())
	{
		if (bUnsetWhenAbsent && bHasUnset) return Values[0].InternalName;
		for (const auto& Value:Values) if (!Value.bUnset) return Value.InternalName;
		return {};
	}
	if (Default.IsNull()) return bAllowNull && bHasUnset ? TOptional<FString>(Values[0].InternalName) : TOptional<FString>();
	if (!Default.IsString()) return {};
	const FString Wire=Default.AsString();
	for (const auto& Value:Values)
		if (!Value.bUnset && Value.WireValue.Len()==Wire.Len() && FMemory::Memcmp(*Value.WireValue,*Wire,Wire.Len()*sizeof(TCHAR))==0) return Value.InternalName;
	return {}; // an invalid declared default must be diagnosed before touching a UDE/UDS
}
