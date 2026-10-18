// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJson.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "VaRestJsonDocument.h"

using namespace VaRest::Json;

namespace
{
	constexpr int64 MaxExactDoubleInt = 9007199254740992ll; // 2^53

	FString Utf8ToString(const FStr& S)
	{
		return FString::ConstructFromPtrSize(S.Data, static_cast<int32>(S.Len));
	}

	TSharedPtr<FJsonValue> ToEngine(const FNode& Node)
	{
		switch (Node.Type)
		{
		case EVaRestJsonType::None:
		case EVaRestJsonType::Null:
			return MakeShared<FJsonValueNull>();
		case EVaRestJsonType::Bool:
			return MakeShared<FJsonValueBoolean>(Node.Bool);
		case EVaRestJsonType::Number:
			switch (Node.NumKind)
			{
			case ENumKind::Int:
				// FJsonValueNumber holds a double; beyond 2^53 keep the exact digits instead
				if (Node.Int >= -MaxExactDoubleInt && Node.Int <= MaxExactDoubleInt)
				{
					return MakeShared<FJsonValueNumber>(static_cast<double>(Node.Int));
				}
				return MakeShared<FJsonValueNumberString>(LexToString(Node.Int));
			case ENumKind::UInt:
				return MakeShared<FJsonValueNumberString>(LexToString(Node.UInt));
			case ENumKind::Double:
				return MakeShared<FJsonValueNumber>(Node.Double);
			}
			break;
		case EVaRestJsonType::String:
			return MakeShared<FJsonValueString>(Utf8ToString(Node.Str));
		case EVaRestJsonType::Array:
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			Items.Reserve(Node.Arr.Num);
			for (uint32 i = 0; i < Node.Arr.Num; ++i)
			{
				Items.Add(ToEngine(*Node.Arr.Items[i]));
			}
			return MakeShared<FJsonValueArray>(Items);
		}
		case EVaRestJsonType::Object:
		{
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			for (uint32 i = 0; i < Node.Obj.Num; ++i)
			{
				const FMember& M = Node.Obj.Members[i];
				Object->SetField(FStringView(Utf8ToString(M.Key)), ToEngine(*M.Value)); // later duplicates overwrite
			}
			return MakeShared<FJsonValueObject>(Object);
		}
		}
		return MakeShared<FJsonValueNull>();
	}

	/**
	 * Exact decimal integer: sign and magnitude. False for anything that is not an integer within the
	 * int64 (negative) or uint64 (positive) range. Allocates nothing, so a rejected candidate leaves no node behind.
	 */
	bool ParseIntegerDigits(const FString& S, bool& bNegative, uint64& Magnitude)
	{
		bNegative = S.StartsWith(TEXT("-"));
		int32 i = bNegative ? 1 : 0;
		if (i >= S.Len())
		{
			return false;
		}
		Magnitude = 0;
		for (; i < S.Len(); ++i)
		{
			if (!FChar::IsDigit(S[i]))
			{
				return false;
			}
			const uint64 Digit = static_cast<uint64>(S[i] - TEXT('0'));
			if (Magnitude > (MAX_uint64 - Digit) / 10)
			{
				return false; // overflow
			}
			Magnitude = Magnitude * 10 + Digit;
		}
		return !bNegative || Magnitude <= 9223372036854775808ull;
	}

	double ToDouble(bool bNegative, uint64 Magnitude)
	{
		const double Result = static_cast<double>(Magnitude); // round to nearest, as strtod does
		return bNegative ? -Result : Result;
	}

	FNode* MakeIntegerNode(FDocument& Doc, bool bNegative, uint64 Magnitude)
	{
		if (!bNegative)
		{
			return Doc.NewUInt(Magnitude); // stored as Int when it fits int64
		}
		return Doc.NewInt(Magnitude == 9223372036854775808ull ? MIN_int64 : -static_cast<int64>(Magnitude));
	}

	FNode* FromEngine(FDocument& Doc, const FJsonValue& Value)
	{
		switch (Value.Type)
		{
		case EJson::None:
		case EJson::Null:
			return Doc.NewNull();
		case EJson::Boolean:
			return Doc.NewBool(Value.AsBool());
		case EJson::String:
		{
			const FString S = Value.AsString();
			const auto Utf8 = StringCast<UTF8CHAR>(*S, S.Len());
			return Doc.NewString(FUtf8StringView(Utf8.Get(), Utf8.Length()));
		}
		case EJson::Number:
		{
			double D = 0.0;
			if (!Value.TryGetNumber(D))
			{
				return Doc.NewNull(); // malformed number string; no engine log
			}
			// Exact digits first. FJsonValueNumber::TryGetString is SanitizeFloat with 6 fractional digits and
			// trailing zeros trimmed (JsonValue.cpp:446-450), so for a plain double its text is not authoritative:
			// 0.9999999 would read as "1". Take the digits only when they round to the same double, which every
			// genuine number string does, so 64-bit values above 2^53 still come back exact.
			FString Digits;
			bool bNegative = false;
			uint64 Magnitude = 0;
			if (Value.TryGetString(Digits) && ParseIntegerDigits(Digits, bNegative, Magnitude) && ToDouble(bNegative, Magnitude) == D)
			{
				return MakeIntegerNode(Doc, bNegative, Magnitude);
			}
			// The engine writes integral doubles without a fraction, so 5.0 becomes 5 and -0.0 becomes 0 by design
			if (FMath::TruncToDouble(D) == D && D >= -9007199254740992.0 && D <= 9007199254740992.0)
			{
				return Doc.NewInt(static_cast<int64>(D));
			}
			return Doc.NewDouble(D);
		}
		case EJson::Array:
		{
			const TArray<TSharedPtr<FJsonValue>>& Items = Value.AsArray();
			FNode* Array = Doc.NewArray(Items.Num());
			for (const TSharedPtr<FJsonValue>& Item : Items)
			{
				Doc.Append(*Array, Item.IsValid() ? FromEngine(Doc, *Item) : Doc.NewNull());
			}
			return Array;
		}
		case EJson::Object:
		{
			const TSharedPtr<FJsonObject>& Object = Value.AsObject();
			FNode* Node = Doc.NewObject(Object.IsValid() ? Object->Values.Num() : 0);
			if (Object.IsValid())
			{
				for (const auto& Pair : Object->Values)
				{
					const FString Key(*Pair.Key); // FSharedString or FString storage: both dereference to TCHAR*
					const auto Utf8 = StringCast<UTF8CHAR>(*Key, Key.Len());
					Doc.Set(*Node, FUtf8StringView(Utf8.Get(), Utf8.Length()), Pair.Value.IsValid() ? FromEngine(Doc, *Pair.Value) : Doc.NewNull());
				}
			}
			return Node;
		}
		}
		return Doc.NewNull();
	}
}

TSharedPtr<FJsonValue> FVaRestJson::ToJsonValue() const
{
	return Node ? ToEngine(*Node) : TSharedPtr<FJsonValue>();
}

TSharedPtr<FJsonObject> FVaRestJson::ToJsonObject() const
{
	return IsObject() ? ToEngine(*Node)->AsObject() : TSharedPtr<FJsonObject>();
}

FVaRestJson FVaRestJson::FromJsonValue(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid() || Value->Type == EJson::None)
	{
		return FVaRestJson();
	}
	FDocumentRef NewDoc = NewDocument();
	FNode* Root = FromEngine(*NewDoc, *Value);
	NewDoc->Root = Root;
	return FVaRestJson(MoveTemp(NewDoc), Root);
}

FVaRestJson FVaRestJson::FromJsonObject(const TSharedPtr<FJsonObject>& InObject)
{
	return InObject.IsValid() ? FromJsonValue(MakeShared<FJsonValueObject>(InObject)) : FVaRestJson();
}
