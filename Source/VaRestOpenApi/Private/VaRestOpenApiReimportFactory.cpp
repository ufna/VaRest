// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiReimportFactory.h"
#include "VaRestOpenApiApplyPrivate.h"
#include "VaRestOpenApiLibrary.h"
#include "VaRestOpenApiReimport.h"
#include "VaRestOpenApiSource.h"
#include "EditorFramework/AssetImportData.h"

UVaRestOpenApiReimportFactory::UVaRestOpenApiReimportFactory()
{
	bEditorImport=false;Formats.Reset();ImportPriority=102;
}
bool UVaRestOpenApiReimportFactory::CanReimport(UObject* Object,TArray<FString>& OutFilenames)
{
	using namespace VaRest::OpenApi;const auto* Api=Cast<UVaRestApi>(Object);if (!Api) return false;
	const auto State=DecodeState(Api->ImportState);if (!State.IsSet() || !Equal(State.GetValue()[TEXT("source")][TEXT("kind")].AsString(),TEXT("file"))) return false;
	if (Api->AssetImportData) Api->AssetImportData->ExtractFilenames(OutFilenames);
	if (OutFilenames.IsEmpty()) OutFilenames.Add(State.GetValue()[TEXT("source")][TEXT("location")].AsString());return true;
}
void UVaRestOpenApiReimportFactory::SetReimportPaths(UObject* Object,const TArray<FString>& NewPaths)
{
	if (!NewPaths.IsEmpty()) UVaRestOpenApiLibrary::ChangeFileSource(Cast<UVaRestApi>(Object),NewPaths[0]);
}
void UVaRestOpenApiReimportFactory::SetReimportPaths(UObject* Object,const FString& NewPath,int32 SourceIndex)
{
	if (SourceIndex==0 || SourceIndex==INDEX_NONE) UVaRestOpenApiLibrary::ChangeFileSource(Cast<UVaRestApi>(Object),NewPath);
	else
	{
		FVaRestOpenApiIssue Issue;Issue.Code=TEXT("OA-REF-UNRESOLVED");Issue.Message=TEXT("External source locations come from $ref. Update or bundle the root document to change an external source.");VaRest::OpenApi::Report(MakeArrayView(&Issue,1));
	}
}
EReimportResult::Type UVaRestOpenApiReimportFactory::Reimport(UObject* Object)
{
	const auto Result=FVaRestOpenApiReimporter::FromSource(Cast<UVaRestApi>(Object),IsAutomatedReimport());
	return Result.bCancelled ? EReimportResult::Cancelled : Result.Api ? EReimportResult::Succeeded : EReimportResult::Failed;
}
