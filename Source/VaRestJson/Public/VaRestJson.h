// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/ValueOrError.h"

#include <type_traits>

#include "VaRestJson.generated.h"

class FJsonObject;
class FJsonValue;

namespace VaRest::Json
{
	struct FNode;
	class FDocument;
}

struct FVaRestJsonMember;
class FVaRestJsonMemberRange;
class FVaRestJsonElementRange;

namespace VaRest::Json
{
	/**
	 * A double at or above this magnitude rounds to an infinite float: it is halfway between FLT_MAX (2^128 - 2^104)
	 * and 2^128, and round-half-even rounds FLT_MAX's odd mantissa up. Below it the cast gives FLT_MAX at most.
	 * Exact as a double: 2^103 * (2^25 - 1).
	 */
	inline constexpr double FloatRoundsToInfinity = 340282356779733661637539395458142568448.0;

	/**
	 * The one nesting limit (spec 2.2). Parse, write, Clone, Equals, the FJsonValue bridges, Serialize and struct convert
	 * all recurse once per level (the parser is iterative but shares the limit), so this bounds stack use on worker
	 * threads too. A tree built deeper through Set/Add is not checked on the way in (that would cost every Set); every
	 * recursive operation on it fails as a whole with an error instead of truncating.
	 */
	inline constexpr int32 MaxDepth = 512;
}

/** Type of the value a FVaRestJson handle points at. None = invalid handle (no value). */
UENUM(BlueprintType)
enum class EVaRestJsonType : uint8
{
	None,
	Null,
	Bool,
	Number,
	String,
	Array,
	Object
};

UENUM(BlueprintType)
enum class EVaRestJsonFormat : uint8
{
	Condensed,
	/** Two-space indent, one member or element per line */
	Pretty
};

USTRUCT(BlueprintType)
struct VARESTJSON_API FVaRestJsonError
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Json")
	FString Message;

	/** Byte offset into the UTF-8 input, or -1 */
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Json")
	int64 Offset = -1;

	/** 1-based; 0 when unknown (in-situ parses of a stolen buffer cannot recover lines) */
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Json")
	int32 Line = 0;

	/** 1-based, in Unicode code points from the start of the line; a leading BOM is not counted. 0 when unknown. */
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Json")
	int32 Column = 0;

	/** JSON path of the offending value (decode errors); empty for syntax errors */
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Json")
	FString Path;

	FString ToString() const;
};

struct FVaRestJsonParseOptions
{
	/** Nesting deeper than this fails the parse. Clamped to [1, VaRest::Json::MaxDepth]: anything that parses also writes. */
	int32 MaxDepth = VaRest::Json::MaxDepth;

	/** Accept comments and trailing commas (config files, not wire data) */
	bool bAllowCommentsAndTrailingCommas = false;

	/** Larger inputs are rejected up front, before anything is allocated. 0 = unlimited */
	int64 MaxInputBytes = 0;
};

/**
 * Handle to a value inside a shared JSON document.
 *
 * REFERENCE semantics: copying a handle aliases the same node, and mutating through any handle is
 * visible through all of them (like VaRest 1 objects and FJsonObjectWrapper). Set/Add COPY their
 * value into this document. Clone() makes an independent tree.
 *
 * A lookup that misses (Get, operator[], At) returns a handle that keeps the document but has no node:
 * IsValid() is false and GetType() is None. Writing through it (Set/Add) logs an error and does nothing,
 * so a missed lookup never silently writes into a detached temporary. A default-constructed handle
 * (no document) auto-vivifies on its first Set/Add. A missing value (a miss or a default handle) passed as a VALUE
 * is undefined (spec 2.2): Set removes the key, Add/SetAt write null. Two node-less handles compare Equal.
 *
 * Threading: any number of threads may read a document nobody mutates; mutation needs exclusive
 * access; handles are safe to copy across threads.
 */
USTRUCT(BlueprintType, meta = (DisplayName = "VaRest Json"))
struct VARESTJSON_API FVaRestJson
{
	GENERATED_BODY()

	using FDocumentRef = TSharedPtr<VaRest::Json::FDocument, ESPMode::ThreadSafe>;

	FVaRestJson() = default;
	FVaRestJson(const FVaRestJson&) = default;
	FVaRestJson& operator=(const FVaRestJson&) = default;

	/**
	 * Moving leaves the source an ordinary default handle. The implicit move moved Doc but copied Node, so a
	 * moved-from handle (MoveTemp(X).Set(...) ends in MoveTemp(*this)) still claimed IsValid() over a document it
	 * no longer kept alive.
	 */
	FVaRestJson(FVaRestJson&& Other)
		: Doc(MoveTemp(Other.Doc))
		, Node(Other.Node)
	{
		Other.Node = nullptr;
	}

	FVaRestJson& operator=(FVaRestJson&& Other)
	{
		if (this != &Other)
		{
			Doc = MoveTemp(Other.Doc);
			Node = Other.Node;
			Other.Node = nullptr;
		}
		return *this;
	}

	// ---- construction

	static FVaRestJson Object();
	static FVaRestJson Array();
	static FVaRestJson Null();

	/** Array of the given values: FVaRestJson::Array(TEXT("a"), 2, true) */
	template <typename... ItemTypes>
	static FVaRestJson Array(ItemTypes&&... Items)
	{
		FVaRestJson Result = Array();
		(Result.Add(Forward<ItemTypes>(Items)), ...);
		return Result;
	}

	/** A one-value document */
	template <typename ValueType>
	static FVaRestJson From(ValueType&& Value)
	{
		// Locals and parameters never reuse the member names (Doc, Node): UE builds with
		// shadow-variable warnings as errors.
		FDocumentRef NewDoc = NewDocument();
		VaRest::Json::FNode* Root = MakeNodeFor(*NewDoc, Forward<ValueType>(Value));
		// A value that could not be made (a copy past MaxDepth, logged) gives a default handle, not a "miss" handle
		// that holds a document and would refuse writes with "a lookup missed" errors
		return Root ? FVaRestJson(MoveTemp(NewDoc), Root) : FVaRestJson();
	}

	// ---- type

	EVaRestJsonType GetType() const;
	bool IsValid() const { return Node != nullptr; }
	bool IsNull() const { return GetType() == EVaRestJsonType::Null; }
	bool IsBool() const { return GetType() == EVaRestJsonType::Bool; }
	bool IsNumber() const { return GetType() == EVaRestJsonType::Number; }
	bool IsString() const { return GetType() == EVaRestJsonType::String; }
	bool IsArray() const { return GetType() == EVaRestJsonType::Array; }
	bool IsObject() const { return GetType() == EVaRestJsonType::Object; }

	/** Members of an object, elements of an array, 0 otherwise */
	int32 Num() const;

	// ---- scalar reads (no coercion between types; integer reads accept integral doubles in range)

	bool AsBool(bool Default = false) const { return TryGetBool().Get(Default); }
	int32 AsInt(int32 Default = 0) const { return TryGet<int32>().Get(Default); }
	int64 AsInt64(int64 Default = 0) const { return TryGetInt64().Get(Default); }
	uint64 AsUInt64(uint64 Default = 0) const { return TryGetUInt64().Get(Default); }
	float AsFloat(float Default = 0.f) const { return TryGet<float>().Get(Default); }
	double AsDouble(double Default = 0.0) const { return TryGetDouble().Get(Default); }
	FString AsString(const FString& Default = FString()) const;

	/** Zero-copy view of a string value; empty if not a string. Valid while any handle to the document lives. */
	FUtf8StringView AsUtf8View() const;

	template <typename T>
	TOptional<T> TryGet() const
	{
		using U = std::decay_t<T>;
		if constexpr (std::is_same_v<U, bool>)
		{
			return TryGetBool();
		}
		else if constexpr (std::is_same_v<U, char> || std::is_same_v<U, wchar_t> || std::is_same_v<U, char8_t> || std::is_same_v<U, char16_t> || std::is_same_v<U, char32_t>)
		{
			// Like MakeNodeFor: a character type would otherwise read a number as a code unit
			static_assert(sizeof(U) == 0, "FVaRestJson::TryGet: a single character is not a JSON value; read a string");
			return {};
		}
		else if constexpr (std::is_integral_v<U> && std::is_signed_v<U>)
		{
			const TOptional<int64> V = TryGetInt64();
			if (V && *V >= static_cast<int64>(TNumericLimits<U>::Min()) && *V <= static_cast<int64>(TNumericLimits<U>::Max()))
			{
				return static_cast<U>(*V);
			}
			return {};
		}
		else if constexpr (std::is_integral_v<U>)
		{
			const TOptional<uint64> V = TryGetUInt64();
			if (V && *V <= static_cast<uint64>(TNumericLimits<U>::Max()))
			{
				return static_cast<U>(*V);
			}
			return {};
		}
		else if constexpr (std::is_floating_point_v<U>)
		{
			const TOptional<double> V = TryGetDouble();
			if (!V)
			{
				return {};
			}
			if constexpr (std::is_same_v<U, float>)
			{
				// Not "> FLT_MAX": 3.4028235e38, the shortest text of FLT_MAX, parses to a double just above it
				// and still rounds to FLT_MAX. Only at the halfway point does the cast give infinity.
				if (FMath::IsFinite(*V) && FMath::Abs(*V) >= VaRest::Json::FloatRoundsToInfinity)
				{
					return {};
				}
			}
			return TOptional<U>(static_cast<U>(*V));
		}
		else if constexpr (std::is_same_v<U, FString>)
		{
			return TryGetString();
		}
		else if constexpr (std::is_same_v<U, FName>)
		{
			// NAME_SIZE characters or more abort FName construction in builds with checks (UnrealNames.cpp:3271-3281): such
			// text is not a name
			const TOptional<FString> S = TryGetString();
			return S && S->Len() < NAME_SIZE ? TOptional<FName>(FName(*S)) : TOptional<FName>();
		}
		else if constexpr (std::is_same_v<U, FText>)
		{
			const TOptional<FString> S = TryGetString();
			return S ? TOptional<FText>(FText::FromString(*S)) : TOptional<FText>();
		}
		else if constexpr (std::is_same_v<U, FVaRestJson>)
		{
			return IsValid() ? TOptional<FVaRestJson>(*this) : TOptional<FVaRestJson>();
		}
		else
		{
			static_assert(sizeof(U) == 0, "FVaRestJson::TryGet: unsupported type (structs arrive with FVaRestJsonConvert in M2)");
			return {};
		}
	}

	// ---- objects

	FVaRestJson Get(FStringView Key) const;
	FVaRestJson Get(FUtf8StringView Key) const;
	FVaRestJson operator[](FStringView Key) const { return Get(Key); }
	FVaRestJson operator[](FUtf8StringView Key) const { return Get(Key); }
	FVaRestJson operator[](const TCHAR* Key) const { return Get(FStringView(Key)); }
	FVaRestJson operator[](const UTF8CHAR* Key) const { return Get(FUtf8StringView(Key)); }

	bool Has(FStringView Key) const { return Get(Key).IsValid(); }
	bool Has(FUtf8StringView Key) const { return Get(Key).IsValid(); }

	/**
	 * Sets (copies) Value under Key. An invalid handle becomes a new object first.
	 * UTF-8 and ANSI keys and values are validated: an invalid sequence becomes U+FFFD with one warning per string;
	 * FString text maps unpaired surrogates to U+FFFD.
	 *
	 * A missing Value is undefined, not null (spec 2.2): the key is removed, the way obj.k = undefined disappears from
	 * JSON.stringify, so Body.Set("p", Resp["p"]) forwards p only when it exists. Explicit null is FVaRestJson::Null()
	 * or nullptr. On an object parsed with Key more than once, Set leaves one member with that key, at the first
	 * occurrence's position.
	 *
	 * Set/Add/SetAt are ref-qualified: on an lvalue they return *this by reference (chaining on a
	 * variable), on a temporary they return the handle by value, so
	 * `const FVaRestJson& B = FVaRestJson::Object().Set(...)` (or auto&&) owns its handle instead of
	 * dangling. A temporary still writes through: `Root[TEXT("user")].Set(...)` changes Root's document.
	 */
	template <typename ValueType>
	FVaRestJson& Set(FStringView Key, ValueType&& Value) &
	{
		if constexpr (std::is_same_v<std::decay_t<ValueType>, FVaRestJson>)
		{
			if (!Value.IsValid())
			{
				// Spec 2.2: a missing value is undefined, not null -- the key is removed. Checked before PrepareContainer,
				// which may replace *this, because Value may be *this (Fresh.Set(K, Fresh)).
				if (PrepareContainer(EVaRestJsonType::Object, TEXT("Set a field")))
				{
					Remove(Key);
				}
				return *this;
			}
		}
		if (VaRest::Json::FDocument* TargetDoc = PrepareContainer(EVaRestJsonType::Object, TEXT("Set a field")))
		{
			// nullptr: copying a value nested deeper than MaxDepth failed (logged); nothing is written
			if (VaRest::Json::FNode* NewValue = MakeNodeFor(*TargetDoc, Forward<ValueType>(Value)))
			{
				SetNode(Key, NewValue);
			}
		}
		return *this;
	}

	template <typename ValueType>
	FVaRestJson Set(FStringView Key, ValueType&& Value) &&
	{
		Set(Key, Forward<ValueType>(Value)); // *this is an lvalue here: the & overload
		return MoveTemp(*this);
	}

	/** UTF-8 key: no TCHAR -> UTF-8 conversion on hot paths */
	template <typename ValueType>
	FVaRestJson& Set(FUtf8StringView Key, ValueType&& Value) &
	{
		if constexpr (std::is_same_v<std::decay_t<ValueType>, FVaRestJson>)
		{
			if (!Value.IsValid())
			{
				// Spec 2.2: a missing value is undefined, not null -- the key is removed. Checked before PrepareContainer,
				// which may replace *this, because Value may be *this (Fresh.Set(K, Fresh)).
				if (PrepareContainer(EVaRestJsonType::Object, TEXT("Set a field")))
				{
					Remove(Key);
				}
				return *this;
			}
		}
		if (VaRest::Json::FDocument* TargetDoc = PrepareContainer(EVaRestJsonType::Object, TEXT("Set a field")))
		{
			if (VaRest::Json::FNode* NewValue = MakeNodeFor(*TargetDoc, Forward<ValueType>(Value)))
			{
				SetNodeUtf8(Key, NewValue);
			}
		}
		return *this;
	}

	template <typename ValueType>
	FVaRestJson Set(FUtf8StringView Key, ValueType&& Value) &&
	{
		Set(Key, Forward<ValueType>(Value));
		return MoveTemp(*this);
	}

	template <typename ValueType>
	FVaRestJson& Set(const TCHAR* Key, ValueType&& Value) & { return Set(FStringView(Key), Forward<ValueType>(Value)); }

	template <typename ValueType>
	FVaRestJson Set(const TCHAR* Key, ValueType&& Value) &&
	{
		Set(FStringView(Key), Forward<ValueType>(Value));
		return MoveTemp(*this);
	}

	template <typename ValueType>
	FVaRestJson& Set(const UTF8CHAR* Key, ValueType&& Value) & { return Set(FUtf8StringView(Key), Forward<ValueType>(Value)); }

	template <typename ValueType>
	FVaRestJson Set(const UTF8CHAR* Key, ValueType&& Value) &&
	{
		Set(FUtf8StringView(Key), Forward<ValueType>(Value));
		return MoveTemp(*this);
	}

	bool Remove(FStringView Key);
	bool Remove(FUtf8StringView Key);
	bool Remove(const TCHAR* Key) { return Remove(FStringView(Key)); }
	bool Remove(const UTF8CHAR* Key) { return Remove(FUtf8StringView(Key)); }
	TArray<FString> GetKeys() const;
	FVaRestJsonMemberRange Members() const;
	FVaRestJsonMember MemberAt(int32 Index) const;

	// ---- arrays

	FVaRestJson At(int32 Index) const;
	FVaRestJson operator[](int32 Index) const { return At(Index); }

	/** Appends (copies) Value. An invalid handle becomes a new array first. Ref-qualified like Set. */
	template <typename ValueType>
	FVaRestJson& Add(ValueType&& Value) &
	{
		if (VaRest::Json::FDocument* TargetDoc = PrepareContainer(EVaRestJsonType::Array, TEXT("Add an element")))
		{
			if (VaRest::Json::FNode* NewValue = MakeNodeFor(*TargetDoc, Forward<ValueType>(Value)))
			{
				AddNode(NewValue);
			}
		}
		return *this;
	}

	template <typename ValueType>
	FVaRestJson Add(ValueType&& Value) &&
	{
		Add(Forward<ValueType>(Value));
		return MoveTemp(*this);
	}

	template <typename ValueType>
	FVaRestJson& SetAt(int32 Index, ValueType&& Value) &
	{
		if (VaRest::Json::FDocument* TargetDoc = PrepareSetAt(Index))
		{
			if (VaRest::Json::FNode* NewValue = MakeNodeFor(*TargetDoc, Forward<ValueType>(Value)))
			{
				SetAtNode(Index, NewValue);
			}
		}
		return *this;
	}

	template <typename ValueType>
	FVaRestJson SetAt(int32 Index, ValueType&& Value) &&
	{
		SetAt(Index, Forward<ValueType>(Value));
		return MoveTemp(*this);
	}

	bool RemoveAt(int32 Index);
	FVaRestJsonElementRange Elements() const;

	// ---- whole value

	/** Past VaRest::Json::MaxDepth: an invalid handle (nullptr / false) and an error log, never a truncated copy */
	FVaRestJson Clone() const;
	/** Past VaRest::Json::MaxDepth: an invalid handle (nullptr / false) and an error log, never a truncated copy */
	bool Equals(const FVaRestJson& Other) const;
	bool operator==(const FVaRestJson& Other) const { return Equals(Other); }
	/** Both handles point at the same node. Two node-less handles (misses, default handles) are not the same node. */
	bool IsSameNode(const FVaRestJson& Other) const { return Node && Node == Other.Node; }

	/** "a.b[2].c", or an RFC 6901 pointer when it starts with '/'. Invalid syntax or a miss -> invalid handle. (Task 7) */
	FVaRestJson Path(FStringView Expression) const;

	// ---- text (Tasks 5-6)

	/**
	 * Numbers: integer literals that fit int64/uint64 stay exact integers; others are doubles. "-0" is the
	 * integer 0 (RapidJSON emits it through Int, reader.h:1491-1493, :1739-1740), so it writes back as "0",
	 * as JSON.stringify(-0) does; "-0.0" stays the double -0.0.
	 */
	static TValueOrError<FVaRestJson, FVaRestJsonError> Parse(FUtf8StringView Text, const FVaRestJsonParseOptions& Options = FVaRestJsonParseOptions());
	static TValueOrError<FVaRestJson, FVaRestJsonError> Parse(FStringView Text, const FVaRestJsonParseOptions& Options = FVaRestJsonParseOptions());
	static TValueOrError<FVaRestJson, FVaRestJsonError> Parse(TConstArrayView<uint8> Utf8Bytes, const FVaRestJsonParseOptions& Options = FVaRestJsonParseOptions());
	/**
	 * Steals the buffer (an HTTP response body); parsed strings point into it.
	 * Zero-copy when Utf8Bytes has at least one byte of slack (Max() > Num()), because the parser needs a
	 * NUL terminator; otherwise one reallocation.
	 */
	static TValueOrError<FVaRestJson, FVaRestJsonError> ParseOwned(TArray<uint8>&& Utf8Bytes, const FVaRestJsonParseOptions& Options = FVaRestJsonParseOptions());

	/**
	 * "" for an invalid handle, "null" for a null value. A value nested deeper than VaRest::Json::MaxDepth (possible
	 * only when built through Set/Add) gives "" and an error log (spec 2.2); TryToString reports it instead.
	 */
	FString ToString(EVaRestJsonFormat Format = EVaRestJsonFormat::Condensed) const;
	/** Appends UTF-8 to Out; nothing for an invalid handle; nothing plus an error log past MaxDepth */
	void ToUtf8(TArray<uint8>& Out, EVaRestJsonFormat Format = EVaRestJsonFormat::Condensed) const;
	/** ToString with the depth failure as an error value instead of a log line ("" for an invalid handle) */
	TValueOrError<FString, FVaRestJsonError> TryToString(EVaRestJsonFormat Format = EVaRestJsonFormat::Condensed) const;
	/** ToUtf8 with the depth failure as an error value; Out keeps its previous contents on failure */
	TValueOrError<void, FVaRestJsonError> TryToUtf8(TArray<uint8>& Out, EVaRestJsonFormat Format = EVaRestJsonFormat::Condensed) const;

	// ---- engine interop (Task 8)

	/** Past VaRest::Json::MaxDepth: an invalid handle (nullptr / false) and an error log, never a truncated copy */
	TSharedPtr<FJsonValue> ToJsonValue() const;
	/**
	 * Null when this is not an object. FJsonObject keys compare case-insensitively (its map is keyed by
	 * UE::FSharedString, JsonObject.h:99 and :237, whose equality and hash ignore case, SharedString.h:100-107
	 * and :132-135 -> StringView.h:509-513), so members whose keys differ only by case ("id" and "ID")
	 * collapse into one: the last one wins.
	 */
	TSharedPtr<FJsonObject> ToJsonObject() const;
	/** Past VaRest::Json::MaxDepth: an invalid handle (nullptr / false) and an error log, never a truncated copy */
	static FVaRestJson FromJsonValue(const TSharedPtr<FJsonValue>& Value);
	static FVaRestJson FromJsonObject(const TSharedPtr<FJsonObject>& InObject);

	// ---- struct ops (TStructOpsTypeTraits below): pin defaults, copy/paste, SaveGame, CDO diffs

	bool ExportTextItem(FString& ValueStr, const FVaRestJson& DefaultValue, UObject* Parent, int32 PortFlags, UObject* ExportRootScope) const;
	bool ImportTextItem(const TCHAR*& Buffer, int32 PortFlags, UObject* Parent, FOutputDevice* ErrorText);
	bool Identical(const FVaRestJson* Other, uint32 PortFlags) const;
	bool Serialize(FArchive& Ar);

	// ---- for sibling VaRest modules and tests; not a stable API

	const FDocumentRef& GetDocumentInternal() const { return Doc; }
	VaRest::Json::FNode* GetNodeInternal() const { return Node; }
	static FVaRestJson MakeInternal(FDocumentRef InDoc, VaRest::Json::FNode* InNode) { return FVaRestJson(MoveTemp(InDoc), InNode); }

private:
	FVaRestJson(FDocumentRef InDoc, VaRest::Json::FNode* InNode)
		: Doc(MoveTemp(InDoc))
		, Node(InNode)
	{
	}

	/** First arena chunk of an API-built document: FArena::MinChunkSize (static_assert in VaRestJson.cpp), so a small document pays one 256 B chunk */
	static constexpr SIZE_T DefaultArenaChunk = 256;
	static FDocumentRef NewDocument(SIZE_T ArenaFirstChunk = DefaultArenaChunk);

	VaRest::Json::FDocument* PrepareContainer(EVaRestJsonType Kind, const TCHAR* Operation);
	void SetNode(FStringView Key, VaRest::Json::FNode* Value);
	void SetNodeUtf8(FUtf8StringView Key, VaRest::Json::FNode* Value);
	void AddNode(VaRest::Json::FNode* Value);
	VaRest::Json::FDocument* PrepareSetAt(int32 Index);
	FVaRestJson Miss() const { return FVaRestJson(Doc, nullptr); }
	void SetAtNode(int32 Index, VaRest::Json::FNode* Value);

	TOptional<bool> TryGetBool() const;
	TOptional<int64> TryGetInt64() const;
	TOptional<uint64> TryGetUInt64() const;
	TOptional<double> TryGetDouble() const;
	TOptional<FString> TryGetString() const;

	static VaRest::Json::FNode* MakeNull(VaRest::Json::FDocument& InDoc);
	static VaRest::Json::FNode* MakeBool(VaRest::Json::FDocument& InDoc, bool bValue);
	static VaRest::Json::FNode* MakeInt(VaRest::Json::FDocument& InDoc, int64 Value);
	static VaRest::Json::FNode* MakeUInt(VaRest::Json::FDocument& InDoc, uint64 Value);
	static VaRest::Json::FNode* MakeDouble(VaRest::Json::FDocument& InDoc, double Value);
	static VaRest::Json::FNode* MakeString(VaRest::Json::FDocument& InDoc, FStringView Value);
	static VaRest::Json::FNode* MakeUtf8String(VaRest::Json::FDocument& InDoc, FUtf8StringView Value);
	static VaRest::Json::FNode* MakeCopy(VaRest::Json::FDocument& InDoc, const FVaRestJson& Value);

	template <typename ValueType>
	static VaRest::Json::FNode* MakeNodeFor(VaRest::Json::FDocument& InDoc, ValueType&& Value)
	{
		using U = std::decay_t<ValueType>;
		// Routed explicitly: plain overloads would make int -> int64 vs int -> double ambiguous.
		if constexpr (std::is_same_v<U, std::nullptr_t>)
		{
			return MakeNull(InDoc);
		}
		else if constexpr (std::is_same_v<U, bool>)
		{
			return MakeBool(InDoc, Value);
		}
		else if constexpr (std::is_same_v<U, char> || std::is_same_v<U, wchar_t> || std::is_same_v<U, char8_t> || std::is_same_v<U, char16_t> || std::is_same_v<U, char32_t>)
		{
			static_assert(sizeof(U) == 0, "FVaRestJson: a single character is not a JSON value; pass a string");
			return nullptr;
		}
		else if constexpr (std::is_integral_v<U> && std::is_signed_v<U>)
		{
			return MakeInt(InDoc, static_cast<int64>(Value));
		}
		else if constexpr (std::is_integral_v<U>)
		{
			return MakeUInt(InDoc, static_cast<uint64>(Value));
		}
		else if constexpr (std::is_floating_point_v<U>)
		{
			return MakeDouble(InDoc, static_cast<double>(Value));
		}
		else if constexpr (std::is_same_v<U, FVaRestJson>)
		{
			return MakeCopy(InDoc, Value);
		}
		else if constexpr (std::is_same_v<U, FName> || std::is_same_v<U, FText>)
		{
			return MakeString(InDoc, FStringView(Value.ToString()));
		}
		else if constexpr (std::is_convertible_v<ValueType, FUtf8StringView>)
		{
			return MakeUtf8String(InDoc, FUtf8StringView(Value));
		}
		else if constexpr (std::is_convertible_v<ValueType, FStringView>)
		{
			return MakeString(InDoc, FStringView(Value));
		}
		else
		{
			static_assert(sizeof(U) == 0, "FVaRestJson: unsupported value type (structs arrive with FVaRestJsonConvert in M2)");
			return nullptr;
		}
	}

	FDocumentRef Doc;
	VaRest::Json::FNode* Node = nullptr;
};

// Document pointer + reference controller + node pointer. Handles are copied freely (members, ranges, BP
// pins), so growing this is an API-visible cost. Not wrapped in #if PLATFORM_64BITS: UE 5.8 deprecates that
// macro with a warning (HAL/Platform.h:737) because it only supports 64-bit targets.
static_assert(sizeof(FVaRestJson) == 24, "FVaRestJson is a 24-byte handle");

template <>
struct TStructOpsTypeTraits<FVaRestJson> : public TStructOpsTypeTraitsBase2<FVaRestJson>
{
	enum
	{
		WithExportTextItem = true,
		WithImportTextItem = true,
		WithIdentical = true,
		WithSerializer = true,
	};
};

struct FVaRestJsonMember
{
	FUtf8StringView Key;
	FVaRestJson Value;

	FString GetKey() const { return FString::ConstructFromPtrSize(Key.GetData(), Key.Len()); }
};

/** Range over an object's members, in insertion order. Indexes are re-read each step, so removing members while iterating is safe. */
class FVaRestJsonMemberRange
{
public:
	struct FIterator
	{
		const FVaRestJson* Owner;
		int32 Index;

		FVaRestJsonMember operator*() const { return Owner->MemberAt(Index); }
		FIterator& operator++()
		{
			++Index;
			return *this;
		}
		bool operator!=(const FIterator& Other) const { return Index < Other.Index && Index < Owner->Num(); }
	};

	explicit FVaRestJsonMemberRange(const FVaRestJson& InOwner)
		: Owner(InOwner)
	{
	}

	FIterator begin() const { return FIterator{ &Owner, 0 }; }
	FIterator end() const { return FIterator{ &Owner, Owner.IsObject() ? Owner.Num() : 0 }; }

private:
	FVaRestJson Owner; // a copy keeps the document alive during the loop
};

class FVaRestJsonElementRange
{
public:
	struct FIterator
	{
		const FVaRestJson* Owner;
		int32 Index;

		FVaRestJson operator*() const { return Owner->At(Index); }
		FIterator& operator++()
		{
			++Index;
			return *this;
		}
		bool operator!=(const FIterator& Other) const { return Index < Other.Index && Index < Owner->Num(); }
	};

	explicit FVaRestJsonElementRange(const FVaRestJson& InOwner)
		: Owner(InOwner)
	{
	}

	FIterator begin() const { return FIterator{ &Owner, 0 }; }
	FIterator end() const { return FIterator{ &Owner, Owner.IsArray() ? Owner.Num() : 0 }; }

private:
	FVaRestJson Owner;
};
