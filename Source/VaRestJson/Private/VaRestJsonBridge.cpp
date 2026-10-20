// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJson.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "VaRestJsonDocument.h"
#include "VaRestJsonLog.h"
#include "VaRestJsonUtf8.h"

namespace VaRest::Json::BridgePrivate
{
	constexpr int64 MaxExactDoubleInt = 9007199254740992ll; // 2^53

	FString Utf8ToString(const FStr& S)
	{
		return FString::ConstructFromPtrSize(S.Data, static_cast<int32>(S.Len));
	}

	/** Engine value for Value; nullptr with bTooDeep past MaxDepth. Depth counts the containers around Value. */
	TSharedPtr<FJsonValue> ToEngine(const FNode& Value, int32 Depth, bool& bTooDeep)
	{
		switch (Value.Type)
		{
		case EVaRestJsonType::None:
		case EVaRestJsonType::Null:
			return MakeShared<FJsonValueNull>();
		case EVaRestJsonType::Bool:
			return MakeShared<FJsonValueBoolean>(Value.Bool);
		case EVaRestJsonType::Number:
			switch (Value.NumKind)
			{
			case ENumKind::Int:
				// FJsonValueNumber holds a double; beyond 2^53 keep the exact digits instead
				if (Value.Int >= -MaxExactDoubleInt && Value.Int <= MaxExactDoubleInt)
				{
					return MakeShared<FJsonValueNumber>(static_cast<double>(Value.Int));
				}
				return MakeShared<FJsonValueNumberString>(LexToString(Value.Int));
			case ENumKind::UInt:
				return MakeShared<FJsonValueNumberString>(LexToString(Value.UInt));
			case ENumKind::Double:
				return MakeShared<FJsonValueNumber>(Value.Double);
			}
			break;
		case EVaRestJsonType::String:
			return MakeShared<FJsonValueString>(Utf8ToString(Value.Str));
		case EVaRestJsonType::Array:
		{
			if (Depth >= MaxDepth)
			{
				bTooDeep = true;
				return nullptr;
			}
			TArray<TSharedPtr<FJsonValue>> Items;
			Items.Reserve(Value.Arr.Num);
			for (uint32 i = 0; i < Value.Arr.Num; ++i)
			{
				TSharedPtr<FJsonValue> Item = ToEngine(*Value.Arr.Items[i], Depth + 1, bTooDeep);
				if (bTooDeep)
				{
					return nullptr;
				}
				Items.Add(MoveTemp(Item));
			}
			return MakeShared<FJsonValueArray>(Items);
		}
		case EVaRestJsonType::Object:
		{
			if (Depth >= MaxDepth)
			{
				bTooDeep = true;
				return nullptr;
			}
			TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
			for (uint32 i = 0; i < Value.Obj.Num; ++i)
			{
				const FMember& M = Value.Obj.Members[i];
				TSharedPtr<FJsonValue> Member = ToEngine(*M.Value, Depth + 1, bTooDeep);
				if (bTooDeep)
				{
					return nullptr;
				}
				Fields->SetField(FStringView(Utf8ToString(M.Key)), Member); // later duplicates overwrite
			}
			return MakeShared<FJsonValueObject>(Fields);
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

	// Out of line on purpose: the 256-byte scratch must not sit in the frame of FromEngine, which recurses to
	// MaxDepth (512) and may run on a small worker-thread stack.
	FORCENOINLINE FNode* NewStringNode(FDocument& Doc, const FString& Text)
	{
		FUtf8Scratch Scratch;
		return Doc.NewString(ConvertForDocument(Text, Scratch));
	}

	FORCENOINLINE void SetMember(FDocument& Doc, FNode& Object, const FString& Key, FNode* Member)
	{
		FUtf8Scratch Scratch;
		Doc.Set(Object, ConvertForDocument(Key, Scratch), Member);
	}

	/** Our node for Value; nullptr with bTooDeep past MaxDepth (an FJsonObject graph may even be cyclic) */
	FNode* FromEngine(FDocument& Doc, const FJsonValue& Value, int32 Depth, bool& bTooDeep)
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
			return NewStringNode(Doc, Value.AsString());
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
			if (Depth >= MaxDepth)
			{
				bTooDeep = true;
				return nullptr;
			}
			const TArray<TSharedPtr<FJsonValue>>& Items = Value.AsArray();
			FNode* Result = Doc.NewArray(Items.Num());
			for (const TSharedPtr<FJsonValue>& Item : Items)
			{
				FNode* Element = Item.IsValid() ? FromEngine(Doc, *Item, Depth + 1, bTooDeep) : Doc.NewNull();
				if (!Element)
				{
					return nullptr;
				}
				Doc.Append(*Result, Element);
			}
			return Result;
		}
		case EJson::Object:
		{
			if (Depth >= MaxDepth)
			{
				bTooDeep = true;
				return nullptr;
			}
			const TSharedPtr<FJsonObject>& Fields = Value.AsObject();
			FNode* Result = Doc.NewObject(Fields.IsValid() ? Fields->Values.Num() : 0);
			if (Fields.IsValid())
			{
				for (const auto& Pair : Fields->Values)
				{
					FNode* Member = Pair.Value.IsValid() ? FromEngine(Doc, *Pair.Value, Depth + 1, bTooDeep) : Doc.NewNull();
					if (!Member)
					{
						return nullptr;
					}
					SetMember(Doc, *Result, FString(*Pair.Key), Member); // FSharedString or FString key: both dereference to TCHAR*
				}
			}
			return Result;
		}
		}
		return Doc.NewNull();
	}
}

TSharedPtr<FJsonValue> FVaRestJson::ToJsonValue() const
{
	if (!Node)
	{
		return nullptr;
	}
	bool bTooDeep = false;
	TSharedPtr<FJsonValue> Result = VaRest::Json::BridgePrivate::ToEngine(*Node, 0, bTooDeep);
	if (bTooDeep)
	{
		UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson::ToJsonValue: cannot convert a value nested deeper than %d levels"), VaRest::Json::MaxDepth);
		return nullptr;
	}
	return Result;
}

TSharedPtr<FJsonObject> FVaRestJson::ToJsonObject() const
{
	if (!IsObject())
	{
		return nullptr;
	}
	const TSharedPtr<FJsonValue> Converted = ToJsonValue();
	return Converted.IsValid() ? Converted->AsObject() : nullptr;
}

FVaRestJson FVaRestJson::FromJsonValue(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid() || Value->Type == EJson::None)
	{
		return FVaRestJson();
	}
	FDocumentRef NewDoc = NewDocument();
	bool bTooDeep = false;
	VaRest::Json::FNode* Root = VaRest::Json::BridgePrivate::FromEngine(*NewDoc, *Value, 0, bTooDeep);
	if (bTooDeep)
	{
		UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson::FromJsonValue: cannot convert a value nested deeper than %d levels (or a cyclic FJsonObject graph)"), VaRest::Json::MaxDepth);
		return FVaRestJson();
	}
	NewDoc->Root = Root;
	return FVaRestJson(MoveTemp(NewDoc), Root);
}

FVaRestJson FVaRestJson::FromJsonObject(const TSharedPtr<FJsonObject>& InObject)
{
	return InObject.IsValid() ? FromJsonValue(MakeShared<FJsonValueObject>(InObject)) : FVaRestJson();
}
