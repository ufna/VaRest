// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

class IPropertyHandle;
class SWidget;

/** Keeps an invalid old selection visible; choosing another API never silently rewrites Entry. */
class FVaRestSioComponentDetails final : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();
	virtual void CustomizeDetails(IDetailLayoutBuilder& Builder) override;
private:
	TSharedRef<SWidget> BuildEntries();
	FText CurrentEntry() const;
	void SelectEntry(FName Entry);
	TSharedPtr<IPropertyHandle> ApiProperty, EntryProperty;
};
