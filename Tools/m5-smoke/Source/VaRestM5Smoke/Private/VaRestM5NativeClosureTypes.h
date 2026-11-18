// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Misc/Optional.h"
#include "VaRestM5NativeClosureTypes.generated.h"

UENUM()
enum class EVaRestM5ClosureMode : uint8 { Zero, One };

/** This nonintrinsic leaf occurs ONLY under Optional; no other declaration masks its qualification. */
USTRUCT()
struct FVaRestM5ClosureLeaf
{
    GENERATED_BODY()
    FVaRestM5ClosureLeaf();
    FVaRestM5ClosureLeaf(const FVaRestM5ClosureLeaf& Other);
    FVaRestM5ClosureLeaf& operator=(const FVaRestM5ClosureLeaf& Other);
    ~FVaRestM5ClosureLeaf();
    bool operator==(const FVaRestM5ClosureLeaf& Other) const;
    friend uint32 GetTypeHash(const FVaRestM5ClosureLeaf& Value);
    UPROPERTY() FString Text;
    UPROPERTY() int32 Number = 7;
};
template<> struct TStructOpsTypeTraits<FVaRestM5ClosureLeaf> : TStructOpsTypeTraitsBase2<FVaRestM5ClosureLeaf>
{ enum { WithCopy = true, WithIdenticalViaEquality = true }; };

/** Actual packaged native value; its constructor reads the declared nonreflected enum dependency on GT. */
USTRUCT()
struct FVaRestM5ClosureRoot
{
    GENERATED_BODY()
    FVaRestM5ClosureRoot();
    FVaRestM5ClosureRoot(const FVaRestM5ClosureRoot& Other);
    FVaRestM5ClosureRoot& operator=(const FVaRestM5ClosureRoot& Other);
    ~FVaRestM5ClosureRoot();
    bool operator==(const FVaRestM5ClosureRoot& Other) const;
    friend uint32 GetTypeHash(const FVaRestM5ClosureRoot& Value);
    UPROPERTY() FString Name;
    UPROPERTY() FString NativeDefault;
    UPROPERTY() TOptional<FVaRestM5ClosureLeaf> Maybe;
    UPROPERTY() EVaRestM5ClosureMode Mode = EVaRestM5ClosureMode::Zero;
    UPROPERTY() TArray<FDateTime> Times;
    UPROPERTY() TSet<FString> Labels;
    UPROPERTY() TMap<FString, FDateTime> Dates;
};
template<> struct TStructOpsTypeTraits<FVaRestM5ClosureRoot> : TStructOpsTypeTraitsBase2<FVaRestM5ClosureRoot>
{ enum { WithCopy = true, WithIdenticalViaEquality = true }; };
