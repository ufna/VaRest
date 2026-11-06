// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "VaRestJson.h"

enum class EVaRestOpenApiSeverity : uint8 { Info, Warning, Error };

struct FVaRestOpenApiLocation
{
	FString Document;
	/** RFC6901 pointer, without '#'. Empty denotes the document root. */
	FString Pointer;
};

struct FVaRestOpenApiIssue
{
	EVaRestOpenApiSeverity Severity=EVaRestOpenApiSeverity::Error;
	FString Code,Message,Document,Pointer,AssetPath;
	int32 Line=0,Column=0;
};

/** An immutable schema identity and annotations belonging to this use, never to its shared target. */
struct FVaRestOpenApiSchemaUse
{
	FString Id;
	FVaRestOpenApiLocation Location;
	FVaRestJson Annotations;
	bool IsValid() const { return !Id.IsEmpty(); }
};

struct FVaRestOpenApiProperty
{
	FString Name;
	FVaRestOpenApiSchemaUse Schema;
	bool bRequired=false;
};

/** Plain native data. No UObject, property, GUID allocation, file handle or live editor state. */
struct FVaRestOpenApiSchema
{
	FString Id,Format,Description;
	FVaRestOpenApiLocation Location;
	TArray<FString> Types;
	/** Retained separately: allOf may require a property declared only in another member. */
	TArray<FString> RequiredFields;
	TArray<FVaRestOpenApiProperty> Properties;
	FVaRestOpenApiSchemaUse Items,AdditionalProperties;
	TArray<FVaRestOpenApiSchemaUse> AllOf,OneOf,AnyOf;
	FVaRestJson Enum,Const,Default,Examples,Discriminator,Source;
	TArray<TPair<FString,FVaRestOpenApiSchemaUse>> DiscriminatorMappings;
	bool bNullable=false,bReadOnly=false,bWriteOnly=false,bDeprecated=false;
	bool bAllowsAdditionalProperties=true,bBooleanSchema=false,bAcceptsAny=true,bDynamicFallback=false;
};

struct FVaRestOpenApiParameter
{
	FString Name,In,Style,Description;
	FVaRestOpenApiLocation Location;
	FVaRestOpenApiSchemaUse Schema;
	FVaRestJson Default,Content;
	bool bRequired=false,bExplode=true,bDeprecated=false;
};

struct FVaRestOpenApiMedia
{
	FString Name;
	FVaRestOpenApiLocation Location;
	FVaRestOpenApiSchemaUse Schema;
	FVaRestJson Encoding,Examples;
};

struct FVaRestOpenApiResponse
{
	FString Status,Description;
	FVaRestOpenApiLocation Location;
	TArray<FVaRestOpenApiMedia> Content;
};

struct FVaRestOpenApiOperation
{
	FString Method,Path,OperationId,Summary,Description;
	FVaRestOpenApiLocation Location;
	TArray<FString> Tags;
	TArray<FVaRestOpenApiParameter> Parameters;
	TArray<FVaRestOpenApiMedia> RequestContent;
	TArray<FVaRestOpenApiResponse> Responses;
	FVaRestJson Security,Servers;
	bool bDeprecated=false,bRequestBodyRequired=false;
};

struct FVaRestOpenApiOperationSource
{
	FString Method,Path,OperationId;
};
struct FVaRestOpenApiModel
{
	FString Version,Title,InfoVersion,Description,RootDocument;
	FVaRestJson Servers,SecuritySchemes,GlobalSecurity,Tags;
	TArray<FVaRestOpenApiSchema> Schemas;
	TArray<FVaRestOpenApiSchemaUse> NamedSchemas;
	TArray<FVaRestOpenApiOperation> Operations;
	/** Declarations survive item errors, so a failed ref never masquerades as an upstream deletion. */
	TArray<FVaRestOpenApiOperationSource> DeclaredOperations,FailedOperations;
	TArray<FString> UnavailablePaths;
	TArray<FString> SourceDocuments;
};

struct FVaRestOpenApiReadRequest
{
	/** A lexically confined file path or a same-origin URL. No remote-origin request is issued. */
	FString Uri,RootBase;
	bool bUrl=false,bAllowOutsideRoot=false;
};

struct FVaRestOpenApiReadResult
{
	TArray<uint8> Bytes;
	/** The loader MUST resolve physical file aliases/redirects and enforce ReadRequest before reading. */
	FString CanonicalUri,Error;
};

struct FVaRestOpenApiLoadOptions
{
	int64 MaxDocumentBytes=64ll*1024*1024;
	int32 MaxExternalDocuments=256,MaxSchemas=50000,MaxOperations=20000;
	bool bAllowOutsideRoot=false;
	/** Successful analyses keep plain source bytes for the final planner diagnostic pass; failures locate immediately. */
	bool bRetainSourceText=false;
	/** Optional IO boundary. Called synchronously on the analysis worker; no default filesystem/network access. */
	TFunction<bool(const FVaRestOpenApiReadRequest&,FVaRestOpenApiReadResult&)> Read;
};

struct FVaRestOpenApiSourceText
{
	FString Document;
	TArray<uint8> Bytes;
};
struct FVaRestOpenApiAnalysis
{
	/** False only for document/limit failures. Item errors leave the remaining model usable. */
	bool bDocumentValid=false;
	FVaRestOpenApiModel Model;
	TArray<FVaRestOpenApiIssue> Issues;
	TArray<FVaRestOpenApiSourceText> SourceText;
};

struct VARESTOPENAPI_API FVaRestOpenApiLoader final
{
	/** RootUri is an absolute file path or HTTP(S) URI. Input is copied; all resulting DOMs are read-only. */
	static FVaRestOpenApiAnalysis Analyze(FString RootUri,TConstArrayView<uint8> Bytes,const FVaRestOpenApiLoadOptions& Options={});
	/** Plain-data SAX pass for diagnostics added after normalization. Does not perform source IO. */
	static void LocateIssues(const FVaRestOpenApiAnalysis& Analysis,TArrayView<FVaRestOpenApiIssue> Issues);
};
