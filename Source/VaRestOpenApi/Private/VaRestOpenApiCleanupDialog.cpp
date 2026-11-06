// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiCleanup.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"

namespace VaRest::OpenApi
{
bool ShowCleanupPicker(TArray<TSharedPtr<FCleanupRow>>& Rows)
{
	if (!FSlateApplication::IsInitialized()) return false;bool Accepted=false;TSharedPtr<SWindow> Window;
	SAssignNew(Window,SWindow).Title(NSLOCTEXT("VaRestOpenApi","CleanupTitle","Clean up removed items")).ClientSize(FVector2D(900,620)).SupportsMinimize(false)
	[
		SNew(SVerticalBox)
		+SVerticalBox::Slot().AutoHeight().Padding(12)[SNew(STextBlock).AutoWrapText(true).Text(NSLOCTEXT("VaRestOpenApi","CleanupHelp","Select obsolete declarations to delete. Referencing graphs may need repair. Cleanup is not undoable; editor undo history is cleared after changes."))]
		+SVerticalBox::Slot().FillHeight(1).Padding(8)
		[
			SNew(SListView<TSharedPtr<FCleanupRow>>).ListItemsSource(&Rows).SelectionMode(ESelectionMode::None)
			.OnGenerateRow_Lambda([](TSharedPtr<FCleanupRow> Row,const TSharedRef<STableViewBase>& Owner)->TSharedRef<ITableRow>
			{
				FString Name=Row->Item.Properties[TEXT("name")].AsString();if (Name.IsEmpty()) Name=Row->Item.Properties[TEXT("display")].AsString();if (Name.IsEmpty()) Name=Row->Item.Target;
				return SNew(STableRow<TSharedPtr<FCleanupRow>>,Owner)
				[
					SNew(SCheckBox).IsChecked_Lambda([Row]() { return Row->bSelected ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }).OnCheckStateChanged_Lambda([Row](ECheckBoxState State) { Row->bSelected=State==ECheckBoxState::Checked; })
					[
						SNew(SVerticalBox)
						+SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(FText::FromString(Row->Item.Kind+TEXT(" · ")+Name))]
						+SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(Row->Referencers))]
					]
				];
			})
		]
		+SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(12)
		[
			SNew(SHorizontalBox)
			+SHorizontalBox::Slot().AutoWidth().Padding(4)[SNew(SButton).Text(NSLOCTEXT("VaRestOpenApi","CancelCleanup","Cancel")).OnClicked_Lambda([&]() { Window->RequestDestroyWindow();return FReply::Handled(); })]
			+SHorizontalBox::Slot().AutoWidth().Padding(4)[SNew(SButton).Text(NSLOCTEXT("VaRestOpenApi","DeleteSelected","Delete selected")).IsEnabled_Lambda([&]() { return Rows.ContainsByPredicate([](const auto& Row) { return Row->bSelected; }); }).OnClicked_Lambda([&]() { Accepted=true;Window->RequestDestroyWindow();return FReply::Handled(); })]
		]
	];
	FSlateApplication::Get().AddModalWindow(Window.ToSharedRef(),nullptr);return Accepted;
}
}
