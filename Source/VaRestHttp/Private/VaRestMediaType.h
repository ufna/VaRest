// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace VaRest::Http
{
/** Complete media-type token/quoted-parameter grammar; response sniffing remains separate. */
bool IsValidMediaType(FStringView Media);
} // namespace VaRest::Http
