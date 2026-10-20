// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/UnrealType.h"
#include "VaRestJsonConvert.h"
#include "VaRestJsonNode.h"
#include "VaRestJsonUtf8.h"

namespace VaRest::Json::Convert
{
	/** One convertible field of a struct, with the three spellings decode matches a JSON key against (spec 2.4) */
	struct FFieldInfo
	{
		const FProperty* Property = nullptr;
		/** The key encode writes: the naming policy (or a registered rename) applied to the authored name, UTF-8 */
		TArray<UTF8CHAR> Name;
		/** Name with ASCII letters lowered: the case-insensitive spelling */
		TArray<UTF8CHAR> Folded;
		/** Lowered, without '_', '-', ' ' and without a bool's 'b': user_id ~ UserId ~ userId */
		TArray<UTF8CHAR> Normalized;

		FUtf8StringView NameView() const { return FUtf8StringView(Name.GetData(), Name.Num()); }
	};

	/**
	 * JSON keys longer than this skip the policy-normalised field match, and decode shortens them in issue paths. No
	 * field's other spelling is that long; a hostile key ("n" + a megabyte of '_' + "umbers") would otherwise reach a
	 * field and repeat in the path of every issue beneath it.
	 */
	inline constexpr int32 LongKeyBytes = 256;

	/** The convertible fields of one struct under one naming policy. Immutable once built, so any thread may read it. */
	struct FFieldMap
	{
		TArray<FFieldInfo> Fields;
		/** Key hash (HashKey: the hash parsed object keys already carry) -> field index, per spelling */
		TMultiMap<uint32, int32> ByName;
		TMultiMap<uint32, int32> ByFolded;
		TMultiMap<uint32, int32> ByNormalized;

		/**
		 * The field a JSON key feeds: exact (OutTier 0), then case-insensitive (1), then normalised (2, only for keys of up
		 * to LongKeyBytes); INDEX_NONE when none
		 */
		int32 Match(FUtf8StringView Key, uint32 KeyHash, FUtf8Scratch& Scratch, uint8& OutTier) const;
	};

	using FFieldMapRef = TSharedRef<const FFieldMap, ESPMode::ThreadSafe>;

	/** Cached per (struct, naming policy), built on first use from any thread */
	FFieldMapRef GetFieldMap(const UScriptStruct* Struct, EVaRestJsonNaming Naming);

	/** The cooked-build Blueprint struct field name: "Score_3_<32 hex>" -> "Score" (UserDefinedStruct.cpp:300-313) */
	FString StripGuidSuffix(const FString& PropertyName);

	/**
	 * The name the policy applies to. A C++ property named like an engine hardcoded name (an EName) gets the engine's
	 * casing, the same in every build (ID for a field id); any other C++ property its declared casing; a Blueprint struct
	 * field the stripped name (never the editor-only friendly one)
	 */
	FString AuthoredFieldName(const UScriptStruct* Owner, const FProperty* Property);

	FString ApplyNaming(const FString& AuthoredName, bool bBoolField, EVaRestJsonNaming Naming);

	/** ASCII letters lowered into Out (reset first) */
	void FoldUtf8(FUtf8StringView Text, FUtf8Scratch& Out);

	/** FoldUtf8 without '_', '-' and ' ' */
	void NormalizeUtf8(FUtf8StringView Text, FUtf8Scratch& Out);

	/** Whether encode/decode convert this property; OutWhy says why not */
	bool IsSupported(const FProperty* Property, FString& OutWhy);

	/** Structs with a JSON form of their own instead of an object of fields */
	enum class ESpecialStruct : uint8
	{
		None,
		/** FVaRestJson: raw passthrough */
		Json,
		/** ISO 8601 string */
		DateTime,
		/** seconds, a number */
		Timespan,
		/** DigitsWithHyphens string */
		Guid,
		/** FSoftObjectPath / FSoftClassPath: the path string */
		SoftObjectPath
	};

	ESpecialStruct ClassifyStruct(const UScriptStruct* Struct);

	bool IsUnsignedInteger(const FNumericProperty* Property);

	/** TArray<uint8> without an enum: the one array that may be base64 */
	bool IsPlainByteArray(const FArrayProperty* Property);

	/**
	 * The double to write for a float: the shortest decimal (6 to 9 significant digits) that reads back as the same
	 * float, so 0.1f is written 0.1 and not 0.10000000149011612. The search scales by a power of ten that must be exact
	 * (10^22 at most), so it tries all four digit counts from 1e-14 up to 1e28, fewer from 1e-17 up to 1e-14, and none
	 * outside 1e-17..1e28: there the exact double is written, which still reads back as the same float.
	 */
	double FloatToJsonDouble(float Value);
}
