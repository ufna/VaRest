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
	 * Exact decimal integer parse with overflow detection. LexTryParseString is not used: it is not
	 * documented to reject out-of-range input, and an overflowed id would come back silently wrong.
	 */
	FNode* TryIntegerNode(FDocument& Doc, const FString& S)
	{
		const bool bNegative = S.StartsWith(TEXT("-"));
		int32 i = bNegative ? 1 : 0;
		if (i >= S.Len())
		{
			return nullptr;
		}
		uint64 Magnitude = 0;
		for (; i < S.Len(); ++i)
		{
			if (!FChar::IsDigit(S[i]))
			{
				return nullptr;
			}
			const uint64 Digit = static_cast<uint64>(S[i] - TEXT('0'));
			if (Magnitude > (MAX_uint64 - Digit) / 10)
			{
				return nullptr; // overflow: not representable as an integer, let it be a double
			}
			Magnitude = Magnitude * 10 + Digit;
		}
		if (bNegative)
		{
			if (Magnitude > 9223372036854775808ull)
			{
				return nullptr;
			}
			return Doc.NewInt(Magnitude == 9223372036854775808ull ? MIN_int64 : -static_cast<int64>(Magnitude));
		}
		return Doc.NewUInt(Magnitude); // stored as Int when it fits int64
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
			// FJsonValueNumberString carries exact digits; prefer them when they are an integer
			FString Digits;
			if (Value.TryGetString(Digits))
			{
				if (FNode* Integer = TryIntegerNode(Doc, Digits))
				{
					return Integer;
				}
			}
			const double D = Value.AsNumber();
			// The engine writes integral doubles without a fraction; keep them integers so text matches
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
