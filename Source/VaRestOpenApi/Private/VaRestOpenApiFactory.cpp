// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiFactory.h"
#include "VaRestOpenApiApply.h"
#include "VaRestOpenApiImportDialog.h"
#include "VaRestOpenApiImportOptions.h"
#include "VaRestOpenApiSource.h"
#include "VaRestOpenApiUrlSource.h"
#include "VaRestApi.h"
#include "AssetImportTask.h"
#include "Async/Async.h"
#include "EditorFramework/AssetImportData.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "Misc/SecureHash.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/Notifications/SNotificationList.h"

namespace
{
template <typename T> T WaitForPlan(TFuture<T>& Future,FScopedSlowTask& Progress,bool& Cancelled)
{
	while (!Future.IsReady()) { Progress.EnterProgressFrame(0);Cancelled=Cancelled || Progress.ShouldCancel();FPlatformProcess::SleepNoStats(.01f); }
	Cancelled=Cancelled || Progress.ShouldCancel();return Future.Get();
}
}
UVaRestOpenApiFactory::UVaRestOpenApiFactory()
{
	SupportedClass=UVaRestApi::StaticClass();bCreateNew=false;bEditorImport=true;bText=false;
	ImportPriority=GetDefault<UVaRestOpenApiSettings>()->bRegisterForJsonFiles ? 102 : -1;
	Formats.Add(TEXT("json;OpenAPI JSON"));Formats.Add(TEXT("yaml;OpenAPI YAML"));Formats.Add(TEXT("yml;OpenAPI YAML"));
}
FText UVaRestOpenApiFactory::GetDisplayName() const { return NSLOCTEXT("VaRestOpenApi","FactoryName","OpenAPI API contract"); }
bool UVaRestOpenApiFactory::FactoryCanImport(const FString& Filename)
{
	if (!GetDefault<UVaRestOpenApiSettings>()->bRegisterForJsonFiles) return false;
	FVaRestOpenApiReadRequest Request;Request.Uri=FPaths::ConvertRelativePathToFull(Filename);Request.RootBase=FPaths::GetPath(Request.Uri);FVaRestOpenApiReadResult Read;
	return VaRest::OpenApi::ReadSourceFile(Request,Read) && VaRest::OpenApi::SniffOpenApi(Read.Bytes);
}
UObject* UVaRestOpenApiFactory::FactoryCreateFile(UClass* InClass,UObject* InParent,FName InName,EObjectFlags Flags,const FString& Filename,const TCHAR*,FFeedbackContext*,bool& bOutOperationCanceled)
{
	return CreateFromSource(InClass,InParent,InName,Flags,Filename,false,bOutOperationCanceled);
}
UObject* UVaRestOpenApiFactory::CreateFromSource(UClass* InClass,UObject* InParent,FName InName,EObjectFlags Flags,const FString& Filename,bool bUrl,bool& bOutOperationCanceled)
{
	using namespace VaRest::OpenApi;check(IsInGameThread());bOutOperationCanceled=false;AdditionalImportedObjects.Reset();
	if (!InClass || !InClass->IsChildOf(UVaRestApi::StaticClass())) return nullptr;
	TStrongObjectPtr<UVaRestOpenApiImportOptions> Options;
	if (AssetImportTask && AssetImportTask->Options)
	{
		if (auto* TaskOptions=Cast<UVaRestOpenApiImportOptions>(AssetImportTask->Options)) Options.Reset(DuplicateObject<UVaRestOpenApiImportOptions>(TaskOptions,GetTransientPackage()));
		else { FVaRestOpenApiIssue Issue;Issue.Code=TEXT("OA-APPLY-OPTIONS");Issue.Message=TEXT("AssetImportTask.Options must be VaRestOpenApiImportOptions.");Report(MakeArrayView(&Issue,1),!IsAutomatedImport());return nullptr; }
	}
	if (!Options.IsValid()) Options.Reset(NewObject<UVaRestOpenApiImportOptions>());
	const bool Automated=IsAutomatedImport();
	FScopedSlowTask Progress(3,NSLOCTEXT("VaRestOpenApi","Planning","Reading and planning OpenAPI"));if (!Automated) Progress.MakeDialog(true);
	const auto Session=bUrl ? NewUrlSourceSession() : TSharedPtr<FUrlSourceSession,ESPMode::ThreadSafe>();
	FLocalSourceAnalysis Loaded;
	for (;;)
	{
		const bool AllowOutside=Options->bAllowReferencesOutsideSpecFolder;
		auto Loading=Async(EAsyncExecution::ThreadPool,[Filename,AllowOutside,Session]() { return Session ? AnalyzeUrlSource(Filename,Session.ToSharedRef()) : AnalyzeLocalFile(Filename,AllowOutside); });
		Loaded=Session ? WaitUrlSource(Loading,Progress,Session.ToSharedRef(),bOutOperationCanceled) : WaitForPlan(Loading,Progress,bOutOperationCanceled);if (bOutOperationCanceled) return nullptr;
		if (!Loaded.Analysis.bDocumentValid) { Report(Loaded.Analysis.Issues,!Automated);return nullptr; }
		if (Automated) break;
		if (Loaded.Analysis.Model.Operations.Num()>150 && Options->SelectedOperations.IsEmpty()) Options->bImportAllOperations=false;
		if (!ShowImportOptions(Loaded.Analysis,*Options,!(AssetImportTask && AssetImportTask->Options))) { bOutOperationCanceled=true;return nullptr; }
		if (bUrl || AllowOutside==Options->bAllowReferencesOutsideSpecFolder) break; // URL confinement always applies
	}
	const auto PlanOptions=Options->PlanOptions(InName.ToString());
	const auto StoredOptions=Options->ToJson().Set(TEXT("knownOperations"),OperationRecords(Loaded.Analysis));
	auto Planning=Async(EAsyncExecution::ThreadPool,[Analysis=MoveTemp(Loaded.Analysis),PlanOptions]() { return FVaRestOpenApiPlanner::Build(Analysis,PlanOptions); });
	auto Plan=WaitForPlan(Planning,Progress,bOutOperationCanceled);if (bOutOperationCanceled) return nullptr;
	Progress.EnterProgressFrame(1,NSLOCTEXT("VaRestOpenApi","Applying","Creating API assets"));
	FVaRestOpenApiApplyOptions Apply;Apply.Parent=InParent;Apply.ApiName=InName;Apply.Flags=Flags;Apply.TypesFolder=Options->TypesFolder;
	Apply.bRegisterAssets=false;Apply.Source=Loaded.Source;Apply.Options=StoredOptions;Apply.IsCancelled=[&]() { return Progress.ShouldCancel(); };
	auto Result=FVaRestOpenApiApplier::Apply(Plan,Apply);bOutOperationCanceled=Result.bCancelled;Report(Result.Issues,!Automated);
	if (!Result.Api) return nullptr;
	if (!bUrl && Result.Api->AssetImportData)
	{
		TArray<FAssetImportInfo::FSourceFile> Sources;
		for (const auto& Document:Loaded.Source[TEXT("documents")].Elements())
		{
			FMD5Hash Hash;LexFromString(Hash,*Document[TEXT("hash")].AsString());
			Sources.Emplace(UAssetImportData::SanitizeImportFilename(Document[TEXT("location")].AsString(),Result.Api->GetOutermost()),FDateTime(0),Hash);
		}
		Result.Api->AssetImportData->SetSourceFiles(MoveTemp(Sources));
	}
	AdditionalImportedObjects=MoveTemp(Result.AdditionalObjects);
	if (!Automated)
	{
		Options->SaveConfig();FNotificationInfo Info(Result.bUndoHistoryCleared ? NSLOCTEXT("VaRestOpenApi","DoneUndo","OpenAPI imported. Undo history was cleared for generated assets.") : NSLOCTEXT("VaRestOpenApi","Done","OpenAPI imported."));Info.ExpireDuration=6;FSlateNotificationManager::Get().AddNotification(Info);
	}
	return Result.Api;
}
