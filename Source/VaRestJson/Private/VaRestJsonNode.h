// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Hash/CityHash.h"
#include "VaRestJson.h"

namespace VaRest::Json
{
	struct FNode;

	/** UTF-8 bytes inside a document (the in-situ input buffer or the arena), NUL-terminated. */
	struct FStr
	{
		const UTF8CHAR* Data;
		uint32 Len;
		/** CityHash32 of the bytes; set for object keys, 0 otherwise */
		uint32 Hash;

		FUtf8StringView View() const { return FUtf8StringView(Data, Len); }
	};

	struct FMember
	{
		FStr Key;
		FNode* Value;
	};

	/** Open-addressing index over an object's members: slot = member index + 1, 0 = empty. */
	struct FIndex
	{
		uint32 Capacity; // power of two, at least twice the member count
		uint32* Slots;
	};

	enum class ENumKind : uint8
	{
		Int,    // any value representable as int64
		UInt,   // only values above INT64_MAX
		Double
	};

	struct FNode
	{
		struct FArr
		{
			FNode** Items;
			uint32 Num;
			uint32 Max;
		};

		struct FObj
		{
			FMember* Members; // insertion order; duplicates allowed (parser)
			uint32 Num;
			uint32 Max;
			FIndex* Index;    // non-null above IndexThreshold members
		};

		EVaRestJsonType Type = EVaRestJsonType::Null;
		ENumKind NumKind = ENumKind::Int;
		union
		{
			bool Bool;
			int64 Int;
			uint64 UInt;
			double Double;
			FStr Str;
			FArr Arr;
			FObj Obj;
		};

		FNode()
			: Obj{ nullptr, 0, 0, nullptr }
		{
		}
	};

	/** Objects with more members than this carry an FIndex, built at parse time or on mutation -- never on read */
	inline constexpr uint32 IndexThreshold = 8;

	inline uint32 HashKey(FUtf8StringView Key)
	{
		return CityHash32(reinterpret_cast<const char*>(Key.GetData()), static_cast<uint32>(Key.Len()));
	}

	/** Byte-exact comparison. TStringView::operator== is case-insensitive in UE, so it must not be used for keys. */
	inline bool KeyEquals(const FStr& A, FUtf8StringView B)
	{
		return A.Len == static_cast<uint32>(B.Len()) && (A.Len == 0 || FMemory::Memcmp(A.Data, B.GetData(), A.Len) == 0);
	}

	/** Number -> int64: any Int, and integral doubles in [-2^63, 2^63) (both bounds are exact doubles). UInt holds only values above INT64_MAX. */
	inline bool NodeToInt64(const FNode& Value, int64& Out)
	{
		if (Value.Type != EVaRestJsonType::Number)
		{
			return false;
		}
		switch (Value.NumKind)
		{
		case ENumKind::Int:
			Out = Value.Int;
			return true;
		case ENumKind::UInt:
			return false;
		case ENumKind::Double:
			if (FMath::IsFinite(Value.Double) && FMath::TruncToDouble(Value.Double) == Value.Double
				&& Value.Double >= -9223372036854775808.0 && Value.Double < 9223372036854775808.0)
			{
				Out = static_cast<int64>(Value.Double);
				return true;
			}
			return false;
		}
		return false;
	}

	/** Number -> uint64: non-negative Int, UInt, and integral doubles in [0, 2^64) */
	inline bool NodeToUInt64(const FNode& Value, uint64& Out)
	{
		if (Value.Type != EVaRestJsonType::Number)
		{
			return false;
		}
		switch (Value.NumKind)
		{
		case ENumKind::Int:
			if (Value.Int < 0)
			{
				return false;
			}
			Out = static_cast<uint64>(Value.Int);
			return true;
		case ENumKind::UInt:
			Out = Value.UInt;
			return true;
		case ENumKind::Double:
			if (FMath::IsFinite(Value.Double) && FMath::TruncToDouble(Value.Double) == Value.Double
				&& Value.Double >= 0.0 && Value.Double < 18446744073709551616.0)
			{
				Out = static_cast<uint64>(Value.Double);
				return true;
			}
			return false;
		}
		return false;
	}

	inline bool NodeToDouble(const FNode& Value, double& Out)
	{
		if (Value.Type != EVaRestJsonType::Number)
		{
			return false;
		}
		switch (Value.NumKind)
		{
		case ENumKind::Int:    Out = static_cast<double>(Value.Int); return true;
		case ENumKind::UInt:   Out = static_cast<double>(Value.UInt); return true;
		case ENumKind::Double: Out = Value.Double; return true;
		}
		return false;
	}
}
