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

	/**
	 * Review M2: the key of a continuation pair made by AddQueryValues (Csv, Pipes, Ssv), standing for the delimiter that
	 * joins its value to the pair before it. Each is a lone low surrogate (U+DC01..U+DC03): valid text never holds one, since
	 * a low surrogate only follows a high one (and ConvertForDocument turns a stray one into U+FFFD), so a plain value,
	 * which is never inspected, cannot be mistaken for a marker. Only a whole key is read, never a value.
	 */
	constexpr TCHAR CsvMarker = 0xDC01;
	constexpr TCHAR PipesMarker = 0xDC02;
	constexpr TCHAR SsvMarker = 0xDC03;

	/** The literal a marker key stands for, or null when Key is not a marker */
	const TCHAR* MarkerDelimiter(const FString& Key)
	{
		if (Key.Len() != 1)
		{
			return nullptr;
		}
		return Key[0] == CsvMarker ? TEXT(",") : Key[0] == PipesMarker ? TEXT("|") : Key[0] == SsvMarker ? TEXT("%20") : nullptr;
	}

	/** The host part of "http(s)://host/...": [Start, End) */
	bool FindAuthority(FStringView Url, int32& Start, int32& End)
	{
		for (const FStringView Scheme : { TEXTVIEW("http://"), TEXTVIEW("https://") })
		{
			if (Url.StartsWith(Scheme, ESearchCase::IgnoreCase))
			{
				Start = Scheme.Len();
				End = Start;
				while (End < Url.Len() && Url[End] != TEXT('/') && Url[End] != TEXT('?') && Url[End] != TEXT('#'))
				{
					++End;
				}
				return true;
			}
		}
		return false;
	}

	/**
	 * RFC 3987 3.1 IRI to URI (review I2): after the authority, every UTF-8 byte >= 0x80, CTL and space becomes %XX; other
	 * ASCII, '%' included, stays so that already-encoded text is not encoded twice. The engine would otherwise turn each
	 * non-ASCII character into '?' (CurlHttp.cpp:926 TCHAR_TO_ANSI; GenericPlatformString.h:90-93) and curl rejects a space.
	 */
	FString ToUri(FStringView Url)
	{
		int32 Start = 0;
		int32 AuthorityEnd = 0;
		if (!FindAuthority(Url, Start, AuthorityEnd))
		{
			AuthorityEnd = 0;
		}
		VaRest::Json::FUtf8Scratch Scratch;
		const FUtf8StringView Utf8 = VaRest::Json::ConvertForDocument(Url.RightChop(AuthorityEnd), Scratch, /*bWarn*/ false);
		FString Out(Url.Left(AuthorityEnd));
		Out.Reserve(Url.Len() + 8);
		for (const UTF8CHAR Ch : Utf8)
		{
			const uint32 C = static_cast<uint8>(Ch);
			if (C >= 0x80 || C < 0x20 || C == 0x7F || C == ' ')
			{
				Out.AppendChar(TEXT('%'));
				Out.AppendChar(HexDigits[C >> 4]);
				Out.AppendChar(HexDigits[C & 15]);
			}
			else
			{
				Out.AppendChar(static_cast<TCHAR>(C));
			}
		}
		return Out;
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
		return UrlPrivate::ToUri(Rest);
	}
	// A fragment is never sent (RFC 9110 7.1), and left on the base it would swallow the path and query that follow
	// ("https://h/v1#f" + "/pets" -> "https://h/v1#f/pets"); the join drops it (review M1).
	int32 BaseFragmentAt = INDEX_NONE;
	if (Base.FindChar(TEXT('#'), BaseFragmentAt))
	{
		Base = Base.Left(BaseFragmentAt);
	}
	// Decision 33 (review M22): a base URL's query stays, after the joined path and before the request's own pairs
	// ("https://h/v1?key=k" + "/pets?limit=2" -> "https://h/v1/pets?key=k&limit=2").
	FStringView BaseQuery;
	int32 BaseQueryAt = INDEX_NONE;
	if (Base.FindChar(TEXT('?'), BaseQueryAt))
	{
		BaseQuery = Base.RightChop(BaseQueryAt + 1);
		Base = Base.Left(BaseQueryAt);
		// "?a=1&" + "&b=2" would otherwise join as "a=1&&b=2" (review M10)
		while (BaseQuery.EndsWith(TEXT('&')))
		{
			BaseQuery.LeftChopInline(1);
		}
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
	return UrlPrivate::ToUri(Out);
}

bool VaRest::Http::HasNonAsciiHost(FStringView Url)
{
	int32 Start = 0;
	int32 End = 0;
	if (!UrlPrivate::FindAuthority(Url.TrimStartAndEnd(), Start, End))
	{
		return false;
	}
	for (const TCHAR C : Url.TrimStartAndEnd().Mid(Start, End - Start))
	{
		if (C > 0x7F)
		{
			return true;
		}
	}
	return false;
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
		// Review I1: '.' is unreserved, so "id=.." would expand to "/users/../delete" and curl (which never sets
		// CURLOPT_PATH_AS_IS) would send "/delete". %2E does not help, servers decode and normalise it.
		if (Params[Found].Value == TEXT(".") || Params[Found].Value == TEXT(".."))
		{
			return MakeError(FString::Printf(TEXT("path parameter {%s} is a dot-segment"), *FString(Name)));
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
	bool bFirst = true;
	for (const TPair<FString, FString>& Pair : Query)
	{
		// A marker key continues the pair before it: "<delimiter><value>", no '&' and no name. At the start of the list
		// there is nothing to continue, so it is an ordinary pair.
		const TCHAR* Delimiter = bFirst ? nullptr : UrlPrivate::MarkerDelimiter(Pair.Key);
		bFirst = false;
		if (Delimiter != nullptr)
		{
			Url += Delimiter;
			Url += PercentEncode(Pair.Value);
			continue;
		}
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
	// Review M2: each value is encoded on its own and the delimiter is written literally (RFC 6570 form expansion,
	// OpenAPI 3 style=form explode=false), so ["a,b","c"] is "a%2Cb,c", not the ambiguous "a%2Cb%2Cc". The pair type has
	// no room for a flag, so every value after the first is a pair whose key is a marker (see CsvMarker); values are
	// stored as given.
	const TCHAR Marker = Style == EVaRestQueryStyle::Csv ? UrlPrivate::CsvMarker : Style == EVaRestQueryStyle::Ssv ? UrlPrivate::SsvMarker : UrlPrivate::PipesMarker;
	Query.Emplace(FString(Name), Values[0]);
	for (int32 i = 1; i < Values.Num(); ++i)
	{
		Query.Emplace(FString(1, &Marker), Values[i]);
	}
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
	// RFC 9110 5.5: a field value holds no CTL but HTAB. CR, LF and NUL are the injection bytes; the rest are refused
	// because a sender must not generate invalid syntax (RFC 9110 2.2, review M5). Bytes >= 0x80 pass: curl sends the
	// header as UTF-8 (CurlHttp.cpp:1030, TCHAR_TO_UTF8), so no character can truncate into a CR or LF.
	for (const TCHAR C : Value)
	{
		if ((C < 0x20 && C != TEXT('\t')) || C == 0x7F)
		{
			return false;
		}
	}
	return true;
}

