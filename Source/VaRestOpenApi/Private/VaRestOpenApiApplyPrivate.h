// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "VaRestOpenApiApply.h"
#include "VaRestOpenApiAnalysis.h"
#include "VaRestOpenApiMerge.h"
#include "VaRestApi.h"
#include "EdGraph/EdGraphPin.h"
#include "UObject/StrongObjectPtr.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"

class UUserDefinedEnum;

namespace VaRest::OpenApi
{
/** Internal staging owns newly created objects across struct compilation/GC and publishes only at commit. */
class FApply
{
public:
	FApply(const FVaRestOpenApiPlan& InPlan,const FVaRestOpenApiApplyOptions& InOptions):Plan(InPlan),Options(InOptions) { Result.Issues=Plan.Issues; }
	FVaRestOpenApiApplyResult Run();
private:
	friend class FReimport;
	void Issue(const TCHAR* Code,const FString& Message,const UObject* Object=nullptr,EVaRestOpenApiSeverity Severity=EVaRestOpenApiSeverity::Warning);
	UPackage* TypePackage(const FString& Name,FName& OutName);
	void Keep(UObject* Object);
	bool Cancelled();
	void Discard();
	void Enums();
	void Structs();
	bool BuildStruct(UUserDefinedStruct* Struct,const FVaRestOpenApiPlannedStruct& Row);
	bool SlowStruct(UUserDefinedStruct* Struct,TArray<FStructVariableDescription>& Members);
	bool CheckStruct(const UUserDefinedStruct* Struct,const TArray<FStructVariableDescription>& Members) const;
	bool EditEnum(UUserDefinedEnum* Enum,TConstArrayView<FVaRestOpenApiMergeItem*> Values);
	bool EditStruct(UUserDefinedStruct* Struct,TConstArrayView<FVaRestOpenApiMergeItem*> Values,const TStringMap<const FVaRestOpenApiMergeItem*>& Live);
	void EditApi(TConstArrayView<FVaRestOpenApiMergeItem*> Items,const TStringMap<const FVaRestOpenApiMergeItem*>& Live);
	void EditRules(TConstArrayView<FVaRestOpenApiMergeItem*> Items);
	FEdGraphPinType Pin(const FVaRestOpenApiPlannedType& Type) const;
	FVaRestTypeRef Type(const FVaRestOpenApiPlannedType& Type) const;
	FString Default(const FVaRestOpenApiPlannedField& Field,const FEdGraphPinType& PinType);
	void Api();
	void Rules();
	bool Manifest();
	const FVaRestOpenApiPlan& Plan;
	const FVaRestOpenApiApplyOptions& Options;
	FVaRestOpenApiApplyResult Result;
	TArray<TStrongObjectPtr<UObject>> Created;
	TStringMap<UField*> Types;
	TStringMap<FGuid> Fields,Endpoints;
	FStringSet FailedTypes;
	FStringSet NewTypeIds;
	FString Folder;
};
bool EncodeState(const FVaRestJson& Json,FVaRestApiImportState& State);
TOptional<FVaRestJson> DecodeState(const FVaRestApiImportState& State);
}
