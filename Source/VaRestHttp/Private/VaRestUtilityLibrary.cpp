// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestUtilityLibrary.h"
#include "VaRestHttpSettings.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Base64.h"
#include "Misc/SecureHash.h"

FString UVaRestUtilityLibrary::PercentEncode(const FString& Source) { return FGenericPlatformHttp::UrlEncode(Source); }
FString UVaRestUtilityLibrary::Base64Encode(const FString& Source)
{
	const FTCHARToUTF8 Text(*Source,Source.Len());return FBase64::Encode(reinterpret_cast<const uint8*>(Text.Get()),Text.Length());
}
bool UVaRestUtilityLibrary::Base64Decode(const FString& Source,FString& Destination)
{
	TArray<uint8> Bytes;Destination.Reset();if (!FBase64::Decode(Source,Bytes)) return false;
	if (!Bytes.IsEmpty()) { const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()),Bytes.Num());Destination=FString(Text.Length(),Text.Get()); }return true;
}
bool UVaRestUtilityLibrary::Base64EncodeData(const TArray<uint8>& Data,FString& Destination)
{
	Destination=Data.IsEmpty() ? FString() : FBase64::Encode(Data);return !Data.IsEmpty();
}
bool UVaRestUtilityLibrary::Base64DecodeData(const FString& Source,TArray<uint8>& Destination) { return FBase64::Decode(Source,Destination); }
FString UVaRestUtilityLibrary::Md5Utf8(const FString& Source)
{
	const FTCHARToUTF8 Text(*Source,Source.Len());FMD5 Hash;Hash.Update(reinterpret_cast<const uint8*>(Text.Get()),Text.Length());uint8 Digest[16];Hash.Final(Digest);return BytesToHex(Digest,16).ToLower();
}
FString UVaRestUtilityLibrary::Sha1Utf8(const FString& Source)
{
	const FTCHARToUTF8 Text(*Source,Source.Len());FSHA1 Hash;Hash.Update(reinterpret_cast<const uint8*>(Text.Get()),Text.Length());Hash.Final();uint8 Digest[20];Hash.GetHash(Digest);return BytesToHex(Digest,20).ToLower();
}
FString UVaRestUtilityLibrary::GetVaRestVersion()
{
	const auto Plugin=IPluginManager::Get().FindPlugin(TEXT("VaRest"));return Plugin ? Plugin->GetDescriptor().VersionName : TEXT("invalid");
}
FVaRestWorldUrl UVaRestUtilityLibrary::GetWorldUrl(const UObject* WorldContextObject)
{
	FVaRestWorldUrl Result;const auto* World=GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject,EGetWorldErrorMode::ReturnNull) : nullptr;if (!World) return Result;
	const auto& Url=World->URL;Result.Protocol=Url.Protocol;Result.Host=Url.Host;Result.Port=Url.Port;Result.Valid=Url.Valid;Result.Map=Url.Map;Result.RedirectURL=Url.RedirectURL;Result.Op=Url.Op;Result.Portal=Url.Portal;return Result;
}
UVaRestHttpSettings* UVaRestUtilityLibrary::GetHttpSettings() { return GetMutableDefault<UVaRestHttpSettings>(); }
