// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiImportDialog.h"
#include "VaRestOpenApiUrlPolicy.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace VaRest::OpenApi
{
bool ShowSourceUrl(FString& Url,FString* Destination)
{
	check(IsInGameThread());if (!FSlateApplication::IsInitialized()) return false;
	bool Accepted=false;FString Value=Url,Package=Destination ? *Destination : FString(),Error;TSharedPtr<SWindow> Window;
	SAssignNew(Window,SWindow).Title(Destination ? NSLOCTEXT("VaRestOpenApi","UrlImportTitle","Import OpenAPI from URL") : NSLOCTEXT("VaRestOpenApi","UrlChangeTitle","Change OpenAPI URL source"))
		.ClientSize(FVector2D(680,280)).SupportsMaximize(false).SupportsMinimize(false)
	[
		SNew(SVerticalBox)
		+SVerticalBox::Slot().AutoHeight().Padding(12,8)[SNew(STextBlock).Text(NSLOCTEXT("VaRestOpenApi","SourceUrlLabel","Source URL (HTTP or HTTPS)"))]
		+SVerticalBox::Slot().AutoHeight().Padding(12,0)[SNew(SEditableTextBox).Text(FText::FromString(Value)).OnTextChanged_Lambda([&](const FText& Text) { Value=Text.ToString();Error.Reset(); })]
		+SVerticalBox::Slot().AutoHeight().Padding(12,8)
		[
			SNew(SVerticalBox).Visibility(Destination ? EVisibility::Visible : EVisibility::Collapsed)
			+SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(NSLOCTEXT("VaRestOpenApi","UrlDestination","Destination asset package (for example /Game/Backend/Api)"))]
			+SVerticalBox::Slot().AutoHeight()[SNew(SEditableTextBox).Text(FText::FromString(Package)).OnTextChanged_Lambda([&](const FText& Text) { Package=Text.ToString();Error.Reset(); })]
		]
		+SVerticalBox::Slot().AutoHeight().Padding(12,8)[SNew(STextBlock).AutoWrapText(true).Text(NSLOCTEXT("VaRestOpenApi","UrlHeadersHint","Private spec? Set fetch headers in Editor Preferences → Plugins → VaRest OpenAPI Sources. Headers are personal and are never stored in this asset."))]
		+SVerticalBox::Slot().FillHeight(1).Padding(12,0)[SNew(STextBlock).AutoWrapText(true).Text_Lambda([&]() { return FText::FromString(Error); })]
		+SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(12,8)
		[
			SNew(SHorizontalBox)
			+SHorizontalBox::Slot().AutoWidth().Padding(4)[SNew(SButton).Text(NSLOCTEXT("VaRestOpenApi","Cancel","Cancel")).OnClicked_Lambda([&]() { Window->RequestDestroyWindow();return FReply::Handled(); })]
			+SHorizontalBox::Slot().AutoWidth().Padding(4)[SNew(SButton).Text(Destination ? NSLOCTEXT("VaRestOpenApi","UrlContinue","Continue") : NSLOCTEXT("VaRestOpenApi","UrlSetSource","Change source")).OnClicked_Lambda([&]()
			{
				Value.TrimStartAndEndInline();Package.TrimStartAndEndInline();const auto Parsed=FSourceUrl::Parse(Value);
				if (!Parsed.IsSet()) { Error=TEXT("Enter an absolute HTTP(S) URL without user-info credentials.");return FReply::Handled(); }
				if (Destination && (Package.Len()>=NAME_SIZE || !FPackageName::IsValidLongPackageName(Package) || FPackageName::DoesPackageExist(Package) || FindPackage(nullptr,*Package)))
				{ Error=TEXT("Choose a valid unused asset package. Use reimport to update an existing API.");return FReply::Handled(); }
				Url=Parsed.GetValue().Uri;if (Destination) *Destination=Package;Accepted=true;Window->RequestDestroyWindow();return FReply::Handled();
			})]
		]
	];
	FSlateApplication::Get().AddModalWindow(Window.ToSharedRef(),nullptr);return Accepted;
}
}
