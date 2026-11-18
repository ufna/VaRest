// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"
#include "VaRestDynamicLifecycleTestTypes.generated.h"

namespace VaRest::Http::DynamicLifecycleTest
{
enum class EOperation : uint8 { DefaultConstruct, CopyConstruct, Assign, Destroy, Equal, Hash, Count };
struct FObservation
{
	int64 Operations[static_cast<uint8>(EOperation::Count)]{};
	int64 WrongThread = 0;
};
void BeginObservationOnGT();
FObservation ObserveOnGT();
void EndObservationOnGT();
}

/** Real native operations have no reflection callout, editor pump or captured callback. */
USTRUCT()
struct FVaRestDynamicLifecycleLeaf
{
	GENERATED_BODY()
	FVaRestDynamicLifecycleLeaf();
	FVaRestDynamicLifecycleLeaf(const FVaRestDynamicLifecycleLeaf& Other);
	FVaRestDynamicLifecycleLeaf& operator=(const FVaRestDynamicLifecycleLeaf& Other);
	~FVaRestDynamicLifecycleLeaf();
	bool operator==(const FVaRestDynamicLifecycleLeaf& Other) const;
	friend uint32 GetTypeHash(const FVaRestDynamicLifecycleLeaf& Value);
	UPROPERTY() FString Text;
	UPROPERTY() int32 Number = 7;
};

template<> struct TStructOpsTypeTraits<FVaRestDynamicLifecycleLeaf> : TStructOpsTypeTraitsBase2<FVaRestDynamicLifecycleLeaf>
{
	enum { WithCopy = true, WithIdenticalViaEquality = true };
};

/** The leaf exists ONLY under Optional: omitting its closure traversal cannot be masked by another field. */
USTRUCT()
struct FVaRestDynamicLifecycleRoot
{
	GENERATED_BODY()
	FVaRestDynamicLifecycleRoot();
	FVaRestDynamicLifecycleRoot(const FVaRestDynamicLifecycleRoot& Other);
	FVaRestDynamicLifecycleRoot& operator=(const FVaRestDynamicLifecycleRoot& Other);
	~FVaRestDynamicLifecycleRoot();
	bool operator==(const FVaRestDynamicLifecycleRoot& Other) const;
	friend uint32 GetTypeHash(const FVaRestDynamicLifecycleRoot& Value);
	UPROPERTY() FString Name;
	UPROPERTY() TOptional<FVaRestDynamicLifecycleLeaf> Maybe;
};

template<> struct TStructOpsTypeTraits<FVaRestDynamicLifecycleRoot> : TStructOpsTypeTraitsBase2<FVaRestDynamicLifecycleRoot>
{
	enum { WithCopy = true, WithIdenticalViaEquality = true };
};

/** Genuine generated destination properties consumed by the same native Take path as the bridge. */
USTRUCT()
struct FVaRestDynamicLifecycleDestinations
{
	GENERATED_BODY()
	UPROPERTY() FVaRestDynamicLifecycleRoot Scalar;
	UPROPERTY() TArray<FVaRestDynamicLifecycleRoot> Array;
	UPROPERTY() TMap<FString, FVaRestDynamicLifecycleRoot> Map;
};
