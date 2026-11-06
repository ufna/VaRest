// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiImportOptions.h"

FVaRestOpenApiPlanOptions UVaRestOpenApiImportOptions::PlanOptions(const FString& ApiName) const
{
	FVaRestOpenApiPlanOptions Out;Out.ApiName=ApiName;Out.TypePrefix=TypePrefix;Out.bDateTimeAsString=bDateTimeAsString;Out.bEnumsAsString=bEnumsAsString;
	Out.bIntegersWithoutFormatAsInt32=bIntegersWithoutFormatAsInt32;Out.bDetectEnvelope=bUnwrapEnvelope;Out.bGenerateUnreferencedSchemas=bGenerateUnreferencedSchemas;
	Out.bSelectAllOperations=bImportAllOperations;if (!bImportAllOperations) Out.SelectedOperations=SelectedOperations;return Out;
}
bool UVaRestOpenApiImportOptions::FromJson(const FVaRestJson& Json)
{
	if (!Json.IsObject()) return false;
	TypesFolder=Json[TEXT("typesFolder")].AsString();TypePrefix=Json[TEXT("typePrefix")].AsString();
	bIntegersWithoutFormatAsInt32=Json[TEXT("int32")].AsBool();bDateTimeAsString=Json[TEXT("dateTimeAsString")].AsBool();bEnumsAsString=Json[TEXT("enumsAsString")].AsBool();bGenerateUnreferencedSchemas=Json[TEXT("unreferenced")].AsBool();
	bUnwrapEnvelope=!Json.Has(TEXT("unwrapEnvelope")) || Json[TEXT("unwrapEnvelope")].AsBool();bAllowReferencesOutsideSpecFolder=Json[TEXT("allowOutside")].AsBool();
	bImportAllOperations=!Json.Has(TEXT("allOperations")) || Json[TEXT("allOperations")].AsBool();bIncludeNewOperations=!Json.Has(TEXT("includeNewOperations")) || Json[TEXT("includeNewOperations")].AsBool();
	SelectedOperations.Reset();for (const auto& Operation:Json[TEXT("operations")].Elements()) SelectedOperations.Add(Operation.AsString());return true;
}
FVaRestJson UVaRestOpenApiImportOptions::ToJson() const
{
	auto Operations=FVaRestJson::Array();for (const auto& Operation:SelectedOperations) Operations.Add(Operation);
	return FVaRestJson::Object().Set(TEXT("typesFolder"),TypesFolder).Set(TEXT("typePrefix"),TypePrefix).Set(TEXT("int32"),bIntegersWithoutFormatAsInt32)
		.Set(TEXT("dateTimeAsString"),bDateTimeAsString).Set(TEXT("enumsAsString"),bEnumsAsString).Set(TEXT("unreferenced"),bGenerateUnreferencedSchemas)
		.Set(TEXT("unwrapEnvelope"),bUnwrapEnvelope).Set(TEXT("allowOutside"),bAllowReferencesOutsideSpecFolder).Set(TEXT("allOperations"),bImportAllOperations)
		.Set(TEXT("operations"),Operations).Set(TEXT("includeNewOperations"),bIncludeNewOperations);
}
