// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestUrl.h"

#include "VaRestJsonUtf8.h"

#include <initializer_list>

namespace VaRest::Http::UrlPrivate
{
	const TCHAR HexDigits[] = TEXT("0123456789ABCDEF");

	bool IsAlnum(uint32 C)
	{
		return (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9');
	}

	/** RFC 3986 2.3 */
	bool IsUnreserved(uint32 C)
	{
		return IsAlnum(C) || C == '-' || C == '.' || C == '_' || C == '~';
	}

	/** WHATWG URL Standard, application/x-www-form-urlencoded byte serializer */
	bool IsFormSafe(uint32 C)
	{
		return IsAlnum(C) || C == '*' || C == '-' || C == '.' || C == '_';
	}

	int32 HexValue(uint32 C)
	{
		if (C >= '0' && C <= '9')
		{
			return static_cast<int32>(C - '0');
		}
		if (C >= 'a' && C <= 'f')
		{
			return static_cast<int32>(C - 'a' + 10);
		}
		if (C >= 'A' && C <= 'F')
		{
			return static_cast<int32>(C - 'A' + 10);
		}
		return -1;
	}

	template <typename PredicateType>
	FString Encode(FStringView Text, PredicateType&& IsSafe, bool bSpaceAsPlus)
	{
		// The UTF-8 bytes are what is encoded; an unpaired surrogate becomes U+FFFD, as everywhere in VaRest (spec 2.2)
		VaRest::Json::FUtf8Scratch Scratch;
		const FUtf8StringView Utf8 = VaRest::Json::ConvertForDocument(Text, Scratch, /*bWarn*/ false);
		FString Out;
		Out.Reserve(Utf8.Len());
		for (const UTF8CHAR Ch : Utf8)
		{
			const uint32 C = static_cast<uint8>(Ch);
			if (IsSafe(C))
			{
				Out.AppendChar(static_cast<TCHAR>(C));
			}
			else if (bSpaceAsPlus && C == ' ')
			{
				Out.AppendChar(TEXT('+'));
			}
			else
			{
				Out.AppendChar(TEXT('%'));
				Out.AppendChar(HexDigits[C >> 4]);
				Out.AppendChar(HexDigits[C & 15]);
			}
		}
		return Out;
	}

	bool IsTokenChar(TCHAR C)
	{
		if (IsAlnum(static_cast<uint32>(C)))
		{
			return true;
		}
		switch (C)
		{
		case TEXT('!'): case TEXT('#'): case TEXT('$'): case TEXT('%'): case TEXT('&'): case TEXT('\''): case TEXT('*'):
		case TEXT('+'): case TEXT('-'): case TEXT('.'): case TEXT('^'): case TEXT('_'): case TEXT('`'): case TEXT('|'): case TEXT('~'):
			return true;
		default:
			return false;
		}
	}
}

FString VaRest::Http::PercentEncode(FStringView Text)
{
	return UrlPrivate::Encode(Text, UrlPrivate::IsUnreserved, false);
}

FString VaRest::Http::FormEncode(FStringView Text)
{
	return UrlPrivate::Encode(Text, UrlPrivate::IsFormSafe, true);
}

FString VaRest::Http::PercentDecode(FStringView Text, bool bPlusIsSpace)
{
	using namespace UrlPrivate;
	VaRest::Json::FUtf8Scratch Scratch;
	const FUtf8StringView Utf8 = VaRest::Json::ConvertForDocument(Text, Scratch, /*bWarn*/ false);
	VaRest::Json::FUtf8Scratch Bytes;
	Bytes.Reserve(Utf8.Len());
	for (int32 i = 0; i < Utf8.Len(); ++i)
	{
		const uint32 C = static_cast<uint8>(Utf8[i]);
		if (C == '%' && i + 2 < Utf8.Len())
		{
			const int32 Hi = HexValue(static_cast<uint8>(Utf8[i + 1]));
			const int32 Lo = HexValue(static_cast<uint8>(Utf8[i + 2]));
			if (Hi >= 0 && Lo >= 0)
			{
				Bytes.Add(static_cast<UTF8CHAR>(Hi * 16 + Lo));
				i += 2;
				continue;
			}
		}
		Bytes.Add(bPlusIsSpace && C == '+' ? static_cast<UTF8CHAR>(' ') : Utf8[i]);
	}
	VaRest::Json::FUtf8Scratch Repaired;
	VaRest::Json::AppendRepairedUtf8(FUtf8StringView(Bytes.GetData(), Bytes.Num()), Repaired);
	return FString::ConstructFromPtrSize(Repaired.GetData(), Repaired.Num());
}

bool VaRest::Http::IsAbsoluteHttpUrl(FStringView Url)
{
	const FStringView Trimmed = Url.TrimStartAndEnd();
	for (const FStringView Scheme : { TEXTVIEW("http://"), TEXTVIEW("https://") })
	{
		if (Trimmed.StartsWith(Scheme, ESearchCase::IgnoreCase))
		{
			const FStringView Rest = Trimmed.RightChop(Scheme.Len());
			return !Rest.IsEmpty() && Rest[0] != TEXT('/') && Rest[0] != TEXT('?') && Rest[0] != TEXT('#');
		}
	}
	return false;
}

FString VaRest::Http::JoinUrl(FStringView BaseUrl, FStringView Path)
{
	FStringView Base = BaseUrl.TrimStartAndEnd();
	FStringView Rest = Path.TrimStartAndEnd();
	if (IsAbsoluteHttpUrl(Rest) || Base.IsEmpty())
	{
		return FString(Rest);
	}
	// Decision 33 (review M22): a base URL's query stays, after the joined path and before the request's own pairs
	// ("https://h/v1?key=k" + "/pets?limit=2" -> "https://h/v1/pets?key=k&limit=2"). A base with a fragment is refused
	// before this (Resolve, Kind=Config).
	FStringView BaseQuery;
	int32 BaseQueryAt = INDEX_NONE;
	if (Base.FindChar(TEXT('?'), BaseQueryAt))
	{
		BaseQuery = Base.RightChop(BaseQueryAt + 1);
		Base = Base.Left(BaseQueryAt);
	}
	// The request's own tail: its query and fragment, after its path
	FStringView Tail;
	for (int32 i = 0; i < Rest.Len(); ++i)
	{
		if (Rest[i] == TEXT('?') || Rest[i] == TEXT('#'))
		{
			Tail = Rest.RightChop(i);
			Rest = Rest.Left(i);
			break;
		}
	}
	FString Out;
	Out.Reserve(Base.Len() + 1 + Rest.Len() + 1 + BaseQuery.Len() + Tail.Len());
	if (Rest.IsEmpty())
	{
		Out += Base;
	}
	else
	{
		while (Base.EndsWith(TEXT('/')))
		{
			Base.LeftChopInline(1);
		}
		while (Rest.StartsWith(TEXT('/')))
		{
			Rest.RightChopInline(1);
		}
		Out += Base;
		Out.AppendChar(TEXT('/'));
		Out += Rest;
	}
	if (!BaseQuery.IsEmpty())
	{
		Out.AppendChar(TEXT('?'));
		Out += BaseQuery;
		if (Tail.StartsWith(TEXT('?')))
		{
			Tail.RightChopInline(1);
			if (!Tail.IsEmpty() && Tail[0] != TEXT('#'))
			{
				Out.AppendChar(TEXT('&'));
			}
		}
	}
	Out += Tail;
	return Out;
}

TValueOrError<FString, FString> VaRest::Http::ExpandPathTemplate(FStringView Template, TConstArrayView<TPair<FString, FString>> Params)
{
	TArray<bool, TInlineAllocator<8>> Used;
	Used.SetNumZeroed(Params.Num());
	FString Out;
	Out.Reserve(Template.Len() + 16);
	for (int32 i = 0; i < Template.Len(); ++i)
	{
		const TCHAR C = Template[i];
		if (C == TEXT('}'))
		{
			return MakeError(FString::Printf(TEXT("unmatched '}' in path template \"%s\""), *FString(Template)));
		}
		if (C != TEXT('{'))
		{
			Out.AppendChar(C);
			continue;
		}
		int32 Close = INDEX_NONE;
		for (int32 j = i + 1; j < Template.Len() && Template[j] != TEXT('{'); ++j)
		{
			if (Template[j] == TEXT('}'))
			{
				Close = j;
				break;
			}
		}
		if (Close == INDEX_NONE)
		{
			return MakeError(FString::Printf(TEXT("unclosed '{' in path template \"%s\""), *FString(Template)));
		}
		const FStringView Name = Template.Mid(i + 1, Close - i - 1);
		if (Name.IsEmpty())
		{
			return MakeError(FString::Printf(TEXT("empty {} in path template \"%s\""), *FString(Template)));
		}
		int32 Found = INDEX_NONE;
		for (int32 p = 0; p < Params.Num(); ++p)
		{
			if (FStringView(Params[p].Key).Equals(Name, ESearchCase::CaseSensitive))
			{
				Used[p] = true;
				Found = p; // keep going: the last value for a name wins
			}
		}
		if (Found == INDEX_NONE)
		{
			return MakeError(FString::Printf(TEXT("path parameter {%s} has no value"), *FString(Name)));
		}
		if (Params[Found].Value.IsEmpty())
		{
			return MakeError(FString::Printf(TEXT("path parameter {%s} is empty"), *FString(Name)));
		}
		Out += PercentEncode(Params[Found].Value);
		i = Close;
	}
	for (int32 p = 0; p < Params.Num(); ++p)
	{
		if (!Used[p])
		{
			return MakeError(FString::Printf(TEXT("path parameter \"%s\" is not in the path template \"%s\""), *Params[p].Key, *FString(Template)));
		}
	}
	return MakeValue(MoveTemp(Out));
}

void VaRest::Http::AppendQuery(FString& Url, TConstArrayView<TPair<FString, FString>> Query)
{
	if (Query.Num() == 0)
	{
		return;
	}
	FString Fragment;
	int32 Hash = INDEX_NONE;
	if (Url.FindChar(TEXT('#'), Hash))
	{
		Fragment = Url.Mid(Hash);
		Url.LeftInline(Hash);
	}
	int32 Question = INDEX_NONE;
	bool bNeedSeparator = true;
	TCHAR Separator = TEXT('?');
	if (Url.FindChar(TEXT('?'), Question))
	{
		Separator = TEXT('&');
		bNeedSeparator = !(Url.EndsWith(TEXT("?")) || Url.EndsWith(TEXT("&")));
	}
	for (const TPair<FString, FString>& Pair : Query)
	{
		if (bNeedSeparator)
		{
			Url.AppendChar(Separator);
		}
		bNeedSeparator = true;
		Separator = TEXT('&');
		Url += PercentEncode(Pair.Key);
		Url.AppendChar(TEXT('='));
		Url += PercentEncode(Pair.Value);
	}
	Url += Fragment;
}

void VaRest::Http::AddQueryValues(TArray<TPair<FString, FString>>& Query, FStringView Name, TConstArrayView<FString> Values, EVaRestQueryStyle Style)
{
	if (Values.Num() == 0)
	{
		return;
	}
	if (Style == EVaRestQueryStyle::Repeat)
	{
		for (const FString& Value : Values)
		{
			Query.Emplace(FString(Name), Value);
		}
		return;
	}
	const TCHAR* Joiner = Style == EVaRestQueryStyle::Csv ? TEXT(",") : Style == EVaRestQueryStyle::Ssv ? TEXT(" ") : TEXT("|");
	Query.Emplace(FString(Name), FString::Join(Values, Joiner));
}

bool VaRest::Http::IsValidHeaderName(FStringView Name)
{
	if (Name.IsEmpty())
	{
		return false;
	}
	for (const TCHAR C : Name)
	{
		if (!UrlPrivate::IsTokenChar(C))
		{
			return false;
		}
	}
	return true;
}

bool VaRest::Http::IsValidHeaderValue(FStringView Value)
{
	for (const TCHAR C : Value)
	{
		if (C == TEXT('\r') || C == TEXT('\n') || C == TEXT('\0'))
		{
			return false;
		}
	}
	return true;
}
