// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiUrlPolicy.h"
#include "VaRestOpenApiAnalysis.h"
#include "VaRestOpenApiPlanning.h"

namespace VaRest::OpenApi
{
TOptional<FSourceUrl> FSourceUrl::Parse(FString Value)
{
	int32 Fragment;if (Value.FindChar(TEXT('#'),Fragment)) Value.LeftInline(Fragment);
	FSourceUrl Out;if (!SplitSourceUrl(Value,Out.Origin,Out.Path,Out.Query)) return {};
	const int32 Scheme=Out.Origin.Find(TEXT("://"));FString Authority=Out.Origin.Mid(Scheme+3),Port;
	if (Authority.StartsWith(TEXT("[")))
	{
		int32 End;if (!Authority.FindChar(TEXT(']'),End) || End<=1) return {};Out.Host=Authority.Mid(1,End-1);
		const FString Tail=Authority.Mid(End+1);if (!Tail.IsEmpty()) { if (!Tail.StartsWith(TEXT(":"))) return {};Port=Tail.Mid(1);if (Port.IsEmpty()) return {}; }
	}
	else
	{
		int32 Colon;if (Authority.FindChar(TEXT(':'),Colon)) { Out.Host=Authority.Left(Colon);Port=Authority.Mid(Colon+1);if (Port.IsEmpty() || Port.Contains(TEXT(":"))) return {}; }else Out.Host=Authority;
	}
	if (Out.Host.IsEmpty()) return {};
	if (!Port.IsEmpty()) { int32 Number=0;if (Port.Len()>5) return {};for (TCHAR Char:Port) { if (Char<TEXT('0') || Char>TEXT('9')) return {};Number=Number*10+Char-TEXT('0'); }if (Number<1 || Number>65535) return {}; }
	Out.Uri=Out.Origin+Out.Path+Out.Query;const int32 Slash=Out.Path.Find(TEXT("/"),ESearchCase::CaseSensitive,ESearchDir::FromEnd);Out.Base=Out.Origin+Out.Path.Left(Slash+1);return Out;
}
bool AllowSourceUrl(const FVaRestOpenApiReadRequest& Request,FSourceUrl& Out,FString& Error)
{
	if (!Request.bUrl) { Error=TEXT("The URL reader accepts only HTTP(S) source documents.");return false; }
	const auto Target=FSourceUrl::Parse(Request.Uri),Root=FSourceUrl::Parse(Request.RootBase);
	if (!Target.IsSet() || !Root.IsSet()) { Error=TEXT("The source URL or its origin/directory boundary is invalid.");return false; }
	FString Prefix=Root.GetValue().Path;if (!Prefix.EndsWith(TEXT("/"))) Prefix+=TEXT("/");
	if (!Equal(Target.GetValue().Origin,Root.GetValue().Origin) || !Target.GetValue().Path.StartsWith(Prefix,ESearchCase::CaseSensitive))
	{ Error=TEXT("The URL is outside the admitted source origin/directory. Bundle external references into the spec folder.");return false; }
	Out=Target.GetValue();return true;
}
TOptional<FSourceUrl> SourceRedirect(const FVaRestOpenApiReadRequest& Request,const FString& Current,const FString& Location,FString& Error)
{
	if (Location.IsEmpty()) { Error=TEXT("The redirect has no Location header.");return {}; }
	FString Relative=Location;int32 Fragment;if (Relative.FindChar(TEXT('#'),Fragment)) Relative.LeftInline(Fragment);
	auto Next=Request;Next.Uri=Relative.IsEmpty() ? Current : ResolveHttpReference(Current,Relative);FSourceUrl Out;if (!AllowSourceUrl(Next,Out,Error)) return {};return Out;
}
}
