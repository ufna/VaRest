// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSioNodeSpawner.h"
#include "K2Node_VaRestSioAsync.h"
#include "VaRestApi.h"
#include "VaRestAssetEntryActionRegistry.h"
#include "BlueprintNodeTemplateCache.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/UObjectIterator.h"

UVaRestSioNodeSpawner* UVaRestSioNodeSpawner::Create(UVaRestApi* InApi, FName InEntry, EVaRestSioCall InOperation,
	const FString& InNamespace, const FString& InEvent, bool bInDeclaration, const FString& Description)
{
	auto* Result = NewObject<UVaRestSioNodeSpawner>();
	Result->NodeClass = (InOperation == EVaRestSioCall::Get || InOperation == EVaRestSioCall::Emit || InOperation == EVaRestSioCall::Reply)
		? static_cast<UClass*>(UK2Node_VaRestSioCall::StaticClass()) : static_cast<UClass*>(UK2Node_VaRestSioAsync::StaticClass());
	Result->Api = InApi; Result->Entry = InEntry; Result->Namespace = InNamespace; Result->Event = InEvent; Result->Operation = InOperation; Result->bDeclaration = bInDeclaration;
	const TWeakObjectPtr<UVaRestApi> Weak(InApi); const bool bHasAsset = InApi != nullptr;
	Result->CustomizeNodeDelegate = FCustomizeNodeDelegate::CreateLambda([Weak, bHasAsset, InEntry, InOperation, InNamespace, InEvent, bInDeclaration](UEdGraphNode* Node, bool)
	{
		const auto Configure = [&](auto* Call)
		{ Call->Api = Weak.Get(); Call->Entry = InEntry; Call->Namespace = InNamespace; Call->Event = InEvent; Call->Operation = InOperation; Call->bAssetEntry = bHasAsset; Call->bDeclaration = bInDeclaration; };
		if (auto* Call = Cast<UK2Node_VaRestSioCall>(Node)) Configure(Call);
		else if (auto* Async = Cast<UK2Node_VaRestSioAsync>(Node)) Configure(Async);
	});
	FString Title;
	switch (InOperation)
	{
	case EVaRestSioCall::Get: Title = InApi ? TEXT("Get Socket.IO ") + InEntry.ToString() : FString(TEXT("Get Socket.IO")); break;
	case EVaRestSioCall::Emit: Title = bInDeclaration ? TEXT("Emit ") + InEvent : FString(TEXT("Emit Socket.IO Event")); break;
	case EVaRestSioCall::Listen: case EVaRestSioCall::ListenWithAck: Title = TEXT("On Socket.IO Event ") + InEvent; break;
	case EVaRestSioCall::EmitWithAck: Title = TEXT("Emit With Ack ") + InEvent; break;
	case EVaRestSioCall::Reply: Title = TEXT("Ack Reply"); break;
	}
	Result->DefaultMenuSignature.MenuName = FText::FromString(Title); FString Category = TEXT("VaRest|Socket.IO");
	if (InApi)
	{
		Category = TEXT("VaRest|") + InApi->GetName() + TEXT("|") + InEntry.ToString();
		const auto* Endpoint = VaRestEditor::SioNodes::ResolveEntry(InApi, InEntry);
		if (!InNamespace.IsEmpty() && (!Endpoint || !InNamespace.Equals(Endpoint->DefaultNamespace, ESearchCase::CaseSensitive))) Category += TEXT("|") + InNamespace;
	}
	Result->DefaultMenuSignature.Category = FText::FromString(Category); Result->DefaultMenuSignature.Tooltip = FText::FromString(Description); return Result;
}
FBlueprintNodeSignature UVaRestSioNodeSpawner::GetSpawnerSignature() const
{
	const auto ExactKey = [](const FString& Text) { const FTCHARToUTF8 Utf8(*Text, Text.Len()); return BytesToHex(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()); };
	FBlueprintNodeSignature Result(NodeClass); Result.AddNamedValue(TEXT("Protocol"), TEXT("SocketIO")); Result.AddNamedValue(TEXT("Asset"), Api ? Api->GetPathName() : FString());
	Result.AddNamedValue(TEXT("Entry"), Entry.ToString()); Result.AddNamedValue(TEXT("Namespace"), ExactKey(Namespace)); Result.AddNamedValue(TEXT("Event"), ExactKey(Event));
	Result.AddNamedValue(TEXT("Operation"), LexToString(static_cast<uint8>(Operation))); Result.AddNamedValue(TEXT("Declaration"), bDeclaration ? TEXT("1") : TEXT("0")); return Result;
}

namespace VaRestEditor
{
	class FSioEndpointProvider final : public IVaRestAssetEntryActionProvider
	{
	public:
		virtual FName GetProtocolTag() const override { return TEXT("SocketIO"); }
		virtual void GatherActions(UVaRestApi& Api, UVaRestApiSocketConfig* Entry, TArray<UBlueprintNodeSpawner*>& Out) const override
		{
			const auto* Config = Cast<UVaRestSioEndpoint>(Entry); if (!Config) return;
			Out.Add(UVaRestSioNodeSpawner::Create(&Api, Config->Name, EVaRestSioCall::Get, {}, {}, false, Config->Description));
			for (const auto& Namespace : Config->Namespaces) for (const auto& Event : Namespace.Events)
			{
				EVaRestSioCall Kind;
				switch (Event.Direction)
				{
				case EVaRestSioEventDirection::In: Kind = EVaRestSioCall::Listen; break;
				case EVaRestSioEventDirection::InWithAck: Kind = EVaRestSioCall::ListenWithAck; break;
				case EVaRestSioEventDirection::Out: Kind = EVaRestSioCall::Emit; break;
				case EVaRestSioEventDirection::OutWithAck: Kind = EVaRestSioCall::EmitWithAck; break;
				default: continue;
				}
				Out.Add(UVaRestSioNodeSpawner::Create(&Api, Config->Name, Kind, Namespace.Name, Event.Event, true, Event.Description));
			}
		}
		virtual void RefreshLoadedNodes(UVaRestApi& Api) const override
		{
			TArray<TStrongObjectPtr<UK2Node>> Nodes;
			for (TObjectIterator<UK2Node_VaRestSioCall> It; It; ++It) if (It->Api == &Api && !It->IsTemplate() && It->GetGraph() && !FBlueprintNodeTemplateCache::IsTemplateOuter(It->GetGraph())) Nodes.Emplace(*It);
			for (TObjectIterator<UK2Node_VaRestSioAsync> It; It; ++It) if (It->Api == &Api && !It->IsTemplate() && It->GetGraph() && !FBlueprintNodeTemplateCache::IsTemplateOuter(It->GetGraph())) Nodes.Emplace(*It);
			TSet<UBlueprint*> Blueprints;
			for (const auto& Node : Nodes)
			{
				if (!IsValid(Node.Get()) || !Node->GetGraph()) continue;
				Node->ReconstructNode(); if (auto* Blueprint = FBlueprintEditorUtils::FindBlueprintForNode(Node.Get())) Blueprints.Add(Blueprint);
			}
			for (auto* Blueprint : Blueprints) FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		}
	};
	FDelegateHandle RegisterSioEndpointProvider()
	{ FString Error; return FVaRestAssetEntryActionRegistry::RegisterProvider(MakeShared<FSioEndpointProvider>(), Error); }
}
