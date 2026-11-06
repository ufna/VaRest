// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiSource.h"
#include "Misc/Paths.h"

namespace VaRest::OpenApi
{
FVaRestJson OperationRecords(const FVaRestOpenApiAnalysis& Analysis)
{
	auto Rows=FVaRestJson::Array();
	if (!Analysis.Model.DeclaredOperations.IsEmpty()) for (const auto& Op:Analysis.Model.DeclaredOperations) Rows.Add(FVaRestJson::Object().Set(TEXT("operationId"),Op.OperationId).Set(TEXT("verb"),Op.Method).Set(TEXT("path"),Op.Path));
	else for (const auto& Op:Analysis.Model.Operations) Rows.Add(FVaRestJson::Object().Set(TEXT("operationId"),Op.OperationId).Set(TEXT("verb"),Op.Method).Set(TEXT("path"),Op.Path));return Rows;
}
FLocalSourceAnalysis AnalyzeLocalFile(const FString& Filename,bool AllowOutside)
{
	FLocalSourceAnalysis Result;FVaRestOpenApiReadRequest Root;Root.Uri=FPaths::ConvertRelativePathToFull(Filename);Root.RootBase=FPaths::GetPath(Root.Uri);
	FVaRestOpenApiReadResult Read;
	if (!ReadSourceFile(Root,Read))
	{
		FVaRestOpenApiIssue Issue;Issue.Code=TEXT("OA-DOC-READ");Issue.Document=Filename;Issue.Message=Read.Error;Result.Analysis.Issues.Add(MoveTemp(Issue));return Result;
	}
	if (!SniffOpenApi(Read.Bytes))
	{
		FVaRestOpenApiIssue Issue;Issue.Code=TEXT("OA-DOC-NOTOPENAPI");Issue.Document=Filename;Issue.Message=TEXT("This document has no top-level openapi or swagger key.");Result.Analysis.Issues.Add(MoveTemp(Issue));return Result;
	}
	auto Documents=FVaRestJson::Array();Documents.Add(SourceRecord(Read.CanonicalUri,Read.Bytes));
	FVaRestOpenApiLoadOptions Load;Load.bAllowOutsideRoot=AllowOutside;Load.bRetainSourceText=true;
	Load.Read=[&](const FVaRestOpenApiReadRequest& Request,FVaRestOpenApiReadResult& Out)
	{
		if (!ReadSourceFile(Request,Out)) return false;
		Documents.Add(SourceRecord(Out.CanonicalUri,Out.Bytes));return true;
	};
	Result.Analysis=FVaRestOpenApiLoader::Analyze(Read.CanonicalUri,Read.Bytes,Load);
	Result.Source=FVaRestJson::Object().Set(TEXT("kind"),TEXT("file")).Set(TEXT("location"),Read.CanonicalUri).Set(TEXT("documents"),Documents).Set(TEXT("fetchedAt"),FDateTime::UtcNow().ToIso8601());return Result;
}
}
