// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestApiTypes.h"

#include "UObject/Class.h"
#include "VaRestBody.h"
#include "VaRestUrl.h"

#define LOCTEXT_NAMESPACE "VaRestApi"

void FVaRestApiValidationContext::Error(FName Code, FStringView PropertyPath, FText Message)
{
	FVaRestApiValidationIssue Issue;
	Issue.Code = Code;
	Issue.PropertyPath = FString(PropertyPath);
	Issue.Message = MoveTemp(Message);
	Issues.Add(MoveTemp(Issue));
}

void FVaRestApiValidationContext::Warning(FName Code, FStringView PropertyPath, FText Message)
{
	Error(Code, PropertyPath, MoveTemp(Message));
	Issues.Last().bWarning = true;
}

bool FVaRestApiValidationContext::HasErrors() const
{
	return Issues.ContainsByPredicate([](const FVaRestApiValidationIssue& Issue) { return !Issue.bWarning; });
}

FVaRestTypeRef FVaRestTypeRef::Json()
{
	FVaRestTypeRef Result;
	Result.Kind = EVaRestTypeKind::Json;
	return Result;
}

bool FVaRestTypeRef::Validate(FVaRestApiValidationContext& Context, FStringView PropertyPath, bool bParameter) const
{
	auto Refuse = [&](FName Code, FText Message) {
		Context.Error(Code, PropertyPath, MoveTemp(Message));
		return false;
	};
	if (static_cast<uint8>(Kind) > static_cast<uint8>(EVaRestTypeKind::DateTime) || static_cast<uint8>(Container) > static_cast<uint8>(EVaRestTypeContainer::Map))
	{
		return Refuse(TEXT("Type.InvalidKind"), LOCTEXT("InvalidTypeKind", "The type or container selection is invalid."));
	}
	if ((Kind == EVaRestTypeKind::Struct && (!Type || !Type->IsA<UScriptStruct>())) || (Kind == EVaRestTypeKind::Enum && (!Type || !Type->IsA<UEnum>())) || (Kind != EVaRestTypeKind::Struct && Kind != EVaRestTypeKind::Enum && Type))
	{
		return Refuse(TEXT("Type.ObjectMismatch"), LOCTEXT("TypeObjectMismatch", "Struct requires a script struct and Enum requires an enum. Other kinds must not specify a type object."));
	}
	if ((Kind == EVaRestTypeKind::None || Kind == EVaRestTypeKind::Bytes) && Container != EVaRestTypeContainer::None)
	{
		return Refuse(TEXT("Type.InvalidContainer"), LOCTEXT("InvalidTypeContainer", "None and Bytes cannot have an Array or Map container. Bytes already denotes a byte array."));
	}
	if (bJsonValue && (Container != EVaRestTypeContainer::None || (Kind != EVaRestTypeKind::String && Kind != EVaRestTypeKind::Bytes) || bParameter))
	{
		return Refuse(TEXT("Type.InvalidJsonValue"), LOCTEXT("InvalidJsonValue", "JSON value mode requires a scalar String or Bytes response/body descriptor, not a parameter."));
	}
	if (bParameter && (Container == EVaRestTypeContainer::Map || Kind == EVaRestTypeKind::None || Kind == EVaRestTypeKind::Json || Kind == EVaRestTypeKind::Struct || Kind == EVaRestTypeKind::Bytes || Kind == EVaRestTypeKind::DateTime))
	{
		return Refuse(TEXT("Type.InvalidParameter"), LOCTEXT("InvalidParameterType", "A parameter requires a string, bool, number or enum, optionally as one array."));
	}
	return true;
}

void FVaRestEndpointParam::Validate(FVaRestApiValidationContext& Context, FStringView PropertyPath) const
{
	bool bContainsNull = false;
	for (int32 Index = 0; Index < Name.Len(); ++Index)
	{
		bContainsNull |= Name[Index] == TCHAR(0);
	}
	if (Name.IsEmpty() || bContainsNull)
	{
		Context.Error(TEXT("Param.InvalidName"), PropertyPath, LOCTEXT("InvalidParameterName", "A parameter requires a nonempty wire name without a null character."));
	}
	if (PinName.IsNone())
	{
		Context.Error(TEXT("Param.MissingPinName"), PropertyPath, LOCTEXT("MissingPinName", "A parameter requires a stable Blueprint pin name."));
	}
	if (static_cast<uint8>(In) > static_cast<uint8>(EVaRestEndpointParamLocation::Header))
	{
		Context.Error(TEXT("Param.InvalidLocation"), PropertyPath, LOCTEXT("InvalidParameterLocation", "The parameter location is invalid."));
	}
	if (In == EVaRestEndpointParamLocation::Header && !VaRest::Http::IsValidHeaderName(Name))
	{
		Context.Error(TEXT("Param.InvalidHeaderName"), PropertyPath, LOCTEXT("InvalidHeaderParameter", "A header parameter name must be an HTTP token."));
	}
	Type.Validate(Context, FString(PropertyPath) + TEXT(".Type"), true);
	if (static_cast<uint8>(Style) > static_cast<uint8>(EVaRestQueryStyle::Pipes))
	{
		Context.Error(TEXT("Param.InvalidStyle"), PropertyPath, LOCTEXT("InvalidParameterStyle", "The parameter array style is invalid."));
	}
	if (Type.Container == EVaRestTypeContainer::Array && In != EVaRestEndpointParamLocation::Query && Style != EVaRestQueryStyle::Csv)
	{
		Context.Error(TEXT("Param.InvalidArrayStyle"), PropertyPath, LOCTEXT("InvalidParameterArrayStyle", "Path and header arrays use comma-separated values."));
	}
}

FVaRestApiEndpoint::FVaRestApiEndpoint()
	: Id(FGuid::NewGuid())
	, ResponseType(FVaRestTypeRef::Json())
{
}

bool VaRest::Http::TryGetNativeBodyKind(EVaRestEndpointBodyKind Kind, EVaRestBodyKind& OutKind)
{
	switch (Kind)
	{
	case EVaRestEndpointBodyKind::Json: OutKind = EVaRestBodyKind::Json; return true;
	case EVaRestEndpointBodyKind::Form: OutKind = EVaRestBodyKind::Form; return true;
	case EVaRestEndpointBodyKind::Multipart: OutKind = EVaRestBodyKind::Multipart; return true;
	case EVaRestEndpointBodyKind::Binary: OutKind = EVaRestBodyKind::Binary; return true;
	case EVaRestEndpointBodyKind::Text: OutKind = EVaRestBodyKind::Text; return true;
	default: return false;
	}
}

TArray<FVaRestHeader> VaRest::Http::ToHeaderData(const FVaRestHeaders& Headers)
{
	TArray<FVaRestHeader> Result;
	Result.Reserve(Headers.Num());
	for (const auto& Pair : Headers)
	{
		FVaRestHeader Header;
		Header.Name = Pair.Key;
		Header.Value = Pair.Value;
		Result.Add(MoveTemp(Header));
	}
	return Result;
}

FVaRestHeaders VaRest::Http::ToNativeHeaders(TConstArrayView<FVaRestHeader> Headers)
{
	FVaRestHeaders Result;
	for (const auto& Header : Headers)
	{
		Result.Add(Header.Name, Header.Value);
	}
	return Result;
}

#undef LOCTEXT_NAMESPACE
