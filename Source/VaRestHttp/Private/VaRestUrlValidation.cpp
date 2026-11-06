// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestUrlValidation.h"

#include "VaRestUrl.h"

namespace VaRest::Http
{
namespace
{
bool ValidIpv6(FString Address)
{
	const int32 Zone = Address.Find(TEXT("%25"), ESearchCase::CaseSensitive);
	if (Zone != INDEX_NONE)
	{
		const FString ZoneId = Address.Mid(Zone + 3);
		if (ZoneId.IsEmpty())
			return false;
		for (TCHAR Character : ZoneId)
		{
			if (!FChar::IsAlnum(Character) && Character != TEXT('_') && Character != TEXT('-') && Character != TEXT('.') && Character != TEXT('~'))
				return false;
		}
		Address.LeftInline(Zone);
	}
	const int32 Compression = Address.Find(TEXT("::"));
	const bool bCompressed = Compression != INDEX_NONE;
	if (Address.Contains(TEXT(":::")) || (bCompressed && Address.Find(TEXT("::"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Compression + 2) != INDEX_NONE) || (Address.StartsWith(TEXT(":")) && !Address.StartsWith(TEXT("::"))) || (Address.EndsWith(TEXT(":")) && !Address.EndsWith(TEXT("::"))))
		return false;
	TArray<FString> Groups;
	Address.ParseIntoArray(Groups, TEXT(":"), true);
	int32 Count = 0;
	for (int32 Index = 0; Index < Groups.Num(); ++Index)
	{
		const FString& Group = Groups[Index];
		if (Group.Contains(TEXT(".")))
		{
			if (Index != Groups.Num() - 1 || !Address.EndsWith(Group, ESearchCase::CaseSensitive))
				return false;
			TArray<FString> Octets;
			Group.ParseIntoArray(Octets, TEXT("."), false);
			if (Octets.Num() != 4)
				return false;
			for (const FString& Octet : Octets)
			{
				if (Octet.IsEmpty() || Octet.Len() > 3 || (Octet.Len() > 1 && Octet[0] == TEXT('0')))
					return false;
				int32 Value = 0;
				for (TCHAR Digit : Octet)
				{
					if (Digit < TEXT('0') || Digit > TEXT('9'))
						return false;
					Value = Value * 10 + Digit - TEXT('0');
				}
				if (Value > 255)
					return false;
			}
			Count += 2;
		}
		else
		{
			if (Group.Len() > 4)
				return false;
			for (TCHAR Digit : Group)
				if (!FChar::IsHexDigit(Digit))
					return false;
			++Count;
		}
	}
	return bCompressed ? Count < 8 : Count == 8;
}

} // namespace

bool IsValidHttpUrl(const FString& Url, bool bAllowUserInfo)
{
	if (!IsAbsoluteHttpUrl(Url) || HasNonAsciiHost(Url) || HasControlInAuthority(Url))
		return false;
	const FString Text = Url.TrimStartAndEnd();
	const int32 Begin = Text.Find(TEXT("://")) + 3;
	int32 End = Begin;
	while (End < Text.Len() && Text[End] != TEXT('/') && Text[End] != TEXT('?') && Text[End] != TEXT('#'))
		++End;
	FString Authority = Text.Mid(Begin, End - Begin);
	const int32 UserInfo = Authority.Find(TEXT("@"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (UserInfo != INDEX_NONE)
	{
		if (!bAllowUserInfo)
			return false;
		Authority.RightChopInline(UserInfo + 1);
	}
	FString Host;
	FString Port;
	if (Authority.StartsWith(TEXT("[")))
	{
		int32 Close;
		if (!Authority.FindChar(TEXT(']'), Close) || Close <= 1 || !ValidIpv6(Authority.Mid(1, Close - 1)))
			return false;
		Host = Authority.Left(Close + 1);
		const FString Tail = Authority.Mid(Close + 1);
		if (!Tail.IsEmpty())
		{
			if (!Tail.StartsWith(TEXT(":")))
				return false;
			Port = Tail.Mid(1);
			if (Port.IsEmpty())
				return false;
		}
	}
	else
	{
		int32 Colon;
		if (Authority.FindChar(TEXT(':'), Colon))
		{
			Host = Authority.Left(Colon);
			Port = Authority.Mid(Colon + 1);
			if (Port.IsEmpty())
				return false;
		}
		else
			Host = Authority;
		for (int32 Index = 0; Index < Host.Len(); ++Index)
		{
			const TCHAR Character = Host[Index];
			if (Character == TEXT('%'))
			{
				if (Index + 2 >= Host.Len() || !FChar::IsHexDigit(Host[Index + 1]) || !FChar::IsHexDigit(Host[Index + 2]))
					return false;
				Index += 2;
			}
			else if (!FChar::IsAlnum(Character) && !FString(TEXT("-._~!$&'()*+,;=")).Contains(FString::Chr(Character)))
				return false;
		}
	}
	if (Host.IsEmpty())
		return false;
	if (!Port.IsEmpty())
	{
		uint32 Number = 0;
		for (TCHAR Digit : Port)
		{
			if (Digit < TEXT('0') || Digit > TEXT('9') || Number > (65535 - static_cast<uint32>(Digit - TEXT('0'))) / 10)
				return false;
			Number = Number * 10 + Digit - TEXT('0');
		}
		if (Number == 0)
			return false;
	}
	return true;
}

} // namespace VaRest::Http
