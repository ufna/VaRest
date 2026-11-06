// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "VaRestOpenApiModel.h"
#include "VaRestOpenApiEnums.h"

enum class EVaRestOpenApiValueKind : uint8 { None,Json,Struct,Enum,String,Bytes,Bool,Int,Int64,Double,DateTime };
enum class EVaRestOpenApiContainer : uint8 { None,Array,Map };
enum class EVaRestOpenApiPresence : uint8 { Always,OmitWhenDefault,NullWhenDefault,DecodeOnly,Ignore };
enum class EVaRestOpenApiBodyKind : uint8 { Json,Form,Multipart,Binary,Text };
enum class EVaRestOpenApiQueryStyle : uint8 { Repeat,Csv,Ssv,Pipes };
enum class EVaRestOpenApiAuthMode : uint8 { Required,IfAvailable,None };
enum class EVaRestOpenApiAuthKind : uint8 { None,Bearer,Basic,ApiKey,BearerRefresh,ClientCredentials };

struct FVaRestOpenApiPlannedType
{
	EVaRestOpenApiValueKind Kind=EVaRestOpenApiValueKind::Json;
	EVaRestOpenApiContainer Container=EVaRestOpenApiContainer::None;
	FString DefinitionId;
	bool bJsonValue=false;
};
struct FVaRestOpenApiPlannedField
{
	FString Id,WireName,FriendlyName,Tooltip,DefaultText;
	FVaRestOpenApiLocation Location;
	FVaRestOpenApiPlannedType Type;
	EVaRestOpenApiPresence Presence=EVaRestOpenApiPresence::Always;
	FVaRestJson Default;
	bool bNullable=false,bBase64=false,bDefaultInTooltipOnly=false,bRequired=false;
};
struct FVaRestOpenApiPlannedStruct
{
	FString Id,AssetName,Tooltip;
	TArray<FVaRestOpenApiPlannedField> Fields;
};
struct FVaRestOpenApiPlannedEnum
{
	FString Id,AssetName,Tooltip;
	FVaRestOpenApiEnumPlan Definition;
};
struct FVaRestOpenApiPlannedParameter
{
	FString Id,Name,PinName,In,Description,DefaultText;
	FVaRestOpenApiLocation Location;
	FVaRestJson Default;
	FVaRestOpenApiPlannedType Type;
	EVaRestOpenApiQueryStyle Style=EVaRestOpenApiQueryStyle::Repeat;
	bool bRequired=false;
};
struct FVaRestOpenApiPlannedEndpoint
{
	FString Id,OperationId,Name,Verb,Path,Category,Description,RequestMedia,BodyContentType;
	FVaRestOpenApiLocation Location;
	TArray<FVaRestOpenApiPlannedParameter> Parameters;
	FVaRestOpenApiPlannedType RequestType,ResponseType,ErrorType;
	EVaRestOpenApiBodyKind BodyKind=EVaRestOpenApiBodyKind::Json;
	EVaRestOpenApiAuthMode AuthMode=EVaRestOpenApiAuthMode::Required;
	bool bDeprecated=false,bIdempotent=false,bHasBody=false,bMixedSuccessShapes=false,bOverrideEnvelope=false,bDisableEnvelopePredicate=false;
	FString EnvelopeDataPath;
};
struct FVaRestOpenApiPlannedEnvironment
{
	FString Id,UrlTemplate,Name,BaseUrl,Description;
};
struct FVaRestOpenApiPlannedAuth
{
	EVaRestOpenApiAuthKind Kind=EVaRestOpenApiAuthKind::None;
	FString SchemeName,Name,Placement,TokenUrl;
	TArray<FString> RedactedKeys;
};
struct FVaRestOpenApiPlannedEnvelope
{
	FString DataPath,SuccessPath,ErrorMessagePath,ErrorCodePath;
	int32 WrappedOperations=0,ConsideredOperations=0;
	bool bParseErrorBodies=false;
};
struct FVaRestOpenApiEndpointIdentity
{
	FString OperationId,Verb,Path,Id;
};
struct FVaRestOpenApiPlanOptions
{
	FString ApiName=TEXT("Api"),TypePrefix;
	TArray<FString> SelectedTags,SelectedOperations,ExistingTypeNames;
	/** Reimport seeds retain semantic identity across operationId/route/name edits. Exact string keys. */
	TArray<FVaRestOpenApiEndpointIdentity> ExistingEndpoints;
	TArray<TPair<FString,FString>> ExistingDefinitions;
	bool bDateTimeAsString=false,bEnumsAsString=false,bIntegersWithoutFormatAsInt32=false,bDetectEnvelope=true;
	bool bSelectAllOperations=true,bGenerateUnreferencedSchemas=false;
};
struct VARESTOPENAPI_API FVaRestOpenApiPlan
{
	bool bValid=false;
	TArray<FVaRestOpenApiPlannedStruct> Structs;
	TArray<FVaRestOpenApiPlannedEnum> Enums;
	TArray<FVaRestOpenApiPlannedEndpoint> Endpoints;
	/** Old endpoints that could not be analyzed; retain their bases and required declarations on reimport. */
	TArray<FString> UnavailableEndpointIds;
	TArray<FVaRestOpenApiPlannedEnvironment> Environments;
	FString DefaultEnvironment;
	FVaRestOpenApiPlannedAuth Auth;
	FVaRestOpenApiPlannedEnvelope Envelope;
	TArray<FVaRestOpenApiIssue> Issues;
	/** Native data only; stable array order and key spelling for tooling/golden comparisons. */
	FVaRestJson ToJson() const;
};
struct VARESTOPENAPI_API FVaRestOpenApiPlanner final
{
	static FVaRestOpenApiPlan Build(const FVaRestOpenApiAnalysis& Analysis,const FVaRestOpenApiPlanOptions& Options={});
};
