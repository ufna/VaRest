// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "VaRestOpenApiModel.h"

namespace VaRest::OpenApi
{
struct FLocalSourceAnalysis { FVaRestOpenApiAnalysis Analysis;FVaRestJson Source; };
FLocalSourceAnalysis AnalyzeLocalFile(const FString& Filename,bool bAllowOutside);
FVaRestJson OperationRecords(const FVaRestOpenApiAnalysis& Analysis);
/** Extension checked before any filesystem query. Refuses aliases rather than reading outside confinement. */
bool ReadSourceFile(const FVaRestOpenApiReadRequest& Request,FVaRestOpenApiReadResult& Out);
bool SniffOpenApi(TConstArrayView<uint8> Bytes);
FVaRestJson SourceRecord(const FString& Uri,TConstArrayView<uint8> Bytes);
void Report(TConstArrayView<FVaRestOpenApiIssue> Issues,bool bOpen=true);
FVaRestJson IssueJson(TConstArrayView<FVaRestOpenApiIssue> Issues);
TArray<FVaRestOpenApiIssue> ReadIssueJson(const FVaRestJson& Rows);
}
