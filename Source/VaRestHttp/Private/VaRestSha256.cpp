// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSha256.h"

namespace VaRest::Http::Sha256Private
{
	/** FIPS 180-4 4.2.2: the first 32 bits of the fractional parts of the cube roots of the first 64 primes */
	const uint32 RoundConstants[64] = {
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
		0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
		0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
		0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

	inline uint32 RotR(uint32 Value, uint32 Bits)
	{
		return (Value >> Bits) | (Value << (32 - Bits));
	}
}

FString VaRest::Http::FSha256Digest::ToHex() const
{
	static const TCHAR Digits[] = TEXT("0123456789abcdef");
	FString Out;
	Out.Reserve(64);
	for (const uint8 Byte : Bytes)
	{
		Out.AppendChar(Digits[Byte >> 4]);
		Out.AppendChar(Digits[Byte & 15]);
	}
	return Out;
}

VaRest::Http::FSha256::FSha256()
{
	// FIPS 180-4 5.3.3
	Words[0] = 0x6a09e667;
	Words[1] = 0xbb67ae85;
	Words[2] = 0x3c6ef372;
	Words[3] = 0xa54ff53a;
	Words[4] = 0x510e527f;
	Words[5] = 0x9b05688c;
	Words[6] = 0x1f83d9ab;
	Words[7] = 0x5be0cd19;
}

void VaRest::Http::FSha256::Update(const void* Data, int64 Size)
{
	check(Size >= 0);
	const uint8* In = static_cast<const uint8*>(Data);
	TotalBytes += static_cast<uint64>(Size);
	while (Size > 0)
	{
		const int32 Take = static_cast<int32>(FMath::Min<int64>(64 - BufferLen, Size));
		FMemory::Memcpy(Buffer + BufferLen, In, Take);
		BufferLen += Take;
		In += Take;
		Size -= Take;
		if (BufferLen == 64)
		{
			Transform(Buffer);
			BufferLen = 0;
		}
	}
}

VaRest::Http::FSha256Digest VaRest::Http::FSha256::Finalize()
{
	// FIPS 180-4 5.1.1: 0x80, zeros to 56 mod 64, the bit length big-endian
	const uint64 BitLength = TotalBytes * 8;
	Buffer[BufferLen++] = 0x80;
	if (BufferLen > 56)
	{
		FMemory::Memzero(Buffer + BufferLen, 64 - BufferLen);
		Transform(Buffer);
		BufferLen = 0;
	}
	FMemory::Memzero(Buffer + BufferLen, 56 - BufferLen);
	for (int32 i = 0; i < 8; ++i)
	{
		Buffer[56 + i] = static_cast<uint8>(BitLength >> (56 - 8 * i));
	}
	Transform(Buffer);
	BufferLen = 0;

	FSha256Digest Digest;
	for (int32 i = 0; i < 8; ++i)
	{
		Digest.Bytes[4 * i + 0] = static_cast<uint8>(Words[i] >> 24);
		Digest.Bytes[4 * i + 1] = static_cast<uint8>(Words[i] >> 16);
		Digest.Bytes[4 * i + 2] = static_cast<uint8>(Words[i] >> 8);
		Digest.Bytes[4 * i + 3] = static_cast<uint8>(Words[i]);
	}
	return Digest;
}

VaRest::Http::FSha256Digest VaRest::Http::FSha256::Hash(TConstArrayView<uint8> Data)
{
	FSha256 Hasher;
	Hasher.Update(Data.GetData(), Data.Num());
	return Hasher.Finalize();
}

void VaRest::Http::FSha256::Transform(const uint8* Block)
{
	using namespace Sha256Private;
	uint32 W[64];
	for (int32 t = 0; t < 16; ++t)
	{
		W[t] = (static_cast<uint32>(Block[4 * t]) << 24) | (static_cast<uint32>(Block[4 * t + 1]) << 16)
			| (static_cast<uint32>(Block[4 * t + 2]) << 8) | static_cast<uint32>(Block[4 * t + 3]);
	}
	for (int32 t = 16; t < 64; ++t)
	{
		const uint32 S0 = RotR(W[t - 15], 7) ^ RotR(W[t - 15], 18) ^ (W[t - 15] >> 3);
		const uint32 S1 = RotR(W[t - 2], 17) ^ RotR(W[t - 2], 19) ^ (W[t - 2] >> 10);
		W[t] = W[t - 16] + S0 + W[t - 7] + S1;
	}
	uint32 A = Words[0], B = Words[1], C = Words[2], D = Words[3], E = Words[4], F = Words[5], G = Words[6], H = Words[7];
	for (int32 t = 0; t < 64; ++t)
	{
		const uint32 Sigma1 = RotR(E, 6) ^ RotR(E, 11) ^ RotR(E, 25);
		const uint32 Choose = (E & F) ^ (~E & G);
		const uint32 T1 = H + Sigma1 + Choose + RoundConstants[t] + W[t];
		const uint32 Sigma0 = RotR(A, 2) ^ RotR(A, 13) ^ RotR(A, 22);
		const uint32 Majority = (A & B) ^ (A & C) ^ (B & C);
		const uint32 T2 = Sigma0 + Majority;
		H = G;
		G = F;
		F = E;
		E = D + T1;
		D = C;
		C = B;
		B = A;
		A = T1 + T2;
	}
	Words[0] += A;
	Words[1] += B;
	Words[2] += C;
	Words[3] += D;
	Words[4] += E;
	Words[5] += F;
	Words[6] += G;
	Words[7] += H;
}
