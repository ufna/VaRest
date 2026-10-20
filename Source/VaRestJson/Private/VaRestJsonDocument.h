// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestJsonArena.h"
#include "VaRestJsonNode.h"

namespace VaRest::Json
{
	/**
	 * Owns every node of one JSON tree. FVaRestJson handles share it through a thread-safe
	 * TSharedPtr; nodes live in the arena and never move, so a handle's raw FNode* stays valid
	 * for as long as any handle keeps the document alive.
	 */
	class FDocument
	{
	public:
		explicit FDocument(SIZE_T ArenaFirstChunk = FArena::MinChunkSize)
			: Arena(ArenaFirstChunk)
		{
		}

		FArena Arena;

		/** In-situ parse buffer, NUL-terminated; parsed strings and keys point into it */
		TArray<uint8> Input;

		FNode* Root = nullptr;

		FNode* NewNull();
		FNode* NewBool(bool bValue);
		FNode* NewInt(int64 Value);
		/** Stored as Int when it fits int64 */
		FNode* NewUInt(uint64 Value);
		FNode* NewDouble(double Value);
		/** Copies Value into the arena */
		FNode* NewString(FUtf8StringView Value);
		/** No copy: Data must live in Input or the arena, NUL-terminated */
		FNode* NewStringRef(const UTF8CHAR* Data, uint32 Len);
		FNode* NewArray(uint32 Reserve = 0);
		FNode* NewObject(uint32 Reserve = 0);

		FStr CopyStr(FUtf8StringView Value, bool bHash);
		static FStr RefStr(const UTF8CHAR* Data, uint32 Len, bool bHash);

		/** Index of the LAST member with this key, or INDEX_NONE */
		static int32 FindMember(const FNode& Object, FUtf8StringView Key, uint32 Hash);
		static FNode* Find(const FNode& Object, FUtf8StringView Key);

		/** Replace the value of Key, or append. On an object with duplicate keys the other occurrences are dropped (spec 2.2). */
		void Set(FNode& Object, FUtf8StringView Key, FNode* Value);
		/** Append a member whose key is not present (callers: Set after a miss), so bDuplicateKeys stays true to the object */
		void Append(FNode& Object, const FStr& Key, FNode* Value);
		/** Remove every occurrence; returns how many were removed */
		int32 Remove(FNode& Object, FUtf8StringView Key);
		/** Returns true when two members share a key (the later one took the slot: last occurrence wins) */
		bool BuildIndex(FNode& Object);

		void Append(FNode& Array, FNode* Value);
		bool SetAt(FNode& Array, uint32 Index, FNode* Value);
		bool RemoveAt(FNode& Array, uint32 Index);

		/** Exact-size fill used by the parser */
		void AssignArray(FNode& Array, TConstArrayView<FNode*> Items);
		void AssignObject(FNode& Object, TConstArrayView<FMember> Members);

		/**
		 * Deep copy of Src, from any document, into this one; reads Src completely before returning. nullptr when Src nests
		 * deeper than MaxDepth (nodes copied so far stay in the arena). OpenContainers: containers already around the copy
		 * (a struct decode copying a value into a nested FVaRestJson field), counted toward MaxDepth.
		 */
		FNode* DeepCopy(const FNode& Src, int32 OpenContainers = 0);

		/** JSON equality: 1 == 1.0, objects ignore member order and compare duplicates by their last occurrence. False, and *bOutTooDeep set, past MaxDepth. */
		static bool Equals(const FNode& A, const FNode& B, bool* bOutTooDeep = nullptr);

	private:
		FNode* DeepCopyAt(const FNode& Src, int32 Depth);
		static bool EqualsAt(const FNode& A, const FNode& B, int32 Depth, bool& bTooDeep);
		FNode* NewNode(EVaRestJsonType Type);
		bool IndexInsert(FNode& Object, uint32 MemberIndex);
		void Reindex(FNode& Object);
	};
}
