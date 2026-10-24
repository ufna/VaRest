// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestCachePolicy.h"

#include "Containers/UnrealString.h"
#include "VaRestJsonRedact.h"
#include "VaRestUrl.h"

bool VaRest::Http::ReadDeltaSeconds(FStringView Text, double& OutSeconds)
{
	if (Text.IsEmpty())
	{
		return false;
	}
	uint64 Value = 0;
	constexpr uint64 Maximum = 315360000ULL; // 10 years; bounded persisted arithmetic
	for (TCHAR Character : Text)
	{
		if (Character < TEXT('0') || Character > TEXT('9'))
		{
			return false;
		}
		const uint64 Digit = static_cast<uint64>(Character - TEXT('0'));
		if (Value > (Maximum - Digit) / 10)
		{
			return false;
		}
		Value = Value * 10 + Digit;
	}
	OutSeconds = static_cast<double>(Value);
	return true;
}

namespace VaRest::Http::CachePolicyPrivate
{
	TArray<FString> Tokens(const FVaRestHeaders& Headers, const TCHAR* Header)
	{
		TArray<FString> Result;
		for (const auto& Pair : Headers)
		{
			if (!Pair.Key.Equals(Header, ESearchCase::IgnoreCase))
			{
				continue;
			}
            int32 Begin=0;
            bool bQuoted=false,bEscaped=false;
            for (int32 Index=0;Index<Pair.Value.Len();++Index)
            {
                const TCHAR Character=Pair.Value[Index];
                if (bEscaped) { bEscaped=false; continue; }
                if (bQuoted && Character==TEXT('\\')) { bEscaped=true; continue; }
                if (Character==TEXT('"')) bQuoted=!bQuoted;
                else if (Character==TEXT(',') && !bQuoted)
                {
                    Result.Add(Pair.Value.Mid(Begin,Index-Begin).TrimStartAndEnd());
                    Begin=Index+1;
                }
            }
            Result.Add(Pair.Value.Mid(Begin).TrimStartAndEnd());
		}
		return Result;
	}
	bool HasDirective(const FVaRestHeaders& Headers, const TCHAR* Wanted)
	{
		for (const FString& Directive : Tokens(Headers, TEXT("Cache-Control")))
		{
			FString Name, Value;
			if (!Directive.Split(TEXT("="), &Name, &Value))
			{
				Name = Directive;
			}
			if (Name.TrimStartAndEnd().Equals(Wanted, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}
	bool Forbidden(const FString& Name, const FVaRestHeaders& Headers)
	{
		const FVaRestRedactRules Rules;
		if (Rules.ShouldRedact(Name, EVaRestRedactKeyKind::Header))
		{
			return true;
		}
		for (const TCHAR* Item : {TEXT("Connection"), TEXT("Keep-Alive"), TEXT("Proxy-Authenticate"),
				 TEXT("Proxy-Authorization"), TEXT("TE"), TEXT("Trailer"), TEXT("Transfer-Encoding"), TEXT("Upgrade"), TEXT("Content-Length")})
		{
			if (Name.Equals(Item, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		for (const FString& Item : Tokens(Headers, TEXT("Connection")))
		{
			if (Name.Equals(Item, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}
} // namespace VaRest::Http::CachePolicyPrivate
bool VaRest::Http::RequestRequiresCacheValidation(const FVaRestHeaders& Headers)
{
    for (const FString& Directive : CachePolicyPrivate::Tokens(Headers,TEXT("Cache-Control")))
    {
        FString Name,Value;
        if (!Directive.Split(TEXT("="),&Name,&Value)) Name=Directive;
        Name.TrimStartAndEndInline();Value.TrimStartAndEndInline();
        if (Name.Equals(TEXT("no-cache"),ESearchCase::IgnoreCase)) return true;
        if (!Name.Equals(TEXT("max-age"),ESearchCase::IgnoreCase)) continue;
        if (Value.StartsWith(TEXT("\"")))
        {
            if (Value.Len()<2 || Value[Value.Len()-1]!=TEXT('"')) continue;
            FString Decoded;
            bool bValid=true;
            for (int32 Index=1;Index<Value.Len()-1;++Index)
            {
                TCHAR Character=Value[Index];
                if (Character==TEXT('\\'))
                {
                    if (++Index>=Value.Len()-1) { bValid=false; break; }
                    Character=Value[Index];
                }
                else if (Character==TEXT('"')) { bValid=false; break; }
                Decoded.AppendChar(Character);
            }
            if (!bValid) continue;
            Value=MoveTemp(Decoded);
        }
        double Seconds=0;
        if (ReadDeltaSeconds(Value,Seconds) && Seconds==0) return true;
    }
    return false;
}
VaRest::Http::FCacheMetadata VaRest::Http::ReadCacheMetadata(const FVaRestHeaders& Headers,
	double DefaultTtl, double Now, FDateTime NowUtc)
{
	FCacheMetadata Result;
	Result.StoredAt = Now;
	Result.StoredUtc = NowUtc;
	Result.Lifetime = FMath::IsFinite(DefaultTtl) ? FMath::Clamp(DefaultTtl, 0.0, 315360000.0) : 0.0;
	bool bSawAge = false;
	bool bInvalidAge = false;
	double MaxAge = 0.0;
	for (const FString& Directive : CachePolicyPrivate::Tokens(Headers, TEXT("Cache-Control")))
	{
		FString Name, Value;
		if (!Directive.Split(TEXT("="), &Name, &Value))
		{
			Name = Directive;
		}
		Name = Name.TrimStartAndEnd().ToLower();
		Value = Value.TrimStartAndEnd();
		if (Name == TEXT("no-store"))
		{
			Result.bNoStore = true;
		}
		if (Name == TEXT("no-cache"))
		{
			Result.bNoCache = true;
		}
		if (Name == TEXT("must-revalidate"))
		{
			Result.bMustRevalidate = true;
		}
		if (Name != TEXT("max-age"))
		{
			continue;
		}
		if (Value.Len() >= 2 && Value[0] == TEXT('"') && Value[Value.Len() - 1] == TEXT('"'))
		{
			Value = Value.Mid(1, Value.Len() - 2);
		}
		double Parsed = 0.0;
		if (!ReadDeltaSeconds(Value, Parsed) || (bSawAge && Parsed != MaxAge))
		{
			bInvalidAge = true;
		}
		MaxAge = Parsed;
		bSawAge = true;
	}
	FDateTime Date, Expires;
	const bool bDate = FDateTime::ParseHttpDate(Headers.Get(TEXT("Date")), Date);
	const bool bExpires = FDateTime::ParseHttpDate(Headers.Get(TEXT("Expires")), Expires);
	if (bSawAge)
	{
		Result.Lifetime = bInvalidAge ? 0.0 : MaxAge;
	}
	else if (bExpires)
	{
		Result.Lifetime = FMath::Clamp((Expires - (bDate ? Date : NowUtc)).GetTotalSeconds(), 0.0, 315360000.0);
	}
	if (bDate)
	{
		Result.InitialAge = FMath::Max(0.0, (NowUtc - Date).GetTotalSeconds());
	}
	for (const auto& Pair : Headers)
	{
		if (!Pair.Key.Equals(TEXT("Age"), ESearchCase::IgnoreCase))
		{
			continue;
		}
		double Parsed = 0.0;
		if (!ReadDeltaSeconds(Pair.Value.TrimStartAndEnd(), Parsed))
		{
			Result.Lifetime = 0.0;
		}
		else
		{
			Result.InitialAge = FMath::Max(Result.InitialAge, Parsed);
		}
	}
	Result.ETag = Headers.Get(TEXT("ETag"));
	Result.LastModified = Headers.Get(TEXT("Last-Modified"));
	for (FString Name : CachePolicyPrivate::Tokens(Headers, TEXT("Vary")))
	{
		Name.ToLowerInline();
		if (Name == TEXT("*") || !IsValidHeaderName(Name))
		{
			Result.bNoStore = true;
		}
		else
		{
			Result.Vary.AddUnique(MoveTemp(Name));
		}
	}
	return Result;
}
bool VaRest::Http::CacheHeadersEligible(const FVaRestHeaders& Request, const FVaRestHeaders& Response)
{
	for (const TCHAR* Name : {TEXT("Range"), TEXT("If-Range"), TEXT("If-None-Match"), TEXT("If-Modified-Since")})
	{
		if (Request.Contains(Name))
		{
			return false;
		}
	}
	if (CachePolicyPrivate::HasDirective(Request, TEXT("no-store")))
	{
		return false;
	}
	if (Response.Contains(TEXT("Set-Cookie")) || Response.Contains(TEXT("WWW-Authenticate")))
	{
		return false;
	}
	for (const FString& Name : CachePolicyPrivate::Tokens(Response, TEXT("Vary")))
	{
		if (Name == TEXT("*") || !IsValidHeaderName(Name) || !Request.Contains(Name))
		{
			return false;
		}
	}
	return true;
}
FVaRestHeaders VaRest::Http::MergeNotModified(const FVaRestHeaders& Stored,
	const FVaRestHeaders& Fresh, int64 BodyBytes, bool bHead)
{
	FVaRestHeaders Result;
	for (const auto& Pair : Stored)
	{
		if (CachePolicyPrivate::Forbidden(Pair.Key, Stored) || CachePolicyPrivate::Forbidden(Pair.Key, Fresh))
		{
			continue;
		}
		if (Pair.Key.Equals(TEXT("Warning"), ESearchCase::IgnoreCase) && Pair.Value.TrimStart().StartsWith(TEXT("1")))
		{
			continue;
		}
		Result.Add(Pair.Key, Pair.Value);
	}
	TArray<FString> Replaced;
	for (const auto& Pair : Fresh)
	{
		if (CachePolicyPrivate::Forbidden(Pair.Key, Stored) || CachePolicyPrivate::Forbidden(Pair.Key, Fresh))
		{
			continue;
		}
		const FString Lower = Pair.Key.ToLower();
		if (!Replaced.Contains(Lower))
		{
			Result.Remove(Pair.Key);
			Replaced.Add(Lower);
		}
		Result.Add(Pair.Key, Pair.Value);
	}
	if (bHead)
	{
		const FString RepresentationLength = Stored.Get(TEXT("Content-Length"));
		if (!RepresentationLength.IsEmpty())
		{
			Result.Set(TEXT("Content-Length"), RepresentationLength);
		}
	}
	else
	{
		Result.Set(TEXT("Content-Length"), LexToString(BodyBytes));
	}
	return Result;
}
