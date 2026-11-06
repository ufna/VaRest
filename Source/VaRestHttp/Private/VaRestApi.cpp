// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestApi.h"

#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UObject/Package.h"
#include "VaRestApiEditorBridge.h"
#include "VaRestBody.h"
#include "VaRestHttpSettings.h"
#include "VaRestMediaType.h"
#include "VaRestUrl.h"
#include "VaRestUrlValidation.h"

#if WITH_EDITOR
#include "EditorFramework/AssetImportData.h"
#include "Misc/DataValidation.h"
#include "UObject/AssetRegistryTagsContext.h"
#endif

#define LOCTEXT_NAMESPACE "VaRestApi"

namespace VaRest::Http::ApiPrivate
{
void ValidateHeaders(const TArray<FVaRestHeader>& Headers, FVaRestApiValidationContext& Context, const FString& Path)
{
	for (int32 Index = 0; Index < Headers.Num(); ++Index)
	{
		if (!IsValidHeaderName(Headers[Index].Name) || !IsValidHeaderValue(Headers[Index].Value))
		{
			Context.Error(TEXT("Header.Invalid"), FString::Printf(TEXT("%s[%d]"), *Path, Index), LOCTEXT("InvalidHeader", "A header requires an HTTP token name and a value without forbidden control characters."));
		}
	}
}

void ValidateRetry(const FVaRestRetryPolicy& Policy, FVaRestApiValidationContext& Context, const FString& Path)
{
	const FVaRestBackoff& Backoff = Policy.Backoff;
	if (Policy.MaxRetries < 0 || !FMath::IsFinite(Policy.Deadline) || Policy.Deadline < 0 || !FMath::IsFinite(Policy.MaxRetryAfter) || Policy.MaxRetryAfter < 0 || !FMath::IsFinite(Backoff.BaseDelay) || Backoff.BaseDelay < 0 || !FMath::IsFinite(Backoff.MaxDelay) || Backoff.MaxDelay < 0 || !FMath::IsFinite(Backoff.Multiplier) || Backoff.Multiplier < 1 || !FMath::IsFinite(Backoff.JitterRatio) || Backoff.JitterRatio < 0 || Backoff.JitterRatio > 1 || static_cast<uint8>(Backoff.Jitter) > static_cast<uint8>(EVaRestJitter::None))
	{
		Context.Error(TEXT("Retry.Invalid"), Path, LOCTEXT("InvalidRetry", "Retry counts, deadlines and backoff settings must be finite and within their declared ranges."));
	}
	for (int32 Status : Policy.RetryStatuses)
	{
		if (Status < 100 || Status > 599)
		{
			Context.Error(TEXT("Retry.InvalidStatus"), Path, LOCTEXT("InvalidRetryStatus", "Retry statuses must be HTTP status codes from 100 through 599."));
			break;
		}
	}
}

void ValidateCache(const FVaRestCachePolicy& Policy, FVaRestApiValidationContext& Context, const FString& Path)
{
	if (static_cast<uint8>(Policy.Mode) > static_cast<uint8>(EVaRestCacheMode::MemoryAndDisk) || Policy.MemoryBytes < 0 || Policy.DiskBytes < 0 || !FMath::IsFinite(Policy.DefaultTtlSeconds) || Policy.DefaultTtlSeconds < 0)
	{
		Context.Error(TEXT("Cache.Invalid"), Path, LOCTEXT("InvalidCache", "Cache sizes and lifetime must be nonnegative and finite, with a valid cache mode."));
	}
}

TArray<FString> TemplateNames(const FString& Path, FVaRestApiValidationContext& Context, const FString& Location)
{
	TArray<FString> Names;
	TArray<TPair<FString, FString>> Dummy;
	for (int32 Index = 0; Index < Path.Len(); ++Index)
	{
		if (Path[Index] == TEXT('{'))
		{
			const int32 End = Path.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Index + 1);
			if (End == INDEX_NONE)
			{
				break; // the native validator below reports malformed braces
			}
			const FString Name = Path.Mid(Index + 1, End - Index - 1);
			if (!Names.ContainsByPredicate([&](const FString& Existing) { return VaRest::Json::EqualWireText(Existing, Name); }))
			{
				Names.Add(Name);
				Dummy.Emplace(Name, TEXT("validation"));
			}
			Index = End;
		}
	}
	if (!ExpandPathTemplate(Path, Dummy).HasValue())
	{
		Context.Error(TEXT("Endpoint.InvalidTemplate"), Location, LOCTEXT("InvalidTemplate", "The endpoint path template is malformed."));
	}
	return Names;
}
} // namespace VaRest::Http::ApiPrivate

UVaRestApi::UVaRestApi()
	: Retry(FVaRestRetryPolicy::Exponential(3))
{
}

const FVaRestApiEndpoint* UVaRestApi::FindEndpoint(const FGuid& Id) const
{
	return Endpoints.FindByPredicate([&](const FVaRestApiEndpoint& Endpoint) { return Endpoint.Id == Id; });
}

const FVaRestApiEndpoint* UVaRestApi::FindEndpoint(FName Name) const
{
	return Endpoints.FindByPredicate([&](const FVaRestApiEndpoint& Endpoint) { return Endpoint.Name == Name; });
}

const FVaRestApiEnvironment* UVaRestApi::FindEnvironment(FName Name) const
{
	return Environments.FindByPredicate([&](const FVaRestApiEnvironment& Environment) { return Environment.Name == Name; });
}

TValueOrError<FName, FString> UVaRestApi::ResolveEnvironmentName() const
{
	FString Selected;
	const FString AssetOption = FString::Printf(TEXT("VaRestEnv.%s="), *GetName());
	if (!FParse::Value(FCommandLine::Get(), *AssetOption, Selected) && !FParse::Value(FCommandLine::Get(), TEXT("VaRestEnv="), Selected))
	{
#if UE_BUILD_SHIPPING
		Selected = ShippingEnvironment.IsNone() ? DefaultEnvironment.ToString() : ShippingEnvironment.ToString();
#else
		Selected = DefaultEnvironment.ToString();
#endif
	}
	const FVaRestApiEnvironment* Environment = Environments.FindByPredicate([&](const FVaRestApiEnvironment& Entry) {
		return Entry.Name.ToString().Equals(Selected, ESearchCase::IgnoreCase);
	});
	if (!Environment)
	{
		return MakeError(FString(TEXT("the selected API environment is not declared")));
	}
	return MakeValue(Environment->Name);
}

TValueOrError<FString, FString> UVaRestApi::ResolveEndpointPath(FName EndpointName) const
{
	const FVaRestApiEndpoint* Endpoint = FindEndpoint(EndpointName);
	if (!Endpoint || !Endpoint->Verb.Equals(TEXT("POST"), ESearchCase::IgnoreCase))
	{
		return MakeError(FString(TEXT("an authentication endpoint reference must name a declared POST endpoint")));
	}
	auto Expanded = VaRest::Http::ExpandPathTemplate(Endpoint->Path, {});
	if (!Expanded.HasValue())
	{
		return MakeError(FString(TEXT("an authentication endpoint reference must not contain unresolved path parameters")));
	}
	return MakeValue(Expanded.StealValue());
}

void UVaRestApi::Validate(FVaRestApiValidationContext& Context) const
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::ApiPrivate;
	TSet<FName> EnvironmentNames;
	for (int32 Index = 0; Index < Environments.Num(); ++Index)
	{
		const auto& Environment = Environments[Index];
		if (Environment.Name.IsNone() || EnvironmentNames.Contains(Environment.Name) || !IsValidHttpUrl(Environment.BaseUrl) || Environment.BaseUrl.Contains(TEXT("#")))
		{
			Context.Error(TEXT("Environment.Invalid"), FString::Printf(TEXT("Environments[%d]"), Index), LOCTEXT("InvalidEnvironment", "Environments need distinct nonempty names and valid HTTP(S) base URLs."));
		}
		EnvironmentNames.Add(Environment.Name);
	}
	if (DefaultEnvironment.IsNone() || !FindEnvironment(DefaultEnvironment) || (!ShippingEnvironment.IsNone() && !FindEnvironment(ShippingEnvironment)))
	{
		Context.Error(TEXT("Environment.MissingDefault"), TEXT("DefaultEnvironment"), LOCTEXT("MissingDefault", "DefaultEnvironment and any ShippingEnvironment must name declared environments."));
	}
	if (!IsValidMediaType(JsonContentType) || !IsJsonContentType(JsonContentType))
	{
		Context.Error(TEXT("JSON.InvalidMediaType"), TEXT("JsonContentType"), LOCTEXT("InvalidJsonMedia", "JSON content type must be application/json or an application/*+json media type."));
	}
	if (!FMath::IsFinite(Timeout) || Timeout < 0 || !FMath::IsFinite(ActivityTimeout) || ActivityTimeout < 0 || MaxJsonBytes < 0 || MaxConcurrent < 1)
	{
		Context.Error(TEXT("Requests.InvalidLimits"), TEXT("Requests"), LOCTEXT("InvalidLimits", "Timeouts and response limits must be nonnegative and finite; MaxConcurrent must be positive."));
	}
	ValidateHeaders(Headers, Context, TEXT("Headers"));
	if (!UserAgent.IsEmpty() && !IsValidHeaderValue(UserAgent))
	{
		Context.Error(TEXT("Header.Invalid"), TEXT("UserAgent"), LOCTEXT("InvalidUserAgent", "UserAgent must be a valid HTTP header value without forbidden control characters."));
	}
	ValidateRetry(Retry, Context, TEXT("Retry"));
	ValidateCache(Cache, Context, TEXT("Cache"));
	if (Offline.MaxQueued <= 0 || Offline.MaxQueued > 4096 || !FMath::IsFinite(Offline.MaxAgeSeconds) || Offline.MaxAgeSeconds <= 0 || Offline.MaxAgeSeconds > 315360000 || Offline.MaxItemBytes <= 0 || Offline.MaxItemBytes > 32 * 1024 * 1024 || Offline.MaxBodyBytes < Offline.MaxItemBytes || Offline.MaxBodyBytes > 1024LL * 1024 * 1024)
	{
		Context.Error(TEXT("Offline.Invalid"), TEXT("Offline"), LOCTEXT("InvalidOffline", "Offline policy exceeds the native queue limits or contains invalid sizes or ages."));
	}
	if (static_cast<uint8>(Naming) > static_cast<uint8>(EVaRestJsonNaming::PascalCase) || static_cast<uint8>(LogLevel) > static_cast<uint8>(EVaRestLogLevel::Body))
	{
		Context.Error(TEXT("API.InvalidEnum"), TEXT("API"), LOCTEXT("InvalidApiEnum", "The naming or log-level selection is invalid."));
	}
	if (!RuleRegistrationError.IsEmpty())
	{
		Context.Error(TEXT("JSON.InvalidRules"), TEXT("JsonTypes"), FText::FromString(RuleRegistrationError));
	}
	const bool bHasRules = JsonTypes.ContainsByPredicate([](const FVaRestApiJsonTypeRules& Rules) {
		return Rules.Type && (!Rules.Fields.IsEmpty() || !Rules.Enums.IsEmpty());
	});
	if (bHasRules && !IsTemplate() && !HasAnyFlags(RF_Transient) && GetOutermost() != GetTransientPackage() && !GetOutermost()->HasAnyFlags(RF_Transient))
	{
		const FSoftObjectPath Identity(this);
		if (Identity.IsValid() && !GetDefault<UVaRestHttpSettings>()->Apis.ContainsByPredicate([&](const TSoftObjectPtr<UVaRestApi>& Listed) { return Listed.ToSoftObjectPath() == Identity; }))
		{
			Context.Warning(TEXT("JSON.MissingSettingsApi"), TEXT("JsonTypes"), LOCTEXT("MissingSettingsApi", "This API has JSON rules but is absent from VaRestHttp Settings.Apis. Add it for settings-based preload and cook inclusion."));
		}
	}
	if (Auth)
	{
		Auth->Validate(Context);
	}
	TSet<FGuid> Ids;
	TSet<FName> Names;
	for (int32 Index = 0; Index < Endpoints.Num(); ++Index)
	{
		const auto& Endpoint = Endpoints[Index];
		const FString Location = FString::Printf(TEXT("Endpoints[%d]"), Index);
		if (!Endpoint.Id.IsValid() || Ids.Contains(Endpoint.Id) || Endpoint.Name.IsNone() || Names.Contains(Endpoint.Name))
		{
			Context.Error(TEXT("Endpoint.DuplicateIdentity"), Location, LOCTEXT("DuplicateEndpoint", "Endpoints require distinct valid GUIDs and distinct nonempty names."));
		}
		Ids.Add(Endpoint.Id);
		Names.Add(Endpoint.Name);
		if (!IsValidHeaderName(Endpoint.Verb))
		{
			Context.Error(TEXT("Endpoint.InvalidVerb"), Location + TEXT(".Verb"), LOCTEXT("InvalidVerb", "The HTTP verb must be a nonempty HTTP token."));
		}
		if (Endpoint.Verb.Equals(TEXT("CONNECT"), ESearchCase::IgnoreCase))
		{
			Context.Error(TEXT("Endpoint.UnsupportedVerb"), Location + TEXT(".Verb"), LOCTEXT("UnsupportedConnect", "CONNECT proxy tunnels are not supported by VaRest."));
		}
		const TArray<FString> PathNames = TemplateNames(Endpoint.Path, Context, Location + TEXT(".Path"));
		Endpoint.RequestType.Validate(Context, Location + TEXT(".RequestType"));
		Endpoint.ResponseType.Validate(Context, Location + TEXT(".ResponseType"));
		Endpoint.ErrorType.Validate(Context, Location + TEXT(".ErrorType"));
		EVaRestBodyKind NativeKind;
		if (!TryGetNativeBodyKind(Endpoint.BodyKind, NativeKind) || static_cast<uint8>(Endpoint.AuthMode) > static_cast<uint8>(EVaRestAuthMode::None))
		{
			Context.Error(TEXT("Endpoint.InvalidPolicy"), Location, LOCTEXT("InvalidEndpointPolicy", "The body kind or authentication mode is invalid."));
		}
		if (Endpoint.RequestType.bJsonValue && Endpoint.BodyKind!=EVaRestEndpointBodyKind::Json)
			Context.Error(TEXT("Endpoint.InvalidJsonValueBody"),Location+TEXT(".RequestType"),LOCTEXT("InvalidJsonValueBody","JSON value mode requires a JSON request body."));
		const int32 MediaSeparator=Endpoint.BodyContentType.Find(TEXT(";"));
		const FString MediaBase=(MediaSeparator==INDEX_NONE ? Endpoint.BodyContentType : Endpoint.BodyContentType.Left(MediaSeparator)).TrimStartAndEnd();
		if (!Endpoint.BodyContentType.IsEmpty() && (!IsValidMediaType(Endpoint.BodyContentType) || Endpoint.BodyKind == EVaRestEndpointBodyKind::Multipart ||
			(Endpoint.BodyKind == EVaRestEndpointBodyKind::Form && !MediaBase.Equals(TEXT("application/x-www-form-urlencoded"),ESearchCase::IgnoreCase)) ||
			(Endpoint.BodyKind == EVaRestEndpointBodyKind::Json && !IsJsonContentType(Endpoint.BodyContentType))))
		{
			Context.Error(TEXT("Endpoint.InvalidMediaType"), Location + TEXT(".BodyContentType"), LOCTEXT("InvalidEndpointMedia", "The body media type is invalid. JSON requires JSON media; multipart generates its own boundary."));
		}
		ValidateHeaders(Endpoint.Headers, Context, Location + TEXT(".Headers"));
		if (Endpoint.BodyKind==EVaRestEndpointBodyKind::Multipart && Endpoint.Headers.ContainsByPredicate([](const FVaRestHeader& Header) { return Header.Name.Equals(TEXT("Content-Type"),ESearchCase::IgnoreCase); }))
			Context.Error(TEXT("Endpoint.MultipartBoundaryOverride"),Location+TEXT(".Headers"),LOCTEXT("MultipartBoundaryOverride","Multipart Content-Type and its boundary are generated from the multipart body."));
		TSet<FName> Pins;
		for (int32 ParamIndex = 0; ParamIndex < Endpoint.Params.Num(); ++ParamIndex)
		{
			const auto& Param = Endpoint.Params[ParamIndex];
			const FString ParamLocation = FString::Printf(TEXT("%s.Params[%d]"), *Location, ParamIndex);
			Param.Validate(Context, ParamLocation);
			if (Pins.Contains(Param.PinName))
			{
				Context.Error(TEXT("Param.DuplicatePin"), ParamLocation, LOCTEXT("DuplicateParamPin", "Parameter Blueprint pin identities must be distinct."));
			}
			Pins.Add(Param.PinName);
			for (int32 Previous = 0; Previous < ParamIndex; ++Previous)
			{
				const auto& Other = Endpoint.Params[Previous];
				if (!Param.bRemoved && !Other.bRemoved && Other.In == Param.In && (Param.In == EVaRestEndpointParamLocation::Header
													? Other.Name.Equals(Param.Name, ESearchCase::IgnoreCase)
													: VaRest::Json::EqualWireText(Other.Name, Param.Name)))
				{
					Context.Error(TEXT("Param.DuplicateWireName"), ParamLocation, LOCTEXT("DuplicateParamName", "Parameter wire names must be distinct within their location; header names compare without case."));
				}
			}
			if (!Param.bRemoved && Param.In == EVaRestEndpointParamLocation::Path && !PathNames.ContainsByPredicate([&](const FString& Name) { return VaRest::Json::EqualWireText(Name, Param.Name); }))
			{
				Context.Error(TEXT("Param.UnusedPathName"), ParamLocation, LOCTEXT("UnusedPathParam", "A declared path parameter must occur in the endpoint path template."));
			}
			if (Param.bRemoved && Param.In == EVaRestEndpointParamLocation::Path && PathNames.ContainsByPredicate([&](const FString& Name) { return VaRest::Json::EqualWireText(Name, Param.Name); }))
			{
				Context.Error(TEXT("Param.RemovedPathName"), ParamLocation, LOCTEXT("RemovedPathParam", "The path template still requires a parameter that is explicitly marked removed."));
			}
		}
		if (Endpoint.bOverrideRetry)
			ValidateRetry(Endpoint.Retry, Context, Location + TEXT(".Retry"));
		if (Endpoint.bOverrideCache)
			ValidateCache(Endpoint.Cache, Context, Location + TEXT(".Cache"));
		if (Endpoint.bOverrideTimeouts && (!FMath::IsFinite(Endpoint.Timeout) || Endpoint.Timeout < 0 || !FMath::IsFinite(Endpoint.ActivityTimeout) || Endpoint.ActivityTimeout < 0))
		{
			Context.Error(TEXT("Endpoint.InvalidTimeout"), Location, LOCTEXT("InvalidEndpointTimeout", "Endpoint timeout overrides must be nonnegative and finite."));
		}
		if ((Endpoint.bOverrideMaxJsonBytes && Endpoint.MaxJsonBytes < 0) || (Endpoint.bOverrideNaming && static_cast<uint8>(Endpoint.Naming) > static_cast<uint8>(EVaRestJsonNaming::PascalCase)) || (Endpoint.bOverridePriority && static_cast<uint8>(Endpoint.Priority) > static_cast<uint8>(EVaRestPriority::Low)))
		{
			Context.Error(TEXT("Endpoint.InvalidOverride"), Location, LOCTEXT("InvalidEndpointOverride", "An endpoint override is outside its declared range."));
		}
	}
	TSet<FName> SocketNames;
	for (int32 Index = 0; Index < Sockets.Num(); ++Index)
	{
		const auto* Socket = Sockets[Index].Get();
		if (!Socket || Socket->Name.IsNone() || SocketNames.Contains(Socket->Name))
		{
			Context.Error(TEXT("Socket.InvalidIdentity"), FString::Printf(TEXT("Sockets[%d]"), Index), LOCTEXT("InvalidSocketIdentity", "Socket entries must be present and have distinct nonempty names."));
		}
		if (Socket)
		{
			SocketNames.Add(Socket->Name);
			Socket->Validate(Context);
		}
	}
}

void UVaRestApi::RegisterJsonRules()
{
	check(IsInGameThread());
	TArray<FVaRestJsonTypeRuleRegistration> Registrations;
	for (const auto& Entry : JsonTypes)
	{
		FVaRestJsonTypeRuleRegistration Registration;
		Registration.Type = Entry.Type.Get();
		Registration.Fields = Entry.Fields;
		Registration.Enums = Entry.Enums;
		Registrations.Add(MoveTemp(Registration));
	}
	TArray<FString> Folders;
	if (!TypesFolder.IsEmpty())
		Folders.Add(TypesFolder);
	RuleRegistrationError.Reset();
	FVaRestJsonRules::ReplaceOwnerRules(this, Registrations, Folders, &RuleRegistrationError);
}

void UVaRestApi::NotifyEdited()
{
	RegisterJsonRules();
	FVaRestApiValidationContext Validation;
	Validate(Validation);
	FVaRestApiEditorBridge::OnEdited().Broadcast(this);
}

void UVaRestApi::PostLoad()
{
	Super::PostLoad();
	RegisterJsonRules();
}

void UVaRestApi::BeginDestroy()
{
	FVaRestJsonRules::UnregisterOwner(this);
	Super::BeginDestroy();
}

#if WITH_EDITOR
void UVaRestApi::PostInitProperties()
{
	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		AssetImportData = NewObject<UAssetImportData>(this, TEXT("AssetImportData"));
	}
	Super::PostInitProperties();
}

void UVaRestApi::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
	Super::PostEditChangeProperty(Event);
	NotifyEdited();
}

EDataValidationResult UVaRestApi::IsDataValid(FDataValidationContext& Context) const
{
	FVaRestApiValidationContext Validation;
	Validate(Validation);
	for (const auto& Issue : Validation.Issues)
	{
		const FText Text = FText::Format(LOCTEXT("ValidationIssue", "{0}: {1}"), FText::FromString(Issue.PropertyPath), Issue.Message);
		if (Issue.bWarning)
			Context.AddWarning(Text);
		else
			Context.AddError(Text);
	}
	return Validation.HasErrors() ? EDataValidationResult::Invalid : EDataValidationResult::Valid;
}

void UVaRestApi::GetAssetRegistryTags(FAssetRegistryTagsContext Context) const
{
	if (!ImportState.Importer.IsNone()) Context.AddTag(FAssetRegistryTag(TEXT("VaRestImporter"),ImportState.Importer.ToString(),FAssetRegistryTag::TT_Hidden));
	if (AssetImportData)
	{
		Context.AddTag(FAssetRegistryTag(SourceFileTagName(), AssetImportData->GetSourceData().ToJson(), FAssetRegistryTag::TT_Hidden));
	}
	Super::GetAssetRegistryTags(Context);
}
#endif

#undef LOCTEXT_NAMESPACE
