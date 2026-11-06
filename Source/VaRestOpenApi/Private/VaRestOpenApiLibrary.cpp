// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiLibrary.h"
#include "VaRestOpenApiFactory.h"
#include "VaRestOpenApiImportOptions.h"
#include "VaRestOpenApiApplyPrivate.h"
#include "VaRestOpenApiSource.h"
#include "VaRestOpenApiReimport.h"
#include "VaRestOpenApiCleanup.h"
#include "VaRestOpenApiUrlPolicy.h"
#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "Logging/MessageLog.h"
#include "UObject/Package.h"
#include "EditorFramework/AssetImportData.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "ScopedTransaction.h"

namespace
{
UVaRestApi* ImportSource(const FString& Source,const FString& DestinationPackage,UVaRestOpenApiImportOptions* Options,bool bUrl)
{
	using namespace VaRest::OpenApi;check(IsInGameThread());
	if (DestinationPackage.Len()>=NAME_SIZE || !FPackageName::IsValidLongPackageName(DestinationPackage) || FPackageName::DoesPackageExist(DestinationPackage))
	{
		FVaRestOpenApiIssue Issue;Issue.Code=TEXT("OA-APPLY-TARGET");Issue.Message=TEXT("Choose a valid unused destination package; use reimport to update an API.");Report(MakeArrayView(&Issue,1),false);return nullptr;
	}
	const FName Name(*FPackageName::GetLongPackageAssetName(DestinationPackage));UPackage* Package=CreatePackage(*DestinationPackage);
	if (FindObjectFast<UObject>(Package,Name)) return nullptr;
	TStrongObjectPtr<UVaRestOpenApiFactory> Factory(NewObject<UVaRestOpenApiFactory>());
	TStrongObjectPtr<UAssetImportTask> Task(NewObject<UAssetImportTask>());Task->bAutomated=true;Task->Options=Options;Factory->SetAssetImportTask(Task.Get());
	bool Cancelled=false;auto* Api=Cast<UVaRestApi>(Factory->CreateFromSource(UVaRestApi::StaticClass(),Package,Name,RF_Public|RF_Standalone|RF_Transactional,Source,bUrl,Cancelled));
	if (!Api) return nullptr;
	FAssetRegistryModule::AssetCreated(Api);for (auto* Object:Factory->GetAdditionalImportedObjects()) FAssetRegistryModule::AssetCreated(Object);return Api;
}
}
UVaRestApi* UVaRestOpenApiLibrary::ImportFile(const FString& Filename,const FString& DestinationPackage,UVaRestOpenApiImportOptions* Options) { return ImportSource(Filename,DestinationPackage,Options,false); }
UVaRestApi* UVaRestOpenApiLibrary::ImportFromFile(const FString& Filename,const FString& DestinationPackage,UVaRestOpenApiImportOptions* Options) { return ImportFile(Filename,DestinationPackage,Options); }
UVaRestApi* UVaRestOpenApiLibrary::ImportFromUrl(const FString& Url,const FString& DestinationPackage,UVaRestOpenApiImportOptions* Options) { return ImportSource(Url,DestinationPackage,Options,true); }
bool UVaRestOpenApiLibrary::ShowLastImportReport(UVaRestApi* Api)
{
	using namespace VaRest::OpenApi;check(IsInGameThread());if (!Api) return false;
	const auto State=DecodeState(Api->ImportState);if (!State.IsSet()) return false;
	const auto Issues=ReadIssueJson(State.GetValue()[TEXT("report")]);
	Report(Issues,false);FMessageLog(TEXT("VaRestOpenAPI")).Open(EMessageSeverity::Info,true);return true;
}
bool UVaRestOpenApiLibrary::Reimport(UVaRestApi* Api,bool bAllowBreaking)
{
	const auto Result=FVaRestOpenApiReimporter::FromSource(Api,true,bAllowBreaking);return Result.Api && !Result.bCancelled;
}
bool UVaRestOpenApiLibrary::CheckForChanges(UVaRestApi* Api)
{
	const auto Result=FVaRestOpenApiReimporter::FromSource(Api,true,false,true);return Result.Api && Result.bNoChanges;
}
bool UVaRestOpenApiLibrary::ChangeFileSource(UVaRestApi* Api,const FString& Filename)
{
	using namespace VaRest::OpenApi;check(IsInGameThread());if (!Api) return false;
	const auto State=DecodeState(Api->ImportState);if (!State.IsSet()) return false;
	FVaRestOpenApiReadRequest Request;Request.Uri=FPaths::ConvertRelativePathToFull(Filename);Request.RootBase=FPaths::GetPath(Request.Uri);FVaRestOpenApiReadResult Read;
	if (!ReadSourceFile(Request,Read)) { FVaRestOpenApiIssue Issue;Issue.Code=TEXT("OA-DOC-READ");Issue.Message=Read.Error;Issue.Document=Filename;Report(MakeArrayView(&Issue,1));return false; }
	auto Updated=State.GetValue().Clone();auto Documents=FVaRestJson::Array();Documents.Add(SourceRecord(Read.CanonicalUri,Read.Bytes));
	Updated.Set(TEXT("source"),FVaRestJson::Object().Set(TEXT("kind"),TEXT("file")).Set(TEXT("location"),Read.CanonicalUri).Set(TEXT("documents"),Documents));
	FVaRestApiImportState Encoded;if (!EncodeState(Updated,Encoded)) return false;
	FScopedTransaction Transaction(NSLOCTEXT("VaRestOpenApi","ChangeFileSource","Change OpenAPI source"));Api->Modify();Api->ImportState=MoveTemp(Encoded);
	if (Api->AssetImportData)
	{
		Api->AssetImportData->Modify();FMD5Hash Hash;LexFromString(Hash,*Documents.At(0)[TEXT("hash")].AsString());TArray<FAssetImportInfo::FSourceFile> Files;
		Files.Emplace(UAssetImportData::SanitizeImportFilename(Read.CanonicalUri,Api->GetOutermost()),FDateTime(0),Hash);Api->AssetImportData->SetSourceFiles(MoveTemp(Files));
	}
	Api->MarkPackageDirty();return true;
}
bool UVaRestOpenApiLibrary::ChangeUrlSource(UVaRestApi* Api,const FString& Url)
{
	using namespace VaRest::OpenApi;check(IsInGameThread());if (!Api) return false;
	const auto State=DecodeState(Api->ImportState);const auto Parsed=FSourceUrl::Parse(Url);if (!State.IsSet() || !Parsed.IsSet()) return false;
	const auto& Previous=State.GetValue()[TEXT("source")];if (Equal(Previous[TEXT("kind")].AsString(),TEXT("url")) && Equal(Previous[TEXT("location")].AsString(),Parsed.GetValue().Uri)) return true;
	auto Updated=State.GetValue().Clone();Updated.Set(TEXT("source"),FVaRestJson::Object().Set(TEXT("kind"),TEXT("url")).Set(TEXT("location"),Parsed.GetValue().Uri));
	FVaRestApiImportState Encoded;if (!EncodeState(Updated,Encoded)) return false;
	FScopedTransaction Transaction(NSLOCTEXT("VaRestOpenApi","ChangeUrlSource","Change OpenAPI URL source"));Api->Modify();Api->ImportState=MoveTemp(Encoded);
	// URLs cannot be engine source files: generic reimport expects a physical file and otherwise asks to replace it.
	if (Api->AssetImportData) { Api->AssetImportData->Modify();Api->AssetImportData->SetSourceFiles({}); }
	Api->MarkPackageDirty();return true;
}
int32 UVaRestOpenApiLibrary::CleanUpRemovedItems(UVaRestApi* Api) { return VaRest::OpenApi::CleanupRemoved(Api); }
