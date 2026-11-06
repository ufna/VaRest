// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestEndpointNodeSpawner.h"
#include "K2Node_VaRestCallEndpoint.h"
#include "VaRestApi.h"
#include "VaRestAssetEntryActionRegistry.h"
#include "BlueprintNodeTemplateCache.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/UObjectIterator.h"

UVaRestEndpointNodeSpawner* UVaRestEndpointNodeSpawner::CreateEndpoint(UVaRestApi& InApi,const FVaRestApiEndpoint& Endpoint)
{
	auto* Result=NewObject<UVaRestEndpointNodeSpawner>(); Result->NodeClass=UK2Node_VaRestCallEndpoint::StaticClass(); Result->Api=&InApi; Result->EndpointId=Endpoint.Id;
	const TWeakObjectPtr<UVaRestApi> Weak(&InApi); const FGuid Id=Endpoint.Id; const FName Name=Endpoint.Name;
	Result->CustomizeNodeDelegate=FCustomizeNodeDelegate::CreateLambda([Weak,Id,Name](UEdGraphNode* Node,bool) {
		if (auto* Call=Cast<UK2Node_VaRestCallEndpoint>(Node)) { Call->Api=Weak.Get(); Call->EndpointId=Id; Call->EndpointName=Name; }
	});
	Result->DefaultMenuSignature.MenuName=FText::FromName(Endpoint.Name);
	Result->DefaultMenuSignature.Category=FText::FromString(FString(TEXT("VaRest|"))+InApi.GetName()+(Endpoint.Category.IsEmpty() ? TEXT("") : FString(TEXT("|"))+Endpoint.Category));
	Result->DefaultMenuSignature.Tooltip=FText::FromString(Endpoint.Description+(Endpoint.bDeprecated ? FString(TEXT("\nDeprecated: "))+Endpoint.DeprecationMessage : TEXT("")));
	return Result;
}
FBlueprintNodeSignature UVaRestEndpointNodeSpawner::GetSpawnerSignature() const
{
	FBlueprintNodeSignature Result(NodeClass); Result.AddNamedValue(TEXT("Asset"),Api ? Api->GetPathName() : FString());
	Result.AddNamedValue(TEXT("Entry"),EndpointId.ToString(EGuidFormats::Digits)); Result.AddNamedValue(TEXT("Protocol"),TEXT("Http")); return Result;
}
namespace VaRestEditor
{
class FHttpEndpointProvider final : public IVaRestAssetEntryActionProvider
{
public:
	virtual FName GetProtocolTag() const override { return TEXT("Http"); }
	virtual void GatherActions(UVaRestApi& Api,UVaRestApiSocketConfig* Entry,TArray<UBlueprintNodeSpawner*>& Out) const override
	{
		if (Entry) return;
		for (const auto& Endpoint:Api.Endpoints) Out.Add(UVaRestEndpointNodeSpawner::CreateEndpoint(Api,Endpoint));
	}
	virtual void RefreshLoadedNodes(UVaRestApi& Api) const override
	{
		TArray<TStrongObjectPtr<UK2Node_VaRestCallEndpoint>> Nodes;
		for (TObjectIterator<UK2Node_VaRestCallEndpoint> It;It;++It)
			if (It->Api==&Api && !It->IsTemplate() && It->GetGraph() && !FBlueprintNodeTemplateCache::IsTemplateOuter(It->GetGraph())) Nodes.Emplace(*It);
		TSet<UBlueprint*> Blueprints;
		for (const auto& Node:Nodes)
		{
			if (!IsValid(Node.Get()) || !Node->GetGraph()) continue;
			if (const auto* Endpoint=Node->ResolveEndpoint()) { Node->EndpointId=Endpoint->Id; Node->EndpointName=Endpoint->Name; }
			Node->ReconstructNode(); if (auto* Blueprint=FBlueprintEditorUtils::FindBlueprintForNode(Node.Get())) Blueprints.Add(Blueprint);
		}
		for (auto* Blueprint:Blueprints) FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}
};
FDelegateHandle RegisterHttpEndpointProvider()
{
	FString Error; return FVaRestAssetEntryActionRegistry::RegisterProvider(MakeShared<FHttpEndpointProvider>(),Error);
}
}
