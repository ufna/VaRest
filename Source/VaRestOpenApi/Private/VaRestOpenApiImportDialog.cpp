// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiImportDialog.h"
#include "VaRestOpenApiImportOptions.h"
#include "VaRestOpenApiAnalysis.h"
#include "Framework/Application/SlateApplication.h"
#include "IDetailsView.h"
#include "PropertyEditorModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/ScopeExit.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STreeView.h"

namespace VaRest::OpenApi
{
namespace
{
struct FOperationRow
{
	FString Text,Identity;TArray<TSharedPtr<FOperationRow>> Children;bool bTag=false;
};
using FRow=TSharedPtr<FOperationRow>;
}
bool ShowImportOptions(const FVaRestOpenApiAnalysis& Analysis,UVaRestOpenApiImportOptions& Options,bool bDefaultIncludeNew)
{
	check(IsInGameThread());if (!FSlateApplication::IsInitialized()) return false;
	bool Accepted=false;FString Search;FStringSet Selected;TArray<FRow> AllTags,Visible;
	TStringMap<int32> OperationCounts;for (const auto& Operation:Analysis.Model.Operations) if (!Operation.OperationId.IsEmpty()) ++OperationCounts.FindOrAdd(Operation.OperationId);
	for (const auto& Operation:Analysis.Model.Operations)
	{
		const FString Route=Operation.Method+TEXT(" ")+Operation.Path;
		const FString Identity=Operation.OperationId.IsEmpty() || OperationCounts.FindRef(Operation.OperationId)>1 ? Route : Operation.OperationId;
		if (Options.bImportAllOperations || Options.SelectedOperations.ContainsByPredicate([&](const FString& Id) { return Equal(Id,Identity) || Equal(Id,Route) || (!Operation.OperationId.IsEmpty() && Equal(Id,Operation.OperationId)); })) Selected.Add(Identity);
		TArray<FString> Tags=Operation.Tags;if (Tags.IsEmpty()) Tags.Add(TEXT("Untagged"));
		for (const auto& Tag:Tags)
		{
			auto* Existing=AllTags.FindByPredicate([&](const auto& Row) { return Equal(Row->Text,Tag); });FRow Parent;
			if (Existing) Parent=*Existing;else { Parent=MakeShared<FOperationRow>();Parent->Text=Tag;Parent->bTag=true;AllTags.Add(Parent); }
			auto Row=MakeShared<FOperationRow>();Row->Identity=Identity;Row->Text=Route+(Operation.OperationId.IsEmpty() ? FString() : TEXT(" — ")+Operation.OperationId);Parent->Children.Add(Row);
		}
	}
	Visible=AllTags;
	bool ExplicitIncludeNew=!bDefaultIncludeNew;
	const auto SelectionChanged=[&]() { if (!ExplicitIncludeNew) Options.bIncludeNewOperations=Selected.Num()==Analysis.Model.Operations.Num(); };
	SelectionChanged();
	FDetailsViewArgs Args;Args.bAllowSearch=true;Args.bUpdatesFromSelection=false;Args.bHideSelectionTip=true;
	auto Details=FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor")).CreateDetailView(Args);Details->SetObject(&Options);
	const auto Changed=Details->OnFinishedChangingProperties().AddLambda([&](const FPropertyChangedEvent& Event) { if (Event.GetPropertyName()==GET_MEMBER_NAME_CHECKED(UVaRestOpenApiImportOptions,bIncludeNewOperations)) ExplicitIncludeNew=true; });
	ON_SCOPE_EXIT { Details->OnFinishedChangingProperties().Remove(Changed); };
	TSharedPtr<STreeView<FRow>> Tree;TSharedPtr<SWindow> Window;
	const auto Refilter=[&]()
	{
		Visible.Reset();for (const auto& Tag:AllTags)
		{
			auto Copy=MakeShared<FOperationRow>(*Tag);Copy->Children.Reset();
			for (const auto& Row:Tag->Children) if (Search.IsEmpty() || Tag->Text.Contains(Search) || Row->Text.Contains(Search)) Copy->Children.Add(Row);
			if (!Copy->Children.IsEmpty()) Visible.Add(Copy);
		}
		if (Tree) { Tree->RequestTreeRefresh();for (const auto& Tag:Visible) Tree->SetItemExpansion(Tag,true); }
	};
	SAssignNew(Window,SWindow).Title(NSLOCTEXT("VaRestOpenApi","ImportTitle","Import OpenAPI")).ClientSize(FVector2D(1000,720)).SupportsMaximize(true).SupportsMinimize(false)
	[
		SNew(SVerticalBox)
		+SVerticalBox::Slot().AutoHeight().Padding(12)[SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("%s %s · OpenAPI %s · %d operations · %d schemas"),*Analysis.Model.Title,*Analysis.Model.InfoVersion,*Analysis.Model.Version,Analysis.Model.Operations.Num(),Analysis.Model.NamedSchemas.Num())))]
		+SVerticalBox::Slot().FillHeight(1).Padding(8)
		[
			SNew(SSplitter)
			+SSplitter::Slot().Value(.42f)[Details]
			+SSplitter::Slot().Value(.58f)
			[
				SNew(SVerticalBox)
				+SVerticalBox::Slot().AutoHeight()[SNew(SSearchBox).OnTextChanged_Lambda([&](const FText& Text) { Search=Text.ToString();Refilter(); })]
				+SVerticalBox::Slot().AutoHeight().Padding(4)
				[
					SNew(SHorizontalBox)
					+SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(NSLOCTEXT("VaRestOpenApi","SelectAll","Select all")).OnClicked_Lambda([&]() { for (const auto& Tag:AllTags) for (const auto& Row:Tag->Children) Selected.Add(Row->Identity);SelectionChanged();return FReply::Handled(); })]
					+SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(NSLOCTEXT("VaRestOpenApi","SelectNone","Select none")).OnClicked_Lambda([&]() { Selected.Reset();SelectionChanged();return FReply::Handled(); })]
					+SHorizontalBox::Slot().FillWidth(1).Padding(8,0)[SNew(STextBlock).Text_Lambda([&]() { return FText::FromString(FString::Printf(TEXT("%d selected"),Selected.Num())); })]
				]
				+SVerticalBox::Slot().FillHeight(1)
				[
					SAssignNew(Tree,STreeView<FRow>).TreeItemsSource(&Visible).SelectionMode(ESelectionMode::None)
					.OnGetChildren_Lambda([](FRow Row,TArray<FRow>& Out) { Out=Row->Children; })
					.OnGenerateRow_Lambda([&](FRow Row,const TSharedRef<STableViewBase>& Owner)->TSharedRef<ITableRow>
					{
						return SNew(STableRow<FRow>,Owner)
						[
							SNew(SCheckBox)
							.IsChecked_Lambda([&,Row]() { if (!Row->bTag) return Selected.Contains(Row->Identity) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;int32 Count=0;for (const auto& Child:Row->Children) Count+=Selected.Contains(Child->Identity);return Count==0 ? ECheckBoxState::Unchecked : Count==Row->Children.Num() ? ECheckBoxState::Checked : ECheckBoxState::Undetermined; })
							.OnCheckStateChanged_Lambda([&,Row](ECheckBoxState State) { const auto Set=[&](const FString& Id) { if (State==ECheckBoxState::Checked) Selected.Add(Id);else Selected.Remove(Id); };if (Row->bTag) for (const auto& Child:Row->Children) Set(Child->Identity);else Set(Row->Identity);SelectionChanged(); })
							[SNew(STextBlock).Text(FText::FromString(Row->Text))]
						];
					})
				]
			]
		]
		+SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(12)
		[
			SNew(SHorizontalBox)
			+SHorizontalBox::Slot().AutoWidth().Padding(4)[SNew(SButton).Text(NSLOCTEXT("VaRestOpenApi","Cancel","Cancel")).OnClicked_Lambda([&]() { Window->RequestDestroyWindow();return FReply::Handled(); })]
			+SHorizontalBox::Slot().AutoWidth().Padding(4)[SNew(SButton).Text(NSLOCTEXT("VaRestOpenApi","Apply","Import")).OnClicked_Lambda([&]() { Accepted=true;Window->RequestDestroyWindow();return FReply::Handled(); })]
		]
	];
	Refilter();FSlateApplication::Get().AddModalWindow(Window.ToSharedRef(),nullptr);
	if (Accepted)
	{
		Options.bImportAllOperations=Selected.Num()==Analysis.Model.Operations.Num();Options.SelectedOperations.Reset();
		// Source order is stable; hash-set iteration must not become serialized selection order.
		if (!Options.bImportAllOperations) for (const auto& Operation:Analysis.Model.Operations) { const FString Id=Operation.OperationId.IsEmpty() || OperationCounts.FindRef(Operation.OperationId)>1 ? Operation.Method+TEXT(" ")+Operation.Path : Operation.OperationId;if (Selected.Contains(Id)) Options.SelectedOperations.Add(Id); }
	}
	Details->SetObject(nullptr);return Accepted;
}
}
