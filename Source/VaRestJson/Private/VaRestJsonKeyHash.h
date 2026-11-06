// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

namespace VaRest::Json::KeyHash
{
	/** Pure SipHash-2-4 primitive. Explicit key arguments permit fixed-vector qualification independently of process entropy. */
	VARESTJSON_API uint64 SipHash24(FUtf8StringView Input, uint64 Key0, uint64 Key1);
}
