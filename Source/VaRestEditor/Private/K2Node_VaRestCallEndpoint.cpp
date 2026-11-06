// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "K2Node_VaRestCallEndpoint.h"
#include "VaRestApi.h"
#include "VaRestAssetEntryActionRegistry.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BlueprintActionDatabaseRegistrar.h"
#include "BlueprintNodeSpawner.h"
#include "Modules/ModuleManager.h"

const FVaRestApiEndpoint* UK2Node_VaRestCallEndpoint::ResolveEndpoint() const { return Api ? (EndpointId.IsValid() ? Api->FindEndpoint(EndpointId) : Api->FindEndpoint(EndpointName)) : nullptr; }
FVaRestTypeRef UK2Node_VaRestCallEndpoint::GetResponseType() const { const auto* Endpoint=ResolveEndpoint();return Endpoint ? Endpoint->ResponseType : FVaRestTypeRef(); }
FVaRestTypeRef UK2Node_VaRestCallEndpoint::GetErrorType() const { const auto* Endpoint=ResolveEndpoint();return Endpoint ? Endpoint->ErrorType : FVaRestTypeRef(); }
bool UK2Node_VaRestCallEndpoint::AllowsQueue() const { const auto* Endpoint=ResolveEndpoint();return Endpoint && Endpoint->bQueueWhenOffline; }
TArray<FString> UK2Node_VaRestCallEndpoint::GetEndpointNames() const
{
	TArray<FString> Names; if (Api) for (const auto& Endpoint:Api->Endpoints) Names.Add(Endpoint.Name.ToString()); Names.Sort(); return Names;
}
FText UK2Node_VaRestCallEndpoint::GetNodeTitle(ENodeTitleType::Type) const
{
	const auto* Endpoint=ResolveEndpoint(); return Endpoint ? FText::FromString(Endpoint->Name.ToString()) : NSLOCTEXT("VaRestRequestNode","CallEndpoint","Call Endpoint");
}
FText UK2Node_VaRestCallEndpoint::GetTooltipText() const
{
	const auto* Endpoint=ResolveEndpoint(); return Endpoint ? FText::FromString(Endpoint->Description) : NSLOCTEXT("VaRestRequestNode","SelectEndpoint","Select an API and endpoint in Details. Missing endpoints retain their existing pins.");
}
void UK2Node_VaRestCallEndpoint::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
	if (Event.GetPropertyName()==GET_MEMBER_NAME_CHECKED(UK2Node_VaRestCallEndpoint,Api)) { EndpointId.Invalidate(); EndpointName=NAME_None; }
	else if (Event.GetPropertyName()==GET_MEMBER_NAME_CHECKED(UK2Node_VaRestCallEndpoint,EndpointName) && Api)
		if (const auto* Endpoint=Api->FindEndpoint(EndpointName)) EndpointId=Endpoint->Id;
	Super::PostEditChangeProperty(Event);
}
void UK2Node_VaRestCallEndpoint::GetMenuActions(FBlueprintActionDatabaseRegistrar& Registrar) const
{
	if (Registrar.IsOpenForRegistration(GetClass())) Registrar.AddBlueprintAction(GetClass(),UBlueprintNodeSpawner::Create(GetClass()));
	const auto Register=[&](UVaRestApi* Asset) {
		if (!Asset || !Registrar.IsOpenForRegistration(Asset)) return;
		for (const auto& Spawner:FVaRestAssetEntryActionRegistry::GatherActions(Asset)) Registrar.AddBlueprintAction(Asset,Spawner.Get());
	};
	if (const auto* Filter=Registrar.GetActionKeyFilter()) { Register(const_cast<UVaRestApi*>(Cast<UVaRestApi>(Filter))); return; }
	TArray<FAssetData> Assets;
	FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssetsByClass(UVaRestApi::StaticClass()->GetClassPathName(),Assets,true);
	for (const auto& Asset:Assets) Register(Cast<UVaRestApi>(Asset.GetAsset()));
}
