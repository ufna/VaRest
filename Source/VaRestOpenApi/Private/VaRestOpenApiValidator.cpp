// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiValidator.h"
#include "VaRestOpenApiApplyPrivate.h"
#include "VaRestHttpSettings.h"
#include "VaRestUrl.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformFile.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/Paths.h"

bool UVaRestOpenApiValidator::CanValidateAsset_Implementation(const FAssetData&,UObject* Object,FDataValidationContext&) const
{
	const auto* Api=Cast<UVaRestApi>(Object);return Api && Api->ImportState.Importer==TEXT("VaRestOpenApi");
}
EDataValidationResult UVaRestOpenApiValidator::ValidateLoadedAsset_Implementation(const FAssetData& AssetData,UObject* Object,FDataValidationContext&)
{
	using namespace VaRest::OpenApi;auto* Api=Cast<UVaRestApi>(Object);if (!Api) return EDataValidationResult::NotValidated;
	const auto State=DecodeState(Api->ImportState);
	if (!State.IsSet()) { AssetFails(Api,NSLOCTEXT("VaRestOpenApi","InvalidManifest","OA-REIMPORT-STATE: The OpenAPI import manifest is unreadable."));return EDataValidationResult::Invalid; }
	bool Failed=false;int32 Removed=0;
	FStringSet Suppressed;for (const auto& Id:State.GetValue()[TEXT("suppressions")].Elements()) Suppressed.Add(Id.AsString());
	for (const auto& Item:FVaRestOpenApiMerge::Capture(Api,State.GetValue()))
	{
		if (!Item.bPresent && Suppressed.Contains(Item.Id)) continue;
		Removed+=Item.bRemovedUpstream;
		if (!Item.bPresent && (Equal(Item.Kind,TEXT("struct")) || Equal(Item.Kind,TEXT("enum"))))
		{
			AssetFails(Api,FText::FromString(TEXT("OA-APPLY-TYPE: A generated type is missing: ")+Item.Target));Failed=true;
		}
	}
	if (Removed>0) AssetWarning(Api,FText::Format(NSLOCTEXT("VaRestOpenApi","RemovedWarning","OA-REIMPORT-REMOVED: {0} imported items were removed upstream and remain available for graph compatibility."),FText::AsNumber(Removed)));
	const int32 Deferred=State.GetValue()[TEXT("deferred")].Num();
	if (Deferred>0) AssetWarning(Api,FText::Format(NSLOCTEXT("VaRestOpenApi","DeferredWarning","OA-REIMPORT-BREAKINGDEFERRED: {0} upstream changes need review."),FText::AsNumber(Deferred)));
	for (const auto& Environment:Api->Environments) if (!VaRest::Http::IsAbsoluteHttpUrl(Environment.BaseUrl)) AssetWarning(Api,FText::FromString(TEXT("OA-ENV-RELATIVE: Environment ")+Environment.Name.ToString()+TEXT(" needs an absolute base URL before requests can run.")));
	FVaRestApiValidationContext Native;Api->Validate(Native);
	for (const auto& Issue:Native.Issues)
	{
		const FText Message=FText::FromString(Issue.Code.ToString()+TEXT(": ")+Issue.PropertyPath+TEXT(": ")+Issue.Message.ToString());
		if (Issue.bWarning) AssetWarning(Api,Message);else { AssetFails(Api,Message);Failed=true; }
	}
	if (!GetDefault<UVaRestHttpSettings>()->Apis.Contains(TSoftObjectPtr<UVaRestApi>(Api))) AssetWarning(Api,NSLOCTEXT("VaRestOpenApi","MissingPreload","OA-APPLY-SETTINGS: Add this API to VaRest HTTP settings / Apis so its JSON rules load at startup and are cooked."));
	if (Api->Naming!=EVaRestJsonNaming::AsIs) AssetMessage(AssetData,EMessageSeverity::Info,NSLOCTEXT("VaRestOpenApi","NamingInfo","OA-NAME-POLICY: Imported JSON rules still win. The selected naming policy applies to user-added fields."));
	const auto Source=State.GetValue()[TEXT("source")];
	if (Equal(Source[TEXT("kind")].AsString(),TEXT("file")))
	{
		const FString Filename=Source[TEXT("location")].AsString(),Extension=FPaths::GetExtension(Filename).ToLower();
		if ((Extension==TEXT("json") || Extension==TEXT("yaml") || Extension==TEXT("yml")) && !FPlatformFileManager::Get().GetPlatformFile().FileExists(*Filename))
			AssetMessage(AssetData,EMessageSeverity::Info,NSLOCTEXT("VaRestOpenApi","MissingSource","OA-DOC-READ: The OpenAPI source file is missing. Change source before reimporting."));
	}
	if (Failed) return EDataValidationResult::Invalid;AssetPasses(Api);return GetValidationResult();
}
