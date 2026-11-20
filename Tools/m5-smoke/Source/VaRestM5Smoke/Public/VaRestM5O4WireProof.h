// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
class UVaRestApi;
class UScriptStruct;

// Tools-only opt-in witness; no reflected actor, Blueprint CDO, or production API seam.
namespace VaRestM5O4
{
inline constexpr const TCHAR* Folder = TEXT("/Game/VaRestO4Wire001");
inline constexpr const TCHAR* ApiPackage = TEXT("/Game/VaRestO4Wire001/O4_Api");
inline constexpr const TCHAR* ApiPath = TEXT("/Game/VaRestO4Wire001/O4_Api.O4_Api");
inline constexpr const TCHAR* TypesFolder = TEXT("/Game/VaRestO4Wire001/Types");
inline constexpr const TCHAR* BaseUrl = TEXT("http://127.0.0.1:18765");
inline constexpr const TCHAR* EchoPath = TEXT("/echo/o4wire");

VARESTM5SMOKE_API bool VerifyConversion(UVaRestApi* Api, UScriptStruct* Root, FString& Wire, FString& Error);
VARESTM5SMOKE_API void Start(bool bCooked);
VARESTM5SMOKE_API void Stop();
// -1 unarmed, 0 pending, 1 passed, 2 failed. Read only on the game thread.
VARESTM5SMOKE_API int32 GetOutcome(FString& Detail, FString& Wire);
}
