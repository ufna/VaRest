// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace VaRest::Http
{
	struct FSha256Digest
	{
		uint8 Bytes[32] = {};

		/** 64 lower-case hex digits */
		VARESTHTTP_API FString ToHex() const;
	};

	/**
	 * SHA-256 (FIPS 180-4). Ours because FPlatformMisc::GetSHA256Signature has no generic implementation
	 * (GenericPlatformMisc.cpp:2019-2024 is checkf(false)). Any thread; one instance is not shared between threads.
	 */
	class VARESTHTTP_API FSha256
	{
	public:
		FSha256();
		void Update(const void* Data, int64 Size);
		/** The digest; the object is spent afterwards */
		FSha256Digest Finalize();

		static FSha256Digest Hash(TConstArrayView<uint8> Data);

	private:
		void Transform(const uint8* Block);

		uint32 Words[8];
		uint64 TotalBytes = 0;
		uint8 Buffer[64];
		int32 BufferLen = 0;
	};
}
