// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "VaRestApiTypes.h"
class UVaRestApi;
class UScriptStruct;
class FProperty;
namespace VaRestM5Endpoint
{
inline constexpr TCHAR Folder[] = TEXT("/VaRestM5Smoke/Endpoint001");
inline constexpr TCHAR ApiPath[] = TEXT("/VaRestM5Smoke/Endpoint001/SM5_Api.SM5_Api");
inline constexpr TCHAR RootPath[] = TEXT("/VaRestM5Smoke/Endpoint001/Types/SM5_Response.SM5_Response");
inline constexpr TCHAR NestedPath[] = TEXT("/VaRestM5Smoke/Endpoint001/Types/SM5_Nested.SM5_Nested");
inline constexpr TCHAR EnumPath[] = TEXT("/VaRestM5Smoke/Endpoint001/Types/SM5_State.SM5_State");
VARESTM5SMOKE_API FString WireBody();
VARESTM5SMOKE_API FString ReplyBody(int32 Case);
VARESTM5SMOKE_API FProperty* RuleProperty(const UVaRestApi* Api, UScriptStruct* Root, const TCHAR* Wire);
/** Editor author only: initializes already-owned valid struct memory using the real default converter. */
VARESTM5SMOKE_API bool PreparePayload(UVaRestApi* Api, UScriptStruct* Root, void* Memory, FString& Error);
VARESTM5SMOKE_API bool VerifyDefaultConversion(UVaRestApi* Api, UScriptStruct* Root, FString& Error);
}
