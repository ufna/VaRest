// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"
#include "VaRestJson.h"

#include "VaRestJsonConvertTestTypes.generated.h"

// Test-only types. Like VaRestJsonTestTypes.h they ship in the module's reflection data until the VaRest-rfd
// M3 item moves test types out of the runtime module.

USTRUCT()
struct FVaRestJsonTestInner
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Id = 0;

	UPROPERTY()
	FString Name;
};

USTRUCT()
struct FVaRestJsonTestNaming
{
	GENERATED_BODY()

	UPROPERTY()
	int32 UserId = 0;

	UPROPERTY()
	bool bIsActive = false;

	UPROPERTY()
	FString URLPath;

	UPROPERTY()
	int32 HTTPStatus2 = 0;

	UPROPERTY()
	FString user_name;

	UPROPERTY(Transient)
	int32 TransientValue = 0;
};

/** Both fields become "userId" in camelCase: the second is skipped */
USTRUCT()
struct FVaRestJsonTestCollision
{
	GENERATED_BODY()

	UPROPERTY()
	int32 user_id = 1;

	UPROPERTY()
	int32 UserId = 2;
};

/** Fields encode/decode do not convert: each is skipped with one warning per field-map build */
USTRUCT()
struct FVaRestJsonTestSkipped
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Kept = 0;

	UPROPERTY()
	TObjectPtr<UObject> ObjectRef;

	UPROPERTY()
	TMap<FGuid, int32> GuidKeys;

	UPROPERTY()
	TArray<TObjectPtr<UObject>> Objects;
};

/** Decode accepts a bool's AsIs spelling ("bOn") only for a field whose 'b' the naming policy dropped */
USTRUCT()
struct FVaRestJsonTestBoolPrefix
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOn = false;

	UPROPERTY()
	bool Old = false;
};

/**
 * A C++ field named like an engine FName in another casing: "ID" is hardcoded (UnrealNames.inl:206), so the FName id
 * reads ID wherever names do not preserve case -- a packaged game (NameTypes.h:33, :1286-1293)
 */
USTRUCT()
struct FVaRestJsonTestEngineName
{
	GENERATED_BODY()

	UPROPERTY()
	int32 id = 0;
};

/**
 * A C++ field named like a Blueprint pin category: not an engine hardcoded name, but the editor's BlueprintGraph module
 * registers "softobject" first (UEdGraphSchema_K2::PC_SoftObject, EdGraphSchema_K2.cpp:616), a packaged game lacks it
 */
USTRUCT()
struct FVaRestJsonTestEditorName
{
	GENERATED_BODY()

	UPROPERTY()
	TSoftObjectPtr<UObject> SoftObject;
};

UENUM()
enum class EVaRestJsonTestColor : uint8
{
	Red,
	Green,
	Blue
};

UENUM()
namespace EVaRestJsonTestLegacy
{
	enum Type : int
	{
		First,
		Second
	};
}

/**
 * Unsigned map keys and fields: a negative JSON integer fits none of them. FNumericProperty::CanHoldValue(int64) says a
 * uint64 holds -1 ((int64)(uint64)-1 == -1, UnrealType.h:2172-2175), so decode must not ask it.
 */
USTRUCT()
struct FVaRestJsonTestUnsigned
{
	GENERATED_BODY()

	UPROPERTY()
	TMap<uint64, int32> ByBig;

	UPROPERTY()
	TMap<uint8, int32> BySmall;

	UPROPERTY()
	uint64 Big = 7;

	UPROPERTY()
	uint32 Mid = 7;
};

/** Every kind of field the converter supports */
USTRUCT()
struct FVaRestJsonTestAll
{
	GENERATED_BODY()

	FVaRestJsonTestAll()
	{
		for (int32& Element : FixedArray)
		{
			Element = 0;
		}
	}

	UPROPERTY()
	bool bIsActive = false;

	UPROPERTY()
	int8 Int8Value = 0;

	UPROPERTY()
	int16 Int16Value = 0;

	UPROPERTY()
	int32 Int32Value = 0;

	UPROPERTY()
	int64 Int64Value = 0;

	UPROPERTY()
	uint8 ByteValue = 0;

	UPROPERTY()
	uint16 Uint16Value = 0;

	UPROPERTY()
	uint32 Uint32Value = 0;

	UPROPERTY()
	uint64 Uint64Value = 0;

	UPROPERTY()
	float FloatValue = 0.f;

	UPROPERTY()
	double DoubleValue = 0.0;

	UPROPERTY()
	FString StringValue;

	UPROPERTY()
	FName NameValue;

	UPROPERTY()
	FText TextValue;

	UPROPERTY()
	EVaRestJsonTestColor Color = EVaRestJsonTestColor::Red;

	UPROPERTY()
	TEnumAsByte<EVaRestJsonTestLegacy::Type> Legacy = EVaRestJsonTestLegacy::First;

	UPROPERTY()
	FVaRestJsonTestInner Inner;

	UPROPERTY()
	TArray<int32> Numbers;

	UPROPERTY()
	TArray<FVaRestJsonTestInner> Items;

	UPROPERTY()
	TSet<FString> Tags;

	UPROPERTY()
	TMap<FString, int32> Scores;

	UPROPERTY()
	TMap<int32, FString> ById;

	UPROPERTY()
	TMap<FName, int32> ByName;

	UPROPERTY()
	TMap<EVaRestJsonTestColor, int32> ByColor;

	UPROPERTY()
	FVaRestJson Extra;

	UPROPERTY()
	FDateTime When;

	UPROPERTY()
	FTimespan Duration;

	UPROPERTY()
	FGuid Guid;

	UPROPERTY()
	FSoftObjectPath AssetPath;

	/**
	 * Not "SoftObject": the editor registers that FName as "softobject" first (EdGraphSchema_K2.cpp:616), so every map
	 * build of this struct would warn; FVaRestJsonTestEditorName covers that case
	 */
	UPROPERTY()
	TSoftObjectPtr<UObject> SoftAsset;

	UPROPERTY()
	TArray<uint8> Bytes;

	UPROPERTY()
	TOptional<int32> MaybeNumber;

	UPROPERTY()
	int32 FixedArray[3];
};

/**
 * Depth through a struct: its own object and Children array are two levels, the FVaRestJson elements add theirs. A
 * struct cannot nest itself, so reflected fields alone never reach MaxDepth: UHT refuses a struct inside an array of
 * itself (UhtArrayProperty.cs:216-222) and a cycle through another struct (its code generation hash is zero,
 * UhtProperty.cs:3066-3071). FVaRestJson values and the writer entries' OpenContainers are what can.
 */
USTRUCT()
struct FVaRestJsonTestTree
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FVaRestJson> Children;
};

/** Bytes in a struct, so they can sit inside containers (UHT rejects nested containers) */
USTRUCT()
struct FVaRestJsonTestBlob
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<uint8> Data;
};

/** Every place a TArray<uint8> can meet the BytesWriter hook: a field, array elements, map values */
USTRUCT()
struct FVaRestJsonTestAttachments
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<uint8> First;

	UPROPERTY()
	TArray<FVaRestJsonTestBlob> List;

	UPROPERTY()
	TMap<FString, FVaRestJsonTestBlob> ByName;
};

/**
 * Socket.IO positional arguments (§18.2 J-2): field i is argument i, so the fields iterated must be the ones the
 * converter writes. Between the two converted fields sit one of each kind it leaves out: transient, deprecated,
 * editor-only (present in this editor build, absent from a cooked game) and unsupported (skipped with a warning).
 */
USTRUCT()
struct FVaRestJsonTestPositional
{
	GENERATED_BODY()

	UPROPERTY()
	int32 PosFirst = 0;

	UPROPERTY(Transient)
	int32 PosTransient = 0;

	UPROPERTY()
	int32 PosOld_DEPRECATED = 0;

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	int32 PosEditorOnly = 0;
#endif

	UPROPERTY()
	TObjectPtr<UObject> PosObject;

	UPROPERTY()
	FString PosLast;
};

/** The shape of Tools/gen_bench_json.py. Bools are Ok/Active, not bOk/bActive: FJsonObjectConverter matches
 *  GetAuthoredName() and keeps a 'b' prefix, so with these names both converters decode every field. */
USTRUCT()
struct FVaRestJsonBenchOwner
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Id = 0;

	UPROPERTY()
	FString Nick;
};

USTRUCT()
struct FVaRestJsonBenchItem
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Id = 0;

	UPROPERTY()
	int64 Uid = 0;

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString Desc;

	UPROPERTY()
	double Price = 0.0;

	UPROPERTY()
	double Weight = 0.0;

	UPROPERTY()
	bool Active = false;

	UPROPERTY()
	FVaRestJsonBenchOwner Owner;

	UPROPERTY()
	TArray<FString> Tags;

	UPROPERTY()
	TMap<FString, int32> Stats;
};

USTRUCT()
struct FVaRestJsonBenchData
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FVaRestJsonBenchItem> Items;

	UPROPERTY()
	int32 Count = 0;
};

USTRUCT()
struct FVaRestJsonBenchResponse
{
	GENERATED_BODY()

	UPROPERTY()
	bool Ok = false;

	UPROPERTY()
	FVaRestJsonBenchData Data;
};
