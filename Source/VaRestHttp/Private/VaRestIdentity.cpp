// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestIdentity.h"
#include "VaRestClientState.h"
#include "VaRestRedactor.h"

#include "Containers/StringConv.h"
#include "VaRestAuth.h"
#include "VaRestSha256.h"
#include "VaRestUrl.h"

FString VaRest::Http::HashFields(FStringView Domain, TConstArrayView<FString> Fields)
{
	FSha256 Hash;
	const auto Feed = [&Hash](FStringView Text)
	{
		const FTCHARToUTF8 Encoded(Text.GetData(), Text.Len());
		const uint64 Length = static_cast<uint64>(Encoded.Length());
		uint8 Size[8];
		for (int32 Index = 0; Index < 8; ++Index) Size[Index] = static_cast<uint8>(Length >> (Index * 8));
		Hash.Update(Size, 8);
		Hash.Update(Encoded.Get(), Encoded.Length());
	};
	Feed(Domain);
	for (const FString& Field : Fields) Feed(Field);
	return Hash.Finalize().ToHex();
}

TArray<FString> VaRest::Http::CanonicalHeaders(const FVaRestHeaders& Headers)
{
	TArray<TPair<FString, FString>> Pairs;
	for (const FVaRestHeaders::FPair& Pair : Headers) Pairs.Emplace(Pair.Key.ToLower(), Pair.Value);
	Pairs.Sort([](const auto& Left, const auto& Right)
	{
		const int32 Compared = Left.Key.Compare(Right.Key, ESearchCase::CaseSensitive);
		return Compared == 0 ? Left.Value.Compare(Right.Value, ESearchCase::CaseSensitive) < 0 : Compared < 0;
	});
	TArray<FString> Fields;
	for (const auto& Pair : Pairs) { Fields.Add(Pair.Key); Fields.Add(Pair.Value); }
	return Fields;
}

FString VaRest::Http::SessionBinding(const FVaRestSessionData& Data)
{
	TArray<FString> Fields{Data.IdentityId.ToString(EGuidFormats::DigitsLower), Data.AccessToken, Data.RefreshToken};
	Fields.Append(CanonicalHeaders(Data.Headers));
	return HashFields(TEXT("VaRest/session-export/v1"), Fields);
}

FString VaRest::Http::SessionScope(const FVaRestSessionData& Data)
{
	TArray<FString> Fields{Data.IdentityId.ToString(EGuidFormats::DigitsLower)};
	Fields.Append(CanonicalHeaders(Data.Headers));
	return HashFields(TEXT("VaRest/session-scope/v1"), Fields);
}

namespace VaRest::Http::IdentityPrivate
{
bool CanQuery(const FNativeClientRegistration* Native)
{
	return !Native || !Native->IsValid() || (!Native->Control->IsStopped() && !Native->Gate->IsClosed());
}
}
bool VaRest::Http::AuthIdentity(const IVaRestAuth* Auth, EVaRestAuthMode Mode,
	const FVaRestSessionData& Session, FString& OutIdentity, const FNativeClientRegistration* Native)
{
	OutIdentity.Reset();
	if (!IdentityPrivate::CanQuery(Native)) return false;
	if (Mode == EVaRestAuthMode::None)
	{
		OutIdentity = HashFields(TEXT("VaRest/anonymous/v1"), TArray<FString>{});
		return true;
	}
	if (!Auth)
	{
		OutIdentity = Session.Headers.IsEmpty()
			? HashFields(TEXT("VaRest/anonymous/v1"), TArray<FString>{}) : SessionScope(Session);
		return true;
	}
	const FString Error=Auth->GetConfigError();
	if (!IdentityPrivate::CanQuery(Native) || !Error.IsEmpty()) return false;
	FString ProviderIdentity;
	const bool bIdentityReady=Auth->GetIdentity(Session, ProviderIdentity);
	if (!IdentityPrivate::CanQuery(Native) || !bIdentityReady || ProviderIdentity.IsEmpty()) return false;
	TArray<FString> Fields{MoveTemp(ProviderIdentity)};
	Fields.Append(CanonicalHeaders(Session.Headers));
	OutIdentity = HashFields(TEXT("VaRest/auth-scope/v1"), Fields);
	return true;
}

FString VaRest::Http::CacheKey(FStringView Verb, FStringView Url, FStringView Identity,
	const FVaRestHeaders& Built, const FVaRestHeaders& SessionHeaders)
{
	FString Fragmentless(Url);
	int32 Fragment = INDEX_NONE;
	if (Fragmentless.FindChar(TEXT('#'), Fragment)) Fragmentless.LeftInline(Fragment);
	FVaRestHeaders Effective = Built;
	for (const auto& Pair : SessionHeaders) Effective.Set(Pair.Key, Pair.Value);
	TArray<FString> Fields{FString(Verb), MoveTemp(Fragmentless), FString(Identity)};
	Fields.Append(CanonicalHeaders(Effective));
	return HashFields(TEXT("VaRest/cache-key/v1"), Fields);
}

bool VaRest::Http::CanonicalOrigin(FStringView Url,FString& OutOrigin)
{
    OutOrigin.Reset();
    const FString Text(Url);
    int32 Separator = Text.Find(TEXT("://"),ESearchCase::CaseSensitive);
    if (Separator <= 0) return false;
    const FString Scheme = Text.Left(Separator).ToLower();
    if (Scheme != TEXT("http") && Scheme != TEXT("https")) return false;
    const int32 Begin = Separator + 3;
    int32 End = Begin;
    while (End < Text.Len() && Text[End]!=TEXT('/') && Text[End]!=TEXT('?') && Text[End]!=TEXT('#')) ++End;
    FString Authority = Text.Mid(Begin,End-Begin);
    if (Authority.IsEmpty()) return false;
    for (TCHAR Character : Authority)
        if (Character<=32 || Character>=127 || Character==TEXT('@') || Character==TEXT('%') || Character==TEXT('\\')) return false;
    FString Host, PortText;
    if (Authority.StartsWith(TEXT("[")))
    {
        int32 Close = INDEX_NONE;
        if (!Authority.FindChar(TEXT(']'),Close) || Close<=1) return false;
        Host = Authority.Left(Close+1);
        for (int32 Index=1;Index<Close;++Index)
        {
            const TCHAR Character=Authority[Index];
            if (!FChar::IsHexDigit(Character) && Character!=TEXT(':') && Character!=TEXT('.')) return false;
        }
        const FString Tail=Authority.Mid(Close+1);
        if (!Tail.IsEmpty())
        {
            if (!Tail.StartsWith(TEXT(":")) || Tail.Len()==1) return false;
            PortText=Tail.Mid(1);
        }
    }
    else
    {
        int32 Colon=INDEX_NONE;
        if (Authority.FindChar(TEXT(':'),Colon))
        {
            Host=Authority.Left(Colon); PortText=Authority.Mid(Colon+1);
            if (PortText.IsEmpty() || PortText.Contains(TEXT(":"))) return false;
        }
        else Host=Authority;
        for (TCHAR Character : Host)
            if (!FChar::IsAlnum(Character) && Character!=TEXT('.') && Character!=TEXT('-')) return false;
    }
    if (Host.IsEmpty()) return false;
    uint32 Port = Scheme==TEXT("https") ? 443 : 80;
    if (!PortText.IsEmpty())
    {
        Port=0;
        for (TCHAR Character : PortText)
        {
            if (Character<TEXT('0') || Character>TEXT('9')) return false;
            const uint32 Digit=static_cast<uint32>(Character-TEXT('0'));
            if (Port>(65535-Digit)/10) return false;
            Port=Port*10+Digit;
        }
        if (Port==0) return false;
    }
    OutOrigin=Scheme+TEXT("://")+Host.ToLower()+TEXT(":")+LexToString(Port);
    return true;
}
bool VaRest::Http::SameOrigin(FStringView Left,FStringView Right)
{
    FString A,B;
    return CanonicalOrigin(Left,A) && CanonicalOrigin(Right,B) && A==B;
}

namespace VaRest::Http::IdentityPrivate
{
    struct FProviderScope
    {
        TWeakPtr<IVaRestAuth,ESPMode::ThreadSafe> Provider;
        FString Nonce;
    };
    FString ProviderNonce(const TSharedPtr<IVaRestAuth,ESPMode::ThreadSafe>& Provider)
    {
        check(IsInGameThread());
        static auto* Providers=new TArray<FProviderScope>();
        Providers->RemoveAll([](const FProviderScope& Entry){ return !Entry.Provider.IsValid(); });
        for (const auto& Entry : *Providers)
            if (Entry.Provider.Pin()==Provider) return Entry.Nonce;
        FProviderScope Entry; Entry.Provider=Provider;
        Entry.Nonce=FGuid::NewGuid().ToString(EGuidFormats::DigitsLower);
        Providers->Add(Entry);
        return Entry.Nonce;
    }
}
bool VaRest::Http::RequestIdentityScope(const FClientConfig& Config,const FVaRestSessionData& Session,
    EVaRestAuthMode Mode,const FVaRestHeaders& BuiltHeaders,FString& OutScope,const FNativeClientRegistration* Native)
{
    OutScope.Reset();
    FString Identity;
    if (!AuthIdentity(Config.Auth.Get(),Mode,Session,Identity,Native)) return false;
    TArray<FString> Fields{Identity,LexToString(static_cast<uint8>(Mode))};
    if (Mode!=EVaRestAuthMode::None && Config.Auth.IsValid())
    {
        Fields.Add(Config.Auth->GetAuthName().ToString().ToLower());
        if (!IdentityPrivate::CanQuery(Native)) return false;
        const FName Contract=Config.Auth->GetIdentityContractName();
        if (!IdentityPrivate::CanQuery(Native)) return false;
        Fields.Add(Contract.ToString().ToLower());
        if (Contract.IsNone()) Fields.Add(IdentityPrivate::ProviderNonce(Config.Auth));
    }
    // Explicit request credentials cannot inherit the owner's anonymous/old-account scope.
    FVaRestHeaders Credentials;
    const FVaRestRedactRules Rules;
    for (const auto& Pair : BuiltHeaders)
        if (Rules.ShouldRedact(Pair.Key,EVaRestRedactKeyKind::Header)) Credentials.Add(Pair.Key,Pair.Value);
    Fields.Append(CanonicalHeaders(Credentials));
    if (Mode!=EVaRestAuthMode::None) Fields.Append(CanonicalHeaders(Session.Headers));
    OutScope=HashFields(TEXT("VaRest/request-scope/v1"),Fields);
    return true;
}
