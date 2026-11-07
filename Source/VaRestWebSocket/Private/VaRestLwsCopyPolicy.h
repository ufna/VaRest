// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace VaRest::WebSocket::Lws
{
// The private archive renames functions AND data. Monolithic no longer means an Engine-shared LWS copy.
#if defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
inline constexpr bool SharesEngineLws = false;
#else
inline constexpr bool SharesEngineLws = IS_MONOLITHIC || UE_MERGED_MODULES;
#endif
}
