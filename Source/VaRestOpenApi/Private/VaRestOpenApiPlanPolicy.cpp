// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiPlanning.h"
#include "VaRestUrl.h"

namespace VaRest::OpenApi
{
FString ResolveHttpReference(const FString& Base,const FString& Relative,bool bBaseDirectory)
{
	if (VaRest::Http::IsAbsoluteHttpUrl(Relative)) return Relative;
	const int32 Scheme=Base.Find(TEXT("://"));if (Scheme==INDEX_NONE) return Relative;
	if (Relative.StartsWith(TEXT("//"))) return Base.Left(Scheme)+TEXT(":")+Relative;
	int32 AuthorityEnd=Scheme+3;while (AuthorityEnd<Base.Len() && Base[AuthorityEnd]!=TEXT('/') && Base[AuthorityEnd]!=TEXT('?') && Base[AuthorityEnd]!=TEXT('#')) ++AuthorityEnd;
	const FString Origin=Base.Left(AuthorityEnd);FString Path=Base.Mid(AuthorityEnd);int32 Mark;
	if (Path.FindChar(TEXT('?'),Mark)) Path=Path.Left(Mark);if (Path.FindChar(TEXT('#'),Mark)) Path=Path.Left(Mark);
	if (Path.IsEmpty()) Path=TEXT("/");
	if (Relative.StartsWith(TEXT("?"))) return Origin+Path+Relative;
	if (!Relative.StartsWith(TEXT("/"))) Path=bBaseDirectory ? (Path.EndsWith(TEXT("/")) ? Path : Path+TEXT("/"))+Relative : Path.Left(Path.Find(TEXT("/"),ESearchCase::CaseSensitive,ESearchDir::FromEnd)+1)+Relative;
	else Path=Relative;
	FString Tail;Mark=Path.Find(TEXT("?"));if (Mark!=INDEX_NONE) { Tail=Path.Mid(Mark);Path=Path.Left(Mark); }
	TArray<FString> Parts,Normalized;Path.ParseIntoArray(Parts,TEXT("/"),false);
	for (const auto& Part:Parts) { if (Equal(Part,TEXT(".."))) { if (!Normalized.IsEmpty()) Normalized.Pop(EAllowShrinking::No); }else if (!Part.IsEmpty() && !Equal(Part,TEXT("."))) Normalized.Add(Part); }
	return Origin+TEXT("/")+FString::Join(Normalized,TEXT("/"))+(Path.EndsWith(TEXT("/")) && !Normalized.IsEmpty() ? TEXT("/") : TEXT(""))+Tail;
}
FString FPlanner::ServerUrl(const FVaRestJson& Server,const FVaRestOpenApiLocation& Location)
{
	FString Url=Server[TEXT("url")].AsString();if (Url.IsEmpty()) Url=TEXT("/");
	const auto Variables=Server[TEXT("variables")];bool bChanged=false,bMissing=false;
	int32 Position=0;
	while ((Position=Url.Find(TEXT("{"),ESearchCase::CaseSensitive,ESearchDir::FromStart,Position))!=INDEX_NONE)
	{
		const int32 End=Url.Find(TEXT("}"),ESearchCase::CaseSensitive,ESearchDir::FromStart,Position+1);if (End==INDEX_NONE) { bMissing=true;break; }
		const FString Key=Url.Mid(Position+1,End-Position-1);const auto Default=Variables.Get(Key)[TEXT("default")];
		if (!Default.IsValid() || Default.IsNull()) { bMissing=true;Position=End+1;continue; }
		const FString Value=Default.IsString() ? Default.AsString() : Default.ToString();Url=Url.Left(Position)+Value+Url.Mid(End+1);Position+=Value.Len();bChanged=true;
	}
	if (bChanged) Issue(Location,TEXT("OA-SRV-VARS"),TEXT("Server variables were substituted using their defaults; enum alternatives remain available in the source."),EVaRestOpenApiSeverity::Info);
	if (bMissing) { Issue(Location,TEXT("OA-SRV-VARS"),TEXT("A server URL variable has no usable default; configure an environment base URL before sending requests."),EVaRestOpenApiSeverity::Error);return TEXT("/"); }
	if (!VaRest::Http::IsAbsoluteHttpUrl(Url)) Url=ResolveHttpReference(Model.RootDocument,Url);
	if (!VaRest::Http::IsAbsoluteHttpUrl(Url)) Issue(Location,TEXT("OA-SRV-RELATIVE"),TEXT("A relative server URL needs a host in the API environment."));
	return Url;
}
void FPlanner::Environments()
{
	TSet<FString> Used;TMap<FString,int32> Suffixes;int32 Index=0;
	for (const auto& Server:Model.Servers.Elements())
	{
		FVaRestOpenApiPlannedEnvironment Row;Row.UrlTemplate=Server[TEXT("url")].AsString();Row.Id=SemanticId(TEXT("environment"),TEXT("server"),Row.UrlTemplate);Row.Description=Server[TEXT("description")].AsString();
		Row.BaseUrl=ServerUrl(Server,{Model.RootDocument,Child(TEXT("/servers"),LexToString(Index++))});
		FString Hint=Row.Description.IsEmpty() ? VaRest::Http::GetUrlHost(Row.BaseUrl) : Row.Description;if (Hint.IsEmpty()) Hint=TEXT("Default");
		const FString Base=FVaRestOpenApiNameAllocator::Pascal(Hint).Left(96);Row.Name=Base;int32& Suffix=Suffixes.FindOrAdd(Base);Suffix=FMath::Max(Suffix,2);
		while (Used.Contains(Row.Name)) Row.Name=Base+TEXT("_")+LexToString(Suffix++);Used.Add(Row.Name);Plan.Environments.Add(MoveTemp(Row));
	}
	if (Plan.Environments.IsEmpty())
	{
		FVaRestOpenApiPlannedEnvironment Row;Row.Name=TEXT("Default");Row.UrlTemplate=TEXT("/");Row.BaseUrl=TEXT("/");Row.Id=SemanticId(TEXT("environment"),TEXT("server"),Row.UrlTemplate);Plan.Environments.Add(MoveTemp(Row));
		Issue({Model.RootDocument,TEXT("/servers")},TEXT("OA-SRV-RELATIVE"),TEXT("No servers are declared; Default uses / until a host is configured."));
	}
	Plan.DefaultEnvironment=Plan.Environments[0].Name;
}
void FPlanner::Authentication()
{
	const auto Scheme=[&](const FString& Name,FVaRestOpenApiPlannedAuth& Out)->bool {
		const auto Definition=Model.SecuritySchemes.Get(Name);const FString Kind=Definition[TEXT("type")].AsString();Out.SchemeName=Name;
		const FVaRestOpenApiLocation Location{Model.RootDocument,Child(TEXT("/components/securitySchemes"),Name)};
		if (Equal(Kind,TEXT("http")))
		{
			const FString Protocol=Definition[TEXT("scheme")].AsString();
			if (Protocol.Equals(TEXT("bearer"),ESearchCase::IgnoreCase)) { Out.Kind=EVaRestOpenApiAuthKind::Bearer;return true; }
			if (Protocol.Equals(TEXT("basic"),ESearchCase::IgnoreCase)) { Out.Kind=EVaRestOpenApiAuthKind::Basic;return true; }
		}
		else if (Equal(Kind,TEXT("apiKey")))
		{
			Out.Name=Definition[TEXT("name")].AsString();Out.Placement=Definition[TEXT("in")].AsString();
			if (Equal(Out.Placement,TEXT("cookie"))) { Issue(Location,TEXT("OA-AUTH-COOKIE"),TEXT("Cookie API keys require application-managed authentication."));return false; }
			if ((Equal(Out.Placement,TEXT("header")) || Equal(Out.Placement,TEXT("query"))) && !Out.Name.IsEmpty())
			{
				if (Equal(Out.Placement,TEXT("header")) && !VaRest::Http::IsValidHeaderName(Out.Name)) { Issue(Location,TEXT("OA-AUTH-UNSUPPORTED"),TEXT("The API key header name is invalid."));return false; }
				Out.Kind=EVaRestOpenApiAuthKind::ApiKey;Out.RedactedKeys.Add(Out.Name);return true;
			}
		}
		else if (Equal(Kind,TEXT("openIdConnect"))) { Out.Kind=EVaRestOpenApiAuthKind::Bearer;return true; }
		else if (Equal(Kind,TEXT("oauth2")))
		{
			const auto Flows=Definition[TEXT("flows")];const auto Password=Flows[TEXT("password")],Credentials=Flows[TEXT("clientCredentials")];
			if (Password.IsObject() || Credentials.IsObject())
			{
				const bool bCredentials=!Password.IsObject();Out.TokenUrl=(bCredentials ? Credentials : Password)[TEXT("tokenUrl")].AsString();
				if (!Out.TokenUrl.IsEmpty())
				{
					Out.TokenUrl=ResolveHttpReference(Plan.Environments[0].BaseUrl,Out.TokenUrl,true);Out.Kind=bCredentials ? EVaRestOpenApiAuthKind::ClientCredentials : EVaRestOpenApiAuthKind::BearerRefresh;
					if (bCredentials) Issue(Location,TEXT("OA-AUTH-CLIENTSECRET"),TEXT("A client secret distributed in a game is extractable; supply credentials according to the application's trust model."));return true;
				}
			}
			if (Flows.Has(TEXT("authorizationCode")) || Flows.Has(TEXT("implicit")) || Flows.Has(TEXT("deviceAuthorization"))) { Out.Kind=EVaRestOpenApiAuthKind::Bearer;return true; }
		}
		Issue(Location,TEXT("OA-AUTH-UNSUPPORTED"),TEXT("This security scheme requires application-managed authentication."));return false;
	};
	FVaRestOpenApiPlannedAuth Partial;bool bPartial=false;
	for (const auto& Alternative:Model.GlobalSecurity.Elements())
	{
		if (!Alternative.IsObject()) continue;
		if (Alternative.Num()==0) { Plan.Auth={};return; }
		FVaRestOpenApiPlannedAuth Candidate;int32 Supported=0;
		for (const auto& Requirement:Alternative.Members()) { FVaRestOpenApiPlannedAuth Auth;if (Scheme(Requirement.GetKey(),Auth)) { if (Supported==0) Candidate=MoveTemp(Auth);++Supported; } }
		if (Supported==Alternative.Num())
		{
			Plan.Auth=MoveTemp(Candidate);if (Supported>1) Issue({Model.RootDocument,TEXT("/security")},TEXT("OA-AUTH-AND"),TEXT("Composite authentication is not generated; the first supported scheme is applied."));return;
		}
		if (Supported>0 && !bPartial) { Partial=MoveTemp(Candidate);bPartial=true; }
	}
	if (bPartial) { Plan.Auth=MoveTemp(Partial);Issue({Model.RootDocument,TEXT("/security")},TEXT("OA-AUTH-AND"),TEXT("No fully supported security alternative exists; the first supported scheme is applied.")); }
}
}
