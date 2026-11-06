// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiNames.h"
#include "Misc/SecureHash.h"
#include "UObject/NameTypes.h"

namespace
{
bool ValidField(const FString& Name)
{
	if (Name.IsEmpty() || Name.Len()>96 || Name.Contains(TEXT("<"))) return false;
	for (TCHAR Char:Name) if (!Char || Char<32 || Char==127) return false;
	return FName::IsValidXName(Name,INVALID_OBJECTNAME_CHARACTERS);
}
FString Prefix(const FString& Name,int32 Limit)
{
	int32 Count=FMath::Min(Name.Len(),Limit);
	if (Count<Name.Len() && Count>0 && static_cast<uint32>(Name[Count-1])>=0xd800 && static_cast<uint32>(Name[Count-1])<=0xdbff) --Count;
	return Name.Left(Count);
}
FString UniqueField(const FString& Base,TSet<FString>& Used,TMap<FString,int32>& NextSuffix)
{
	FString Result=Base;
	int32& Suffix=NextSuffix.FindOrAdd(Base);Suffix=FMath::Max(Suffix,2);
	while (Used.Contains(Result))
	{
		const FString Tail=TEXT("_")+LexToString(Suffix++);Result=Prefix(Base,96-Tail.Len())+Tail;
	}
	Used.Add(Result);return Result;
}
}
FVaRestOpenApiNameAllocator::FVaRestOpenApiNameAllocator(TConstArrayView<FString> Existing)
{
	for (const auto& Name:Existing) UsedNames.Add(Name);
}
FString FVaRestOpenApiNameAllocator::Pascal(const FString& Name)
{
	FString Result;bool bStart=true;
	for (TCHAR Char:Name)
	{
		if (!FChar::IsAlnum(Char)) { bStart=true;continue; }
		Result.AppendChar(bStart ? FChar::ToUpper(Char) : Char);bStart=false;
	}
	return Result.IsEmpty() ? TEXT("Type") : Result;
}
FString FVaRestOpenApiNameAllocator::ShortAssetName(const FString& Name)
{
	if (Name.Len()<=64) return Name;
	const FTCHARToUTF8 Utf8(*Name);uint8 Digest[20];FSHA1::HashBuffer(Utf8.Get(),Utf8.Length(),Digest);
	return Prefix(Name,57)+TEXT("_")+BytesToHex(Digest,20).Left(6);
}
FString FVaRestOpenApiNameAllocator::AllocateType(const FString& SemanticName,bool bEnum,const FString& TypePrefix)
{
	FString SafePrefix;
	for (TCHAR Char:TypePrefix) SafePrefix.AppendChar(FChar::IsAlnum(Char) || Char==TEXT('_') ? Char : TEXT('_'));
	const FString Base=FString(bEnum ? TEXT("E_") : TEXT("S_"))+SafePrefix+Pascal(SemanticName);
	FString Result=ShortAssetName(Base);
	int32& Suffix=NextTypeSuffix.FindOrAdd(Base);Suffix=FMath::Max(Suffix,2);
	while (UsedNames.Contains(Result)) Result=ShortAssetName(Base+TEXT("_")+LexToString(Suffix++));
	UsedNames.Add(Result);return Result;
}
FString FVaRestOpenApiNameAllocator::SanitizeField(const FString& Name)
{
	FString Result;bool bUnderscore=false;
	for (TCHAR Char:Name)
	{
		const bool bInvalid=!Char || Char<32 || Char==127 || Char==TEXT('<') || FCString::Strchr(INVALID_OBJECTNAME_CHARACTERS,Char);
		if (bInvalid || Char==TEXT('_')) { if (!Result.IsEmpty() && !bUnderscore) Result.AppendChar(TEXT('_'));bUnderscore=true; }
		else { Result.AppendChar(Char);bUnderscore=false; }
	}
	while (Result.EndsWith(TEXT("_"))) Result.LeftChopInline(1,EAllowShrinking::No);
	Result=Prefix(Result,96);return Result.IsEmpty() ? TEXT("Field") : Result;
}
TArray<FVaRestOpenApiFieldName> FVaRestOpenApiNameAllocator::Fields(TConstArrayView<FString> WireNames)
{
	TArray<FVaRestOpenApiFieldName> Result;Result.SetNum(WireNames.Num());TSet<FString> Used;TMap<FString,int32> NextSuffix;
	for (int32 Index=0;Index<WireNames.Num();++Index)
	{
		Result[Index].WireName=WireNames[Index];
		if (ValidField(WireNames[Index]) && !Used.Contains(WireNames[Index]))
		{ Result[Index].FriendlyName=WireNames[Index];Used.Add(WireNames[Index]); }
	}
	for (auto& Field:Result) if (Field.FriendlyName.IsEmpty())
	{ Field.FriendlyName=UniqueField(SanitizeField(Field.WireName),Used,NextSuffix);Field.bSanitized=true; }
	return Result;
}
TArray<FVaRestOpenApiParameterName> FVaRestOpenApiNameAllocator::Parameters(TConstArrayView<FVaRestOpenApiParameter> Parameters)
{
	TArray<FVaRestOpenApiParameterName> Result;TSet<FString> Used;TMap<FString,int32> NextSuffix;
	TMap<FString,TSet<FString>> Locations; // the grouping is intentionally case-insensitive for K2 pin identities
	for (const auto& Parameter:Parameters) Locations.FindOrAdd(Parameter.Name).Add(Parameter.In);
	for (const auto& Parameter:Parameters)
	{
		FString Base=Parameter.Name;
		const bool bOtherLocation=Locations.FindChecked(Parameter.Name).Num()>1;
		if (bOtherLocation && !Parameter.In.Equals(TEXT("path"),ESearchCase::CaseSensitive)) Base+=TEXT(" (")+Parameter.In+TEXT(")");
		// A pin name can contain more characters than an object member. Keep the wire spelling separately in all cases.
		Base=Prefix(Base,96);if (Base.IsEmpty()) Base=TEXT("Param");
		FVaRestOpenApiParameterName Out;Out.WireName=Parameter.Name;Out.Location=Parameter.In;Out.PinName=UniqueField(Base,Used,NextSuffix);Result.Add(MoveTemp(Out));
	}
	return Result;
}
