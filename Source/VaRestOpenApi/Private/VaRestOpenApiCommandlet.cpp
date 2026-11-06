// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiCommandlet.h"
#include "VaRestOpenApiLibrary.h"
#include "VaRestOpenApiImportOptions.h"
#include "VaRestOpenApiReimport.h"
#include "VaRestOpenApiApplyPrivate.h"
#include "VaRestOpenApiSource.h"
#include "VaRestOpenApiSnapshots.h"
#include "VaRestApi.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
UVaRestApi* LoadApi(FString Path)
{
	if (!Path.Contains(TEXT("."))) Path+=TEXT(".")+FPackageName::GetLongPackageAssetName(Path);
	return VaRest::OpenApi::SafeAssetPath(Path) ? LoadObject<UVaRestApi>(nullptr,*Path) : nullptr;
}
bool SavePackages(UVaRestApi& Api,TConstArrayView<FName> Changed={})
{
	TSet<UPackage*> Packages;Packages.Add(Api.GetOutermost());
	for (const auto& Rules:Api.JsonTypes) if (Rules.Type) Packages.Add(Rules.Type->GetOutermost());
	for (const FName Name:Changed) if (auto* Package=FindPackage(nullptr,*Name.ToString())) Packages.Add(Package);
	bool Success=true;
	for (auto* Package:Packages)
	{
		if (!Package->IsDirty() || Package==GetTransientPackage() || Package->HasAnyPackageFlags(PKG_CompiledIn)) continue;
		FString Filename;
		const auto& Extension=Package->HasAnyPackageFlags(PKG_ContainsMap) ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension();
		if (!FPackageName::IsValidLongPackageName(Package->GetName()) || !FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(),Filename,Extension))
		{ UE_LOG(LogTemp,Error,TEXT("OpenAPI: cannot save package %s"),*Package->GetName());Success=false;continue; }
		FSavePackageArgs Args;Args.TopLevelFlags=RF_Public|RF_Standalone;Args.SaveFlags=SAVE_NoError;
		if (!UPackage::SavePackage(Package,nullptr,*Filename,Args)) { UE_LOG(LogTemp,Error,TEXT("OpenAPI: failed saving %s"),*Filename);Success=false; }
	}
	return Success;
}
bool HasErrors(TConstArrayView<FVaRestOpenApiIssue> Issues)
{
	bool Errors=false;for (const auto& Issue:Issues)
	{
		if (Issue.Severity==EVaRestOpenApiSeverity::Error) { UE_LOG(LogTemp,Error,TEXT("%s:%d:%d [%s] %s #%s"),*Issue.Document,Issue.Line,Issue.Column,*Issue.Code,*Issue.Message,*Issue.Pointer);Errors=true; }
		else if (Issue.Severity==EVaRestOpenApiSeverity::Warning) { UE_LOG(LogTemp,Warning,TEXT("[%s] %s"),*Issue.Code,*Issue.Message); }
		else { UE_LOG(LogTemp,Display,TEXT("[%s] %s"),*Issue.Code,*Issue.Message); }
	}return Errors;
}
}
UVaRestOpenApiCommandlet::UVaRestOpenApiCommandlet()
{
	IsClient=false;IsServer=false;IsEditor=true;LogToConsole=true;ShowErrorCount=true;
}
int32 UVaRestOpenApiCommandlet::Main(const FString& Params)
{
	FString Check,Reimport,Import,Destination;const bool DoCheck=FParse::Value(*Params,TEXT("Check="),Check),DoReimport=FParse::Value(*Params,TEXT("Reimport="),Reimport),DoImport=FParse::Value(*Params,TEXT("Import="),Import);
	FParse::Value(*Params,TEXT("Dest="),Destination);
	if (static_cast<int32>(DoCheck)+static_cast<int32>(DoReimport)+static_cast<int32>(DoImport)!=1 || (DoImport && Destination.IsEmpty()))
	{ UE_LOG(LogTemp,Error,TEXT("Use -run=VaRestOpenApi -Check=/Game/Api, -Reimport=/Game/Api [-AllowBreaking], or -Import=<file> -Dest=/Game/Api."));return 2; }
	if (DoImport)
	{
		TStrongObjectPtr<UVaRestOpenApiImportOptions> Options(NewObject<UVaRestOpenApiImportOptions>());Options->bImportAllOperations=true;
		TStrongObjectPtr<UVaRestApi> Api(UVaRestOpenApiLibrary::ImportFile(Import,Destination,Options.Get()));if (!Api.IsValid()) return 1;
		const auto State=VaRest::OpenApi::DecodeState(Api->ImportState);const bool Errors=!State.IsSet() || HasErrors(VaRest::OpenApi::ReadIssueJson(State.GetValue()[TEXT("report")]));
		const bool Saved=SavePackages(*Api);return Saved && !Errors ? 0 : 1;
	}
	TStrongObjectPtr<UVaRestApi> Api(LoadApi(DoCheck ? Check : Reimport));if (!Api.IsValid()) { UE_LOG(LogTemp,Error,TEXT("OpenAPI: API asset could not be loaded."));return 1; }
	const auto Result=FVaRestOpenApiReimporter::FromSource(Api.Get(),true,FParse::Param(*Params,TEXT("AllowBreaking")),DoCheck);
	const bool Errors=HasErrors(Result.Issues);if (!Result.Api || Result.bCancelled) return 1;
	if (DoCheck) return !Errors && Result.bNoChanges ? 0 : 1;
	if (!Result.bNoChanges && !SavePackages(*Api,Result.ChangedPackages)) return 1;return Errors ? 1 : 0;
}
