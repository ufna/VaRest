// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Crc.h"

namespace VaRest::WebSocket
{
	/** FString's == ignores case; JSON routes/ids/keys must also distinguish text after an embedded NUL. */
	inline bool SameText(FStringView A, FStringView B)
	{
		return A.Len() == B.Len() && (A.IsEmpty() || FMemory::Memcmp(A.GetData(), B.GetData(), static_cast<SIZE_T>(A.Len()) * sizeof(TCHAR)) == 0);
	}
	struct FSocketTextKey
	{
		FString Text;
		FSocketTextKey() = default;
		explicit FSocketTextKey(FStringView InText) : Text(InText) {}
		friend bool operator==(const FSocketTextKey& A, const FSocketTextKey& B) { return SameText(A.Text, B.Text); }
		friend uint32 GetTypeHash(const FSocketTextKey& Key)
		{
			// Collisions (including strings with a NUL prefix) are resolved by the exact equality above.
			return FCrc::StrCrc32(*Key.Text);
		}
	};
}
