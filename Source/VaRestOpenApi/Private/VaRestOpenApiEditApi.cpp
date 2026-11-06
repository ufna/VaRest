// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiApplyPrivate.h"
#include "VaRestOpenApiReimportValues.h"

namespace VaRest::OpenApi
{
void FApply::EditApi(TConstArrayView<FVaRestOpenApiMergeItem*> Items,const TStringMap<const FVaRestOpenApiMergeItem*>& Live)
{
	auto& Asset=*Result.Api;
	TStringMap<FString> EnvironmentNames;
	const auto Changed=[&](const FVaRestOpenApiMergeItem& Item,const TCHAR* Property)
	{
		const auto* Old=Live.Find(Item.Id);return !Old || !(*Old)->bPresent || !(*Old)->Properties[Property].Equals(Item.Properties[Property]);
	};
	const auto ReadType=[&](const FVaRestOpenApiMergeItem& Item,const TCHAR* Property,FVaRestTypeRef& Out)
	{
		const auto Planned=PlannedType(Item.Properties[Property]);
		if (Planned.IsSet() && (Changed(Item,Property) || FailedTypes.Contains(Planned.GetValue().DefinitionId)))
		{
			Out=Type(Planned.GetValue());
			if (Planned.GetValue().Kind==EVaRestOpenApiValueKind::Enum && Out.Kind==EVaRestTypeKind::String && Out.Container==EVaRestTypeContainer::None &&
				(Equal(Property,TEXT("responseType")) || Equal(Property,TEXT("errorType")) || (Equal(Property,TEXT("requestType")) && Equal(Item.Properties[TEXT("bodyKind")].AsString(),TEXT("Json"))))) Out.bJsonValue=true;
		}
	};
	for (auto* Item:Items) if (Item->bPresent)
	{
		const auto& Properties=Item->Properties;
		if (Equal(Item->Kind,TEXT("endpoint")))
		{
			FGuid Guid;FGuid::Parse(Item->Target,Guid);auto* Endpoint=Asset.Endpoints.FindByPredicate([&](const auto& Row) { return Row.Id==Guid; });
			if (!Endpoint) { FVaRestApiEndpoint Row;if (Guid.IsValid()) Row.Id=Guid;Endpoint=&Asset.Endpoints.Add_GetRef(MoveTemp(Row)); }
			const FString Base=Properties[TEXT("name")].AsString();FString Name=Base;int32 Suffix=2;
			if (Changed(*Item,TEXT("name"))) while (Asset.Endpoints.ContainsByPredicate([&](const auto& Row) { return Row.Id!=Endpoint->Id && Row.Name==FName(*Name); })) Name=Base.Left(116)+TEXT("_")+LexToString(Suffix++);
			Endpoint->Name=*Name;Item->Properties.Set(TEXT("name"),Name);Endpoint->Verb=Properties[TEXT("verb")].AsString();Endpoint->Path=Properties[TEXT("path")].AsString();Endpoint->Category=Properties[TEXT("category")].AsString();Endpoint->Description=Properties[TEXT("description")].AsString();
			Endpoint->bDeprecated=Properties[TEXT("deprecated")].AsBool();Endpoint->DeprecationMessage=Properties[TEXT("deprecationMessage")].AsString();Endpoint->bIdempotent=Properties[TEXT("idempotent")].AsBool();
			Endpoint->BodyKind=NativeEnum(Properties[TEXT("bodyKind")],Endpoint->BodyKind);Endpoint->BodyContentType=Properties[TEXT("contentType")].AsString();Endpoint->AuthMode=NativeEnum(Properties[TEXT("authMode")],Endpoint->AuthMode);
			ReadType(*Item,TEXT("requestType"),Endpoint->RequestType);ReadType(*Item,TEXT("responseType"),Endpoint->ResponseType);ReadType(*Item,TEXT("errorType"),Endpoint->ErrorType);
			Endpoint->bOverrideEnvelope=Properties[TEXT("overrideEnvelope")].AsBool();ReadEnvelope(Properties[TEXT("envelope")],Endpoint->Envelope);Item->Target=Endpoint->Id.ToString();Endpoints.Add(Item->Id,Endpoint->Id);
		}
		else if (Equal(Item->Kind,TEXT("environment")))
		{
			const auto* Old=Live.Find(Item->Id);const FString OldName=Old ? (*Old)->Properties[TEXT("name")].AsString() : Item->Target;
			auto* Environment=Asset.Environments.FindByPredicate([&](const auto& Row) { return Row.Name==FName(*OldName); });
			if (Environment && (!Old || !(*Old)->bPresent)) { Item->bPresent=false;Issue(TEXT("OA-REIMPORT-USERCOLLISION"),TEXT("A user-owned environment occupies the generated name ")+OldName,&Asset,EVaRestOpenApiSeverity::Error);continue; }
			if (!Environment) Environment=&Asset.Environments.Add_GetRef(FVaRestApiEnvironment());
			const FString Base=Properties[TEXT("name")].AsString();FString Name=Base;int32 Suffix=2;
			if (Changed(*Item,TEXT("name"))) while (Asset.Environments.ContainsByPredicate([&](const auto& Row) { return &Row!=Environment && Row.Name==FName(*Name); })) Name=Base+TEXT("_")+LexToString(Suffix++);
			Environment->Name=*Name;Environment->BaseUrl=Properties[TEXT("url")].AsString();Item->Target=Name;Item->Properties.Set(TEXT("name"),Name);EnvironmentNames.Add(Base,Name);
		}
		else if (Equal(Item->Kind,TEXT("defaults")))
		{
			FString Name=Properties[TEXT("defaultEnvironment")].AsString();if (Changed(*Item,TEXT("defaultEnvironment"))) if (const auto* Actual=EnvironmentNames.Find(Name)) Name=*Actual;Asset.DefaultEnvironment=*Name;Item->Properties.Set(TEXT("defaultEnvironment"),Name);
		}
		else if (Equal(Item->Kind,TEXT("envelope"))) ReadEnvelope(Properties,Asset.Envelope);
		else if (Equal(Item->Kind,TEXT("auth")))
		{
			const auto* Old=Live.Find(Item->Id);if (!Old || !(*Old)->Properties.Equals(Properties)) ReadAuth(Properties,Asset);
		}
	}
	// Retire old pins before admitting replacements that may keep the same normalized header wire name.
	for (auto* Item:Items) if (Item->bPresent && Equal(Item->Kind,TEXT("param")) && Item->Properties[TEXT("removed")].AsBool())
	{
		const auto* Old=Live.Find(Item->Id);const auto* Guid=Endpoints.Find(Item->Owner);
		if (Old && (*Old)->bPresent && Guid) if (auto* Endpoint=Asset.Endpoints.FindByPredicate([&](const auto& Row) { return Row.Id==*Guid; }))
			if (auto* Param=Endpoint->Params.FindByPredicate([&](const auto& Row) { return Row.PinName==FName(*(*Old)->Target); })) Param->bRemoved=true;
	}
	for (auto* Item:Items) if (Item->bPresent && Equal(Item->Kind,TEXT("param")))
	{
		const auto* EndpointGuid=Endpoints.Find(Item->Owner);auto* Endpoint=EndpointGuid ? Asset.Endpoints.FindByPredicate([&](const auto& Row) { return Row.Id==*EndpointGuid; }) : nullptr;
		if (!Endpoint) { Item->bPresent=false;continue; }
		const auto& Properties=Item->Properties;const auto* Old=Live.Find(Item->Id);const FString OldPin=Old ? (*Old)->Target : Item->Target;
		auto* Param=Endpoint->Params.FindByPredicate([&](const auto& Row) { return Row.PinName==FName(*OldPin); });
		const FString Wire=Properties[TEXT("wire")].AsString(),Location=Properties[TEXT("in")].AsString();
		const auto NewLocation=Equal(Location,TEXT("path")) ? EVaRestEndpointParamLocation::Path : Equal(Location,TEXT("header")) ? EVaRestEndpointParamLocation::Header : EVaRestEndpointParamLocation::Query;
		if (!Param || !Old || !(*Old)->bPresent)
		{
			// An unrelated user parameter with this pin is not the missing generated item.
			if (Param)
			{
				bool ImportedOther=false;
				for (const auto& Entry:Live) if (Entry.Value->bPresent && Equal(Entry.Value->Kind,TEXT("param")) && Equal(Entry.Value->Owner,Item->Owner) && !Equal(Entry.Key,Item->Id) && FName(*Entry.Value->Target)==Param->PinName) { ImportedOther=true;break; }
				if (!ImportedOther) { Item->bPresent=false;Issue(TEXT("OA-REIMPORT-USERCOLLISION"),TEXT("A user-owned parameter occupies the generated pin ")+OldPin,&Asset,EVaRestOpenApiSeverity::Error);continue; }
			}
			if (Endpoint->Params.ContainsByPredicate([&](const auto& Row) { return !Row.bRemoved && Row.In==NewLocation && (NewLocation==EVaRestEndpointParamLocation::Header ? Row.Name.Equals(Wire,ESearchCase::IgnoreCase) : Equal(Row.Name,Wire)); })) { Item->bPresent=false;Issue(TEXT("OA-REIMPORT-USERCOLLISION"),TEXT("An active parameter occupies the generated wire identity ")+Wire,&Asset,EVaRestOpenApiSeverity::Error);continue; }
			Param=&Endpoint->Params.Add_GetRef(FVaRestEndpointParam());
		}
		Param->Name=Wire;const FString Base=Properties[TEXT("pin")].AsString();FString Pin=Base;int32 Suffix=2;
		if (Changed(*Item,TEXT("pin"))) while (Endpoint->Params.ContainsByPredicate([&](const auto& Row) { return &Row!=Param && Row.PinName==FName(*Pin); })) Pin=Base.Left(116)+TEXT("_")+LexToString(Suffix++);
		Param->PinName=*Pin;Item->Properties.Set(TEXT("pin"),Pin);Param->In=NewLocation;
		ReadType(*Item,TEXT("type"),Param->Type);Param->bRequired=Properties[TEXT("required")].AsBool();Param->Default=Properties[TEXT("defaultText")].AsString();Param->Description=Properties[TEXT("description")].AsString();Param->Style=NativeEnum(Properties[TEXT("style")],Param->Style);Param->bRemoved=Properties[TEXT("removed")].AsBool();Item->Target=Param->PinName.ToString();
	}
}
}
