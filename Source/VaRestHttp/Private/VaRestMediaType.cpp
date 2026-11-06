// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestMediaType.h"
#include "VaRestHttpTypes.h"
#include "VaRestUrl.h"

namespace VaRest::Http
{
bool IsValidMediaType(FStringView Media)
{
	if (!IsValidHeaderValue(Media))
	{
		return false;
	}
	int32 Index = 0;
	auto SkipSpace = [&] { while (Index < Media.Len() && (Media[Index] == TEXT(' ') || Media[Index] == TEXT('\t'))) ++Index; };
	auto Token = [&] {
		const int32 Begin = Index;
		while (Index < Media.Len() && IsValidHeaderName(FStringView(Media.GetData() + Index, 1)))
			++Index;
		return Index > Begin;
	};
	SkipSpace();
	if (!Token() || Index >= Media.Len() || Media[Index++] != TEXT('/') || !Token())
		return false;
	SkipSpace();
	while (Index < Media.Len())
	{
		if (Media[Index++] != TEXT(';'))
			return false;
		SkipSpace();
		if (!Token())
			return false;
		// Media parameters require name=value; OWS is permitted around semicolons, not around '='.
		if (Index >= Media.Len() || Media[Index++] != TEXT('='))
			return false;
		if (Index >= Media.Len())
			return false;
		if (Media[Index] == TEXT('"'))
		{
			++Index;
			bool bClosed = false;
			while (Index < Media.Len())
			{
				const TCHAR Character = Media[Index++];
				if (Character == TEXT('"'))
				{
					bClosed = true;
					break;
				}
				if (Character == TEXT('\\'))
				{
					if (Index >= Media.Len())
						return false;
					++Index;
				}
			}
			if (!bClosed)
				return false;
		}
		else if (!Token())
			return false;
		SkipSpace();
	}
	return true;
}

} // namespace VaRest::Http
