// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "IPropertyTypeCustomization.h"
#include "VaRestApiTypes.h"
#include "EdGraph/EdGraphPin.h"

namespace VaRest::Http::Editor
{
bool TypeToPin(const FVaRestTypeRef& Type,FEdGraphPinType& Pin,FString& Error);
bool IsTransferredType(const FVaRestTypeRef& Type);
}

class FVaRestTypeRefDetails final : public IPropertyTypeCustomization
{
public:
	static TSharedRef<IPropertyTypeCustomization> MakeInstance();
	virtual void CustomizeHeader(TSharedRef<IPropertyHandle> Property,FDetailWidgetRow& Row,IPropertyTypeCustomizationUtils& Utils) override;
	virtual void CustomizeChildren(TSharedRef<IPropertyHandle> Property,IDetailChildrenBuilder& Builder,IPropertyTypeCustomizationUtils& Utils) override;
};
