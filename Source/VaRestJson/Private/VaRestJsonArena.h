// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include <type_traits>

namespace VaRest::Json
{
	/**
	 * Chunked bump allocator. Memory is released only when the arena dies, and nothing it hands
	 * out ever moves -- that is what lets a FVaRestJson handle hold a raw node pointer while the
	 * node's parent keeps growing.
	 */
	class FArena
	{
	public:
		static constexpr SIZE_T MaxChunkSize = 64 * 1024;

		explicit FArena(SIZE_T InFirstChunkSize = 4096);
		~FArena();

		FArena(const FArena&) = delete;
		FArena& operator=(const FArena&) = delete;

		void* Allocate(SIZE_T Size, SIZE_T Alignment);

		/** Uninitialised storage for Count trivially destructible T; nullptr when Count is 0 */
		template <typename T>
		T* NewArray(uint32 Count)
		{
			static_assert(std::is_trivially_destructible_v<T>, "arena memory is never destructed");
			return Count ? static_cast<T*>(Allocate(sizeof(T) * Count, alignof(T))) : nullptr;
		}

		template <typename T, typename... ArgTypes>
		T* New(ArgTypes&&... Args)
		{
			static_assert(std::is_trivially_destructible_v<T>, "arena memory is never destructed");
			return new (Allocate(sizeof(T), alignof(T))) T(Forward<ArgTypes>(Args)...);
		}

		/** Bytes requested from FMemory, chunk headers included */
		SIZE_T GetReservedBytes() const { return ReservedBytes; }

		/** Bytes handed out, alignment padding included */
		SIZE_T GetUsedBytes() const { return UsedBytes; }

	private:
		struct FChunk
		{
			FChunk* Next;
		};

		FChunk* NewChunk(SIZE_T Payload);

		FChunk* Chunks = nullptr;
		uint8* Cursor = nullptr;
		uint8* End = nullptr;
		SIZE_T NextChunkSize;
		SIZE_T ReservedBytes = 0;
		SIZE_T UsedBytes = 0;
	};
}
