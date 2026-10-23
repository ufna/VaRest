// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestPersistenceGuard.h"

#include "Containers/StringConv.h"
#include "VaRestIdentity.h"
#include "VaRestJson.h"
#include "VaRestRedactor.h"
#include "VaRestUrl.h"

bool VaRest::Http::FPersistenceGuard::ContainsSecret(TConstArrayView<uint8> Bytes) const
{
    if (bRefusePersistence || bWorkExhausted || RemainingWork <= 0
        || (AbortSignal.IsValid() && AbortSignal->load(std::memory_order_relaxed))) return true;
    int64 Work = 0;
    const auto Tick = [&]()
    {
        if (RemainingWork <= 0) { bWorkExhausted = true; return false; }
        --RemainingWork;
        ++Work;
        if (RemainingWork == 0 || ((Work & 4095) == 0 && AbortSignal.IsValid()
            && AbortSignal->load(std::memory_order_relaxed)))
        { bWorkExhausted = true; return false; }
        return true;
    };
    for (const TArray<uint8>& Needle : Needles)
    {
        if (Needle.IsEmpty() || Needle.Num() > Bytes.Num()) continue;
        TArray<int32> Prefix;
        Prefix.Init(0, Needle.Num());
        for (int32 Index = 1, Matched = 0; Index < Needle.Num(); ++Index)
        {
            for (;;)
            {
                if (!Tick()) return true;
                if (Needle[Index] == Needle[Matched]) { ++Matched; break; }
                if (Matched == 0) break;
                Matched = Prefix[Matched - 1];
            }
            Prefix[Index] = Matched;
        }
        for (int32 Index = 0, Matched = 0; Index < Bytes.Num(); ++Index)
        {
            for (;;)
            {
                if (!Tick()) return true;
                if (Bytes[Index] == Needle[Matched]) { ++Matched; break; }
                if (Matched == 0) break;
                Matched = Prefix[Matched - 1];
            }
            if (Matched == Needle.Num()) return true;
        }
    }
    return false;
}

bool VaRest::Http::FPersistenceGuard::AllowsBody(EVaRestBodyKind Kind, FStringView ContentType,
    TConstArrayView<uint8> Bytes) const
{
    if (Kind == EVaRestBodyKind::Stream) return false;
    if (ContainsSecret(Bytes)) return false;
    if (Kind == EVaRestBodyKind::Json || FString(ContentType).Contains(TEXT("json")))
    {
        TArray<uint8> Discarded;
        const auto Result = VaRest::Json::RedactPrefix(
            FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num()),
            Rules, EVaRestJsonFormat::Condensed, Discarded, true);
        return !Result.bRedacted && Result.StopReason == EVaRestRedactStop::Complete;
    }
    // Raw text/byte builders can declare a known form media type. Check its essence, not a substring.
    FString MediaType(ContentType);
    int32 Parameter = INDEX_NONE;
    if (MediaType.FindChar(TEXT(';'), Parameter)) MediaType.LeftInline(Parameter);
    MediaType.TrimStartAndEndInline();
    if (Kind == EVaRestBodyKind::Form
        || MediaType.Equals(TEXT("application/x-www-form-urlencoded"), ESearchCase::IgnoreCase))
    {
        for (int32 Start=0;Start<Bytes.Num();)
        {
            int32 End=Start; while (End<Bytes.Num() && Bytes[End]!='&') ++End;
            int32 Equals=Start; while (Equals<End && Bytes[Equals]!='=') ++Equals;
            if (Equals-Start>8192) return false; // bounded key conversion, never copy form values
            const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()+Start),Equals-Start);
            const FString Name(Converted.Length(),Converted.Get());
            if (Rules.ShouldRedact(PercentDecode(Name,true),EVaRestRedactKeyKind::Form)) return false;
            Start=End+1;
        }
    }
    return true;
}

int64 VaRest::Http::PersistenceGuardReservation(EVaRestBodyKind Kind,FStringView ContentType,int64 BodyBytes)
{
    const bool bJson=Kind==EVaRestBodyKind::Json || FString(ContentType).Contains(TEXT("json"));
    return BodyBytes*(bJson ? 8LL : 1LL)+2*1024*1024;
}

void VaRest::Http::FPersistenceGuard::AddSecret(FStringView Value)
{
    if (Value.IsEmpty()) return;
    if (Value.Len()>8192) { bRefusePersistence=true; return; }
    const auto AddForm=[this](FStringView Form)
    {
        const FTCHARToUTF8 Utf8(Form.GetData(),Form.Len());
        TArray<uint8> Bytes(reinterpret_cast<const uint8*>(Utf8.Get()),Utf8.Length());
        if (Needles.Contains(Bytes)) return;
        if (Needles.Num()>=64 || Utf8.Length()>16384 || NeedleBytes>65536-Utf8.Length())
        { bRefusePersistence=true; return; }
        NeedleBytes+=Utf8.Length(); Needles.Add(MoveTemp(Bytes));
    };
    AddForm(Value);
    FString Percent=PercentEncode(Value);
    AddForm(Percent);
    for (int32 Index=0;Index+2<Percent.Len();++Index)
        if (Percent[Index]==TEXT('%')) { Percent[Index+1]=FChar::ToLower(Percent[Index+1]); Percent[Index+2]=FChar::ToLower(Percent[Index+2]); Index+=2; }
    AddForm(Percent);
    FString Form=FormEncode(Value);
    AddForm(Form);
    for (int32 Index=0;Index+2<Form.Len();++Index)
        if (Form[Index]==TEXT('%')) { Form[Index+1]=FChar::ToLower(Form[Index+1]); Form[Index+2]=FChar::ToLower(Form[Index+2]); Index+=2; }
    AddForm(Form);
    const FString Quoted=FVaRestJson::From(FString(Value)).ToString();
    if (Quoted.Len()>=2) AddForm(Quoted.Mid(1,Quoted.Len()-2));
    const FTCHARToUTF8 Raw(Value.GetData(),Value.Len());
    FString Hex;
    const TCHAR* Digits=TEXT("0123456789abcdef");
    for (int32 Index=0;Index<Raw.Length();++Index)
    {
        const uint8 Byte=static_cast<uint8>(Raw.Get()[Index]);
        Hex.AppendChar(Digits[Byte>>4]); Hex.AppendChar(Digits[Byte&15]);
    }
    AddForm(Hex); AddForm(Hex.ToUpper());
}
void VaRest::Http::FPersistenceGuard::AddKey(FStringView Key)
{
    if (ExtraKeys.ContainsByPredicate([Key](const FString& Existing)
        { return Existing.Equals(FString(Key), ESearchCase::CaseSensitive); })) return;
    int64 Bytes=0;
    for (const FString& Existing : ExtraKeys) { const FTCHARToUTF8 Utf8(*Existing); Bytes+=Utf8.Length(); }
    const FTCHARToUTF8 Incoming(Key.GetData(),Key.Len());
    if (ExtraKeys.Num()>=128 || Incoming.Length()>16384-Bytes)
    { bRefusePersistence=true; return; }
    if (!FVaRestRedactRules::IsAcceptableKey(Key)) { bRefusePersistence=true; return; }
    ExtraKeys.Add(FString(Key)); Rules.AddKey(Key);
}
void VaRest::Http::FPersistenceGuard::UnionWith(const FPersistenceGuard& Other)
{
    bRefusePersistence|=Other.bRefusePersistence;
    for (const auto& Needle : Other.Needles)
    {
        if (Needles.Contains(Needle)) continue;
        if (Needles.Num()>=64 || Needle.Num()>16384 || NeedleBytes>65536-Needle.Num()) { bRefusePersistence=true; break; }
        NeedleBytes+=Needle.Num(); Needles.Add(Needle);
    }
    for (const FString& Key : Other.ExtraKeys) AddKey(Key);
}
bool VaRest::Http::FPersistenceGuard::AllowsUrl(FStringView Url) const
{
    FString Origin;
    if (!CanonicalOrigin(Url,Origin)) return false;
    const FVaRestRedactor Structural(Rules,TArray<FString>());
    if (!Structural.RedactUrl(Url).Equals(FString(Url),ESearchCase::CaseSensitive)) return false;
    const FTCHARToUTF8 Utf8(Url.GetData(),Url.Len());
    return !ContainsSecret(MakeArrayView(reinterpret_cast<const uint8*>(Utf8.Get()),Utf8.Length()));
}
FVaRestHeaders VaRest::Http::FPersistenceGuard::SafeHeaders(const FVaRestHeaders& Input) const
{
    FVaRestHeaders Result;
    TArray<FString> HopNames{TEXT("connection"),TEXT("keep-alive"),TEXT("proxy-authenticate"),
        TEXT("proxy-authorization"),TEXT("te"),TEXT("trailer"),TEXT("transfer-encoding"),TEXT("upgrade")};
    for (const auto& Pair : Input)
        if (Pair.Key.Equals(TEXT("Connection"),ESearchCase::IgnoreCase))
        {
            TArray<FString> Named; Pair.Value.ParseIntoArray(Named,TEXT(","),false);
            for (const FString& Name : Named) HopNames.AddUnique(Name.TrimStartAndEnd().ToLower());
        }
    for (const auto& Pair : Input)
    {
        if (Rules.ShouldRedact(Pair.Key,EVaRestRedactKeyKind::Header) || HopNames.Contains(Pair.Key.ToLower())) continue;
        const FTCHARToUTF8 Utf8(*Pair.Value,Pair.Value.Len());
        if (ContainsSecret(MakeArrayView(reinterpret_cast<const uint8*>(Utf8.Get()),Utf8.Length()))) continue;
        Result.Add(Pair.Key,Pair.Value);
    }
    return Result;
}
