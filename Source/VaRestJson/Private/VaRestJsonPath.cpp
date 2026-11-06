// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJson.h"

namespace VaRest::Json::PathPrivate
{
	bool ParseIndex(FStringView Digits, int32& OutIndex)
	{
		// At most 9 digits, so the value always fits int32 (no wrap: "/m/4294967296" must not reach m[0])
		if (Digits.IsEmpty() || Digits.Len() > 9 || (Digits.Len() > 1 && Digits[0] == TEXT('0')))
		{
			return false;
		}
		int32 Value = 0;
		for (TCHAR C : Digits)
		{
			if (C < TEXT('0') || C > TEXT('9'))
			{
				return false;
			}
			Value = Value * 10 + (C - TEXT('0'));
		}
		OutIndex = Value;
		return true;
	}

	/**
	 * RFC 6901 §4 unescaping in one left-to-right pass: "~1" -> '/', "~0" -> '~'. One pass is what makes
	 * "~01" decode to "~1" (the RFC's "first ~1, then ~0" order). Any other '~' makes the pointer invalid.
	 */
	bool UnescapePointerToken(FStringView Token, FString* OutKey = nullptr)
	{
		if (OutKey)
		{
			OutKey->Reset(Token.Len());
		}
		for (int32 i = 0; i < Token.Len(); ++i)
		{
			TCHAR Char = Token[i];
			if (Char == TEXT('~'))
			{
				if (i + 1 >= Token.Len() || (Token[i + 1] != TEXT('0') && Token[i + 1] != TEXT('1')))
				{
					return false;
				}
				Char = Token[++i] == TEXT('0') ? TEXT('~') : TEXT('/');
			}
			if (OutKey)
			{
				// AppendChar(0) is ignored by FString; JSON member names keep counted U+0000.
				OutKey->AppendChars(&Char, 1);
			}
		}
		return true;
	}

	/** One pointer token; Miss is returned (it keeps the document) for anything that does not resolve. */
	FVaRestJson PointerStep(const FVaRestJson& Current, FStringView Token, const FVaRestJson& Miss)
	{
		if (Current.IsArray())
		{
			int32 Index = 0;
			return ParseIndex(Token, Index) ? Current.At(Index) : Miss;
		}
		if (Current.IsObject())
		{
			FString Key;
			return UnescapePointerToken(Token, &Key) ? Current.Get(FStringView(Key)) : Miss;
		}
		return Miss;
	}

	template <typename FStep>
	bool VisitPointer(FStringView Expression, FStep&& Step)
	{
		int32 Start = 1; // skip the leading '/'
		while (true)
		{
			int32 End = Start;
			while (End < Expression.Len() && Expression[End] != TEXT('/'))
			{
				++End;
			}
			if (!Step(Expression.Mid(Start, End - Start)))
			{
				return false;
			}
			if (End >= Expression.Len())
			{
				return true;
			}
			Start = End + 1;
		}
	}

	template <typename FNameStep, typename FIndexStep>
	bool VisitDotted(FStringView Expression, FNameStep&& NameStep, FIndexStep&& IndexStep)
	{
		int32 i = 0;
		bool bFirst = true;
		while (i < Expression.Len())
		{
			// name
			const int32 NameStart = i;
			while (i < Expression.Len() && Expression[i] != TEXT('.') && Expression[i] != TEXT('[') && Expression[i] != TEXT(']'))
			{
				++i;
			}
			const FStringView Name = Expression.Mid(NameStart, i - NameStart);
			const bool bHasBracket = i < Expression.Len() && Expression[i] == TEXT('[');
			if (Name.IsEmpty() && !(bFirst && bHasBracket))
			{
				return false;
			}
			if (!Name.IsEmpty())
			{
				if (!NameStep(Name))
				{
					return false;
				}
			}
			// [index]*
			while (i < Expression.Len() && Expression[i] == TEXT('['))
			{
				const int32 DigitsStart = ++i;
				while (i < Expression.Len() && Expression[i] != TEXT(']'))
				{
					++i;
				}
				int32 Index = 0;
				if (i >= Expression.Len() || !ParseIndex(Expression.Mid(DigitsStart, i - DigitsStart), Index) || !IndexStep(Index))
				{
					return false;
				}
				++i; // ']'
			}
			if (i < Expression.Len())
			{
				if (Expression[i] != TEXT('.') || i + 1 == Expression.Len())
				{
					return false; // stray ']' or trailing '.'
				}
				++i;
			}
			bFirst = false;
		}
		return true;
	}

	FVaRestJson WalkPointer(const FVaRestJson& Root, FStringView Expression, const FVaRestJson& Miss)
	{
		FVaRestJson Current = Root;
		const bool bFound = VisitPointer(Expression, [&](FStringView Token) {
			Current = PointerStep(Current, Token, Miss);
			return Current.IsValid();
		});
		return bFound ? Current : Miss;
	}

	FVaRestJson WalkDotted(const FVaRestJson& Root, FStringView Expression, const FVaRestJson& Miss)
	{
		FVaRestJson Current = Root;
		const bool bFound = VisitDotted(Expression,
			[&](FStringView Name) {
				if (!Current.IsObject())
					return false;
				Current = Current.Get(Name);
				return Current.IsValid();
			},
			[&](int32 Index) {
				if (!Current.IsArray())
					return false;
				Current = Current.At(Index);
				return Current.IsValid();
			});
		return bFound ? Current : Miss;
	}
}

bool FVaRestJson::IsValidPath(FStringView Expression)
{
	if (Expression.IsEmpty())
	{
		return true; // the root, as Path("")
	}
	using namespace VaRest::Json::PathPrivate;
	return Expression[0] == TEXT('/')
		? VisitPointer(Expression, [](FStringView Token) { return UnescapePointerToken(Token); })
		: VisitDotted(Expression, [](FStringView) { return true; }, [](int32) { return true; });
}

FVaRestJson FVaRestJson::Path(FStringView Expression) const
{
	if (Expression.IsEmpty())
	{
		return *this;
	}
	// Every miss and every syntax error keeps the document (like Get/At), so a write through the
	// result is refused with an error instead of auto-vivifying a detached object.
	const FVaRestJson MissHandle = Miss();
	return Expression[0] == TEXT('/')
		? VaRest::Json::PathPrivate::WalkPointer(*this, Expression, MissHandle)
		: VaRest::Json::PathPrivate::WalkDotted(*this, Expression, MissHandle);
}
