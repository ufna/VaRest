// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSocketNodeSpawner.h"
#include "K2Node_VaRestSocketAsync.h"
#include "VaRestApi.h"
#include "VaRestAssetEntryActionRegistry.h"
#include "BlueprintNodeTemplateCache.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/UObjectIterator.h"

UVaRestSocketNodeSpawner* UVaRestSocketNodeSpawner::Create(UVaRestApi* InApi, FName InEntry, EVaRestSocketCall InOperation,
	const FString& InChannel, const FString& InRoute, bool bInDeclaration, const FString& Description)
{
	auto* Result = NewObject<UVaRestSocketNodeSpawner>();
	Result->NodeClass = (InOperation == EVaRestSocketCall::Get || InOperation == EVaRestSocketCall::Emit || InOperation == EVaRestSocketCall::Reply)
		? static_cast<UClass*>(UK2Node_VaRestSocketCall::StaticClass()) : static_cast<UClass*>(UK2Node_VaRestSocketAsync::StaticClass());
	Result->Api = InApi; Result->Entry = InEntry; Result->Channel = InChannel; Result->Route = InRoute; Result->Operation = InOperation; Result->bDeclaration = bInDeclaration;
	const TWeakObjectPtr<UVaRestApi> Weak(InApi);
	const bool bHasAsset = InApi != nullptr;
	Result->CustomizeNodeDelegate = FCustomizeNodeDelegate::CreateLambda([Weak, bHasAsset, InEntry, InOperation, InChannel, InRoute, bInDeclaration](UEdGraphNode* Node, bool) {
		const auto Configure = [&](auto* Call)
		{
			Call->Api = Weak.Get(); Call->Entry = InEntry; Call->Operation = InOperation; Call->Channel = InChannel; Call->Route = InRoute;
			Call->bAssetEntry = bHasAsset; Call->bDeclaration = bInDeclaration;
		};
		if (auto* Call = Cast<UK2Node_VaRestSocketCall>(Node)) Configure(Call);
		else if (auto* Async = Cast<UK2Node_VaRestSocketAsync>(Node)) Configure(Async);
	});
	FString Title;
	switch (InOperation)
	{
	case EVaRestSocketCall::Get: Title = InApi ? TEXT("Get Socket ") + InEntry.ToString() : FString(TEXT("Get Socket")); break;
	case EVaRestSocketCall::Emit: Title = bInDeclaration ? TEXT("Emit ") + InRoute : FString(TEXT("Emit Socket Message")); break;
	case EVaRestSocketCall::Listen: Title = TEXT("On Socket Message ") + InRoute; break;
	case EVaRestSocketCall::ListenRequest: Title = TEXT("On Socket Request ") + InRoute; break;
	case EVaRestSocketCall::Request: Title = TEXT("Socket JSON Request ") + InRoute; break;
	case EVaRestSocketCall::Reply: Title = bInDeclaration ? TEXT("Reply ") + InRoute : FString(TEXT("Reply to Socket Request")); break;
	}
	Result->DefaultMenuSignature.MenuName = FText::FromString(Title);
	FString Category = TEXT("VaRest|Socket");
	if (InApi) { Category = TEXT("VaRest|") + InApi->GetName() + TEXT("|") + InEntry.ToString(); if (!InChannel.IsEmpty()) Category += TEXT("|") + InChannel; }
	Result->DefaultMenuSignature.Category = FText::FromString(Category);
	Result->DefaultMenuSignature.Tooltip = FText::FromString(Description);
	return Result;
}
FBlueprintNodeSignature UVaRestSocketNodeSpawner::GetSpawnerSignature() const
{
	const auto ExactKey = [](const FString& Text) { const FTCHARToUTF8 Utf8(*Text, Text.Len()); return BytesToHex(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()); };
	FBlueprintNodeSignature Result(NodeClass);
	Result.AddNamedValue(TEXT("Protocol"), TEXT("WebSocket")); Result.AddNamedValue(TEXT("Asset"), Api ? Api->GetPathName() : FString());
	Result.AddNamedValue(TEXT("Entry"), Entry.ToString()); Result.AddNamedValue(TEXT("Channel"), ExactKey(Channel)); Result.AddNamedValue(TEXT("Route"), ExactKey(Route));
	Result.AddNamedValue(TEXT("Operation"), LexToString(static_cast<uint8>(Operation))); Result.AddNamedValue(TEXT("Declaration"), bDeclaration ? TEXT("1") : TEXT("0"));
	return Result;
}
namespace VaRestEditor
{
class FSocketEndpointProvider final : public IVaRestAssetEntryActionProvider
{
public:
	virtual FName GetProtocolTag() const override { return TEXT("WebSocket"); }
	virtual void GatherActions(UVaRestApi& Api, UVaRestApiSocketConfig* Entry, TArray<UBlueprintNodeSpawner*>& Out) const override
	{
		const auto* Config = Cast<UVaRestSocketEndpoint>(Entry); if (!Config) return;
		Out.Add(UVaRestSocketNodeSpawner::Create(&Api, Config->Name, EVaRestSocketCall::Get, {}, {}, false, Config->Description));
		for (const auto& Message : Config->Messages)
		{
			EVaRestSocketCall Kind;
			switch (Message.Direction)
			{
			case EVaRestSocketMessageDirection::In: Kind = EVaRestSocketCall::Listen; break;
			case EVaRestSocketMessageDirection::Out: Kind = EVaRestSocketCall::Emit; break;
			case EVaRestSocketMessageDirection::Request: Kind = EVaRestSocketCall::Request; break;
			case EVaRestSocketMessageDirection::InRequest: Kind = EVaRestSocketCall::ListenRequest; break;
			default: continue;
			}
			Out.Add(UVaRestSocketNodeSpawner::Create(&Api, Config->Name, Kind, Message.Channel, Message.Route, true, Message.Description));
			if (Message.Direction == EVaRestSocketMessageDirection::InRequest)
				Out.Add(UVaRestSocketNodeSpawner::Create(&Api, Config->Name, EVaRestSocketCall::Reply, Message.Channel, Message.Route, true, Message.Description));
		}
	}
	virtual void RefreshLoadedNodes(UVaRestApi& Api) const override
	{
		TArray<TStrongObjectPtr<UK2Node>> Nodes;
		for (TObjectIterator<UK2Node_VaRestSocketCall> It; It; ++It)
			if (It->Api == &Api && !It->IsTemplate() && It->GetGraph() && !FBlueprintNodeTemplateCache::IsTemplateOuter(It->GetGraph())) Nodes.Emplace(*It);
		for (TObjectIterator<UK2Node_VaRestSocketAsync> It; It; ++It)
			if (It->Api == &Api && !It->IsTemplate() && It->GetGraph() && !FBlueprintNodeTemplateCache::IsTemplateOuter(It->GetGraph())) Nodes.Emplace(*It);
		TSet<UBlueprint*> Blueprints;
		for (const auto& Node : Nodes)
		{
			if (!IsValid(Node.Get()) || !Node->GetGraph()) continue;
			Node->ReconstructNode(); if (auto* Blueprint = FBlueprintEditorUtils::FindBlueprintForNode(Node.Get())) Blueprints.Add(Blueprint);
		}
		for (auto* Blueprint : Blueprints) FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}
};
FDelegateHandle RegisterSocketEndpointProvider()
{
	FString Error; return FVaRestAssetEntryActionRegistry::RegisterProvider(MakeShared<FSocketEndpointProvider>(), Error);
}
}
