// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Containers/ArrayView.h"
#include "CoreMinimal.h"
#include "UObject/ObjectKey.h"

#include "VaRestJsonRules.generated.h"

class UScriptStruct;
class UEnum;

namespace VaRest::Json
{
/** Wire text is exact, including case and embedded nulls; FString's ordinary equality is case-insensitive. */
inline bool EqualWireText(const FString& A, const FString& B)
{
	return A.Len() == B.Len() && (A.IsEmpty() || FMemory::Memcmp(*A, *B, static_cast<SIZE_T>(A.Len()) * sizeof(TCHAR)) == 0);
}
} // namespace VaRest::Json

UENUM(BlueprintType)
enum class EVaRestJsonPresence : uint8
{
	Always,
	OmitWhenDefault,
	NullWhenDefault,
	DecodeOnly,
	Ignore
};

/** A field's stable Blueprint GUID, rather than its mutable display/property name. */
USTRUCT(BlueprintType)
struct VARESTJSON_API FVaRestJsonFieldRule
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|JSON")
	FGuid Field;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|JSON")
	FString Json;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|JSON")
	EVaRestJsonPresence Presence = EVaRestJsonPresence::Always;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|JSON")
	bool bNullable = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|JSON")
	bool bBase64 = false;

	bool operator==(const FVaRestJsonFieldRule& Other) const
	{
		return Field == Other.Field && VaRest::Json::EqualWireText(Json, Other.Json) && Presence == Other.Presence && bNullable == Other.bNullable && bBase64 == Other.bBase64;
	}
};

USTRUCT(BlueprintType)
struct VARESTJSON_API FVaRestJsonEnumRule
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|JSON")
	FName Enumerator;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|JSON")
	FString Json;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|JSON")
	bool bUnset = false;

	bool operator==(const FVaRestJsonEnumRule& Other) const
	{
		return Enumerator == Other.Enumerator && VaRest::Json::EqualWireText(Json, Other.Json) && bUnset == Other.bUnset;
	}
};

/** One atomic owner batch. Type must be a UScriptStruct (Fields) or UEnum (Enums). */
struct FVaRestJsonTypeRuleRegistration
{
	const UObject* Type = nullptr;
	TArray<FVaRestJsonFieldRule> Fields;
	TArray<FVaRestJsonEnumRule> Enums;
};

/** Plain copied data; an immutable snapshot never owns or dereferences an owner UObject. */
struct FVaRestJsonResolvedRules
{
	TArray<FVaRestJsonFieldRule> Fields;
	TArray<FVaRestJsonEnumRule> Enums;
	/** Nonempty means this type has conflicting declarations and must refuse conversion. */
	FString Error;
};

class VARESTJSON_API FVaRestJsonRuleSnapshot final
{
public:
	uint64 GetId() const { return Id; }
	const FVaRestJsonResolvedRules* Find(const UObject* Type) const;
	/** Warns once per unregistered type under an explicitly registered content folder; non-Shipping only. */
	void WarnIfMissing(const UObject* Type) const;
	/** Dynamic activation refuses missing imported rules under declared folders in every build. GT type access only. */
	bool ValidateRequiredTypeOnGT(const UObject* Type, FString& Error) const;

private:
	friend struct FVaRestJsonRules;
	FVaRestJsonRuleSnapshot(uint64 InId, TMap<FObjectKey, FVaRestJsonResolvedRules>&& InTypes, TArray<FString>&& InFolders);
	const uint64 Id;
	const TMap<FObjectKey, FVaRestJsonResolvedRules> Types;
	const TArray<FString> Folders;
};

/**
 * GT registration; any-thread immutable reads. Each update publishes one generation and one notification pair.
 * Equal declarations from independent owners coexist. A conflict refuses that type; explicit owner snapshots
 * override only their own declarations and retain the captured default resolution for every other type.
 * Callers still own the reflection-lifetime contract of FVaRestJsonConvert.
 */
struct VARESTJSON_API FVaRestJsonRules
{
	DECLARE_MULTICAST_DELEGATE(FRulesChanged);

	static bool ReplaceOwnerRules(const UObject* Owner, TConstArrayView<FVaRestJsonTypeRuleRegistration> Registrations,
		TConstArrayView<FString> TypesFolders = {}, FString* OutError = nullptr);
	static bool RegisterFields(const UObject* Owner, const UScriptStruct* Struct, TConstArrayView<FVaRestJsonFieldRule> Rules, FString* OutError = nullptr);
	static bool RegisterEnum(const UObject* Owner, const UEnum* Enum, TConstArrayView<FVaRestJsonEnumRule> Rules, FString* OutError = nullptr);
	static bool RegisterTypesFolder(const UObject* Owner, const FString& ContentFolder, FString* OutError = nullptr);
	static void UnregisterOwner(const UObject* Owner);
	static TSharedRef<const FVaRestJsonRuleSnapshot, ESPMode::ThreadSafe> Snapshot(const UObject* Owner);
	static TSharedRef<const FVaRestJsonRuleSnapshot, ESPMode::ThreadSafe> DefaultSnapshot();
	static uint64 GetGeneration(const UObject* Owner);
	static uint64 GetDefaultGeneration();
	/** GT only: true throughout both publication notifications, before the registration call returns. */
	static bool IsChangingOnGameThread();
	static FRulesChanged& OnRulesChanging();
	static FRulesChanged& OnRulesChanged();
};
