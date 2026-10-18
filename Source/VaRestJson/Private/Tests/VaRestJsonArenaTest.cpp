// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Templates/AlignmentTemplates.h"
#include "VaRestJsonArena.h"

using VaRest::Json::FArena;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonArenaAlignmentTest, "VaRest.Json.Arena.Alignment",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonArenaAlignmentTest::RunTest(const FString& Parameters)
{
	FArena Arena(256);
	for (SIZE_T Alignment : { 1, 2, 4, 8, 16, 64 })
	{
		Arena.Allocate(3, 1); // knock the cursor off alignment first
		void* P = Arena.Allocate(24, Alignment);
		TestTrue(*FString::Printf(TEXT("aligned to %llu"), (uint64)Alignment), IsAligned(P, Alignment));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonArenaStabilityTest, "VaRest.Json.Arena.NothingMoves",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonArenaStabilityTest::RunTest(const FString& Parameters)
{
	FArena Arena;
	TArray<uint32*> Blocks;
	for (uint32 i = 0; i < 100000; ++i)
	{
		uint32* P = static_cast<uint32*>(Arena.Allocate(sizeof(uint32) * 6, alignof(uint32)));
		for (uint32 k = 0; k < 6; ++k)
		{
			P[k] = i * 7 + k;
		}
		Blocks.Add(P);
	}
	// Every block must still hold what was written: no chunk was moved or reused.
	int32 Bad = 0;
	for (uint32 i = 0; i < 100000; ++i)
	{
		for (uint32 k = 0; k < 6; ++k)
		{
			Bad += Blocks[i][k] != i * 7 + k;
		}
	}
	TestEqual(TEXT("corrupted words"), Bad, 0);
	TestTrue(TEXT("used <= reserved"), Arena.GetUsedBytes() <= Arena.GetReservedBytes());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonArenaBigBlockTest, "VaRest.Json.Arena.BigBlockOwnChunk",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonArenaBigBlockTest::RunTest(const FString& Parameters)
{
	FArena Arena(4096);
	uint8* Small1 = static_cast<uint8*>(Arena.Allocate(16, 8));
	const SIZE_T Big = 1024 * 1024;
	uint8* BigBlock = static_cast<uint8*>(Arena.Allocate(Big, 16));
	uint8* Small2 = static_cast<uint8*>(Arena.Allocate(16, 8));

	TestTrue(TEXT("big block reserved"), Arena.GetReservedBytes() >= Big + 4096);
	// The big block got a chunk of its own; the current chunk keeps serving small blocks.
	TestTrue(TEXT("small block after the big one is outside it"), Small2 < BigBlock || Small2 >= BigBlock + Big);
	TestTrue(TEXT("small blocks share the first chunk"), Small2 > Small1 && Small2 < Small1 + 4096);
	FMemory::Memset(BigBlock, 0xAB, Big); // writable end to end
	TestTrue(TEXT("zero-count array is null"), Arena.NewArray<uint32>(0) == nullptr);
	return true;
}

#endif
