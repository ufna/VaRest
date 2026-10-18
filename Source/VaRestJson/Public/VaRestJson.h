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

	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Json")
	int32 Column = 0;

	/** JSON path of the offending value (decode errors); empty for syntax errors */
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Json")
	FString Path;

	FString ToString() const;
};

struct FVaRestJsonParseOptions
{
	/** Nesting deeper than this fails the parse (hostile-input guard) */
	int32 MaxDepth = 512;

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
 * (no document) auto-vivifies on its first Set/Add. Two node-less handles compare Equal.
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
		return FVaRestJson(MoveTemp(NewDoc), Root);
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
				if (FMath::IsFinite(*V) && FMath::Abs(*V) > static_cast<double>(TNumericLimits<float>::Max()))
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
			const TOptional<FString> S = TryGetString();
			return S ? TOptional<FName>(FName(*S)) : TOptional<FName>();
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

	/** Sets (copies) Value under Key. An invalid handle becomes a new object first. */
	template <typename ValueType>
	FVaRestJson& Set(FStringView Key, ValueType&& Value)
	{
		if (VaRest::Json::FDocument* TargetDoc = PrepareContainer(EVaRestJsonType::Object, TEXT("Set a field")))
		{
			SetNode(Key, MakeNodeFor(*TargetDoc, Forward<ValueType>(Value)));
		}
		return *this;
	}

	/** UTF-8 key: no TCHAR -> UTF-8 conversion on hot paths */
	template <typename ValueType>
	FVaRestJson& Set(FUtf8StringView Key, ValueType&& Value)
	{
		if (VaRest::Json::FDocument* TargetDoc = PrepareContainer(EVaRestJsonType::Object, TEXT("Set a field")))
		{
			SetNodeUtf8(Key, MakeNodeFor(*TargetDoc, Forward<ValueType>(Value)));
		}
		return *this;
	}

	template <typename ValueType>
	FVaRestJson& Set(const TCHAR* Key, ValueType&& Value) { return Set(FStringView(Key), Forward<ValueType>(Value)); }

	template <typename ValueType>
	FVaRestJson& Set(const UTF8CHAR* Key, ValueType&& Value) { return Set(FUtf8StringView(Key), Forward<ValueType>(Value)); }

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

	/** Appends (copies) Value. An invalid handle becomes a new array first. */
	template <typename ValueType>
	FVaRestJson& Add(ValueType&& Value)
	{
		if (VaRest::Json::FDocument* TargetDoc = PrepareContainer(EVaRestJsonType::Array, TEXT("Add an element")))
		{
			AddNode(MakeNodeFor(*TargetDoc, Forward<ValueType>(Value)));
		}
		return *this;
	}

	template <typename ValueType>
	FVaRestJson& SetAt(int32 Index, ValueType&& Value)
	{
		if (VaRest::Json::FDocument* TargetDoc = PrepareSetAt(Index))
		{
			SetAtNode(Index, MakeNodeFor(*TargetDoc, Forward<ValueType>(Value)));
		}
		return *this;
	}

	bool RemoveAt(int32 Index);
	FVaRestJsonElementRange Elements() const;

	// ---- whole value

	FVaRestJson Clone() const;
	bool Equals(const FVaRestJson& Other) const;
	bool operator==(const FVaRestJson& Other) const { return Equals(Other); }
	bool IsSameNode(const FVaRestJson& Other) const { return Node == Other.Node; }

	/** "a.b[2].c", or an RFC 6901 pointer when it starts with '/'. Invalid syntax or a miss -> invalid handle. (Task 7) */
	FVaRestJson Path(FStringView Expression) const;

	// ---- text (Tasks 5-6)

	static TValueOrError<FVaRestJson, FVaRestJsonError> Parse(FUtf8StringView Text, const FVaRestJsonParseOptions& Options = FVaRestJsonParseOptions());
	static TValueOrError<FVaRestJson, FVaRestJsonError> Parse(FStringView Text, const FVaRestJsonParseOptions& Options = FVaRestJsonParseOptions());
	static TValueOrError<FVaRestJson, FVaRestJsonError> Parse(TConstArrayView<uint8> Utf8Bytes, const FVaRestJsonParseOptions& Options = FVaRestJsonParseOptions());
	/**
	 * Steals the buffer (an HTTP response body); parsed strings point into it.
	 * Zero-copy when Utf8Bytes has at least one byte of slack (Max() > Num()), because the parser needs a
	 * NUL terminator; otherwise one reallocation.
	 */
	static TValueOrError<FVaRestJson, FVaRestJsonError> ParseOwned(TArray<uint8>&& Utf8Bytes, const FVaRestJsonParseOptions& Options = FVaRestJsonParseOptions());

	/** "" for an invalid handle, "null" for a null value */
	FString ToString(EVaRestJsonFormat Format = EVaRestJsonFormat::Condensed) const;
	/** Appends UTF-8 to Out; nothing for an invalid handle */
	void ToUtf8(TArray<uint8>& Out, EVaRestJsonFormat Format = EVaRestJsonFormat::Condensed) const;

	// ---- engine interop (Task 8)

	TSharedPtr<FJsonValue> ToJsonValue() const;
	/** Null when this is not an object */
	TSharedPtr<FJsonObject> ToJsonObject() const;
	static FVaRestJson FromJsonValue(const TSharedPtr<FJsonValue>& Value);
	static FVaRestJson FromJsonObject(const TSharedPtr<FJsonObject>& InObject);

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

	static FDocumentRef NewDocument(SIZE_T ArenaFirstChunk = 4096);

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
