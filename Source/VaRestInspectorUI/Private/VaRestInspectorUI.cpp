// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestInspectorUI.h"
#include "VaRestInspectorViewModel.h"
#include "VaRestInspectorUserSettings.h"
#include "VaRestInspectorSettings.h"
#include "SVaRestInspectorWaterfall.h"
#include "VaRestInspectorJsonMarshaller.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "GenericPlatform/GenericPlatformApplicationMisc.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/SMultiLineEditableText.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/STreeView.h"
#include "Widgets/Views/SExpanderArrow.h"
#if WITH_EDITOR
#include "Editor.h"
#include "ISettingsModule.h"
#include "EdGraph/EdGraphNode.h"
#include "Kismet2/KismetDebugUtilities.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "SourceCodeNavigation.h"
#include "ToolMenus.h"
#endif

namespace
{
	const FName InspectorTab(TEXT("VaRestInspector"));
	const FName ColumnIds[] = {TEXT("Status"), TEXT("Method"), TEXT("Name"), TEXT("Initiator"), TEXT("Duration"), TEXT("Size"), TEXT("Attempts"), TEXT("Scope"), TEXT("Client"), TEXT("Url"), TEXT("Cache"), TEXT("Offline"), TEXT("Tag"), TEXT("Id"), TEXT("Time")};
	FOptionalSize TableWidth(const TSharedPtr<SHeaderRow>& Value)
	{
		float Width = 16; const auto Hidden = Value->GetHiddenColumnIds();
		for (const auto& Column : Value->GetColumns()) if (!Hidden.Contains(Column.ColumnId)) { const float Size = Column.GetWidth(); Width += FMath::IsFinite(Size) ? FMath::Clamp(Size, 40.f, 600.f) : 90.f; }
		return FOptionalSize(FMath::Max(160.f, Width));
	}
	using FRow = TSharedPtr<FVaRestInspectorRowModel>;
	using FBodyNode = TSharedPtr<FVaRestInspectorBodyTreeNode>;
	using FScope = TSharedPtr<FVaRestInspectorScopeOption>;
	using FAttempt = TSharedPtr<FVaRestInspectorAttemptOption>;
	using FFrame = TSharedPtr<FVaRestInspectorFrameRowModel>;
	const FName FrameColumns[] = {TEXT("Id"), TEXT("Dir"), TEXT("Clock"), TEXT("Kind"), TEXT("Channel"), TEXT("Route"), TEXT("MsgId"), TEXT("Protocol"), TEXT("Bytes"), TEXT("Lane"), TEXT("Attachments")};
	using FStatsRow = TSharedPtr<FVaRestInspectorStatsRowModel>;
	const FName StatsColumns[] = {TEXT("Type"),TEXT("Scope"),TEXT("Client"),TEXT("Key / socket"),TEXT("Completed / closed"),TEXT("Errors"),TEXT("Error %"),TEXT("Attempts"),TEXT("Retries / reconnects"),TEXT("Auth replays"),TEXT("Cache hits"),TEXT("Bytes up"),TEXT("Bytes down"),TEXT("p50 ms"),TEXT("p95 ms"),TEXT("p99 ms"),TEXT("Max ms"),TEXT("Frames in"),TEXT("Frames out"),TEXT("Drops"),TEXT("Avg RTT ms"),TEXT("Partial"),TEXT("Incomplete")};
	static_assert(UE_ARRAY_COUNT(StatsColumns) == FVaRestInspectorStatsRowModel::ColumnCount);
	class SInspectorStatsRow final : public SMultiColumnTableRow<FStatsRow>
	{
	public:
		SLATE_BEGIN_ARGS(SInspectorStatsRow) {} SLATE_ARGUMENT(FStatsRow, Row) SLATE_END_ARGS()
		void Construct(const FArguments& Args, const TSharedRef<STableViewBase>& Owner)
		{ Row = Args._Row; SMultiColumnTableRow::Construct(FSuperRowType::FArguments().Padding(4), Owner); }
		TSharedRef<SWidget> GenerateWidgetForColumn(const FName& Column) override
		{
			int32 Index = 0; for (; Index < UE_ARRAY_COUNT(StatsColumns); ++Index) if (StatsColumns[Index] == Column) break;
			if (Index >= UE_ARRAY_COUNT(StatsColumns)) return SNew(STextBlock);
			return SNew(STextBlock).Text(Row->Columns[Index].Kind ? Row->Columns[Index].Text : FText::FromString(TEXT("—"))).ToolTipText(Row->Columns[Index].Text);
		}
	private: FStatsRow Row;
	};
	class SInspectorFrameRow final : public SMultiColumnTableRow<FFrame>
	{
	public:
		SLATE_BEGIN_ARGS(SInspectorFrameRow) {} SLATE_ARGUMENT(FFrame, Row) SLATE_END_ARGS()
		void Construct(const FArguments& Args, const TSharedRef<STableViewBase>& Owner)
		{ Row = Args._Row; SMultiColumnTableRow::Construct(FSuperRowType::FArguments().Padding(4), Owner); }
		TSharedRef<SWidget> GenerateWidgetForColumn(const FName& Column) override
		{
			int32 Index = 0; for (; Index < UE_ARRAY_COUNT(FrameColumns); ++Index) if (FrameColumns[Index] == Column) break;
			if (Index >= UE_ARRAY_COUNT(FrameColumns)) return SNew(STextBlock);
			return SNew(STextBlock).Text_Lambda([Value = Row, Index] { return Value->Columns[Index]; });
		}
	private: FFrame Row;
	};
	class SInspectorRow final : public SMultiColumnTableRow<FRow>
	{
	public:
		SLATE_BEGIN_ARGS(SInspectorRow) {} SLATE_ARGUMENT(FRow, Row) SLATE_END_ARGS()
		void Construct(const FArguments& Args, const TSharedRef<STableViewBase>& Owner)
		{ Row = Args._Row; SMultiColumnTableRow::Construct(FSuperRowType::FArguments().Padding(4), Owner); }
		TSharedRef<SWidget> GenerateWidgetForColumn(const FName& Column) override
		{
			int32 Index = 0; for (; Index < UE_ARRAY_COUNT(ColumnIds); ++Index) if (ColumnIds[Index] == Column) break;
			if (Index >= UE_ARRAY_COUNT(ColumnIds)) return SNew(STextBlock);
			return SNew(STextBlock).Text_Lambda([Value = Row, Index] { return Value->Columns[Index]; }).ToolTipText_Lambda([Value = Row] { return Value->Tooltip; });
		}
	private: FRow Row;
	};
	class SVaRestInspector final : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SVaRestInspector) {}
			SLATE_ARGUMENT(TSharedPtr<FVaRestInspectorViewModel>, Model)
			SLATE_ARGUMENT(TSharedPtr<FVaRestInspectorUserSettings>, Preferences)
		SLATE_END_ARGS()
		void Construct(const FArguments& Args)
		{
			Model = Args._Model; Preferences = Args._Preferences; BodyView = FVaRestInspectorBodyViewModel::Create(); BodyMarshaller = MakeShared<FVaRestInspectorJsonMarshaller>();
			if (!BodyView) { ChildSlot[SNew(STextBlock).Text(FText::FromString(TEXT("Body viewer capacity is exhausted. Close another Inspector view.")))]; return; }
			const TWeakPtr<SVaRestInspector> Weak = SharedThis(this);
			Model->SetWake([Weak] { if (auto Panel = Weak.Pin()) Panel->Wake(); });
			Model->SetScope(Preferences->bAllScopes ? TOptional<uint64>() : TOptional<uint64>(Preferences->ScopeId));
			Model->SetFilter(Preferences->Filter, FPlatformTime::Seconds() - 1); Model->Sort(static_cast<EVaRestInspectorColumn>(Preferences->SortColumn), Preferences->bAscending);
			bFollow = Preferences->SortColumn == static_cast<int32>(EVaRestInspectorColumn::Time) && Preferences->bAscending;
			bHideEngineIo = GetDefault<UVaRestInspectorSettings>()->bHideEngineIoFrames; Model->SetHideEngineIo(bHideEngineIo);
			auto StatsHeader = SNew(SHeaderRow).CanSelectGeneratedColumn(true);
			for (int32 Index = 0; Index < UE_ARRAY_COUNT(StatsColumns); ++Index)
				{
				StatsWidths[Index] = Index == 3 ? 240.f : Index >= 13 && Index <= 15 ? 190.f : 120.f;
				StatsHeader->AddColumn(SHeaderRow::Column(StatsColumns[Index]).DefaultLabel(FText::FromName(StatsColumns[Index]))
					.ManualWidth_Lambda([Weak, Index] { const auto Panel = Weak.Pin(); return Panel ? Panel->StatsWidths[Index] : 120.f; })
					.OnWidthChanged_Lambda([Weak, Index](float Width) { if (auto Panel = Weak.Pin(); Panel && FMath::IsFinite(Width)) Panel->StatsWidths[Index] = FMath::Clamp(Width, 40.f, 600.f); })
					.SortMode_Lambda([Weak, Index] { const auto Panel = Weak.Pin(); return !Panel || Panel->StatsSort != Index ? EColumnSortMode::None : Panel->bStatsSortAscending ? EColumnSortMode::Ascending : EColumnSortMode::Descending; })
					.OnSort_Lambda([Weak, Index](EColumnSortPriority::Type, const FName&, EColumnSortMode::Type Mode)
					{ if (auto Panel = Weak.Pin()) { Panel->StatsSort = Index; Panel->bStatsSortAscending = Mode != EColumnSortMode::Descending; Panel->Model->SortStats(Index, Panel->bStatsSortAscending); Panel->StatsList->RequestListRefresh(); } }));
			}
			auto FrameHeader = SNew(SHeaderRow).CanSelectGeneratedColumn(true);
			for (int32 Index = 0; Index < UE_ARRAY_COUNT(FrameColumns); ++Index)
				{
				FrameWidths[Index] = Index == 3 || Index == 5 ? 160.f : 90.f;
				FrameHeader->AddColumn(SHeaderRow::Column(FrameColumns[Index]).DefaultLabel(FText::FromName(FrameColumns[Index]))
					.ManualWidth_Lambda([Weak, Index] { const auto Panel = Weak.Pin(); return Panel ? Panel->FrameWidths[Index] : 90.f; })
					.OnWidthChanged_Lambda([Weak, Index](float Width) { if (auto Panel = Weak.Pin(); Panel && FMath::IsFinite(Width)) Panel->FrameWidths[Index] = FMath::Clamp(Width, 40.f, 600.f); }));
			}
			TArray<FName> Hidden; for (int32 I = 0; I < UE_ARRAY_COUNT(ColumnIds); ++I) if (Preferences->Hidden[I]) Hidden.Add(ColumnIds[I]);
			Header = SNew(SHeaderRow).CanSelectGeneratedColumn(true).HiddenColumnsList(Hidden).OnHiddenColumnsListChanged_Lambda([Weak]
			{
				if (auto Panel = Weak.Pin())
				{
					const auto Ids = Panel->Header->GetHiddenColumnIds();
					for (int32 I = 0; I < UE_ARRAY_COUNT(ColumnIds); ++I) Panel->Preferences->Hidden[I] = Ids.Contains(ColumnIds[I]);
					Panel->Preferences->Save();
				}
			});
			for (int32 Position = 0; Position < UE_ARRAY_COUNT(ColumnIds); ++Position)
			{
				const int32 Index = Preferences->Order[Position];
				Header->AddColumn(SHeaderRow::Column(ColumnIds[Index]).DefaultLabel(FText::FromName(ColumnIds[Index]))
					.ManualWidth_Lambda([Weak, Index] { const auto Panel = Weak.Pin(); return Panel ? Panel->Preferences->Widths[Index] : 90.f; })
					.OnWidthChanged_Lambda([Weak, Index](float Width) { if (auto Panel = Weak.Pin(); Panel && FMath::IsFinite(Width)) { Panel->Preferences->Widths[Index] = FMath::Clamp(Width, 40.f, 600.f); Panel->Preferences->Save(); } })
					.SortMode_Lambda([Weak, Index] { const auto Panel = Weak.Pin(); return !Panel || Panel->Preferences->SortColumn != Index ? EColumnSortMode::None : Panel->Preferences->bAscending ? EColumnSortMode::Ascending : EColumnSortMode::Descending; })
					.OnSort_Lambda([Weak, Index](EColumnSortPriority::Type, const FName&, EColumnSortMode::Type Mode)
					{ if (auto Panel = Weak.Pin()) { Panel->bFollow = false; Panel->Preferences->SortColumn = Index; Panel->Preferences->bAscending = Mode != EColumnSortMode::Descending; Panel->Preferences->Save();
						Panel->Model->Sort(static_cast<EVaRestInspectorColumn>(Index), Panel->Preferences->bAscending); Panel->List->RequestListRefresh(); } }));
			}
			ChildSlot
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(6)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(2)[SNew(SButton).Text_Lambda([Weak] { const auto Panel = Weak.Pin(); return FText::FromString(Panel && Panel->Model->IsCapturing() ? TEXT("Pause") : TEXT("Record")); })
						.OnClicked_Lambda([Weak] { if (auto Panel = Weak.Pin()) { IVaRestInspector::Get().SetCapture(!Panel->Model->IsCapturing()); Panel->Model->Invalidate(); } return FReply::Handled(); })]
					+ SHorizontalBox::Slot().AutoWidth().Padding(2)[SNew(SButton).Text(FText::FromString(TEXT("Clear"))).OnClicked_Lambda([Weak]
						{ if (auto Panel = Weak.Pin()) { IVaRestInspector::Get().Clear(); Panel->Model->Invalidate(); } return FReply::Handled(); })]
					+ SHorizontalBox::Slot().AutoWidth().Padding(2)[SNew(SCheckBox).IsChecked_Lambda([Weak] { const auto Panel = Weak.Pin(); return Panel && Panel->Preferences->bPreserve ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
						.OnCheckStateChanged_Lambda([Weak](ECheckBoxState State) { if (auto Panel = Weak.Pin()) { Panel->Preferences->bPreserve = State == ECheckBoxState::Checked; Panel->Preferences->Save(); } })[SNew(STextBlock).Text(FText::FromString(TEXT("Preserve log")))]]
					+ SHorizontalBox::Slot().AutoWidth().Padding(2)
					[SAssignNew(ScopePicker, SComboBox<FScope>).OptionsSource(&Model->Scopes())
						.OnGenerateWidget_Lambda([](FScope Scope) { return SNew(STextBlock).Text(Scope->Label); })
						.OnSelectionChanged_Lambda([Weak](FScope Scope, ESelectInfo::Type) { if (auto Panel = Weak.Pin(); Panel && Scope)
							{ Panel->Preferences->bAllScopes = Scope->bAll; Panel->Preferences->ScopeId = Scope->Id; Panel->Preferences->Save(); Panel->Model->SetScope(Scope->bAll ? TOptional<uint64>() : TOptional<uint64>(Scope->Id)); } })
						[SNew(STextBlock).Text_Lambda([Weak]
						{
							if (const auto Panel = Weak.Pin())
							{
								for (const auto& Scope : Panel->Model->Scopes())
									if (Scope->bAll == Panel->Preferences->bAllScopes && (Scope->bAll || Scope->Id == Panel->Preferences->ScopeId)) return Scope->Label;
							}
							return FText::FromString(TEXT("All scopes"));
						})]]
					+ SHorizontalBox::Slot().AutoWidth().Padding(2)[SNew(SCheckBox).IsChecked_Lambda([Weak] { const auto Panel = Weak.Pin(); return Panel && Panel->bFollow ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
						.OnCheckStateChanged_Lambda([Weak](ECheckBoxState State) { if (auto Panel = Weak.Pin()) Panel->SetFollow(State == ECheckBoxState::Checked); })[SNew(STextBlock).Text(FText::FromString(TEXT("Live tail")))]]
					+ SHorizontalBox::Slot().FillWidth(1).Padding(8, 2)[SNew(SSearchBox).InitialText(FText::FromString(Preferences->Filter)).HintText(FText::FromString(TEXT("Filter: status:4xx method:POST -is:cached"))).OnTextChanged_Lambda([Weak](const FText& Text)
						{ if (auto Panel = Weak.Pin()) { Panel->Preferences->Filter = Text.ToString().Left(4096); Panel->Preferences->Save(); Panel->Model->SetFilter(Panel->Preferences->Filter, FPlatformTime::Seconds()); } })]
					+ SHorizontalBox::Slot().AutoWidth().Padding(2)[SNew(SComboButton).OnGetMenuContent(this, &SVaRestInspector::LayoutMenu).ButtonContent()[SNew(STextBlock).Text(FText::FromString(TEXT("Columns")))]]
					+ SHorizontalBox::Slot().AutoWidth().Padding(2)[SNew(SComboButton).OnGetMenuContent(this, &SVaRestInspector::ExportMenu).ButtonContent()[SNew(STextBlock).Text(FText::FromString(TEXT("Export HAR…")))]]
#if WITH_EDITOR
					+ SHorizontalBox::Slot().AutoWidth().Padding(2)[SNew(SButton).Text(FText::FromString(TEXT("Settings"))).Visibility(GIsEditor ? EVisibility::Visible : EVisibility::Collapsed).OnClicked_Lambda([]
						{ if (auto* Settings = FModuleManager::LoadModulePtr<ISettingsModule>(TEXT("Settings"))) Settings->ShowViewer(TEXT("Project"), TEXT("Plugins"), TEXT("VaRestInspector")); return FReply::Handled(); })]
#endif
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(6)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth()[KindButton(TEXT("All"), EVaRestInspectorKindFilter::All)]
					+ SHorizontalBox::Slot().AutoWidth()[KindButton(TEXT("HTTP"), EVaRestInspectorKindFilter::Http)]
					+ SHorizontalBox::Slot().AutoWidth()[KindButton(TEXT("WebSocket"), EVaRestInspectorKindFilter::WebSocket)]
					+ SHorizontalBox::Slot().AutoWidth()[KindButton(TEXT("Socket.IO"), EVaRestInspectorKindFilter::SocketIO)]
					+ SHorizontalBox::Slot().AutoWidth()[KindButton(TEXT("Errors"), EVaRestInspectorKindFilter::Errors)]
					+ SHorizontalBox::Slot().AutoWidth()[KindButton(TEXT("Markers"), EVaRestInspectorKindFilter::Markers)]
				]
				+ SVerticalBox::Slot().FillHeight(1)
				[
					SNew(SSplitter)
					+ SSplitter::Slot().Value(Preferences->Split).OnSlotResized_Lambda([Weak](float Value) { if (auto Panel = Weak.Pin(); Panel && FMath::IsFinite(Value)) { Panel->Preferences->Split = FMath::Clamp(Value, .15f, .85f); Panel->Preferences->Save(); } })
					[
						SNew(SScrollBox).Orientation(Orient_Horizontal) + SScrollBox::Slot().VAlign(VAlign_Fill)[SNew(SBox).WidthOverride_Lambda([Weak] { const auto Panel = Weak.Pin(); return Panel ? TableWidth(Panel->Header) : FOptionalSize(160.f); })[
						SAssignNew(List, SListView<FRow>).ListItemsSource(&Model->Rows()).SelectionMode(ESelectionMode::Multi).HeaderRow(Header)
						.OnGenerateRow_Lambda([](FRow Row, const TSharedRef<STableViewBase>& Owner) { return SNew(SInspectorRow, Owner).Row(Row); })
						.OnListViewScrolled_Lambda([Weak](double Offset) { if (auto Panel = Weak.Pin()) { if (Offset + .01 < Panel->LastScroll) Panel->bFollow = false; Panel->LastScroll = Offset; } })
						.OnSelectionChanged_Lambda([Weak](FRow Row, ESelectInfo::Type) { if (auto Panel = Weak.Pin()) { if (!Row || !Panel->List->IsItemSelected(Row)) { const auto Selected = Panel->List->GetSelectedItems(); Row = Selected.IsEmpty() ? nullptr : Selected.Last(); } Panel->Model->Select(Row); Panel->UpdateDetails(); } })]]
					]
					+ SSplitter::Slot().Value(1 - Preferences->Split)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SScrollBox).Orientation(Orient_Horizontal)
							+ SScrollBox::Slot()
							[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth()[DetailButton(TEXT("Overview"), 0)]
							+ SHorizontalBox::Slot().AutoWidth()[DetailButton(TEXT("Request"), 1)]
							+ SHorizontalBox::Slot().AutoWidth()[DetailButton(TEXT("Response"), 2)]
							+ SHorizontalBox::Slot().AutoWidth()[DetailButton(TEXT("Timing"), 3)]
							+ SHorizontalBox::Slot().AutoWidth()[DetailButton(TEXT("Body"), 4)]
							+ SHorizontalBox::Slot().AutoWidth()[DetailButton(TEXT("Stats"), 5)]
							+ SHorizontalBox::Slot().AutoWidth()[DetailButton(TEXT("Connection"), 7)]
							+ SHorizontalBox::Slot().AutoWidth()[DetailButton(TEXT("Frames"), 6)]
							+ SHorizontalBox::Slot().AutoWidth()[SNew(SComboButton).Visibility_Lambda([Weak] { const auto Panel = Weak.Pin(); return Panel && !Panel->Model->Details().bSocket ? EVisibility::Visible : EVisibility::Collapsed; }).OnGetMenuContent(this, &SVaRestInspector::LinksMenu)
								.ButtonContent()[SNew(STextBlock).Text(FText::FromString(TEXT("Links"))).ToolTipText_Lambda([Weak] { const auto Panel = Weak.Pin(); return Panel ? Panel->Model->LinksText() : FText(); })]]
							+ SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(FText::FromString(TEXT("Initiator"))).OnClicked_Lambda([Weak]
								{ if (auto Panel = Weak.Pin()) Panel->Navigate(); return FReply::Handled(); })]
							+ SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(FText::FromString(TEXT("Copy"))).OnClicked_Lambda([Weak]
								{ if (auto Panel = Weak.Pin()) FPlatformApplicationMisc::ClipboardCopy(*Panel->CurrentDetails.ToString()); return FReply::Handled(); })]
							+ SHorizontalBox::Slot().AutoWidth()[SNew(SComboButton).OnGetMenuContent(this, &SVaRestInspector::CopyMenu).ButtonContent()[SNew(STextBlock).Text(FText::FromString(TEXT("Copy as…")))]]
							]
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().FillWidth(1)
							[SAssignNew(AttemptPicker, SComboBox<FAttempt>).OptionsSource(&Model->Attempts())
								.OnGenerateWidget_Lambda([](FAttempt Attempt) { return SNew(STextBlock).Text(Attempt->Label); })
								.OnSelectionChanged_Lambda([Weak](FAttempt Attempt, ESelectInfo::Type) { if (auto Panel = Weak.Pin(); Panel && Attempt) Panel->Model->SelectAttempt(Attempt->Id); })
								[SNew(STextBlock).Text_Lambda([Weak] { const auto Panel = Weak.Pin(); return Panel ? Panel->Model->Details().AttemptLabel : FText(); })]]
							+ SHorizontalBox::Slot().AutoWidth()[SNew(SCheckBox).Visibility_Lambda([Weak] { const auto Panel = Weak.Pin(); return Panel && !Panel->Model->Details().bSocket && Panel->DetailTab == 1 ? EVisibility::Visible : EVisibility::Collapsed; }).IsChecked_Lambda([Weak] { const auto Panel = Weak.Pin(); return Panel && Panel->bRequestAsBuilt ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
								.OnCheckStateChanged_Lambda([Weak](ECheckBoxState State) { if (auto Panel = Weak.Pin()) { Panel->bRequestAsBuilt = State == ECheckBoxState::Checked; Panel->Model->SetRequestAsBuilt(Panel->bRequestAsBuilt); } })
								[SNew(STextBlock).Text(FText::FromString(TEXT("As built")))]]
						]
						+ SVerticalBox::Slot().FillHeight(1).Padding(6)
						[
							SNew(SWidgetSwitcher).WidgetIndex_Lambda([Weak] { const auto Panel = Weak.Pin(); return Panel && Panel->DetailTab == 4 ? 1 : Panel && Panel->DetailTab == 3 ? 2 : Panel && Panel->DetailTab == 6 ? 3 : Panel && Panel->DetailTab == 5 ? 4 : 0; })
							+ SWidgetSwitcher::Slot()[SNew(SMultiLineEditableText).IsReadOnly(true).AutoWrapText(true).Text_Lambda([Weak]
								{ const auto Panel = Weak.Pin(); return Panel ? Panel->CurrentDetails : FText(); })]
							+ SWidgetSwitcher::Slot()
							[
								SNew(SVerticalBox)
								+ SVerticalBox::Slot().AutoHeight()
								[
									SNew(SHorizontalBox).Visibility_Lambda([Weak] { const auto Panel = Weak.Pin(); return Panel && !Panel->Model->Details().bSocket ? EVisibility::Visible : EVisibility::Collapsed; })
									+ SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(FText::FromString(TEXT("Request"))).OnClicked_Lambda([Weak]
										{ if (auto Panel = Weak.Pin()) { Panel->bRequestBody = true; Panel->UpdateDetails(); } return FReply::Handled(); })]
									+ SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(FText::FromString(TEXT("Selected attempt response"))).OnClicked_Lambda([Weak]
										{ if (auto Panel = Weak.Pin()) { Panel->bRequestBody = false; Panel->UpdateDetails(); } return FReply::Handled(); })]
								]
								+ SVerticalBox::Slot().AutoHeight()
								[
									SNew(SHorizontalBox)
									+ SHorizontalBox::Slot().AutoWidth()[BodyButton(TEXT("Raw"), 0)]
									+ SHorizontalBox::Slot().AutoWidth()[BodyButton(TEXT("Pretty"), 1)]
									+ SHorizontalBox::Slot().AutoWidth()[BodyButton(TEXT("Hex"), 2)]
									+ SHorizontalBox::Slot().AutoWidth()[BodyButton(TEXT("Tree"), 3)]
									+ SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(FText::FromString(TEXT("Save excerpt…"))).OnClicked_Lambda([Weak]
										{ if (auto Panel = Weak.Pin()) Panel->SaveBody(); return FReply::Handled(); })]
									+ SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(FText::FromString(TEXT("Copy path"))).OnClicked_Lambda([Weak]
										{ if (auto Panel = Weak.Pin(); Panel && Panel->SelectedNode) FPlatformApplicationMisc::ClipboardCopy(*Panel->SelectedNode->Path); return FReply::Handled(); })]
									+ SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(FText::FromString(TEXT("Copy value"))).OnClicked_Lambda([Weak]
										{ if (auto Panel = Weak.Pin(); Panel && Panel->SelectedNode) FPlatformApplicationMisc::ClipboardCopy(*Panel->SelectedNode->Value.ToString()); return FReply::Handled(); })]
								]
								+ SVerticalBox::Slot().FillHeight(1)
								[
									SNew(SWidgetSwitcher).WidgetIndex_Lambda([Weak] { const auto Panel = Weak.Pin(); return Panel && Panel->BodyMode == 3 ? 1 : 0; })
									+ SWidgetSwitcher::Slot()[SNew(SMultiLineEditableText).Marshaller(BodyMarshaller).IsReadOnly(true).AutoWrapText(true).Text_Lambda([Weak]
										{ const auto Panel = Weak.Pin(); return Panel ? Panel->CurrentDetails : FText(); })]
									+ SWidgetSwitcher::Slot()
									[
										SAssignNew(BodyTree, STreeView<FBodyNode>).TreeItemsSource(&BodyView->Roots())
										.OnGetChildren_Lambda([Weak](FBodyNode Parent, TArray<FBodyNode>& Children) { if (auto Panel = Weak.Pin()) Panel->BodyView->Children(Parent, Children); })
										.OnGenerateRow_Lambda([](FBodyNode Node, const TSharedRef<STableViewBase>& Owner)
										{
											auto Row = SNew(STableRow<FBodyNode>, Owner);
											Row->SetContent(SNew(SHorizontalBox) + SHorizontalBox::Slot().AutoWidth()[SNew(SExpanderArrow, Row)]
												+ SHorizontalBox::Slot().FillWidth(1)[SNew(STextBlock).Text(Node->Label)]); return Row;
										})
										.OnSelectionChanged_Lambda([Weak](FBodyNode Node, ESelectInfo::Type) { if (auto Panel = Weak.Pin()) Panel->SelectedNode = MoveTemp(Node); })
									]
								]
								+ SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).AutoWrapText(true).Text_Lambda([Weak]
									{ const auto Panel = Weak.Pin(); return Panel ? Panel->BodyView->Notice() : FText(); })]
							]
							+ SWidgetSwitcher::Slot()
							[
								SNew(SVerticalBox)
								+ SVerticalBox::Slot().FillHeight(.35f)[SNew(SScrollBox) + SScrollBox::Slot()[SAssignNew(Waterfall, SVaRestInspectorWaterfall).Model(Model)]]
								+ SVerticalBox::Slot().FillHeight(.65f)[SNew(SMultiLineEditableText).IsReadOnly(true).AutoWrapText(true).Text_Lambda([Weak] { const auto Panel = Weak.Pin(); return Panel ? Panel->CurrentDetails : FText(); })]
							]
							+ SWidgetSwitcher::Slot()
							[
								SNew(SVerticalBox)
								+ SVerticalBox::Slot().AutoHeight()
								[
									SNew(SHorizontalBox)
									+ SHorizontalBox::Slot().FillWidth(1)[SNew(SSearchBox).HintText(FText::FromString(TEXT("Frames: dir:in kind:text route:orders ns:/game"))).OnTextChanged_Lambda([Weak](const FText& Text) { if (auto Panel = Weak.Pin()) Panel->Model->SetFrameFilter(Text.ToString()); })]
									+ SHorizontalBox::Slot().AutoWidth()[SNew(SCheckBox).IsChecked_Lambda([Weak] { const auto Panel = Weak.Pin(); return Panel && Panel->bHideEngineIo ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
										.OnCheckStateChanged_Lambda([Weak](ECheckBoxState State) { if (auto Panel = Weak.Pin()) { Panel->bHideEngineIo = State == ECheckBoxState::Checked; Panel->Model->SetHideEngineIo(Panel->bHideEngineIo); } })[SNew(STextBlock).Text(FText::FromString(TEXT("Hide Engine.IO")))]]
									+ SHorizontalBox::Slot().AutoWidth()[SNew(SCheckBox).IsChecked_Lambda([Weak] { const auto Panel = Weak.Pin(); return Panel && Panel->bHideControl ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
										.OnCheckStateChanged_Lambda([Weak](ECheckBoxState State) { if (auto Panel = Weak.Pin()) { Panel->bHideControl = State == ECheckBoxState::Checked; Panel->Model->SetHideControl(Panel->bHideControl); } })[SNew(STextBlock).Text(FText::FromString(TEXT("Hide control")))]]
									+ SHorizontalBox::Slot().AutoWidth()[SNew(SCheckBox).IsChecked_Lambda([Weak] { const auto Panel = Weak.Pin(); return Panel && Panel->bFrameFollow ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
										.OnCheckStateChanged_Lambda([Weak](ECheckBoxState State) { if (auto Panel = Weak.Pin()) { Panel->bFrameFollow = State == ECheckBoxState::Checked; Panel->UpdateDetails(); } })[SNew(STextBlock).Text(FText::FromString(TEXT("Follow")))]]
								]
								+ SVerticalBox::Slot().FillHeight(.65f)
								[SNew(SScrollBox).Orientation(Orient_Horizontal) + SScrollBox::Slot().VAlign(VAlign_Fill)[SNew(SBox).WidthOverride_Lambda([FrameHeader] { return TableWidth(FrameHeader); })[SAssignNew(FrameList, SListView<FFrame>).ListItemsSource(&Model->Frames()).SelectionMode(ESelectionMode::Single).HeaderRow(FrameHeader)
									.OnGenerateRow_Lambda([](FFrame Frame, const TSharedRef<STableViewBase>& Owner) { return SNew(SInspectorFrameRow, Owner).Row(Frame); })
									.OnListViewScrolled_Lambda([Weak](double Offset) { if (auto Panel = Weak.Pin()) { if (Offset + .01 < Panel->LastFrameScroll) Panel->bFrameFollow = false; Panel->LastFrameScroll = Offset; } })
									.OnSelectionChanged_Lambda([Weak](FFrame Frame, ESelectInfo::Type) { if (auto Panel = Weak.Pin()) { if (Frame && Frame->bSynthetic) Panel->Model->SelectFrameMarker(Frame->MarkerDescription); else Panel->Model->SelectFrame(Frame ? Frame->Id : 0); } })]]]
								+ SVerticalBox::Slot().AutoHeight()
								[
									SNew(SHorizontalBox)
									+ SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(FText::FromString(TEXT("Inspect body"))).OnClicked_Lambda([Weak] { if (auto Panel = Weak.Pin()) { Panel->DetailTab = 4; Panel->UpdateDetails(); } return FReply::Handled(); })]
									+ SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(FText::FromString(TEXT("Linked request / reply"))).IsEnabled_Lambda([Weak] { const auto Panel = Weak.Pin(); return Panel && Panel->Model->Details().LinkedFrameId != 0; }).OnClicked_Lambda([Weak]
										{ if (auto Panel = Weak.Pin()) { const uint64 Id = Panel->Model->Details().LinkedFrameId; if (Id) { Panel->bFrameFollow = false; Panel->Model->RevealFrame(Id); Panel->Model->Refresh(FPlatformTime::Seconds()); Panel->UpdateDetails(); for (const auto& Frame : Panel->Model->Frames()) if (Frame->Id == Id) { Panel->FrameList->SetSelection(Frame); Panel->FrameList->RequestScrollIntoView(Frame); break; } } } return FReply::Handled(); })]
								]
								+ SVerticalBox::Slot().FillHeight(.35f)[SNew(SMultiLineEditableText).IsReadOnly(true).AutoWrapText(true).Text_Lambda([Weak] { const auto Panel = Weak.Pin(); return Panel ? Panel->Model->Details().FrameOverview : FText(); })]
								+ SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).AutoWrapText(true).Text_Lambda([Weak] { const auto Panel = Weak.Pin(); return Panel ? Panel->Model->Details().FrameStatus : FText(); })]
							]
							+ SWidgetSwitcher::Slot()
							[
								SNew(SVerticalBox)
								+ SVerticalBox::Slot().AutoHeight()[SNew(SButton).Text(FText::FromString(TEXT("Copy table (TSV)"))).OnClicked_Lambda([Weak]
									{ if (auto Panel = Weak.Pin()) { FPlatformApplicationMisc::ClipboardCopy(*Panel->Model->StatsTsv()); Panel->ActionMessage = FText::FromString(TEXT("Copied the displayed statistics table as TSV.")); } return FReply::Handled(); })]
								+ SVerticalBox::Slot().FillHeight(.3f)[SNew(SMultiLineEditableText).IsReadOnly(true).AutoWrapText(true).Text_Lambda([Weak] { const auto Panel = Weak.Pin(); return Panel ? Panel->Model->StatsText() : FText(); })]
								+ SVerticalBox::Slot().FillHeight(.7f)
								[SNew(SScrollBox).Orientation(Orient_Horizontal) + SScrollBox::Slot().VAlign(VAlign_Fill)[SNew(SBox).WidthOverride_Lambda([StatsHeader] { return TableWidth(StatsHeader); })[SAssignNew(StatsList, SListView<FStatsRow>).ListItemsSource(&Model->StatsRows()).SelectionMode(ESelectionMode::None).HeaderRow(StatsHeader)
									.OnGenerateRow_Lambda([](FStatsRow Row, const TSharedRef<STableViewBase>& Owner) { return SNew(SInspectorStatsRow, Owner).Row(Row); })]]]
							]
						]
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(6)[SNew(STextBlock).Text_Lambda([Weak] { const auto Panel = Weak.Pin(); return Panel ? Panel->Model->StatusText() : FText(); })]
				+ SVerticalBox::Slot().AutoHeight().Padding(6)[SNew(STextBlock).Text_Lambda([Weak] { const auto Panel = Weak.Pin(); return Panel ? Panel->ActionMessage : FText(); })]
				+ SVerticalBox::Slot().AutoHeight().Padding(6)[SNew(SButton).Text(FText::FromString(TEXT("Show export in folder"))).Visibility_Lambda([Weak] { const auto Panel = Weak.Pin(); return Panel && !Panel->ExportedPath.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
					.OnClicked_Lambda([Weak] { if (auto Panel = Weak.Pin()) FPlatformProcess::ExploreFolder(*FPaths::GetPath(Panel->ExportedPath)); return FReply::Handled(); })]
			];
			Model->Refresh(FPlatformTime::Seconds()); ScopePicker->RefreshOptions(); List->RequestListRefresh(); Follow(); UpdateDetails();
		}
		~SVaRestInspector() { if (Model) Model->SetWake({}); if (BodyView) BodyView->Shutdown(); if (Preferences) Preferences->Save(true); }
	private:
		TSharedRef<SWidget> KindButton(const TCHAR* Name, EVaRestInspectorKindFilter Kind)
		{
			return SNew(SButton).Text(FText::FromString(Name)).OnClicked_Lambda([this, Kind] { Model->SetKind(Kind); return FReply::Handled(); });
		}
		const FVaRestInspectorBodySource& ActiveBody() const
		{ const auto& Detail = Model->Details(); return Detail.bSocket ? Detail.FrameSource : bRequestBody ? Detail.RequestSource : Detail.AttemptResponseSource; }
		TSharedRef<SWidget> ExportMenu()
		{
			FMenuBuilder Menu(true, nullptr); const TCHAR* Names[] = {TEXT("All captured HTTP and sockets"), TEXT("Filtered HTTP and sockets"), TEXT("Selected request or socket")};
			for (int32 I = 0; I < UE_ARRAY_COUNT(Names); ++I)
				Menu.AddMenuEntry(FText::FromString(Names[I]), FText(), FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SVaRestInspector::ExportHar, I)));
			return Menu.MakeWidget();
		}
		void ExportHar(int32 Selection)
		{
			if (ExportTask.IsValid() && !ExportTask.IsCompleted()) { ActionMessage = FText::FromString(TEXT("This view is already exporting.")); return; }
			FVaRestInspectorExportOptions Options;
			if (Selection)
			{
				Options.bRestrictRequestIds = true;
				if (Selection == 1 && !Preferences->bAllScopes) Options.ScopeId = Preferences->ScopeId;
				const auto Add = [&Options](const auto& Row)
				{
					if (Row->Identity.Kind == EVaRestInspectorRecordKind::Http) Options.RequestIds.Add(Row->Identity.Id);
					else if (Row->Identity.Kind == EVaRestInspectorRecordKind::Socket) Options.SocketIds.Add(Row->Identity.Id);
				};
				if (Selection == 1) { for (const auto& Row : Model->Rows()) Add(Row); }
				else { for (const auto& Row : List->GetSelectedItems()) Add(Row); }
			}
			auto* Dialogs = FDesktopPlatformModule::Get(); if (!Dialogs) return; TArray<FString> Paths;
			if (!Dialogs->SaveFileDialog(nullptr, TEXT("Export captured HTTP and sockets as HAR"), FPaths::ProjectSavedDir(), TEXT("varest.har"), TEXT("HAR files|*.har"), 0, Paths) || Paths.IsEmpty()) return;
			if (!Model->IsAttached()) return;
			ExportedPath.Reset(); ExportTask = IVaRestInspector::Get().ExportHar(Paths[0], Options); ActionMessage = FText::FromString(TEXT("Exporting captured requests and sockets…")); Wake();
		}
		void PollExport()
		{
			if (!ExportTask.IsValid() || !ExportTask.IsCompleted()) return;
			const auto& Result = ExportTask.GetResult();
			if (Result.HasValue()) { ExportedPath = Result.GetValue(); ActionMessage = FText::FromString(TEXT("Exported captured requests and sockets to ") + ExportedPath); }
			else ActionMessage = FText::FromString(Result.HasError() ? Result.GetError() : FString(TEXT("Export completed without a result.")));
			ExportTask = {};
		}
		TSharedRef<SWidget> LinksMenu()
		{
			FMenuBuilder Menu(true, nullptr);
			Menu.AddWidget(SNew(STextBlock).Text(Model->LinksText()), FText(), true);
			for (const auto& Link : Model->Links())
			{
				const bool Available = Link->bAvailable;
				Menu.AddMenuEntry(Link->Label, FText::FromString(TEXT("Reveal the linked request in the list.")), FSlateIcon(),
					FUIAction(FExecuteAction::CreateSP(this, &SVaRestInspector::OpenLinkedRequest, Link->TargetId), FCanExecuteAction::CreateLambda([Available] { return Available; })));
			}
			return Menu.MakeWidget();
		}
		void OpenLinkedRequest(uint64 Id)
		{
			if (auto Row = Model->RevealRequest(Id))
			{
				bFollow = false; List->RequestListRefresh(); List->ClearSelection(); List->SetSelection(Row); List->RequestScrollIntoView(Row); DetailTab = 0; Model->EnableStats(false); UpdateDetails();
				ActionMessage = FText::FromString(TEXT("Linked request revealed. Changing filters ends this temporary reveal."));
			}
			else ActionMessage = FText::FromString(TEXT("The linked request is no longer available in this view."));
		}
		TSharedRef<SWidget> CopyMenu()
		{
			FMenuBuilder Menu(true, nullptr);
			if (Model->Details().bSocket)
			{
				const TCHAR* Names[] = {TEXT("URL"), TEXT("Socket id"), TEXT("Selected frame excerpt")};
				for (int32 I = 0; I < UE_ARRAY_COUNT(Names); ++I) Menu.AddMenuEntry(FText::FromString(Names[I]), FText(), FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SVaRestInspector::CopyValue, I + 1)));
			}
			else
			{
				const TCHAR* Names[] = {TEXT("cURL (bash)"), TEXT("URL"), TEXT("Request id"), TEXT("Request excerpt"), TEXT("Selected response excerpt")};
				for (int32 I = 0; I < UE_ARRAY_COUNT(Names); ++I) Menu.AddMenuEntry(FText::FromString(Names[I]), FText(), FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SVaRestInspector::CopyValue, I)));
			}
			return Menu.MakeWidget();
		}
		void CopyValue(int32 Kind)
		{
			const auto& Detail = Model->Details(); FString Value;
			if (Kind == 0) Value = Detail.Curl; else if (Kind == 1) Value = Detail.Url; else if (Kind == 2) Value = Detail.Id;
			else
			{
				const auto& Source = Detail.bSocket ? Detail.FrameSource : Kind == 3 ? Detail.RequestSource : Detail.AttemptResponseSource;
				if (!Source.bAvailable || Source.Kind == EVaRestExcerptKind::Binary)
				{ ActionMessage = FText::FromString(TEXT("No captured text excerpt is available. Use Save excerpt for binary data.")); return; }
				Value = FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Source.Bytes.GetData()), Source.Bytes.Num());
				ActionMessage = FText::FromString(Source.bTruncated ? TEXT("Copied a truncated captured excerpt.") : TEXT("Copied the captured excerpt."));
			}
			FPlatformApplicationMisc::ClipboardCopy(*Value);
			if (Kind < 3) ActionMessage = FText::FromString(Kind == 0 ? TEXT("Copied cURL text. Review the capture comments and replace redacted values before replay.") : TEXT("Copied the recorded value."));
		}
		void Follow() { if (bFollow && List && !Model->Rows().IsEmpty()) List->RequestScrollIntoView(Model->Rows().Last()); }
		void SetFollow(bool Value)
		{
			bFollow = Value; if (!Value) return;
			Preferences->SortColumn = static_cast<int32>(EVaRestInspectorColumn::Time); Preferences->bAscending = true; Preferences->Save();
			Model->Sort(EVaRestInspectorColumn::Time, true); List->RequestListRefresh(); Follow();
		}
		TSharedRef<SWidget> LayoutMenu()
		{
			FMenuBuilder Menu(true, nullptr);
			Menu.AddMenuEntry(FText::FromString(TEXT("Hide/show columns: right-click a column header")), FText(), FSlateIcon(), FUIAction());
			for (int32 Position = 0; Position < UE_ARRAY_COUNT(ColumnIds); ++Position)
			{
				const int32 Index = Preferences->Order[Position];
				Menu.AddSubMenu(FText::FromName(ColumnIds[Index]), FText(), FNewMenuDelegate::CreateSP(this, &SVaRestInspector::ColumnMenu, Position));
			}
			return Menu.MakeWidget();
		}
		void ColumnMenu(FMenuBuilder& Menu, int32 Position)
		{
			for (int32 Step : {-1, 1})
			{
				const int32 Other = Position + Step; if (Other < 0 || Other >= UE_ARRAY_COUNT(ColumnIds)) continue;
				Menu.AddMenuEntry(FText::FromString(Step < 0 ? TEXT("Move left") : TEXT("Move right")), FText(), FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SVaRestInspector::MoveColumn, Position, Other)));
			}
		}
		void MoveColumn(int32 Position, int32 Other)
		{ Swap(Preferences->Order[Position], Preferences->Order[Other]); Header->SwapColumns(Position, Other); Preferences->Save(); }
		void SaveBody()
		{
			const FVaRestInspectorBodySource Source = ActiveBody();
			if (!Source.bAvailable) { ActionMessage = FText::FromString(TEXT("No captured excerpt is available to save.")); return; }
			if (Source.Kind == EVaRestExcerptKind::Binary && !Source.bBinaryCaptured)
			{ ActionMessage = FText::FromString(TEXT("This binary body has a descriptor only. Enable binary capture before capturing its content.")); return; }
			auto* Dialogs = FDesktopPlatformModule::Get(); if (!Dialogs) return;
			const FString Extension = Source.Kind == EVaRestExcerptKind::Binary ? TEXT("bin") : Source.Kind == EVaRestExcerptKind::Json ? TEXT("json") : TEXT("txt");
			TArray<FString> Files;
			if (!Dialogs->SaveFileDialog(nullptr, TEXT("Save captured redacted excerpt"), FPaths::ProjectSavedDir(),
				(Model->Details().bSocket ? FString(TEXT("frame-excerpt.")) : bRequestBody ? FString(TEXT("request-excerpt.")) : FString(TEXT("attempt-response-excerpt."))) + Extension, TEXT("All files|*.*"), 0, Files) || Files.IsEmpty()) return;
			if (!Model->IsAttached()) return; // a modal dialog may have closed the Inspector/module
			const bool Saved = FFileHelper::SaveArrayToFile(Source.Bytes, *Files[0]);
			ActionMessage = FText::FromString(Saved ? (Source.bTruncated ? TEXT("Saved the truncated captured excerpt.") : TEXT("Saved the captured excerpt.")) : TEXT("Could not write the excerpt file."));
		}
		void Navigate()
		{
#if WITH_EDITOR
			if (GIsEditor)
			{
				const auto Detail = Model->Details(); auto* Object = FindObject<UObject>(nullptr, *Detail.ScriptObject);
				auto* Function = Object && !Detail.ScriptFunction.IsEmpty() && Detail.ScriptFunction.Len() < NAME_SIZE ? Object->FindFunction(FName(*Detail.ScriptFunction)) : nullptr;
				if (Object && Function && Detail.ScriptCodeOffset != INDEX_NONE)
					if (auto* Node = FKismetDebugUtilities::FindSourceNodeForCodeLocation(Object, Function, Detail.ScriptCodeOffset))
					{ FKismetEditorUtilities::BringKismetToFocusAttentionOnObject(Node); return; }
				if (!Detail.SourceFile.IsEmpty() && FSourceCodeNavigation::OpenSourceFile(Detail.SourceFile, FMath::Max(1, Detail.SourceLine))) return;
			}
#endif
			ActionMessage = FText::FromString(TEXT("The captured initiator is unavailable for navigation. Its recorded location remains in Overview."));
		}
		TSharedRef<SWidget> BodyButton(const TCHAR* Name, int32 Mode)
		{
			return SNew(SButton).Text(FText::FromString(Name)).OnClicked_Lambda([this, Mode] { BodyMode = Mode; UpdateDetails(); return FReply::Handled(); });
		}
		TSharedRef<SWidget> DetailButton(const TCHAR* Name, int32 Index)
		{
			return SNew(SButton).Text(FText::FromString(Name)).Visibility_Lambda([this, Index]
			{ const bool Socket = Model->Details().bSocket; return ((Index == 1 || Index == 2 || Index == 3) && Socket) || ((Index == 6 || Index == 7) && !Socket) ? EVisibility::Collapsed : EVisibility::Visible; }).OnClicked_Lambda([this, Index]
			{ DetailTab = Index; Model->EnableStats(Index == 5); UpdateDetails(); return FReply::Handled(); });
		}
		void UpdateDetails()
		{
			const auto& Detail = Model->Details();
			if (Detail.bSocket && (DetailTab == 1 || DetailTab == 2 || DetailTab == 3)) DetailTab = 7;
			else if (!Detail.bSocket && (DetailTab == 6 || DetailTab == 7)) DetailTab = 0;
			if (AttemptPicker) AttemptPicker->RefreshOptions();
			if (StatsList && DetailTab == 5) StatsList->RequestListRefresh();
			if (Waterfall) Waterfall->Invalidate(EInvalidateWidgetReason::Layout | EInvalidateWidgetReason::Paint);
			if (FrameList) { FrameList->RequestListRefresh(); if (DetailTab == 6 && bFrameFollow && !Model->Frames().IsEmpty()) FrameList->RequestScrollIntoView(Model->Frames().Last()); }
			CurrentDetails = DetailTab == 0 ? Detail.Overview : DetailTab == 1 ? Detail.Request : DetailTab == 2 ? Detail.Response : DetailTab == 5 ? Model->StatsText() : DetailTab == 7 ? Detail.Connection : DetailTab == 6 ? Detail.FrameOverview : Detail.Timing;
			if (DetailTab == 4)
			{
				if (BodyView->SetSource(ActiveBody()))
				{ SelectedNode.Reset(); if (BodyTree) BodyTree->ClearSelection(); }
				CurrentDetails = BodyMode == 0 ? BodyView->Raw() : BodyMode == 2 ? BodyView->Hex() : BodyView->Pretty();
				if (BodyTree) BodyTree->RequestTreeRefresh();
				if (BodyView->IsPending()) Wake();
			}
			BodyMarshaller->Enable(DetailTab == 4 && BodyMode == 1 && BodyView->IsPrettyJson());
		}
		void Wake()
		{
			if (bTimer) return; bTimer = true;
			RegisterActiveTimer(.1f, FWidgetActiveTimerDelegate::CreateSP(this, &SVaRestInspector::Refresh));
		}
		EActiveTimerReturnType Refresh(double, float)
		{
			if (Model->Refresh(FPlatformTime::Seconds())) { ScopePicker->RefreshOptions(); List->RequestListRefresh(); Follow(); UpdateDetails(); }
			if (BodyView->Poll()) UpdateDetails();
			PollExport();
			if (Model->NeedsRefresh() || BodyView->IsPending() || ExportTask.IsValid()) return EActiveTimerReturnType::Continue;
			bTimer = false; return EActiveTimerReturnType::Stop;
		}
		TSharedPtr<FVaRestInspectorViewModel> Model;
		TSharedPtr<FVaRestInspectorUserSettings> Preferences;
		TSharedPtr<SHeaderRow> Header;
		TSharedPtr<SComboBox<FScope>> ScopePicker;
		TSharedPtr<SComboBox<FAttempt>> AttemptPicker;
		TSharedPtr<SVaRestInspectorWaterfall> Waterfall;
		TSharedPtr<SListView<FFrame>> FrameList;
		TSharedPtr<SListView<FStatsRow>> StatsList;
		TSharedPtr<SListView<FRow>> List;
		TSharedPtr<FVaRestInspectorBodyViewModel> BodyView;
		TSharedPtr<FVaRestInspectorJsonMarshaller> BodyMarshaller;
		TSharedPtr<STreeView<FBodyNode>> BodyTree;
		FBodyNode SelectedNode;
		FText CurrentDetails;
		FText ActionMessage;
		FVaRestInspectorExportTask ExportTask;
		FString ExportedPath;
		float FrameWidths[UE_ARRAY_COUNT(FrameColumns)]{};
		float StatsWidths[UE_ARRAY_COUNT(StatsColumns)]{};
		int32 StatsSort = 1;
		bool bStatsSortAscending = true;
		int32 DetailTab = 0;
		int32 BodyMode = 1;
		double LastScroll = 0, LastFrameScroll = 0;
		bool bTimer = false, bRequestBody = false, bFollow = true, bRequestAsBuilt = false, bHideEngineIo = true, bHideControl = false, bFrameFollow = true;
	};
}

class FVaRestInspectorUIModule final : public IVaRestInspectorUI
{
public:
	void StartupModule() override
	{
		Preferences = MakeShared<FVaRestInspectorUserSettings>(); Preferences->Load();
#if WITH_EDITOR
		if (GIsEditor) PrePIE = FEditorDelegates::PreBeginPIE.AddRaw(this, &FVaRestInspectorUIModule::OnPrePIE);
#endif
		OpenCommand = IConsoleManager::Get().RegisterConsoleCommand(TEXT("VaRest.Inspector.Open"), TEXT("Open VaRest Network Inspector"), FConsoleCommandDelegate::CreateRaw(this, &FVaRestInspectorUIModule::OpenFromConsole), ECVF_Default);
		if (GIsRunning) Register(); else PostInit = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FVaRestInspectorUIModule::Register);
	}
	void ShutdownModule() override
	{
		if (Preferences) Preferences->Save(true);
		FVaRestInspectorBodyViewModel::ShutdownAll();
		FCoreDelegates::GetOnPostEngineInit().Remove(PostInit);
		if (OpenCommand) IConsoleManager::Get().UnregisterConsoleObject(OpenCommand);
#if WITH_EDITOR
		FEditorDelegates::PreBeginPIE.Remove(PrePIE);
		UToolMenus::UnRegisterStartupCallback(MenuStartup); UToolMenus::UnregisterOwner(this);
#endif
		if (FSlateApplication::IsInitialized())
		{
			if (auto Tab = FGlobalTabmanager::Get()->FindExistingLiveTab(InspectorTab)) Tab->RequestCloseTab();
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(InspectorTab);
			if (auto Window = GameWindow.Pin()) Window->RequestDestroyWindow();
		}
		for (const auto& Weak : Models) if (auto Model = Weak.Pin()) Model->Detach(); Models.Reset();
	}
	bool SupportsDynamicReloading() override { return false; }
	bool Show() override
	{
		check(IsInGameThread());
		if (!FApp::CanEverRender() || FApp::IsUnattended() || !FSlateApplication::IsInitialized()) return false;
		Register();
		if (GIsEditor) { FGlobalTabmanager::Get()->TryInvokeTab(InspectorTab); return true; }
		if (auto Window = GameWindow.Pin()) { Window->BringToFront(); return true; }
		auto Window = SNew(SWindow).Title(FText::FromString(TEXT("VaRest Network Inspector"))).ClientSize(FVector2D(1400, 800))[Panel()];
		GameWindow = Window; FSlateApplication::Get().AddWindow(Window); return true;
	}
private:
	TSharedRef<SWidget> Panel()
	{
		Models.RemoveAll([](const auto& Weak) { return !Weak.IsValid(); });
		if (Models.Num() >= 16) return SNew(STextBlock).Text(FText::FromString(TEXT("Inspector view capacity is exhausted.")));
		IVaRestInspector::Get(); auto Model = FVaRestInspectorViewModel::Create(); Models.Add(Model); return SNew(SVaRestInspector).Model(Model).Preferences(Preferences);
	}
	void Register()
	{
		if (bRegistered || !FApp::CanEverRender() || FApp::IsUnattended() || !FSlateApplication::IsInitialized()) return;
		bRegistered = true;
		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(InspectorTab, FOnSpawnTab::CreateLambda([this](const FSpawnTabArgs&)
		{ return SNew(SDockTab).TabRole(ETabRole::NomadTab)[Panel()]; })).SetDisplayName(FText::FromString(TEXT("VaRest Network Inspector")));
#if WITH_EDITOR
		if (GIsEditor) MenuStartup = UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FVaRestInspectorUIModule::RegisterMenus));
#endif
	}
	void OpenFromConsole() { Show(); }
#if WITH_EDITOR
	void OnPrePIE(bool) { if (auto* Inspector = IVaRestInspector::TryGet()) Inspector->BeginPIESession(Preferences->bPreserve); }
	void RegisterMenus()
	{
		FToolMenuOwnerScoped Owner(this); auto* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools"));
		Menu->FindOrAddSection(TEXT("Debug")).AddMenuEntry(TEXT("VaRestInspector"), FText::FromString(TEXT("VaRest Network Inspector")), FText::FromString(TEXT("Inspect captured VaRest traffic")),
			FSlateIcon(), FUIAction(FExecuteAction::CreateRaw(this, &FVaRestInspectorUIModule::OpenFromConsole)));
	}
#endif
	IConsoleObject* OpenCommand = nullptr;
	TSharedPtr<FVaRestInspectorUserSettings> Preferences;
	FDelegateHandle PostInit, MenuStartup, PrePIE;
	TArray<TWeakPtr<FVaRestInspectorViewModel>> Models;
	TWeakPtr<SWindow> GameWindow;
	bool bRegistered = false;
};
bool IVaRestInspectorUI::Open()
{
	auto* Module = FModuleManager::LoadModulePtr<IVaRestInspectorUI>(TEXT("VaRestInspectorUI")); return Module && Module->Show();
}
IMPLEMENT_MODULE(FVaRestInspectorUIModule, VaRestInspectorUI)
