// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "VaRestOpenApiAnalysis.h"
#include "VaRestOpenApiPlan.h"
#include "VaRestOpenApiNames.h"

namespace VaRest::OpenApi
{
FString SemanticId(const FString& Owner,const TCHAR* Role,const FString& Name={});
FString ResolveHttpReference(const FString& Base,const FString& Relative,bool bBaseDirectory=false);
struct FPlannedValue
{
	FVaRestOpenApiPlannedType Type;
	bool bNullable=false,bBase64=false,bCycle=false;
};
struct FFlatProperty
{
	FString Name;
	FVaRestOpenApiSchemaUse Use;
	TArray<FVaRestOpenApiSchemaUse> PriorTypes;
	bool bRequired=false;
};
class FPlanner
{
public:
	FPlanner(const FVaRestOpenApiAnalysis& Analysis,const FVaRestOpenApiPlanOptions& InOptions);
	FVaRestOpenApiPlan Run();
private:
	void Issue(const FVaRestOpenApiLocation& Location,const TCHAR* Code,FString Message,EVaRestOpenApiSeverity Severity=EVaRestOpenApiSeverity::Warning);
	const FVaRestOpenApiSchema* Schema(const FString& Id) const;
	FVaRestJson Meta(const FVaRestOpenApiSchemaUse& Use,const TCHAR* Key) const;
	FString TypeId(const FString& SourceId,const FString& Owner);
	FString TypeHint(const FVaRestOpenApiSchema& Node,const FString& Hint) const;
	FString AllocateTypeName(const FString& Id,const FString& Hint,bool bEnum);
	FPlannedValue Type(const FVaRestOpenApiSchemaUse& Use,const FString& Owner,const FString& Hint,bool bNeedsUnset=false,bool bGenerateBase=false);
	FPlannedValue Enum(const FVaRestOpenApiSchemaUse& Use,const FVaRestJson& Values,const FString& Owner,const FString& Hint,bool bNeedsUnset);
	void Flatten(const FVaRestOpenApiSchemaUse& Use,TArray<FFlatProperty>& Fields,FStringSet& Required,FStringSet& Visiting);
	FPlannedValue Struct(const FVaRestOpenApiSchemaUse& Use,const FString& Owner,const FString& Hint);
	void Defaults();
	FString DefaultText(const FVaRestOpenApiPlannedType& Type,const FVaRestJson& Value,bool bUnset,bool bNullable,const FVaRestOpenApiLocation& Location);
	FString Shape(const FVaRestOpenApiSchemaUse& Use) const;
	FVaRestJson Canonical(const FVaRestJson& Value) const;
	bool Selected(const FVaRestOpenApiOperation& Operation) const;
	const FVaRestOpenApiMedia* Preferred(const TArray<FVaRestOpenApiMedia>& Content,bool bResponse) const;
	void Endpoints();
	void Environments();
	FString ServerUrl(const FVaRestJson& Server,const FVaRestOpenApiLocation& Location);
	void Authentication();
	void DetectEnvelope();
	FString Category(const FVaRestOpenApiOperation& Operation);
	const FVaRestOpenApiModel& Model;
	const FVaRestOpenApiPlanOptions& Options;
	FVaRestOpenApiPlan Plan;
	FVaRestOpenApiNameAllocator Names;
	TStringMap<const FVaRestOpenApiSchema*> Schemas;
	TStringMap<FString> SemanticIds;
	TStringMap<FString> DefinitionSources;
	TStringMap<FString> AssetNames;
	TStringMap<int32> EnumIndices;
	TStringMap<FVaRestOpenApiPlannedType> Built;
	TStringMap<FVaRestOpenApiPlannedType> BuiltDefinitions;
	TStringMap<TArray<FString>> Variants;
	FStringSet Named,Building,ResolvingTypes,Polymorphic;
	TArray<const FVaRestOpenApiOperation*> Chosen;
	TMap<const FVaRestOpenApiOperation*,FString> EndpointIds;
	TArray<const FVaRestOpenApiResponse*> SuccessResponses;
	TArray<const FVaRestOpenApiMedia*> SuccessMedia;
	TArray<FVaRestOpenApiSchemaUse> ErrorSchemas;
	int32 Depth=0;
};
}
