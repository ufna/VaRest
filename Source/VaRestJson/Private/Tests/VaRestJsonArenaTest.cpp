// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Templates/AlignmentTemplates.h"
#include "VaRestJson.h"
#include "VaRestJsonArena.h"
#include "VaRestJsonDocument.h"

// Usings live inside each test function: a file-scope using would leak into the files after this one
// in a unity blob.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonArenaAlignmentTest, "VaRest.Json.Arena.Alignment",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonArenaAlignmentTest::RunTest(const FString& Parameters)
{
	using VaRest::Json::FArena;

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
	using VaRest::Json::FArena;

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
	using VaRest::Json::FArena;

	FArena Arena(4096);
	uint8* Small1 = static_cast<uint8*>(Arena.Allocate(16, 8));
	const SIZE_T Big = FArena::MaxChunkSize / 2 + 1; // just above the own-chunk threshold
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonArenaGrowthTest, "VaRest.Json.Arena.GrowthDoublesToMaxChunk",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonArenaGrowthTest::RunTest(const FString& Parameters)
{
	using VaRest::Json::FArena;

	FArena Arena;
	TArray<SIZE_T> Chunks; // payload of each new chunk, from the change in reserved bytes
	SIZE_T Reserved = 0;
	SIZE_T Header = 0;
	while (Arena.GetUsedBytes() < 8 * FArena::MaxChunkSize)
	{
		Arena.Allocate(64, 8);
		if (Arena.GetReservedBytes() != Reserved)
		{
			const SIZE_T Delta = Arena.GetReservedBytes() - Reserved;
			if (Chunks.Num() == 0)
			{
				Header = Delta - FArena::MinChunkSize;
			}
			Chunks.Add(Delta - Header);
			Reserved = Arena.GetReservedBytes();
		}
	}
	TestEqual(TEXT("default first chunk is the minimum"), Chunks[0], FArena::MinChunkSize);
	TestTrue(TEXT("chunk header is small"), Header > 0 && Header <= 64);
	int32 Bad = 0;
	for (int32 i = 1; i < Chunks.Num(); ++i)
	{
		Bad += Chunks[i] != FMath::Min<SIZE_T>(Chunks[i - 1] * 2, FArena::MaxChunkSize);
	}
	TestEqual(TEXT("each chunk doubles the previous one, capped at MaxChunkSize"), Bad, 0);
	TestEqual(TEXT("growth reached MaxChunkSize"), Chunks.Last(), FArena::MaxChunkSize);
	TestTrue(TEXT("at most one chunk of slack"), Arena.GetReservedBytes() - Arena.GetUsedBytes() <= FArena::MaxChunkSize + Chunks.Num() * Header);

	// A first chunk larger than MaxChunkSize (a root Clone sizes it from the source) is followed by
	// MaxChunkSize chunks, not by double its size
	FArena Hinted(4 * FArena::MaxChunkSize);
	Hinted.Allocate(64, 8);
	const SIZE_T First = Hinted.GetReservedBytes();
	TestEqual(TEXT("hinted first chunk"), First, 4 * FArena::MaxChunkSize + Header);
	while (Hinted.GetReservedBytes() == First)
	{
		Hinted.Allocate(64, 8);
	}
	TestEqual(TEXT("chunk after a large first chunk is MaxChunkSize"), Hinted.GetReservedBytes() - First, FArena::MaxChunkSize + Header);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonArenaDocumentSizingTest, "VaRest.Json.Arena.DocumentSizing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonArenaDocumentSizingTest::RunTest(const FString& Parameters)
{
	using VaRest::Json::FArena;

	const auto Reserved = [](const FVaRestJson& J) -> SIZE_T
	{
		return J.GetDocumentInternal() ? J.GetDocumentInternal()->Arena.GetReservedBytes() : 0;
	};

	// Small documents (SaveGame fields, pin defaults, `{}`) must not pay a 4 KiB chunk each
	TestTrue(TEXT("parsed {} < 1 KiB"), Reserved(FVaRestJson::Parse(FUtf8StringView(UTF8TEXT("{}"))).StealValue()) < 1024);
	TestTrue(TEXT("Object() < 1 KiB"), Reserved(FVaRestJson::Object()) < 1024);
	TestTrue(TEXT("Array() < 1 KiB"), Reserved(FVaRestJson::Array()) < 1024);
	TestTrue(TEXT("From(1) < 1 KiB"), Reserved(FVaRestJson::From(1)) < 1024);
	TestTrue(TEXT("small Set chain < 1 KiB"), Reserved(FVaRestJson::Object().Set(TEXT("id"), 1).Set(TEXT("name"), TEXT("x"))) < 1024);
	TestTrue(TEXT("FromJsonValue < 1 KiB"), Reserved(FVaRestJson::FromJsonValue(MakeShared<FJsonValueNumber>(1.0))) < 1024);
	TestTrue(TEXT("Clone of a small root < 1 KiB"), Reserved(FVaRestJson::Object().Set(TEXT("a"), 1).Clone()) < 1024);

	FVaRestJson Imported;
	const TCHAR* Buffer = TEXT("{}");
	Imported.ImportTextItem(Buffer, PPF_None, nullptr, GLog);
	TestTrue(TEXT("ImportTextItem {} < 1 KiB"), Imported.IsObject() && Reserved(Imported) < 1024);

	TArray<uint8> Saved;
	FMemoryWriter Writer(Saved);
	FVaRestJson::Object().Set(TEXT("a"), 1).Serialize(Writer);
	FMemoryReader Reader(Saved);
	FVaRestJson Loaded;
	Loaded.Serialize(Reader);
	TestTrue(TEXT("Serialize-loaded {\"a\":1} < 1 KiB"), Loaded.IsObject() && Reserved(Loaded) < 1024);

	// A string-heavy input: in situ, the string stays in the input buffer, so the arena holds one node.
	// The first chunk must not be sized like the input.
	TArray<uint8> Blob;
	const int32 BlobLen = 1024 * 1024;
	Blob.Reserve(BlobLen + 3);
	Blob.Add('"');
	static const char Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	for (int32 i = 0; i < BlobLen; ++i)
	{
		Blob.Add(static_cast<uint8>(Alphabet[(i * 7) % 64]));
	}
	Blob.Add('"');
	const FVaRestJson Big = FVaRestJson::ParseOwned(MoveTemp(Blob)).StealValue();
	TestEqual(TEXT("blob parsed"), Big.AsUtf8View().Len(), BlobLen);
	TestTrue(*FString::Printf(TEXT("1 MiB single-string document reserves < 64 KiB of arena (got %llu)"), (uint64)Reserved(Big)), Reserved(Big) < 64 * 1024);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonArenaMidSizeTest, "VaRest.Json.Arena.MidSizeSlack",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonArenaMidSizeTest::RunTest(const FString& Parameters)
{
	using VaRest::Json::FArena;

	// A typical mid-size API response (~300 KB, number-heavy): the arena may hold at most one partly filled
	// growth chunk beyond what the DOM uses. With 1 MiB growth chunks (M1) the slack reached ~950 KB here.
	FString Text = TEXT("[");
	for (int32 i = 0; Text.Len() < 300 * 1024; ++i)
	{
		Text += FString::Printf(TEXT("%s{\"id\":%d,\"x\":%d.5,\"tags\":[1,2,3],\"ok\":true}"), i ? TEXT(",") : TEXT(""), i, i);
	}
	Text += TEXT("]");
	const FVaRestJson J = FVaRestJson::Parse(FStringView(Text)).StealValue();
	if (!TestTrue(TEXT("parsed"), J.IsArray()))
	{
		return false;
	}
	const SIZE_T Used = J.GetDocumentInternal()->Arena.GetUsedBytes();
	const SIZE_T Reserved = J.GetDocumentInternal()->Arena.GetReservedBytes();
	TestTrue(*FString::Printf(TEXT("slack %llu B <= one 256 KiB chunk + headers (used %llu, reserved %llu)"), (uint64)(Reserved - Used), (uint64)Used, (uint64)Reserved),
		Reserved - Used <= 256 * 1024 + 64 * 64);
	TestEqual(TEXT("growth cap"), FArena::MaxChunkSize, static_cast<SIZE_T>(256 * 1024));
	return true;
}

#endif
