// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
class UScriptStruct;
class UVaRestApi;

namespace VaRestM5SettingsOnly
{
// Textual FindObject identifiers only: no UObject/native hard reference or soft-path loader.
inline constexpr TCHAR Folder[] = TEXT("/VaRestM5Smoke/SettingsOnly001");
inline constexpr TCHAR ApiPath[] = TEXT("/VaRestM5Smoke/SettingsOnly001/SM5_SettingsOnlyApi.SM5_SettingsOnlyApi");
inline constexpr TCHAR RootPath[] = TEXT("/VaRestM5Smoke/SettingsOnly001/Types/SM5_SettingsOnlyResponse.SM5_SettingsOnlyResponse");
inline constexpr TCHAR NestedPath[] = TEXT("/VaRestM5Smoke/SettingsOnly001/Types/SM5_SettingsOnlyNested.SM5_SettingsOnlyNested");
inline constexpr TCHAR EnumPath[] = TEXT("/VaRestM5Smoke/SettingsOnly001/Types/SM5_SettingsOnlyState.SM5_SettingsOnlyState");

/** Calls only the real public default-options converter; no rule registration or loading occurs here.
 * Editor authoring invokes the same semantic oracle before save. Runtime supplies no-load lookups. */
VARESTM5SMOKE_API bool VerifyDefaultConversion(UVaRestApi* Api, UScriptStruct* Root, FString& Error);
}
