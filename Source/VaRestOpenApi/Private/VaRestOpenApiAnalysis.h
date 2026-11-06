// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "VaRestOpenApiModel.h"
#include "Misc/Crc.h"

namespace VaRest::OpenApi
{
inline bool Equal(const FString& A,const FString& B)
{
	return A.Len()==B.Len() && FMemory::Memcmp(*A,*B,A.Len()*sizeof(TCHAR))==0;
}
struct FStringSetKeys : BaseKeyFuncs<FString,FString,false>
{
	static const FString& GetSetKey(const FString& Value) { return Value; }
	static bool Matches(const FString& A,const FString& B) { return Equal(A,B); }
	static uint32 GetKeyHash(const FString& Key) { return FCrc::MemCrc32(*Key,Key.Len()*sizeof(TCHAR)); }
};
template <typename V> struct FStringMapKeys : BaseKeyFuncs<TPair<FString,V>,FString,false>
{
	static const FString& GetSetKey(const TPair<FString,V>& Value) { return Value.Key; }
	static bool Matches(const FString& A,const FString& B) { return Equal(A,B); }
	static uint32 GetKeyHash(const FString& Key) { return FStringSetKeys::GetKeyHash(Key); }
};
using FStringSet=TSet<FString,FStringSetKeys>;
template <typename V> using TStringMap=TMap<FString,V,FDefaultSetAllocator,FStringMapKeys<V>>;

FString Child(const FString& Pointer,const FString& Key);
/** Shared strict URI spelling used by both lexical resolution and the before-connect URL boundary. */
bool SplitSourceUrl(const FString& Raw,FString& Origin,FString& Path,FString& Query);
struct FDocument
{
	FString Uri;
	TArray<uint8> Bytes;
	FVaRestJson Root;
};
struct FResolved
{
	FVaRestJson Value,Annotations;
	FVaRestOpenApiLocation Location;
	bool IsValid() const { return Value.IsValid(); }
};
class FAnalysis
{
public:
	FAnalysis(FString RootUri,const FVaRestOpenApiLoadOptions& InOptions);
	FVaRestOpenApiAnalysis Run(TConstArrayView<uint8> Bytes);
private:
	void Issue(const FVaRestOpenApiLocation& Location,const TCHAR* Code,FString Message,EVaRestOpenApiSeverity Severity=EVaRestOpenApiSeverity::Error);
	TSharedPtr<FDocument> Load(const FString& Uri,TConstArrayView<uint8> Bytes);
	TSharedPtr<FDocument> Document(const FString& Uri,const FVaRestOpenApiLocation& From);
	FResolved At(const FVaRestOpenApiLocation& Location);
	FResolved Reference(const FVaRestOpenApiLocation& From,const FString& Ref);
	FResolved Object(const FVaRestOpenApiLocation& Location,FStringSet* Chain=nullptr);
	FString Identity(const FVaRestOpenApiLocation& Location) const;
	void Duplicates(const FVaRestJson& Node,const FVaRestOpenApiLocation& Location);
	void LocateIssues();
	void Normalize(const TSharedRef<FDocument>& Root);
	FVaRestOpenApiSchemaUse Schema(const FVaRestOpenApiLocation& Location,FStringSet* Chain=nullptr);
	void SchemaList(TArray<FVaRestOpenApiSchemaUse>& Out,const FResolved& Parent,const TCHAR* Key);
	TArray<FVaRestOpenApiMedia> Media(const FResolved& Parent,bool& bValid);
	bool Parameter(const FVaRestOpenApiLocation& Location,FVaRestOpenApiParameter& Out);
	bool Parameters(TArray<FVaRestOpenApiParameter>& Out,const FResolved& Parent);
	void Operations(const FResolved& Paths);
	bool Boolean(const FVaRestJson& Value,const FVaRestOpenApiLocation& Location,bool Default=false);
	const FVaRestOpenApiLoadOptions& Options;
	FVaRestOpenApiAnalysis Result;
	FString RootUri,RootBase,RootOrigin;
	bool bUrl=false,bFatal=false;
	int32 ExternalReads=0;
	int32 SchemaDepth=0;
	TStringMap<TSharedPtr<FDocument>> Documents;
	FStringSet FailedDocuments;
	TStringMap<int32> SchemaIndices;
};
}
