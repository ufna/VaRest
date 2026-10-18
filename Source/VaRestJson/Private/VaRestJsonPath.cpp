// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJson.h"

namespace
{
	bool ParseIndex(FStringView Digits, int32& OutIndex)
	{
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

	FVaRestJson Step(const FVaRestJson& Current, FStringView Token, bool bPointer)
	{
		if (Current.IsArray())
		{
			int32 Index = 0;
			return ParseIndex(Token, Index) ? Current.At(Index) : FVaRestJson();
		}
		if (Current.IsObject())
		{
			if (!bPointer)
			{
				return Current.Get(Token);
			}
			// RFC 6901 §4: ~1 -> '/', then ~0 -> '~'
			FString Key(Token);
			Key.ReplaceInline(TEXT("~1"), TEXT("/"), ESearchCase::CaseSensitive);
			Key.ReplaceInline(TEXT("~0"), TEXT("~"), ESearchCase::CaseSensitive);
			return Current.Get(FStringView(Key));
		}
		return FVaRestJson();
	}

	FVaRestJson WalkPointer(const FVaRestJson& Root, FStringView Expression)
	{
		FVaRestJson Current = Root;
		int32 Start = 1; // skip the leading '/'
		while (Current.IsValid())
		{
			int32 End = Start;
			while (End < Expression.Len() && Expression[End] != TEXT('/'))
			{
				++End;
			}
			Current = Step(Current, Expression.Mid(Start, End - Start), true);
			if (End >= Expression.Len())
			{
				break;
			}
			Start = End + 1;
		}
		return Current;
	}

	FVaRestJson WalkDotted(const FVaRestJson& Root, FStringView Expression)
	{
		FVaRestJson Current = Root;
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
				return FVaRestJson();
			}
			if (!Name.IsEmpty())
			{
				if (!Current.IsObject())
				{
					return FVaRestJson();
				}
				Current = Current.Get(Name);
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
				if (i >= Expression.Len() || !ParseIndex(Expression.Mid(DigitsStart, i - DigitsStart), Index) || !Current.IsArray())
				{
					return FVaRestJson();
				}
				Current = Current.At(Index);
				++i; // ']'
			}
			if (i < Expression.Len())
			{
				if (Expression[i] != TEXT('.') || i + 1 == Expression.Len())
				{
					return FVaRestJson(); // stray ']' or trailing '.'
				}
				++i;
			}
			bFirst = false;
			if (!Current.IsValid())
			{
				return FVaRestJson();
			}
		}
		return Current;
	}
}

FVaRestJson FVaRestJson::Path(FStringView Expression) const
{
	if (Expression.IsEmpty())
	{
		return *this;
	}
	return Expression[0] == TEXT('/') ? WalkPointer(*this, Expression) : WalkDotted(*this, Expression);
}
