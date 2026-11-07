// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiImportDialog.h"
#include "VaRestOpenApiReimport.h"
#include "VaRestOpenApiAnalysis.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STreeView.h"

namespace VaRest::OpenApi
{
namespace
{
struct FChangeRow { FString Text;int32 Index=INDEX_NONE;TArray<TSharedPtr<FChangeRow>> Children; };
using FReimportRow=TSharedPtr<FChangeRow>;
bool Actionable(const FVaRestOpenApiPropertyChange& Row)
{
	return Row.Change!=EVaRestOpenApiChange::KeptUserEdit && (Row.Change!=EVaRestOpenApiChange::Removed || !Row.Mine.Equals(Row.Theirs));
}
FString ValuePreview(const FVaRestJson& Value) { return (Value.IsString() ? Value.AsString() : Value.ToString()).Left(512); }
}
bool ShowReimportPreview(FVaRestOpenApiReimportPreview& Preview,const FVaRestOpenApiAnalysis&,UVaRestOpenApiImportOptions&,bool& bChangeOptions)
{
	bChangeOptions=false;if (!FSlateApplication::IsInitialized()) return false;
	FString Diagnostics;
	for (const auto& Issue:Preview.Issues)
	{
		const TCHAR* Severity=Issue.Severity==EVaRestOpenApiSeverity::Error ? TEXT("Error") : Issue.Severity==EVaRestOpenApiSeverity::Warning ? TEXT("Warning") : TEXT("Info");
		Diagnostics+=FString(Severity)+TEXT(" · ")+Issue.Code+TEXT(" · ")+Issue.Message+TEXT("\n");
		if (!Issue.AssetPath.IsEmpty()) Diagnostics+=Issue.AssetPath+TEXT("\n");
	}
	bool Accepted=false,ShowSuppressed=false;FString Search;TArray<FReimportRow> Roots;TSharedPtr<STreeView<FReimportRow>> Tree;TSharedPtr<SWindow> Window;
	const auto Label=[&](const FVaRestOpenApiPropertyChange& Change)
	{
		static constexpr const TCHAR* Kinds[]={TEXT("Added"),TEXT("Changed"),TEXT("Removed"),TEXT("Restore"),TEXT("Conflict"),TEXT("Kept your edits"),TEXT("Suppressed")};
		return FString(Kinds[static_cast<uint8>(Change.Change)])+TEXT(" · ")+Change.Kind+(Change.Property.IsEmpty() ? FString() : TEXT(" · ")+Change.Property)+(Change.bBreaking ? TEXT(" · BREAKING") : TEXT(""));
	};
	const auto Rebuild=[&]()
	{
		Roots.Reset();TStringMap<FReimportRow> Parents;
		for (int32 Index=0;Index<Preview.Merge.Changes.Num();++Index)
		{
			const auto& Change=Preview.Merge.Changes[Index];if (!ShowSuppressed && Change.Change==EVaRestOpenApiChange::Suppressed) continue;
			const FString Text=Label(Change);if (!Search.IsEmpty() && !Text.Contains(Search) && !Change.Id.Contains(Search)) continue;
			const FString Group=Change.Owner.IsEmpty() ? Change.Id : Change.Owner;FReimportRow Parent;
			if (auto* Found=Parents.Find(Group)) Parent=*Found;
			else
			{
				Parent=MakeShared<FChangeRow>();Parent->Text=Group;
				for (const auto& Item:Preview.Merge.Items) if (Equal(Item.Id,Group)) { const FString Name=Item.Properties[TEXT("name")].AsString();Parent->Text=Item.Kind+TEXT(" · ")+(Name.IsEmpty() ? Item.Target.IsEmpty() ? Item.Id : Item.Target : Name);break; }
				Parents.Add(Group,Parent);Roots.Add(Parent);
			}
			auto Row=MakeShared<FChangeRow>();Row->Text=Text;Row->Index=Index;Parent->Children.Add(Row);
		}
		if (Tree) { Tree->RequestTreeRefresh();for (const auto& Row:Roots) Tree->SetItemExpansion(Row,true); }
	};
	const auto Set=[&](int32 Index,ECheckBoxState State)
	{
		auto& Change=Preview.Merge.Changes[Index];if (!Actionable(Change)) return;
		Change.bApply=State==ECheckBoxState::Checked;Change.bDeferred=false;
		Change.bSuppress=!Change.bApply && (Change.Change==EVaRestOpenApiChange::Restore || Change.Change==EVaRestOpenApiChange::Suppressed || Change.Change==EVaRestOpenApiChange::Added);
	};
	SAssignNew(Window,SWindow).Title(NSLOCTEXT("VaRestOpenApi","ReimportPreview","OpenAPI changes")).ClientSize(FVector2D(1000,720)).SupportsMaximize(true).SupportsMinimize(false)
	[
		SNew(SVerticalBox)
		+SVerticalBox::Slot().AutoHeight().Padding(12)[SNew(STextBlock).Text(NSLOCTEXT("VaRestOpenApi","MergeHelp","Conflicts keep your edits. Tick a conflict to take upstream. Untick a restore to suppress it."))]
		+SVerticalBox::Slot().AutoHeight().Padding(8)
		[
			SNew(SBox).MaxDesiredHeight(180).Visibility(Diagnostics.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
			[SNew(SScrollBox)+SScrollBox::Slot()[SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(Diagnostics))]]
		]
		+SVerticalBox::Slot().AutoHeight().Padding(8)[SNew(SSearchBox).OnTextChanged_Lambda([&](const FText& Text) { Search=Text.ToString();Rebuild(); })]
		+SVerticalBox::Slot().AutoHeight().Padding(8)[SNew(SCheckBox).OnCheckStateChanged_Lambda([&](ECheckBoxState State) { ShowSuppressed=State==ECheckBoxState::Checked;Rebuild(); })[SNew(STextBlock).Text(NSLOCTEXT("VaRestOpenApi","ShowSuppressed","Show suppressed"))]]
		+SVerticalBox::Slot().FillHeight(1).Padding(8)
		[
			SAssignNew(Tree,STreeView<FReimportRow>).TreeItemsSource(&Roots).SelectionMode(ESelectionMode::None)
			.OnGetChildren_Lambda([](FReimportRow Row,TArray<FReimportRow>& Out) { Out=Row->Children; })
			.OnGenerateRow_Lambda([&](FReimportRow Row,const TSharedRef<STableViewBase>& Owner)->TSharedRef<ITableRow>
			{
				return SNew(STableRow<FReimportRow>,Owner)
				[
					SNew(SCheckBox).IsEnabled_Lambda([&,Row]() { if (Row->Index!=INDEX_NONE) return Actionable(Preview.Merge.Changes[Row->Index]);for (const auto& Child:Row->Children) if (Actionable(Preview.Merge.Changes[Child->Index])) return true;return false; })
					.IsChecked_Lambda([&,Row]() { if (Row->Index!=INDEX_NONE) return Preview.Merge.Changes[Row->Index].bApply ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;int32 Count=0,Total=0;for (const auto& Child:Row->Children) if (Actionable(Preview.Merge.Changes[Child->Index])) { ++Total;Count+=Preview.Merge.Changes[Child->Index].bApply; }return Count==0 ? ECheckBoxState::Unchecked : Count==Total ? ECheckBoxState::Checked : ECheckBoxState::Undetermined; })
					.OnCheckStateChanged_Lambda([&,Row](ECheckBoxState State) { if (Row->Index!=INDEX_NONE) Set(Row->Index,State);else for (const auto& Child:Row->Children) Set(Child->Index,State); })
					.ToolTipText_Lambda([&,Row]() { if (Row->Index==INDEX_NONE) return FText::FromString(Row->Text);const auto& Change=Preview.Merge.Changes[Row->Index];return FText::FromString(Change.Id+TEXT("\nBase: ")+ValuePreview(Change.Base)+TEXT("\nYours: ")+ValuePreview(Change.Mine)+TEXT("\nUpstream: ")+ValuePreview(Change.Theirs)); })
					[SNew(STextBlock).Text(FText::FromString(Row->Text))]
				];
			})
		]
		+SVerticalBox::Slot().AutoHeight().Padding(12)[SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("%d potentially affected packages · %d conflicts"),Preview.AffectedPackages.Num(),Preview.Merge.Conflicts)))]
		+SVerticalBox::Slot().AutoHeight().Padding(8)
		[
			SNew(SHorizontalBox)
			+SHorizontalBox::Slot().AutoWidth().Padding(4)[SNew(SButton).Text(NSLOCTEXT("VaRestOpenApi","ChangeOptions","Change options…")).OnClicked_Lambda([&]() { bChangeOptions=true;Window->RequestDestroyWindow();return FReply::Handled(); })]
			+SHorizontalBox::Slot().FillWidth(1)
			+SHorizontalBox::Slot().AutoWidth().Padding(4)[SNew(SButton).Text(NSLOCTEXT("VaRestOpenApi","CancelReimport","Cancel")).OnClicked_Lambda([&]() { Window->RequestDestroyWindow();return FReply::Handled(); })]
			+SHorizontalBox::Slot().AutoWidth().Padding(4)[SNew(SButton).Text(NSLOCTEXT("VaRestOpenApi","ApplyReimport","Apply")).OnClicked_Lambda([&]() { Accepted=true;Window->RequestDestroyWindow();return FReply::Handled(); })]
		]
	];
	Rebuild();FSlateApplication::Get().AddModalWindow(Window.ToSharedRef(),nullptr);return Accepted;
}
}
