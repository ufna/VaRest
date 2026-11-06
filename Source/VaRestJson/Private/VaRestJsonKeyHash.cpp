// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonKeyHash.h"
#include "VaRestJsonNode.h"
#include "VaRestJsonLog.h"
#if PLATFORM_LINUX || PLATFORM_ANDROID
#include <errno.h>
#if PLATFORM_ANDROID
#include <sys/syscall.h>
#include <unistd.h>
#else
#include <sys/random.h>
#endif
#elif PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
THIRD_PARTY_INCLUDES_START
#include <bcrypt.h>
THIRD_PARTY_INCLUDES_END
#include "Windows/HideWindowsPlatformTypes.h"
#elif PLATFORM_APPLE
THIRD_PARTY_INCLUDES_START
#include <Security/SecRandom.h>
THIRD_PARTY_INCLUDES_END
#endif

namespace VaRest::Json::KeyHash
{
	namespace
	{
		uint64 Rotate(uint64 Value, uint32 Bits) { return (Value << Bits) | (Value >> (64 - Bits)); }
		void Round(uint64& A, uint64& B, uint64& C, uint64& D)
		{
			A += B; B = Rotate(B, 13); B ^= A; A = Rotate(A, 32);
			C += D; D = Rotate(D, 16); D ^= C;
			A += D; D = Rotate(D, 21); D ^= A;
			C += B; B = Rotate(B, 17); B ^= C; C = Rotate(C, 32);
		}
		uint64 LoadLittleEndian(const uint8* Bytes)
		{
			// Byte assembly is alignment-safe and endian-independent; compilers fold it into an unaligned load where valid.
			uint64 Word = 0; for (uint32 Index = 0; Index < 8; ++Index) Word |= static_cast<uint64>(Bytes[Index]) << (8 * Index); return Word;
		}
	}
	uint64 SipHash24(FUtf8StringView Input, uint64 Key0, uint64 Key1)
	{
		uint64 A = 0x736f6d6570736575ULL ^ Key0, B = 0x646f72616e646f6dULL ^ Key1;
		uint64 C = 0x6c7967656e657261ULL ^ Key0, D = 0x7465646279746573ULL ^ Key1;
		const auto* Bytes = reinterpret_cast<const uint8*>(Input.GetData()); int32 Remaining = Input.Len();
		while (Remaining >= 8)
		{
			const uint64 Word = LoadLittleEndian(Bytes); D ^= Word;
			Round(A, B, C, D); Round(A, B, C, D); A ^= Word; Bytes += 8; Remaining -= 8;
		}
		uint64 Tail = static_cast<uint64>(Input.Len()) << 56;
		for (int32 Index = 0; Index < Remaining; ++Index) Tail |= static_cast<uint64>(Bytes[Index]) << (8 * Index);
		D ^= Tail; Round(A, B, C, D); Round(A, B, C, D); A ^= Tail;
		C ^= 0xff; Round(A, B, C, D); Round(A, B, C, D); Round(A, B, C, D); Round(A, B, C, D);
		return A ^ B ^ C ^ D;
	}
}

uint32 VaRest::Json::HashKey(FUtf8StringView Input)
{
	struct FProcessKey { uint64 First, Second; };
	// One function-local initialization in this module, shared by all callers and threads. Never written to disk.
	static const FProcessKey ProcessKey = []
	{
		// GUID providers can contain timestamps, fixed bits or noncryptographic fallbacks.
		// Every supported target obtains the full key from its OS, with no weak fallback.
		FProcessKey Key{};
#if PLATFORM_LINUX || PLATFORM_ANDROID
		auto* Bytes = reinterpret_cast<uint8*>(&Key);
		SIZE_T Filled = 0;
		while (Filled < sizeof(Key))
		{
#if PLATFORM_ANDROID
			const ssize_t Count = static_cast<ssize_t>(syscall(SYS_getrandom, Bytes + Filled, sizeof(Key) - Filled, 0));
#else
			const ssize_t Count = getrandom(Bytes + Filled, sizeof(Key) - Filled, 0);
#endif
			if (Count < 0 && errno == EINTR) continue;
			if (Count <= 0) UE_LOG(LogVaRestJson, Fatal, TEXT("Could not initialize the JSON member hash process key from OS entropy."));
			Filled += SIZE_T(Count);
		}
#elif PLATFORM_WINDOWS
		if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&Key), static_cast<ULONG>(sizeof(Key)), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
			UE_LOG(LogVaRestJson, Fatal, TEXT("Could not initialize the JSON member hash process key from OS entropy."));
#elif PLATFORM_APPLE
		if (SecRandomCopyBytes(kSecRandomDefault, sizeof(Key), reinterpret_cast<uint8*>(&Key)) != 0)
			UE_LOG(LogVaRestJson, Fatal, TEXT("Could not initialize the JSON member hash process key from OS entropy."));
#else
		UE_LOG(LogVaRestJson, Fatal, TEXT("This target has no JSON member hash OS entropy provider."));
#endif
		return Key;
	}();
	return static_cast<uint32>(KeyHash::SipHash24(Input, ProcessKey.First, ProcessKey.Second));
}
