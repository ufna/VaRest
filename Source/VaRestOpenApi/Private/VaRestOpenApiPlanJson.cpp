// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiPlan.h"

namespace
{
FVaRestJson PlanType(const FVaRestOpenApiPlannedType& Type)
{
	static constexpr const TCHAR* Kinds[]={TEXT("None"),TEXT("Json"),TEXT("Struct"),TEXT("Enum"),TEXT("String"),TEXT("Bytes"),TEXT("Bool"),TEXT("Int"),TEXT("Int64"),TEXT("Double"),TEXT("DateTime")};
	static constexpr const TCHAR* Containers[]={TEXT("None"),TEXT("Array"),TEXT("Map")};
	return FVaRestJson::Object().Set(TEXT("kind"),Kinds[static_cast<uint8>(Type.Kind)])
		.Set(TEXT("container"),Containers[static_cast<uint8>(Type.Container)]).Set(TEXT("definition"),Type.DefinitionId).Set(TEXT("jsonValue"),Type.bJsonValue);
}
FVaRestJson PlanLocation(const FVaRestOpenApiLocation& Location)
{
	return FVaRestJson::Object().Set(TEXT("document"),Location.Document).Set(TEXT("pointer"),Location.Pointer);
}
}
FVaRestJson FVaRestOpenApiPlan::ToJson() const
{
	auto Root=FVaRestJson::Object().Set(TEXT("valid"),bValid);
	auto Unavailable=FVaRestJson::Array();for (const auto& Id:UnavailableEndpointIds) Unavailable.Add(Id);Root.Set(TEXT("unavailableEndpoints"),Unavailable);
	auto StructRows=FVaRestJson::Array();
	static constexpr const TCHAR* Presence[]={TEXT("Always"),TEXT("OmitWhenDefault"),TEXT("NullWhenDefault"),TEXT("DecodeOnly"),TEXT("Ignore")};
	for (const auto& Struct:Structs)
	{
		auto Fields=FVaRestJson::Array();
		for (const auto& Field:Struct.Fields)
			Fields.Add(FVaRestJson::Object().Set(TEXT("id"),Field.Id).Set(TEXT("wire"),Field.WireName).Set(TEXT("name"),Field.FriendlyName)
				.Set(TEXT("type"),PlanType(Field.Type)).Set(TEXT("location"),PlanLocation(Field.Location)).Set(TEXT("presence"),Presence[static_cast<uint8>(Field.Presence)])
				.Set(TEXT("default"),Field.Default).Set(TEXT("defaultText"),Field.DefaultText).Set(TEXT("defaultTooltipOnly"),Field.bDefaultInTooltipOnly)
				.Set(TEXT("nullable"),Field.bNullable).Set(TEXT("base64"),Field.bBase64).Set(TEXT("required"),Field.bRequired).Set(TEXT("tooltip"),Field.Tooltip));
		StructRows.Add(FVaRestJson::Object().Set(TEXT("id"),Struct.Id).Set(TEXT("name"),Struct.AssetName).Set(TEXT("tooltip"),Struct.Tooltip).Set(TEXT("fields"),Fields));
	}
	Root.Set(TEXT("structs"),StructRows);
	auto EnumRows=FVaRestJson::Array();
	for (const auto& Enum:Enums)
	{
		auto Values=FVaRestJson::Array();
		for (const auto& Value:Enum.Definition.Values)
			Values.Add(FVaRestJson::Object().Set(TEXT("name"),Value.InternalName).Set(TEXT("display"),Value.DisplayName).Set(TEXT("wire"),Value.WireValue).Set(TEXT("unset"),Value.bUnset));
		EnumRows.Add(FVaRestJson::Object().Set(TEXT("id"),Enum.Id).Set(TEXT("name"),Enum.AssetName).Set(TEXT("tooltip"),Enum.Tooltip)
			.Set(TEXT("hasUnset"),Enum.Definition.bHasUnset).Set(TEXT("stringFallback"),Enum.Definition.bStringFallback).Set(TEXT("fallbackReason"),Enum.Definition.StringFallbackReason).Set(TEXT("values"),Values));
	}
	Root.Set(TEXT("enums"),EnumRows);
	auto EndpointRows=FVaRestJson::Array();
	static constexpr const TCHAR* Styles[]={TEXT("Repeat"),TEXT("Csv"),TEXT("Ssv"),TEXT("Pipes")};
	static constexpr const TCHAR* Bodies[]={TEXT("Json"),TEXT("Form"),TEXT("Multipart"),TEXT("Binary"),TEXT("Text")};
	static constexpr const TCHAR* AuthModes[]={TEXT("Required"),TEXT("IfAvailable"),TEXT("None")};
	for (const auto& Endpoint:Endpoints)
	{
		auto Params=FVaRestJson::Array();
		for (const auto& Param:Endpoint.Parameters)
			Params.Add(FVaRestJson::Object().Set(TEXT("id"),Param.Id).Set(TEXT("wire"),Param.Name).Set(TEXT("pin"),Param.PinName).Set(TEXT("in"),Param.In)
				.Set(TEXT("type"),PlanType(Param.Type)).Set(TEXT("style"),Styles[static_cast<uint8>(Param.Style)]).Set(TEXT("required"),Param.bRequired)
				.Set(TEXT("default"),Param.Default).Set(TEXT("defaultText"),Param.DefaultText).Set(TEXT("location"),PlanLocation(Param.Location)).Set(TEXT("description"),Param.Description));
		EndpointRows.Add(FVaRestJson::Object().Set(TEXT("id"),Endpoint.Id).Set(TEXT("operationId"),Endpoint.OperationId).Set(TEXT("name"),Endpoint.Name).Set(TEXT("verb"),Endpoint.Verb).Set(TEXT("path"),Endpoint.Path)
			.Set(TEXT("location"),PlanLocation(Endpoint.Location)).Set(TEXT("category"),Endpoint.Category).Set(TEXT("description"),Endpoint.Description)
			.Set(TEXT("requestType"),PlanType(Endpoint.RequestType)).Set(TEXT("responseType"),PlanType(Endpoint.ResponseType)).Set(TEXT("errorType"),PlanType(Endpoint.ErrorType))
			.Set(TEXT("bodyKind"),Bodies[static_cast<uint8>(Endpoint.BodyKind)]).Set(TEXT("requestMedia"),Endpoint.RequestMedia).Set(TEXT("contentType"),Endpoint.BodyContentType)
			.Set(TEXT("authMode"),AuthModes[static_cast<uint8>(Endpoint.AuthMode)]).Set(TEXT("deprecated"),Endpoint.bDeprecated).Set(TEXT("idempotent"),Endpoint.bIdempotent)
			.Set(TEXT("hasBody"),Endpoint.bHasBody).Set(TEXT("mixedSuccessShapes"),Endpoint.bMixedSuccessShapes).Set(TEXT("overrideEnvelope"),Endpoint.bOverrideEnvelope)
			.Set(TEXT("disableEnvelopePredicate"),Endpoint.bDisableEnvelopePredicate).Set(TEXT("envelopeDataPath"),Endpoint.EnvelopeDataPath).Set(TEXT("parameters"),Params));
	}
	Root.Set(TEXT("endpoints"),EndpointRows);
	auto EnvironmentRows=FVaRestJson::Array();
	for (const auto& Environment:Environments) EnvironmentRows.Add(FVaRestJson::Object().Set(TEXT("id"),Environment.Id).Set(TEXT("template"),Environment.UrlTemplate).Set(TEXT("name"),Environment.Name).Set(TEXT("url"),Environment.BaseUrl).Set(TEXT("description"),Environment.Description));
	Root.Set(TEXT("environments"),EnvironmentRows).Set(TEXT("defaultEnvironment"),DefaultEnvironment);
	static constexpr const TCHAR* AuthKinds[]={TEXT("None"),TEXT("Bearer"),TEXT("Basic"),TEXT("ApiKey"),TEXT("BearerRefresh"),TEXT("ClientCredentials")};
	auto Redacted=FVaRestJson::Array();for (const auto& Key:Auth.RedactedKeys) Redacted.Add(Key);
	Root.Set(TEXT("auth"),FVaRestJson::Object().Set(TEXT("kind"),AuthKinds[static_cast<uint8>(Auth.Kind)]).Set(TEXT("scheme"),Auth.SchemeName)
		.Set(TEXT("name"),Auth.Name).Set(TEXT("placement"),Auth.Placement).Set(TEXT("tokenUrl"),Auth.TokenUrl).Set(TEXT("redactedKeys"),Redacted));
	Root.Set(TEXT("envelope"),FVaRestJson::Object().Set(TEXT("dataPath"),Envelope.DataPath).Set(TEXT("successPath"),Envelope.SuccessPath)
		.Set(TEXT("errorMessagePath"),Envelope.ErrorMessagePath).Set(TEXT("errorCodePath"),Envelope.ErrorCodePath).Set(TEXT("parseErrorBodies"),Envelope.bParseErrorBodies)
		.Set(TEXT("wrapped"),Envelope.WrappedOperations).Set(TEXT("considered"),Envelope.ConsideredOperations));
	// Preview evidence is distinct from the runtime envelope and is not an identity/adoption directive.
	Root.Set(TEXT("envelopeCandidate"),FVaRestJson::Object().Set(TEXT("dataPath"),EnvelopeCandidate.DataPath)
		.Set(TEXT("wrapped"),EnvelopeCandidate.WrappedOperations).Set(TEXT("considered"),EnvelopeCandidate.ConsideredOperations));
	auto IssueRows=FVaRestJson::Array();
	static constexpr const TCHAR* Severities[]={TEXT("Info"),TEXT("Warning"),TEXT("Error")};
	for (const auto& Issue:Issues)
		IssueRows.Add(FVaRestJson::Object().Set(TEXT("severity"),Severities[static_cast<uint8>(Issue.Severity)]).Set(TEXT("code"),Issue.Code).Set(TEXT("message"),Issue.Message)
			.Set(TEXT("document"),Issue.Document).Set(TEXT("pointer"),Issue.Pointer).Set(TEXT("line"),Issue.Line).Set(TEXT("column"),Issue.Column).Set(TEXT("asset"),Issue.AssetPath));
	return Root.Set(TEXT("issues"),IssueRows);
}
