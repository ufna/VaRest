// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiApplyPrivate.h"
#include "VaRestOpenApiSource.h"
#include "VaRestOpenApiMerge.h"
#include "VaRestOpenApiSnapshots.h"
#include "Engine/UserDefinedEnum.h"
#include "Kismet2/StructureEditorUtils.h"
#include "UObject/UnrealType.h"
#include "VaRestBuiltinAuthConfigs.h"

namespace VaRest::OpenApi
{
namespace
{
EVaRestJsonPresence Presence(EVaRestOpenApiPresence Value)
{
	switch (Value)
	{
	case EVaRestOpenApiPresence::OmitWhenDefault:return EVaRestJsonPresence::OmitWhenDefault;
	case EVaRestOpenApiPresence::NullWhenDefault:return EVaRestJsonPresence::NullWhenDefault;
	case EVaRestOpenApiPresence::DecodeOnly:return EVaRestJsonPresence::DecodeOnly;
	case EVaRestOpenApiPresence::Ignore:return EVaRestJsonPresence::Ignore;
	default:return EVaRestJsonPresence::Always;
	}
}
}
void FApply::Api()
{
	Result.Api=NewObject<UVaRestApi>(Options.Parent,Options.ApiName,Options.Flags|RF_Public|RF_Standalone|RF_Transactional);Keep(Result.Api);auto& Asset=*Result.Api;
	Asset.TypesFolder=Folder;Asset.Naming=EVaRestJsonNaming::AsIs;
	for (const auto& Row:Plan.Environments) { FVaRestApiEnvironment Environment;Environment.Name=*Row.Name;Environment.BaseUrl=Row.BaseUrl;Asset.Environments.Add(MoveTemp(Environment)); }
	Asset.DefaultEnvironment=*Plan.DefaultEnvironment;
	Asset.Envelope.DataPath=Plan.Envelope.DataPath;Asset.Envelope.SuccessPath=Plan.Envelope.SuccessPath;
	if (!Plan.Envelope.SuccessPath.IsEmpty()) Asset.Envelope.SuccessValues={TEXT("true")};
	Asset.Envelope.ErrorMessagePath=Plan.Envelope.ErrorMessagePath;Asset.Envelope.ErrorCodePath=Plan.Envelope.ErrorCodePath;Asset.Envelope.bParseErrorBodies=Plan.Envelope.bParseErrorBodies;
	switch (Plan.Auth.Kind)
	{
	case EVaRestOpenApiAuthKind::Bearer:Asset.Auth=NewObject<UVaRestBearerAuthConfig>(&Asset);break;
	case EVaRestOpenApiAuthKind::Basic:Asset.Auth=NewObject<UVaRestBasicAuthConfig>(&Asset);break;
	case EVaRestOpenApiAuthKind::ApiKey:
	{
		auto* Auth=NewObject<UVaRestApiKeyAuthConfig>(&Asset);Auth->Name=Plan.Auth.Name;Auth->Placement=Equal(Plan.Auth.Placement,TEXT("query")) ? EVaRestApiKeyPlacement::Query : EVaRestApiKeyPlacement::Header;Asset.Auth=Auth;break;
	}
	case EVaRestOpenApiAuthKind::BearerRefresh:
	{
		auto* Auth=NewObject<UVaRestBearerRefreshAuthConfig>(&Asset);Auth->Config.RefreshPath=Plan.Auth.TokenUrl;Auth->Config.Style=EVaRestRefreshStyle::OAuth2Form;
		Auth->Config.AccessTokenPath=TEXT("access_token");Auth->Config.RefreshTokenPath=TEXT("refresh_token");Auth->Config.ExpiresInPath=TEXT("expires_in");Asset.Auth=Auth;break;
	}
	case EVaRestOpenApiAuthKind::ClientCredentials:
	{
		auto* Auth=NewObject<UVaRestClientCredentialsAuthConfig>(&Asset);Auth->Config.TokenPath=Plan.Auth.TokenUrl;Asset.Auth=Auth;break;
	}
	default:Asset.Auth=nullptr;break;
	}
	Asset.RedactedKeys=Plan.Auth.RedactedKeys;
	for (const auto& Row:Plan.Endpoints)
	{
		FVaRestApiEndpoint Endpoint;Endpoint.Name=*Row.Name;Endpoint.Verb=Row.Verb;Endpoint.Path=Row.Path;Endpoint.Category=Row.Category;Endpoint.Description=Row.Description;
		Endpoint.bDeprecated=Row.bDeprecated;Endpoint.bIdempotent=Row.bIdempotent;Endpoint.RequestType=Type(Row.RequestType);Endpoint.ResponseType=Type(Row.ResponseType);Endpoint.ErrorType=Type(Row.ErrorType);
		Endpoint.BodyContentType=Row.BodyContentType;
		if (Row.BodyKind==EVaRestOpenApiBodyKind::Json && Row.RequestType.Kind==EVaRestOpenApiValueKind::Enum && Endpoint.RequestType.Kind==EVaRestTypeKind::String && Endpoint.RequestType.Container==EVaRestTypeContainer::None) Endpoint.RequestType.bJsonValue=true;
		if (Row.ResponseType.Kind==EVaRestOpenApiValueKind::Enum && Endpoint.ResponseType.Kind==EVaRestTypeKind::String && Endpoint.ResponseType.Container==EVaRestTypeContainer::None) Endpoint.ResponseType.bJsonValue=true;
		if (Row.ErrorType.Kind==EVaRestOpenApiValueKind::Enum && Endpoint.ErrorType.Kind==EVaRestTypeKind::String && Endpoint.ErrorType.Container==EVaRestTypeContainer::None) Endpoint.ErrorType.bJsonValue=true;
		switch (Row.BodyKind)
		{
		case EVaRestOpenApiBodyKind::Form:Endpoint.BodyKind=EVaRestEndpointBodyKind::Form;break;
		case EVaRestOpenApiBodyKind::Multipart:Endpoint.BodyKind=EVaRestEndpointBodyKind::Multipart;break;
		case EVaRestOpenApiBodyKind::Text:Endpoint.BodyKind=EVaRestEndpointBodyKind::Text;break;
		case EVaRestOpenApiBodyKind::Binary:Endpoint.BodyKind=EVaRestEndpointBodyKind::Binary;break;
		default:Endpoint.BodyKind=EVaRestEndpointBodyKind::Json;break;
		}
		switch (Row.AuthMode)
		{
		case EVaRestOpenApiAuthMode::IfAvailable:Endpoint.AuthMode=EVaRestAuthMode::IfAvailable;break;
		case EVaRestOpenApiAuthMode::None:Endpoint.AuthMode=EVaRestAuthMode::None;break;
		default:Endpoint.AuthMode=EVaRestAuthMode::Required;break;
		}
		Endpoint.bOverrideEnvelope=Row.bOverrideEnvelope;
		if (Row.bOverrideEnvelope)
		{
			Endpoint.Envelope=Asset.Envelope;Endpoint.Envelope.DataPath=Row.EnvelopeDataPath;
			if (Row.bDisableEnvelopePredicate) { Endpoint.Envelope.SuccessPath.Reset();Endpoint.Envelope.SuccessValues.Reset(); }
		}
		for (const auto& Parameter:Row.Parameters)
		{
			FVaRestEndpointParam Param;Param.Name=Parameter.Name;Param.PinName=*Parameter.PinName;Param.Type=Type(Parameter.Type);Param.bRequired=Parameter.bRequired;Param.Default=Parameter.DefaultText;Param.Description=Parameter.Description;
			if (Parameter.Type.Kind==EVaRestOpenApiValueKind::Enum && Param.Type.Kind==EVaRestTypeKind::String)
				Param.Default=Parameter.Type.Container==EVaRestOpenApiContainer::Array ? (Parameter.Default.IsArray() ? Parameter.Default.ToString() : FString()) : Parameter.Default.AsString();
			Param.In=Equal(Parameter.In,TEXT("path")) ? EVaRestEndpointParamLocation::Path : Equal(Parameter.In,TEXT("header")) ? EVaRestEndpointParamLocation::Header : EVaRestEndpointParamLocation::Query;
			switch (Parameter.Style)
			{
			case EVaRestOpenApiQueryStyle::Csv:Param.Style=EVaRestQueryStyle::Csv;break;
			case EVaRestOpenApiQueryStyle::Ssv:Param.Style=EVaRestQueryStyle::Ssv;break;
			case EVaRestOpenApiQueryStyle::Pipes:Param.Style=EVaRestQueryStyle::Pipes;break;
			default:Param.Style=EVaRestQueryStyle::Repeat;break;
			}
			Endpoint.Params.Add(MoveTemp(Param));
		}
		Endpoints.Add(Row.Id,Endpoint.Id);Asset.Endpoints.Add(MoveTemp(Endpoint));
	}
}
void FApply::Rules()
{
	for (const auto& Row:Plan.Enums)
	{
		const auto* Found=Types.Find(Row.Id);if (!Found || FailedTypes.Contains(Row.Id)) continue;const auto* Enum=CastChecked<UEnum>(*Found);
		FVaRestApiJsonTypeRules Rules;Rules.Type=*Found;
		for (int32 Index=0;Index<Row.Definition.Values.Num();++Index)
		{
			FVaRestJsonEnumRule Rule;Rule.Enumerator=Enum->GetNameByIndex(Index);Rule.Json=Row.Definition.Values[Index].WireValue;Rule.bUnset=Row.Definition.Values[Index].bUnset;Rules.Enums.Add(MoveTemp(Rule));
		}
		Result.Api->JsonTypes.Add(MoveTemp(Rules));
	}
	for (const auto& Row:Plan.Structs)
	{
		const auto* Found=Types.Find(Row.Id);if (!Found || FailedTypes.Contains(Row.Id)) continue;
		FVaRestApiJsonTypeRules Rules;Rules.Type=*Found;
		for (const auto& Field:Row.Fields)
		{
			const auto* Guid=Fields.Find(Field.Id);if (!Guid) continue;
			const auto* Property=FStructureEditorUtils::GetPropertyByGuid(CastChecked<UUserDefinedStruct>(*Found),*Guid);
			const auto* Array=CastField<FArrayProperty>(Property);const auto* Byte=Array ? CastField<FByteProperty>(Array->Inner) : nullptr;
			FVaRestJsonFieldRule Rule;Rule.Field=*Guid;Rule.Json=Field.WireName;Rule.Presence=Presence(Field.Presence);Rule.bNullable=Field.bNullable;Rule.bBase64=Field.bBase64 && Byte && !Byte->Enum;Rules.Fields.Add(MoveTemp(Rule));
		}
		Result.Api->JsonTypes.Add(MoveTemp(Rules));
	}
}
bool FApply::Manifest()
{
	auto Items=FVaRestJson::Array();auto Json=Plan.ToJson();
	for (const TCHAR* Collection:{TEXT("enums"),TEXT("structs")}) for (const auto& Row:Json[Collection].Elements())
	{
		const FString Id=Row[TEXT("id")].AsString();const auto* Object=Types.Find(Id);if (!Object || FailedTypes.Contains(Id)) continue;
		Items.Add(FVaRestJson::Object().Set(TEXT("kind"),Equal(Collection,TEXT("enums")) ? TEXT("enum") : TEXT("struct")).Set(TEXT("id"),Id).Set(TEXT("target"),(*Object)->GetPathName()).Set(TEXT("base"),Row));
		for (const auto& Field:Row[TEXT("fields")].Elements()) if (const auto* Guid=Fields.Find(Field[TEXT("id")].AsString()))
			Items.Add(FVaRestJson::Object().Set(TEXT("kind"),TEXT("field")).Set(TEXT("id"),Field[TEXT("id")]).Set(TEXT("owner"),Id).Set(TEXT("target"),Guid->ToString()).Set(TEXT("base"),Field));
	}
	for (const auto& Row:Json[TEXT("endpoints")].Elements()) if (const auto* Guid=Endpoints.Find(Row[TEXT("id")].AsString()))
		Items.Add(FVaRestJson::Object().Set(TEXT("kind"),TEXT("endpoint")).Set(TEXT("id"),Row[TEXT("id")]).Set(TEXT("target"),Guid->ToString()).Set(TEXT("base"),Row));
	for (const auto& Row:Json[TEXT("environments")].Elements()) Items.Add(FVaRestJson::Object().Set(TEXT("kind"),TEXT("environment")).Set(TEXT("id"),Row[TEXT("id")]).Set(TEXT("target"),Row[TEXT("name")]).Set(TEXT("base"),Row));
	for (const TCHAR* Key:{TEXT("auth"),TEXT("envelope")}) Items.Add(FVaRestJson::Object().Set(TEXT("kind"),Key).Set(TEXT("id"),Key).Set(TEXT("base"),Json[Key]));
	auto State=FVaRestJson::Object().Set(TEXT("version"),1).Set(TEXT("source"),Options.Source).Set(TEXT("options"),Options.Options)
		.Set(TEXT("plan"),Json).Set(TEXT("items"),Items).Set(TEXT("report"),IssueJson(Result.Issues)).Set(TEXT("suppressions"),FVaRestJson::Array()).Set(TEXT("deferred"),FVaRestJson::Array());
	// Base is the actual applied native state (including rejected defaults/fallbacks), never a recomputed old plan.
	State.Set(TEXT("items"),FVaRestOpenApiMerge::ItemsJson(FVaRestOpenApiMerge::Desired(Plan,State)));
	State.Set(TEXT("items"),FVaRestOpenApiMerge::ItemsJson(FVaRestOpenApiMerge::Capture(Result.Api,State)));
	State.Set(TEXT("endpointIdentities"),EndpointIdentityHistory({},Json));
	if (!EncodeState(State,Result.Api->ImportState)) { Issue(TEXT("OA-APPLY-STATE"),TEXT("The import manifest could not be encoded; no assets were published."),Result.Api,EVaRestOpenApiSeverity::Error);return false; }
	return true;
}
}
