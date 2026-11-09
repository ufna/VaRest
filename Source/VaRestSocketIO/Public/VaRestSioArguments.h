// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestSocketCodec.h"
#include "VaRestJsonUtf8.h"
#include <type_traits>

namespace VaRest::Sio
{
template <typename T> struct TArgumentArray { static constexpr bool Value = false; };
template <typename T, typename A> struct TArgumentArray<TArray<T, A>> { static constexpr bool Value = true; using Element = T; };
template <typename T> struct TArgumentSet { static constexpr bool Value = false; };
template <typename T, typename K, typename A> struct TArgumentSet<TSet<T, K, A>> { static constexpr bool Value = true; using Element = T; };
template <typename T> struct TArgumentMap { static constexpr bool Value = false; };
template <typename K, typename V, typename A, typename F> struct TArgumentMap<TMap<K, V, A, F>> { static constexpr bool Value = true; using Key = K; using Mapped = V; };
template <typename T> struct TArgumentOptional { static constexpr bool Value = false; };
template <typename T> struct TArgumentOptional<TOptional<T>> { static constexpr bool Value = true; using Element = T; };

/** Borrowed writer and values. This object is used only during the native snapshot's synchronous Encode call. */
struct FArgumentWriter
{
	VaRest::Json::FCondensedWriter& Writer;
	const FVaRestJsonConvertOptions& Options;
	FVaRestJsonError& Error;
	TFunctionRef<bool()> WithinBudget;
	int64 Values = 0;
	bool String(FUtf8StringView Value, bool bKey = false)
	{
		if ((Options.MaxOutputBytes > 0 && Value.Len() > Options.MaxOutputBytes)
			|| !VaRest::Json::IsValidUtf8(Value)) return false;
		const char* Data = Value.IsEmpty() ? "" : reinterpret_cast<const char*>(Value.GetData());
		if (bKey) Writer.Key(Data, static_cast<rapidjson::SizeType>(Value.Len()));
		else Writer.String(Data, static_cast<rapidjson::SizeType>(Value.Len()));
		return WithinBudget();
	}
	bool String(FStringView Value, bool bKey = false)
	{
		if (Options.MaxOutputBytes > 0 && Value.Len() > Options.MaxOutputBytes) return false;
		const FTCHARToUTF8 Utf8(Value.IsEmpty() ? TEXT("") : Value.GetData(), Value.Len());
		return String(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Utf8.Get()), Utf8.Length()), bKey);
	}
	// Every constexpr arm returns its own result: string-only instantiations must
	// not retain an unreachable common tail under strict MSVC warnings.
	template <typename T> bool Value(const T& Into, int32 Depth = 1)
	{
		using U = std::remove_cvref_t<T>;
		if (Depth > VaRest::Json::MaxDepth || (Options.MaxValues > 0 && ++Values > Options.MaxValues) || !WithinBudget()) return false;
		if constexpr (std::is_same_v<U, std::nullptr_t>) { Writer.Null(); return WithinBudget(); }
		else if constexpr (std::is_same_v<U, bool>) { Writer.Bool(Into); return WithinBudget(); }
		else if constexpr (std::is_integral_v<U> && !std::is_same_v<U, TCHAR>)
		{
			if constexpr (std::is_signed_v<U>) Writer.Int64(static_cast<int64>(Into)); else Writer.Uint64(static_cast<uint64>(Into));
			return WithinBudget();
		}
		else if constexpr (std::is_floating_point_v<U>)
		{
			if (!FMath::IsFinite(Into)) Writer.Null();
			else if constexpr (std::is_same_v<U, float>) Writer.Double(VaRest::Json::FloatToJsonDoubleInternal(Into));
			else Writer.Double(static_cast<double>(Into));
			return WithinBudget();
		}
		else if constexpr (std::is_same_v<U, FString> || std::is_same_v<U, FStringView>) return String(FStringView(Into));
		else if constexpr (std::is_convertible_v<const T&, const TCHAR*>)
		{
			const TCHAR* Text = Into;
			if (!Text) { Writer.Null(); return WithinBudget(); }
			return String(FStringView(Text));
		}
		else if constexpr (std::is_same_v<U, FUtf8StringView>) return String(Into);
		else if constexpr (std::is_same_v<U, FName>) return String(FStringView(Into.ToString()));
		else if constexpr (std::is_same_v<U, FText>) return String(FStringView(Into.BuildSourceString()));
		else if constexpr (std::is_same_v<U, FVaRestJson>)
		{ if (!VaRest::Json::WriteJsonInternal(Writer, Into, Depth, &Error)) return false; return WithinBudget(); }
		else if constexpr (VaRest::Json::bHasScriptStruct<U>)
		{ if (!VaRest::Json::WriteStructInternal(Writer, VaRest::Json::GetScriptStruct<U>(), &Into, Options, Depth, &Error)) return false; return WithinBudget(); }
		else if constexpr (TArgumentOptional<U>::Value)
		{
			if (!Into.IsSet()) { Writer.Null(); return WithinBudget(); }
			return Value(Into.GetValue(), Depth);
		}
		else if constexpr (TArgumentArray<U>::Value)
		{
			if constexpr (std::is_same_v<typename TArgumentArray<U>::Element, uint8>)
			{
				FVaRestJson Replacement;
				if (!Options.BytesWriter || !Options.BytesWriter(MakeArrayView(Into), Replacement)) return false;
				if (!VaRest::Json::WriteJsonInternal(Writer, Replacement, Depth, &Error)) return false;
			}
			else
			{ Writer.StartArray(); for (const auto& Item : Into) if (!Value(Item, Depth + 1)) return false; Writer.EndArray(); }
			return WithinBudget();
		}
		else if constexpr (TArgumentSet<U>::Value)
		{ Writer.StartArray(); for (const auto& Item : Into) if (!Value(Item, Depth + 1)) return false; Writer.EndArray(); return WithinBudget(); }
		else if constexpr (TArgumentMap<U>::Value)
		{
			static_assert(std::is_same_v<typename TArgumentMap<U>::Key, FString>, "Socket.IO map arguments require FString keys");
			Writer.StartObject();
			for (const auto& Pair : Into) if (!String(FStringView(Pair.Key), true) || !Value(Pair.Value, Depth + 1)) return false;
			Writer.EndObject();
			return WithinBudget();
		}
		else static_assert(sizeof(T) == 0, "Socket.IO arguments support strings, numbers, bool, null, JSON, USTRUCT, TArray, TSet, TMap<FString,T> and TOptional");
	}
};
} // namespace VaRest::Sio
