// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonArena.h"

#include "HAL/UnrealMemory.h"
#include "Templates/AlignmentTemplates.h"

namespace VaRest::Json
{
	FArena::FArena(SIZE_T InFirstChunkSize)
		: NextChunkSize(FMath::Max<SIZE_T>(InFirstChunkSize, MinChunkSize))
	{
	}

	FArena::~FArena()
	{
		for (FChunk* Chunk = Chunks; Chunk;)
		{
			FChunk* Next = Chunk->Next;
			FMemory::Free(Chunk);
			Chunk = Next;
		}
	}

	FArena::FChunk* FArena::NewChunk(SIZE_T Payload)
	{
		const SIZE_T Total = sizeof(FChunk) + Payload;
		FChunk* Chunk = static_cast<FChunk*>(FMemory::Malloc(Total, alignof(FChunk)));
		Chunk->Next = Chunks;
		Chunks = Chunk;
		ReservedBytes += Total;
		return Chunk;
	}

	void* FArena::Allocate(SIZE_T Size, SIZE_T Alignment)
	{
		check(FMath::IsPowerOfTwo(Alignment));

		if (Cursor)
		{
			uint8* Aligned = Align(Cursor, Alignment);
			if (Aligned + Size <= End)
			{
				UsedBytes += (Aligned - Cursor) + Size;
				Cursor = Aligned + Size;
				return Aligned;
			}
		}

		const SIZE_T Needed = Size + Alignment;
		if (Cursor && Needed > MaxChunkSize / 2)
		{
			// A big block gets a chunk of its own and the current chunk keeps serving small
			// ones, so one long string does not strand the rest of a nearly empty chunk.
			FChunk* Big = NewChunk(Needed);
			uint8* Payload = Align(reinterpret_cast<uint8*>(Big + 1), Alignment);
			UsedBytes += Size;
			return Payload;
		}

		const SIZE_T Payload = FMath::Max(Needed, NextChunkSize);
		FChunk* Chunk = NewChunk(Payload);
		Cursor = reinterpret_cast<uint8*>(Chunk + 1);
		End = Cursor + Payload;
		// The first chunk may be sized from a hint (a parse sizes it from the input, Clone from the source);
		// growth chunks double from the previous chunk size up to MaxChunkSize.
		NextChunkSize = FMath::Min<SIZE_T>(FMath::Max<SIZE_T>(NextChunkSize, MinChunkSize) * 2, MaxChunkSize);

		uint8* Aligned = Align(Cursor, Alignment);
		UsedBytes += (Aligned - Cursor) + Size;
		Cursor = Aligned + Size;
		return Aligned;
	}
}
