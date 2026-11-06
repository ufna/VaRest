// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Containers/ArrayView.h"
#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "UObject/Class.h"
#include "VaRestJson.h"
#include "VaRestJsonRules.h"

#include <initializer_list>

#include "VaRestJsonConvert.generated.h"

class FProperty;

/**
 * How struct field names become JSON keys (spec 2.4). Decode accepts every policy's spelling regardless.
 * Word boundaries and case conversion use ASCII letters/digits; other characters stay unchanged on every platform.
 */
UENUM(BlueprintType)
enum class EVaRestJsonNaming : uint8
{
	/**
	 * The authored field name unchanged: the C++ property name, or a Blueprint struct field's name. A packaged game keeps
	 * one casing per FName, the one registered first, so for C++ names:
	 * - named like an engine hardcoded name (id, name) in another casing: the engine's casing (ID, Name) in every build,
	 *   the editor included;
	 * - any other name: its declared casing, which a packaged game may still replace with an FName of the same letters
	 *   registered earlier (the editor warns when it sees one, e.g. Blueprint's "softobject" for a field SoftObject).
	 * For an exact key register a rename (FVaRestJsonNames). The other policies normalise each word's case, so only a
	 * casing that moves a word boundary (SoftObject vs softobject) changes their output. FName values and FName map keys
	 * are written with their FName text and share this hazard, with no rename to fix it.
	 */
	AsIs,
	/** userId (the default) */
	CamelCase,
	/** user_id */
	SnakeCase,
	/** UserId */
	PascalCase
};

/**
 * Encode: offered every TArray<uint8> value (a field, or bytes inside a struct in a container) before the built-in
 * forms (an array of numbers; a base64 string with bBytesAsBase64). Return true after setting OutReplacement: the
 * encoder writes OutReplacement in place of the bytes through the same sink (RapidJSON writer or DOM builder),
 * counting its depth. Return false for the built-in forms. Called in walk order (field declaration order, then
 * container order), so a caller that numbers the values gets a deterministic numbering; empty arrays are offered too.
 * Socket.IO writes {"_placeholder":true,"num":N} and keeps a copy of Bytes (spec §18.2 J-1). Runs on the encoding thread.
 */
using FVaRestJsonBytesWriter = TFunction<bool(TConstArrayView<uint8> Bytes, FVaRestJson& OutReplacement)>;

/**
 * Decode: offered every value bound for a TArray<uint8> before the built-in forms (an array of numbers, a base64
 * string). Return true after filling OutBytes; false falls back to the built-in forms. Socket.IO binary attachments
 * plug in here ({"_placeholder":true,"num":0}). Path is the value's JSON path (a key over 256 bytes shortened, as in
 * decode issues). Runs on the decoding thread. Value is read-only: other threads may be reading the same document (spec
 * 2.2 thread contract).
 */
using FVaRestJsonBytesResolver = TFunction<bool(const FVaRestJson& Value, const FString& Path, TArray<uint8>& OutBytes)>;

struct FVaRestJsonConvertOptions
{
	EVaRestJsonNaming Naming = EVaRestJsonNaming::CamelCase;

	/** Decode: lenient coercions ("42" -> 42, 1 -> true) are refused and every mismatch is an error */
	bool bStrict = false;

	/** Encode enums as their numeric value instead of the enumerator name (decode accepts both) */
	bool bEnumsAsNumbers = false;

	/** Encode TArray<uint8> as one base64 string instead of an array of numbers (decode accepts both) */
	bool bBytesAsBase64 = false;

	/** Encode: per-value replacement for TArray<uint8> values (see FVaRestJsonBytesWriter); unset: the built-in forms */
	FVaRestJsonBytesWriter BytesWriter;

	/** Decode: per-value source of TArray<uint8> fields (see FVaRestJsonBytesResolver); unset: the built-in forms only */
	FVaRestJsonBytesResolver BytesResolver;

	/** Null captures the registered default once per conversion; an explicit snapshot applies throughout the walk. */
	TSharedPtr<const FVaRestJsonRuleSnapshot, ESPMode::ThreadSafe> Rules;

	/**
	 * Cumulative logical output/storage and controlled temporary byte charge, not heap/RSS or a peak cap.
	 * Includes conservative string sizing, DOM slots and typed element storage; excludes allocator slack,
	 * metadata caches and arbitrary native defaults/constructors/user callbacks. 0 preserves unlimited calls.
	 */
	int64 MaxOutputBytes = 0;

	/** Semantic conversion/default visits and emitted nodes/keys. 0 = unlimited. */
	int64 MaxValues = 0;

	/** Declared walk/match/copy units, not a bound on native/hash/callback CPU. 0 = unlimited. */
	int64 MaxWorkUnits = 0;
};

/** What a decode found, each entry with the JSON path of the value ("items[3].name"). Strict mode reports Errors. */
struct FVaRestJsonDecodeReport
{
	TArray<FVaRestJsonError> Warnings;
	TArray<FVaRestJsonError> Errors;
	/** Logical byte/value/work refusal, distinct from JSON type mismatch; sticky when this report is reused. */
	bool bBudgetExceeded = false;
};

/** A C++ field rename: Field is the property name, Json the key it is written as (no naming policy applies to it) */
struct FVaRestJsonRename
{
	FName Field;
	FString Json;
};

/**
 * C++ per-field renames (spec 2.4): FVaRestJsonNames::Register<FMyStruct>({ { TEXT("UserId"), TEXT("user_id") } }).
 * UPROPERTY meta is editor-only data, so names cannot come from meta in a cooked game; register at startup instead.
 * A rename is an FString and keeps its case: it is how a C++ field gets an exact key when another casing of its FName
 * wins -- an engine hardcoded name (id is written ID, name is written Name, in every build), or letters some module
 * registered earlier (in a packaged game it depends on registration order; the editor warns). FName values and FName
 * map keys are written with their FName text and share that hazard; a rename covers field names only.
 * A registered property name that does not exist warns when a field map is built; cached lookups do not repeat it.
 */
struct VARESTJSON_API FVaRestJsonNames
{
	static void Register(const UScriptStruct* Struct, TConstArrayView<FVaRestJsonRename> Renames);

	template <typename T>
	static void Register(std::initializer_list<FVaRestJsonRename> Renames)
	{
		Register(TBaseStructure<T>::Get(), TConstArrayView<FVaRestJsonRename>(Renames.begin(), static_cast<int32>(Renames.size())));
	}

	static void Unregister(const UScriptStruct* Struct);
	/** Native rename/cache publication epoch; dynamic callers capture/recheck it. */
	static uint64 GetGeneration();
};

/**
 * Struct <-> JSON (spec 2.4). Every function may run on any thread. The struct's reflection must not change meanwhile:
 * a Blueprint struct recompiled in the editor while a worker thread decodes into it frees the FProperty objects that
 * decode is reading (UserDefinedStructureCompilerUtils.cpp:221-225), and dropping the cache cannot help a decode
 * already in flight. Any reflection user shares that hazard; it exists only in the editor.
 * Native FString map keys of the same counted length compare without case and stop at U+0000;
 * distinct JSON keys can therefore merge (last wins).
 * A key containing U+0000 warns in lenient mode and is refused in strict mode; keep FVaRestJson for exact keys.
 */
struct VARESTJSON_API FVaRestJsonConvert
{
	// ---- encode: straight to UTF-8 bytes with the RapidJSON writer, no DOM, no FString (spec 2.4)

	/**
	 * Appends Struct's JSON to Out. False when a value nests deeper than VaRest::Json::MaxDepth: Out is left as it was
	 * and OutError is filled (or, without OutError, an error is logged).
	 */
	static bool StructToUtf8(const UScriptStruct* Struct, const void* Data, TArray<uint8>& Out,
		const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions(), EVaRestJsonFormat Format = EVaRestJsonFormat::Condensed,
		FVaRestJsonError* OutError = nullptr);

	/** StructToUtf8 as an FString; "" plus an error log on failure */
	static FString StructToString(const UScriptStruct* Struct, const void* Data,
		const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions(), EVaRestJsonFormat Format = EVaRestJsonFormat::Condensed);

	/** Struct as a DOM (to Set into another document); an invalid handle plus an error log on failure */
	static FVaRestJson StructToJson(const UScriptStruct* Struct, const void* Data, const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions());

	/** One property's value as a DOM; Value points at the value, not at its container (the BP wildcard nodes' encode) */
	static FVaRestJson PropertyToJson(const FProperty* Property, const void* Value, const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions());

	template <typename T>
	static TArray<uint8> ToUtf8(const T& Value, const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions(), EVaRestJsonFormat Format = EVaRestJsonFormat::Condensed)
	{
		TArray<uint8> Out;
		StructToUtf8(TBaseStructure<T>::Get(), &Value, Out, Options, Format);
		return Out;
	}

	template <typename T>
	static FVaRestJson ToJson(const T& Value, const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions())
	{
		return StructToJson(TBaseStructure<T>::Get(), &Value, Options);
	}

	// ---- decode: bytes -> arena DOM -> struct walk (spec 2.4; a SAX -> struct decoder is M7)

	/**
	 * Fills Data, an instance of Struct, from Json. Keys match fields exactly, then ignoring case, then normalised
	 * (user_id ~ UserId ~ userId), the best match winning whatever the member order; unknown keys are ignored; an absent
	 * field keeps its value; null keeps a struct and clears a container. Lenient coercions ("42" -> 42, 1 -> true) are
	 * refused in strict mode. Every mismatch is recorded with its JSON path in OutReport (without one: one summary
	 * warning per call). False when Json is neither an object nor null, past VaRest::Json::MaxDepth, or in strict mode
	 * on any error; Data may then be partly written. Any thread; Json's document must not be mutated meanwhile.
	 * Untrusted input: [0,0,...] into a TArray of a large struct allocates Count * sizeof(element) (two input bytes per
	 * element), and every distinct string decoded into an FName stays in the name table for the process's life. Text of
	 * 1024 characters or more bound for an FName (a field, a map key) or an object path is a mismatch: FName would abort.
	 * So is text with U+0000 bound for an FName, a date, a GUID or an object path (it would be cut there), and text
	 * bound for an object path that is not one ("Game/X": SetPath would make it the null path; "" and "None" are that).
	 * A string bound for a number field is coerced only up to 64 bytes. A report keeps the first 100 issues plus one
	 * closing entry that counts the rest. Keys over 256 bytes match a field only exactly or ignoring case, and are
	 * shortened ("...") in issue paths.
	 */
	static bool JsonToStruct(const FVaRestJson& Json, const UScriptStruct* Struct, void* Data,
		const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions(), FVaRestJsonDecodeReport* OutReport = nullptr);

	/** Parse, then JsonToStruct; a syntax error is reported as an Error (with its line and column) */
	static bool Utf8ToStruct(TConstArrayView<uint8> Utf8, const UScriptStruct* Struct, void* Data,
		const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions(), FVaRestJsonDecodeReport* OutReport = nullptr);
	/** Explicit M7 candidate. Supported native struct/array layouts use a validated SAX walk without a response DOM.
	 * Other layouts, raw JSON/byte callbacks, duplicate or reordered bound fields use the existing DOM semantics.
	 * This does not change the default decoder or qualify reflected worker/cooked conversion. */
	static bool Utf8ToStructStreaming(TConstArrayView<uint8> Utf8, const UScriptStruct* Struct, void* Data,
		const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions(), FVaRestJsonDecodeReport* OutReport = nullptr,
		bool* OutUsedStreaming = nullptr);

	/** One property's value (Value points at the value): the BP wildcard nodes' decode. True when the value was stored. */
	static bool JsonToProperty(const FVaRestJson& Json, const FProperty* Property, void* Value,
		const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions(), FVaRestJsonDecodeReport* OutReport = nullptr);
	/** Internal protocol adapter: array element i feeds converted struct field i, with one aggregate conversion budget.
	 * Missing arguments keep authored defaults and warn (strict: error); extras are ignored. No intermediate DOM. */
	static bool JsonArrayToStructInternal(const FVaRestJson& Json, const UScriptStruct* Struct, void* Data,
		const FVaRestJsonConvertOptions& Options, FVaRestJsonDecodeReport* OutReport);

	/**
	 * JsonToStruct into a new T (USTRUCT, TBaseStructure type or FTimespan); the error is the first error (or warning) reported; warnings of a successful decode are dropped.
	 * JSON null gives a default T (null keeps a struct's values, spec 2.4); TryGet treats null as absent instead.
	 */
	template <typename T>
	static TValueOrError<T, FVaRestJsonError> FromJson(const FVaRestJson& Json, const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions())
	{
		static_assert(VaRest::Json::bHasScriptStruct<T>, "FromJson<T>: T must be a USTRUCT, an engine struct with a TBaseStructure, or FTimespan");
		UScriptStruct* Struct = VaRest::Json::GetScriptStruct<T>();
		alignas(T) uint8 Storage[sizeof(T)];
		Struct->InitializeStruct(Storage);
		FVaRestJsonDecodeReport Report;
		const bool bDecoded = JsonToStruct(Json, Struct, Storage, Options, &Report);
		if (bDecoded)
		{
			T Value(MoveTemp(*reinterpret_cast<T*>(Storage)));
			Struct->DestroyStruct(Storage);
			return MakeValue(MoveTemp(Value));
		}
		Struct->DestroyStruct(Storage);
		FVaRestJsonError Error = Report.Errors.Num() > 0 ? Report.Errors[0] : Report.Warnings.Num() > 0 ? Report.Warnings[0]
																										: FVaRestJsonError();
		if (Error.Message.IsEmpty())
		{
			Error.Message = TEXT("the value is not an object");
		}
		return MakeError(MoveTemp(Error));
	}

	/** Drops every cached field map. The editor calls it when a Blueprint struct changes; FVaRestJsonNames calls it too. */
	static void ResetCache();

	/** For tests: how many (struct, naming policy) field maps are cached */
	static int32 NumCachedFieldMapsInternal();
};
