// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSioComponentDetails.h"
#include "VaRestSioComponent.h"
#include "VaRestSioEndpoint.h"
#include "VaRestApi.h"
#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "IDetailPropertyRow.h"
#include "PropertyHandle.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "VaRestSioComponentDetails"

TSharedRef<IDetailCustomization> FVaRestSioComponentDetails::MakeInstance() { return MakeShared<FVaRestSioComponentDetails>(); }
void FVaRestSioComponentDetails::CustomizeDetails(IDetailLayoutBuilder& Builder)
{
	ApiProperty = Builder.GetProperty(GET_MEMBER_NAME_CHECKED(UVaRestSocketComponent, Api), UVaRestSocketComponent::StaticClass());
	EntryProperty = Builder.GetProperty(GET_MEMBER_NAME_CHECKED(UVaRestSocketComponent, Entry), UVaRestSocketComponent::StaticClass());
	if (!ApiProperty->IsValidHandle() || !EntryProperty->IsValidHandle()) return;
	auto& Row = Builder.EditCategory(TEXT("VaRest|Socket")).AddProperty(EntryProperty);
	Row.CustomWidget().NameContent()[EntryProperty->CreatePropertyNameWidget()]
	.ValueContent().MinDesiredWidth(220)
	[
		SNew(SComboButton)
		.IsEnabled_Lambda([Property = EntryProperty] { return Property.IsValid() && Property->IsEditable(); })
		.OnGetMenuContent(this, &FVaRestSioComponentDetails::BuildEntries)
		.ButtonContent()[SNew(STextBlock).Text(this, &FVaRestSioComponentDetails::CurrentEntry)]
	];
}
FText FVaRestSioComponentDetails::CurrentEntry() const
{
	FName Entry;
	if (!EntryProperty || EntryProperty->GetValue(Entry) != FPropertyAccess::Success) return LOCTEXT("Multiple", "Multiple Values");
	return Entry.IsNone() ? LOCTEXT("None", "None") : FText::FromName(Entry);
}
void FVaRestSioComponentDetails::SelectEntry(FName Entry)
{ if (EntryProperty && EntryProperty->IsValidHandle() && EntryProperty->IsEditable()) EntryProperty->SetValue(Entry); }
TSharedRef<SWidget> FVaRestSioComponentDetails::BuildEntries()
{
	FMenuBuilder Menu(true, nullptr);
	Menu.AddMenuEntry(LOCTEXT("None", "None"), LOCTEXT("UseUrl", "Use the component URL when no API asset is selected."), FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &FVaRestSioComponentDetails::SelectEntry, FName(NAME_None))));
	UObject* Object = nullptr;
	const auto* Api = ApiProperty && ApiProperty->GetValue(Object) == FPropertyAccess::Success ? Cast<UVaRestApi>(Object) : nullptr;
	if (!Api)
	{
		Menu.AddWidget(SNew(STextBlock).Text(LOCTEXT("ChooseApi", "Select one API asset to list its Socket.IO entries.")), FText::GetEmpty());
		return Menu.MakeWidget();
	}
	TMap<FName, int32> Counts;
	for (const auto& Candidate : Api->Sockets) if (Candidate) ++Counts.FindOrAdd(Candidate->Name);
	bool bAny = false;
	for (const auto& Candidate : Api->Sockets)
	{
		const auto* Endpoint = Cast<UVaRestSioEndpoint>(Candidate.Get());
		if (!Endpoint || Endpoint->Name.IsNone() || Counts.FindRef(Endpoint->Name) != 1) continue;
		bAny = true;
		Menu.AddMenuEntry(FText::FromName(Endpoint->Name), FText::FromString(Endpoint->Description), FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &FVaRestSioComponentDetails::SelectEntry, Endpoint->Name)));
	}
	if (!bAny) Menu.AddWidget(SNew(STextBlock).Text(LOCTEXT("NoEntries", "This API has no unambiguous Socket.IO entries.")), FText::GetEmpty());
	return Menu.MakeWidget();
}

#undef LOCTEXT_NAMESPACE
