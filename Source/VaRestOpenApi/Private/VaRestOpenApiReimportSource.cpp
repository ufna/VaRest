// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiReimport.h"
#include "VaRestOpenApiApplyPrivate.h"
#include "VaRestOpenApiImportDialog.h"
#include "VaRestOpenApiImportOptions.h"
#include "VaRestOpenApiSource.h"
#include "VaRestOpenApiUrlSource.h"
#include "Async/Async.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopedSlowTask.h"
#include "Logging/MessageLog.h"
#include "Widgets/Notifications/SNotificationList.h"

namespace
{
using namespace VaRest::OpenApi;
bool SameOperation(const FVaRestOpenApiOperation& Operation,const FVaRestJson& Previous)
{
	return (!Operation.OperationId.IsEmpty() && Equal(Operation.OperationId,Previous[TEXT("operationId")].AsString())) ||
		(Equal(Operation.Method,Previous[TEXT("verb")].AsString()) && Equal(Operation.Path,Previous[TEXT("path")].AsString()));
}
template <typename T> T Wait(TFuture<T>& Future,FScopedSlowTask& Progress,bool& Cancelled)
{
	while (!Future.IsReady()) { Progress.EnterProgressFrame(0);Cancelled=Cancelled || Progress.ShouldCancel();FPlatformProcess::SleepNoStats(.01f); }
	Cancelled=Cancelled || Progress.ShouldCancel();return Future.Get();
}
}
FVaRestOpenApiReimportResult FVaRestOpenApiReimporter::FromSource(UVaRestApi* Api,bool bAutomated,bool bAllowBreaking,bool bCheckOnly)
{
	using namespace VaRest::OpenApi;check(IsInGameThread());FVaRestOpenApiReimportResult Result;TStrongObjectPtr<UVaRestApi> Root(Api);
	const auto Fail=[&](const TCHAR* Code,const FString& Message) { FVaRestOpenApiIssue Issue;Issue.Code=Code;Issue.Message=Message;if (Api) Issue.AssetPath=Api->GetPathName();Result.Issues.Add(MoveTemp(Issue));Report(Result.Issues,!bAutomated); };
	const auto State=Api ? DecodeState(Api->ImportState) : TOptional<FVaRestJson>();if (!State.IsSet()) { Fail(TEXT("OA-REIMPORT-STATE"),TEXT("This API has no readable OpenAPI import manifest."));return Result; }
	const FString Kind=State.GetValue()[TEXT("source")][TEXT("kind")].AsString();const bool bUrl=Equal(Kind,TEXT("url"));
	if (!bUrl && !Equal(Kind,TEXT("file"))) { Fail(TEXT("OA-DOC-READ"),TEXT("The source must be a local document or an HTTP(S) URL."));return Result; }
	const FString Filename=State.GetValue()[TEXT("source")][TEXT("location")].AsString();
	TStrongObjectPtr<UVaRestOpenApiImportOptions> Options(NewObject<UVaRestOpenApiImportOptions>());Options->FromJson(State.GetValue()[TEXT("options")]);
	// Compare user edits against decoded options, not legacy serialization defaults.
	const auto InitialOptions=Options->ToJson();
	FScopedSlowTask Progress(3,NSLOCTEXT("VaRestOpenApi","ReimportPlanning","Checking OpenAPI source"));if (!bAutomated) Progress.MakeDialog(true);
	const auto Session=bUrl ? NewUrlSourceSession() : TSharedPtr<FUrlSourceSession,ESPMode::ThreadSafe>();
	for (;;)
	{
		const bool AllowOutside=Options->bAllowReferencesOutsideSpecFolder;auto Loading=Async(EAsyncExecution::ThreadPool,[Filename,AllowOutside,Session]() { return Session ? AnalyzeUrlSource(Filename,Session.ToSharedRef()) : AnalyzeLocalFile(Filename,AllowOutside); });
		bool Cancelled=false;auto Loaded=Session ? WaitUrlSource(Loading,Progress,Session.ToSharedRef(),Cancelled) : Wait(Loading,Progress,Cancelled);if (Cancelled) { Result.bCancelled=true;return Result; }
		if (!Loaded.Analysis.bDocumentValid) { Result.Issues=MoveTemp(Loaded.Analysis.Issues);Report(Result.Issues,!bAutomated);return Result; }
		auto Analysis=MakeShared<FVaRestOpenApiAnalysis>(MoveTemp(Loaded.Analysis));auto PlanOptions=Options->PlanOptions(Api->GetName());SeedOptions(Api,PlanOptions);PlanOptions.ExistingTypeNames=SnapshotExistingTypeNames();
		const auto OldKnown=State.GetValue()[TEXT("options")][TEXT("knownOperations")];
		if (Options->bImportAllOperations && !Options->bIncludeNewOperations)
		{
			PlanOptions.bSelectAllOperations=false;PlanOptions.SelectedOperations.Reset();
			const auto Known=OldKnown.IsArray() ? OldKnown : State.GetValue()[TEXT("plan")][TEXT("endpoints")];
			for (const auto& Operation:Analysis->Model.Operations) for (const auto& Previous:Known.Elements()) if (SameOperation(Operation,Previous)) { PlanOptions.SelectedOperations.Add(Operation.Method+TEXT(" ")+Operation.Path);break; }
		}
		else if (!Options->bImportAllOperations)
		{
			for (const auto& Operation:Analysis->Model.Operations)
			{
				bool Selected=false;
				for (const auto& Previous:PlanOptions.ExistingEndpoints)
				{
					const bool WasSelected=Options->SelectedOperations.ContainsByPredicate([&](const FString& Id) { return Equal(Id,Previous.OperationId) || Equal(Id,Previous.Verb+TEXT(" ")+Previous.Path); });
				if (WasSelected && ((!Operation.OperationId.IsEmpty() && Equal(Operation.OperationId,Previous.OperationId)) || (Equal(Operation.Method,Previous.Verb) && Equal(Operation.Path,Previous.Path)))) { Selected=true;break; }
				}
				if (!Selected && Options->bIncludeNewOperations && OldKnown.IsArray()) { Selected=true;for (const auto& Previous:OldKnown.Elements()) if (SameOperation(Operation,Previous)) { Selected=false;break; } }
				if (Selected) { const FString Route=Operation.Method+TEXT(" ")+Operation.Path;if (!PlanOptions.SelectedOperations.ContainsByPredicate([&](const FString& Id) { return Equal(Id,Route); })) PlanOptions.SelectedOperations.Add(Route); }
			}
		}
		auto Planning=Async(EAsyncExecution::ThreadPool,[Analysis,PlanOptions]() { return FVaRestOpenApiPlanner::Build(*Analysis,PlanOptions); });
		auto Plan=Wait(Planning,Progress,Cancelled);if (Cancelled) { Result.bCancelled=true;return Result; }
		FVaRestOpenApiMergeOptions MergeOptions;MergeOptions.bAutomated=bAutomated;MergeOptions.bAllowBreaking=bAllowBreaking;
		auto Changes=Preview(Api,Plan,MergeOptions);if (!Changes.bValid) { Result.Issues=MoveTemp(Changes.Issues);Report(Result.Issues,!bAutomated);return Result; }
		if (!Changes.Manifest.Equals(State.GetValue())) { Fail(TEXT("OA-REIMPORT-STALE"),TEXT("The import source or options changed while planning. Restart reimport."));return Result; }
		if (bCheckOnly)
		{
			const auto Next=Changes.Merge.NextBase(Changes.Base,Changes.Theirs);Result.Api=Api;Result.bNoChanges=!Changes.Merge.HasWrites() && FVaRestOpenApiMerge::ItemsJson(Next).Equals(Changes.Manifest[TEXT("items")]);Result.Issues=Changes.Issues;
			for (const auto& Change:Changes.Merge.Changes) if (Change.Change!=EVaRestOpenApiChange::KeptUserEdit && Change.Change!=EVaRestOpenApiChange::Suppressed)
			{ FVaRestOpenApiIssue Issue;Issue.Severity=EVaRestOpenApiSeverity::Info;Issue.Code=TEXT("OA-REIMPORT-CHANGE");Issue.Message=Change.Kind+TEXT(" ")+Change.Id+TEXT(" ")+Change.Property+(Change.bBreaking ? TEXT(" [Breaking]") : TEXT(""));Issue.AssetPath=Api->GetPathName();Result.Issues.Add(MoveTemp(Issue)); }
			Report(Result.Issues,!bAutomated);if (!bAutomated) FMessageLog(TEXT("VaRestOpenAPI")).Open(EMessageSeverity::Info,true);return Result;
		}
		const auto ProposedBases=Changes.Merge.NextBase(Changes.Base,Changes.Theirs);
		const bool ContentChanged=Changes.Merge.HasWrites() || !FVaRestOpenApiMerge::ItemsJson(ProposedBases).Equals(Changes.Manifest[TEXT("items")]);
		if (!bAutomated && ContentChanged)
		{
			bool ChangeOptions=false;
			if (!ShowReimportPreview(Changes,*Analysis,*Options,ChangeOptions))
			{
				if (ChangeOptions && ShowImportOptions(*Analysis,*Options,false)) continue;
				Result.bCancelled=true;return Result;
			}
		}
		// Ignored source changes must not advance discovery/options or the stored source hash.
		// Apply still owns stale-preview checks and real suppression/deferred metadata updates.
		auto StoredOptions=State.GetValue()[TEXT("options")];
		if (ContentChanged || !Options->ToJson().Equals(InitialOptions))
		{
			StoredOptions=Options->ToJson().Set(TEXT("knownOperations"),OperationRecords(*Analysis));
			if (!PlanOptions.bSelectAllOperations && (!Options->bImportAllOperations || PlanOptions.SelectedOperations.Num()!=Analysis->Model.Operations.Num()))
			{
				auto Selection=FVaRestJson::Array();for (const auto& Id:PlanOptions.SelectedOperations) Selection.Add(Id);StoredOptions.Set(TEXT("allOperations"),false).Set(TEXT("operations"),Selection);
			}
		}
		FVaRestOpenApiApplyOptions ApplyOptions;ApplyOptions.Source=Loaded.Source;ApplyOptions.Options=StoredOptions;ApplyOptions.TypesFolder=Options->TypesFolder;ApplyOptions.IsCancelled=[&]() { return Progress.ShouldCancel(); };
		Result=Apply(Api,Plan,Changes,ApplyOptions);Report(Result.Issues,!bAutomated);
		if (!bAutomated && Result.Api)
		{
			FText Message=Result.bNoChanges ? NSLOCTEXT("VaRestOpenApi","UpToDate","Up to date") : Result.bUndoHistoryCleared ? NSLOCTEXT("VaRestOpenApi","ReimportDoneUndo","OpenAPI reimported. Undo history was cleared.") : NSLOCTEXT("VaRestOpenApi","ReimportDone","OpenAPI reimported.");
			if (Changes.Merge.Deferred>0) Message=FText::Format(NSLOCTEXT("VaRestOpenApi","DeferredToast","{0} changes need review"),FText::AsNumber(Changes.Merge.Deferred));
			FNotificationInfo Info(Message);Info.ExpireDuration=6;FSlateNotificationManager::Get().AddNotification(Info);
		}
		return Result;
	}
}
