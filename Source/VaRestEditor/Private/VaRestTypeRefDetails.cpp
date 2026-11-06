// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestTypeRefDetails.h"
#include "VaRestJson.h"
#include "EdGraphSchema_K2.h"
#include "DetailWidgetRow.h"
#include "IDetailChildrenBuilder.h"
#include "IDetailPropertyRow.h"
#include "PropertyHandle.h"
#include "UObject/NoExportTypes.h"
#include "Widgets/Text/STextBlock.h"

namespace VaRest::Http::Editor
{
bool IsTransferredType(const FVaRestTypeRef& Type)
{
	return Type.bJsonValue || Type.Container!=EVaRestTypeContainer::None || (Type.Kind!=EVaRestTypeKind::None && Type.Kind!=EVaRestTypeKind::Json &&
		Type.Kind!=EVaRestTypeKind::String && Type.Kind!=EVaRestTypeKind::Bytes);
}
bool TypeToPin(const FVaRestTypeRef& Type,FEdGraphPinType& Pin,FString& Error)
{
	FVaRestApiValidationContext Validation;
	if (!Type.Validate(Validation,TEXTVIEW("Type")) || Type.Kind==EVaRestTypeKind::None) { Error=TEXT("missing or invalid VaRest type descriptor"); return false; }
	Pin=FEdGraphPinType();
	switch (Type.Kind)
	{
	case EVaRestTypeKind::Json:Pin.PinCategory=UEdGraphSchema_K2::PC_Struct; Pin.PinSubCategoryObject=FVaRestJson::StaticStruct();break;
	case EVaRestTypeKind::Struct:Pin.PinCategory=UEdGraphSchema_K2::PC_Struct;Pin.PinSubCategoryObject=Cast<UScriptStruct>(Type.Type);break;
	case EVaRestTypeKind::DateTime:Pin.PinCategory=UEdGraphSchema_K2::PC_Struct;Pin.PinSubCategoryObject=TBaseStructure<FDateTime>::Get();break;
	case EVaRestTypeKind::String:Pin.PinCategory=UEdGraphSchema_K2::PC_String;break;
	case EVaRestTypeKind::Bytes:Pin.PinCategory=UEdGraphSchema_K2::PC_Byte;Pin.ContainerType=EPinContainerType::Array;return true;
	case EVaRestTypeKind::Bool:Pin.PinCategory=UEdGraphSchema_K2::PC_Boolean;break;
	case EVaRestTypeKind::Int:Pin.PinCategory=UEdGraphSchema_K2::PC_Int;break;
	case EVaRestTypeKind::Int64:Pin.PinCategory=UEdGraphSchema_K2::PC_Int64;break;
	case EVaRestTypeKind::Double:Pin.PinCategory=UEdGraphSchema_K2::PC_Real;Pin.PinSubCategory=UEdGraphSchema_K2::PC_Double;break;
	case EVaRestTypeKind::Enum:Pin.PinCategory=UEdGraphSchema_K2::PC_Byte;Pin.PinSubCategoryObject=Cast<UEnum>(Type.Type);break;
	default:Error=TEXT("unsupported VaRest type descriptor");return false;
	}
	if (Type.Container==EVaRestTypeContainer::Array) Pin.ContainerType=EPinContainerType::Array;
	else if (Type.Container==EVaRestTypeContainer::Map)
	{
		Pin.PinValueType.TerminalCategory=Pin.PinCategory;Pin.PinValueType.TerminalSubCategory=Pin.PinSubCategory;
		Pin.PinValueType.TerminalSubCategoryObject=Pin.PinSubCategoryObject;
		Pin.PinCategory=UEdGraphSchema_K2::PC_String;Pin.PinSubCategory=NAME_None;Pin.PinSubCategoryObject=nullptr;Pin.ContainerType=EPinContainerType::Map;
	}
	return true;
}
}
TSharedRef<IPropertyTypeCustomization> FVaRestTypeRefDetails::MakeInstance() { return MakeShared<FVaRestTypeRefDetails>(); }
void FVaRestTypeRefDetails::CustomizeHeader(TSharedRef<IPropertyHandle> Property,FDetailWidgetRow& Row,IPropertyTypeCustomizationUtils&)
{
	Row.NameContent()[Property->CreatePropertyNameWidget()];
	Row.ValueContent()[SNew(STextBlock).Text_Lambda([Property] {
		uint8 Kind=0,Container=0;Property->GetChildHandle(GET_MEMBER_NAME_CHECKED(FVaRestTypeRef,Kind))->GetValue(Kind);
		Property->GetChildHandle(GET_MEMBER_NAME_CHECKED(FVaRestTypeRef,Container))->GetValue(Container);
		FString Name=StaticEnum<EVaRestTypeKind>()->GetNameStringByValue(Kind);
		if (Container==static_cast<uint8>(EVaRestTypeContainer::Array)) Name=FString(TEXT("Array<"))+Name+TEXT(">");
		else if (Container==static_cast<uint8>(EVaRestTypeContainer::Map)) Name=FString(TEXT("Map<String, "))+Name+TEXT(">");
		bool bJsonValue=false;Property->GetChildHandle(GET_MEMBER_NAME_CHECKED(FVaRestTypeRef,bJsonValue))->GetValue(bJsonValue);
		if (bJsonValue) Name=TEXT("Json<")+Name+TEXT(">");
		return FText::FromString(Name);
	})];
}
void FVaRestTypeRefDetails::CustomizeChildren(TSharedRef<IPropertyHandle> Property,IDetailChildrenBuilder& Builder,IPropertyTypeCustomizationUtils&)
{
	const auto Kind=Property->GetChildHandle(GET_MEMBER_NAME_CHECKED(FVaRestTypeRef,Kind));
	const auto Object=Property->GetChildHandle(GET_MEMBER_NAME_CHECKED(FVaRestTypeRef,Type));
	const auto Container=Property->GetChildHandle(GET_MEMBER_NAME_CHECKED(FVaRestTypeRef,Container));
	const auto JsonValue=Property->GetChildHandle(GET_MEMBER_NAME_CHECKED(FVaRestTypeRef,bJsonValue));
	// Do not leave an invisible, incompatible object/container behind when switching the descriptor kind.
	const auto Normalize=FSimpleDelegate::CreateLambda([WeakKind=TWeakPtr<IPropertyHandle>(Kind),WeakObject=TWeakPtr<IPropertyHandle>(Object),WeakContainer=TWeakPtr<IPropertyHandle>(Container),WeakJson=TWeakPtr<IPropertyHandle>(JsonValue)] {
		const auto CurrentKind=WeakKind.Pin(),CurrentObject=WeakObject.Pin(),CurrentContainer=WeakContainer.Pin();
		const auto CurrentJson=WeakJson.Pin();if (!CurrentKind || !CurrentObject || !CurrentContainer || !CurrentJson) return;
		uint8 Value=0;
		if (CurrentKind->GetValue(Value)!=FPropertyAccess::Success) return;
		UObject* Selected=nullptr;
		if (CurrentObject->GetValue(Selected)==FPropertyAccess::Success && Selected &&
			!((Value==static_cast<uint8>(EVaRestTypeKind::Struct) && Selected->IsA<UScriptStruct>()) ||
			  (Value==static_cast<uint8>(EVaRestTypeKind::Enum) && Selected->IsA<UEnum>())))
			CurrentObject->SetValue(static_cast<UObject*>(nullptr));
		uint8 ContainerValue=0;CurrentContainer->GetValue(ContainerValue);
		if ((Value==static_cast<uint8>(EVaRestTypeKind::None) || Value==static_cast<uint8>(EVaRestTypeKind::Bytes)) && ContainerValue!=static_cast<uint8>(EVaRestTypeContainer::None))
		{ CurrentContainer->SetValue(static_cast<uint8>(EVaRestTypeContainer::None));ContainerValue=static_cast<uint8>(EVaRestTypeContainer::None); }
		bool bJsonValue=false;CurrentJson->GetValue(bJsonValue);
		if (bJsonValue && (ContainerValue!=static_cast<uint8>(EVaRestTypeContainer::None) || (Value!=static_cast<uint8>(EVaRestTypeKind::String) && Value!=static_cast<uint8>(EVaRestTypeKind::Bytes)))) CurrentJson->SetValue(false);
	});
	Kind->SetOnPropertyValueChanged(Normalize);Container->SetOnPropertyValueChanged(Normalize);
	Builder.AddProperty(Kind.ToSharedRef());
	Builder.AddProperty(Object.ToSharedRef()).Visibility(TAttribute<EVisibility>::CreateLambda([Kind] {
		uint8 Value=0;Kind->GetValue(Value);return Value==static_cast<uint8>(EVaRestTypeKind::Struct) || Value==static_cast<uint8>(EVaRestTypeKind::Enum) ? EVisibility::Visible : EVisibility::Collapsed;
	}));
	Builder.AddProperty(Container.ToSharedRef()).IsEnabled(TAttribute<bool>::CreateLambda([Kind] {
		uint8 Value=0;return Kind->GetValue(Value)==FPropertyAccess::Success && Value!=static_cast<uint8>(EVaRestTypeKind::None) && Value!=static_cast<uint8>(EVaRestTypeKind::Bytes);
	}));
	Builder.AddProperty(JsonValue.ToSharedRef()).IsEnabled(TAttribute<bool>::CreateLambda([Kind,Container] {
		uint8 Value=0,ContainerValue=0;
		return Kind->GetValue(Value)==FPropertyAccess::Success && Container->GetValue(ContainerValue)==FPropertyAccess::Success &&
			ContainerValue==static_cast<uint8>(EVaRestTypeContainer::None) && (Value==static_cast<uint8>(EVaRestTypeKind::String) || Value==static_cast<uint8>(EVaRestTypeKind::Bytes));
	}));
}
