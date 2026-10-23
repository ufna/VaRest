// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestIdentity.h"

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

bool VaRest::Http::AuthIdentity(const IVaRestAuth* Auth, EVaRestAuthMode Mode,
	const FVaRestSessionData& Session, FString& OutIdentity)
{
	OutIdentity.Reset();
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
	if (!Auth->GetConfigError().IsEmpty()) return false;
	FString ProviderIdentity;
	if (!Auth->GetIdentity(Session, ProviderIdentity) || ProviderIdentity.IsEmpty()) return false;
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
