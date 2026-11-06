// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace VaRest::Http
{
/** HTTP(S) authority syntax, including IPv6/embedded IPv4. This does not resolve DNS or establish availability. */
bool IsValidHttpUrl(const FString& Url, bool bAllowUserInfo = true);
} // namespace VaRest::Http
