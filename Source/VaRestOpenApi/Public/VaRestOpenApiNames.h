// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "VaRestOpenApiModel.h"

struct FVaRestOpenApiFieldName
{
	FString WireName,FriendlyName;
	bool bSanitized=false;
};
struct FVaRestOpenApiParameterName
{
	FString WireName,Location,PinName;
};

/** UE object/pin identities are deliberately case-insensitive; wire keys and schema identities are not. */
class VARESTOPENAPI_API FVaRestOpenApiNameAllocator
{
public:
	/** Existing names are plain snapshots; this class never queries the asset registry. */
	explicit FVaRestOpenApiNameAllocator(TConstArrayView<FString> Existing={});
	FString AllocateType(const FString& SemanticName,bool bEnum,const FString& TypePrefix={});
	static FString Pascal(const FString& Name);
	/** Distinct exact source keys, in source order. Every valid original key reserves its name before sanitization. */
	static TArray<FVaRestOpenApiFieldName> Fields(TConstArrayView<FString> WireNames);
	static TArray<FVaRestOpenApiParameterName> Parameters(TConstArrayView<FVaRestOpenApiParameter> Parameters);
	static FString SanitizeField(const FString& Name);
private:
	static FString ShortAssetName(const FString& Name);
	TSet<FString> UsedNames; // intentional engine identity comparison, not a wire-key table
	TMap<FString,int32> NextTypeSuffix;
};
