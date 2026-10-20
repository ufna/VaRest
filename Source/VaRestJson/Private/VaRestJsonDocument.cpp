// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonDocument.h"

namespace VaRest::Json
{
	namespace DocumentPrivate
	{
		template <typename T>
		T* GrowVector(FArena& Arena, T* Old, uint32 Num, uint32& InOutMax)
		{
			// The old vector is abandoned in the arena; the nodes it pointed at do not move.
			const uint32 NewMax = FMath::Max<uint32>(4, InOutMax * 2);
			T* New = Arena.NewArray<T>(NewMax);
			if (Num)
			{
				FMemory::Memcpy(New, Old, sizeof(T) * Num);
			}
			InOutMax = NewMax;
			return New;
		}

		/** At most IndexThreshold members: at most 28 hash comparisons */
		bool HasDuplicateKeys(const FMember* Members, uint32 Count)
		{
			for (uint32 i = 1; i < Count; ++i)
			{
				for (uint32 j = 0; j < i; ++j)
				{
					if (Members[i].Key.Hash == Members[j].Key.Hash && KeyEquals(Members[j].Key, Members[i].Key.View()))
					{
						return true;
					}
				}
			}
			return false;
		}

		bool NumbersEqual(const FNode& A, const FNode& B)
		{
			if (A.NumKind == B.NumKind)
			{
				switch (A.NumKind)
				{
				case ENumKind::Int:    return A.Int == B.Int;
				case ENumKind::UInt:   return A.UInt == B.UInt;
				case ENumKind::Double: return A.Double == B.Double;
				}
			}
			if (A.NumKind != ENumKind::Double && B.NumKind != ENumKind::Double)
			{
				return false; // Int and UInt never overlap: UInt holds only values above INT64_MAX
			}
			const FNode& D = A.NumKind == ENumKind::Double ? A : B;
			const FNode& I = A.NumKind == ENumKind::Double ? B : A;
			const double V = D.Double;
			if (!FMath::IsFinite(V) || FMath::TruncToDouble(V) != V)
			{
				return false;
			}
			if (I.NumKind == ENumKind::Int)
			{
				return V >= -9223372036854775808.0 && V < 9223372036854775808.0 && static_cast<int64>(V) == I.Int;
			}
			return V >= 9223372036854775808.0 && V < 18446744073709551616.0 && static_cast<uint64>(V) == I.UInt;
		}
	}

	FNode* FDocument::NewNode(EVaRestJsonType Type)
	{
		FNode* Node = Arena.New<FNode>();
		Node->Type = Type;
		return Node;
	}

	FNode* FDocument::NewNull()
	{
		return NewNode(EVaRestJsonType::Null);
	}

	FNode* FDocument::NewBool(bool bValue)
	{
		FNode* Node = NewNode(EVaRestJsonType::Bool);
		Node->Bool = bValue;
		return Node;
	}

	FNode* FDocument::NewInt(int64 Value)
	{
		FNode* Node = NewNode(EVaRestJsonType::Number);
		Node->NumKind = ENumKind::Int;
		Node->Int = Value;
		return Node;
	}

	FNode* FDocument::NewUInt(uint64 Value)
	{
		if (Value <= static_cast<uint64>(MAX_int64))
		{
			return NewInt(static_cast<int64>(Value));
		}
		FNode* Node = NewNode(EVaRestJsonType::Number);
		Node->NumKind = ENumKind::UInt;
		Node->UInt = Value;
		return Node;
	}

	FNode* FDocument::NewDouble(double Value)
	{
		FNode* Node = NewNode(EVaRestJsonType::Number);
		Node->NumKind = ENumKind::Double;
		Node->Double = Value;
		return Node;
	}

	FStr FDocument::CopyStr(FUtf8StringView Value, bool bHash)
	{
		const uint32 Len = static_cast<uint32>(Value.Len());
		UTF8CHAR* Data = Arena.NewArray<UTF8CHAR>(Len + 1);
		if (Len)
		{
			FMemory::Memcpy(Data, Value.GetData(), Len);
		}
		Data[Len] = UTF8CHAR(0);
		return FStr{ Data, Len, bHash ? HashKey(Value) : 0u };
	}

	FStr FDocument::RefStr(const UTF8CHAR* Data, uint32 Len, bool bHash)
	{
		return FStr{ Data, Len, bHash ? HashKey(FUtf8StringView(Data, Len)) : 0u };
	}

	FNode* FDocument::NewString(FUtf8StringView Value)
	{
		FNode* Node = NewNode(EVaRestJsonType::String);
		Node->Str = CopyStr(Value, false);
		return Node;
	}

	FNode* FDocument::NewStringRef(const UTF8CHAR* Data, uint32 Len)
	{
		FNode* Node = NewNode(EVaRestJsonType::String);
		Node->Str = RefStr(Data, Len, false);
		return Node;
	}

	FNode* FDocument::NewArray(uint32 Reserve)
	{
		FNode* Node = NewNode(EVaRestJsonType::Array);
		Node->Arr = FNode::FArr{ Arena.NewArray<FNode*>(Reserve), 0, Reserve };
		return Node;
	}

	FNode* FDocument::NewObject(uint32 Reserve)
	{
		FNode* Node = NewNode(EVaRestJsonType::Object);
		Node->Obj = FNode::FObj{ Arena.NewArray<FMember>(Reserve), 0, Reserve, nullptr };
		return Node;
	}

	int32 FDocument::FindMember(const FNode& Object, FUtf8StringView Key, uint32 Hash)
	{
		check(Object.Type == EVaRestJsonType::Object);
		const FNode::FObj& O = Object.Obj;
		if (const FIndex* Index = O.Index)
		{
			const uint32 Mask = Index->Capacity - 1;
			for (uint32 Slot = Hash & Mask;; Slot = (Slot + 1) & Mask)
			{
				const uint32 Entry = Index->Slots[Slot];
				if (Entry == 0)
				{
					return INDEX_NONE;
				}
				const FMember& M = O.Members[Entry - 1];
				if (M.Key.Hash == Hash && KeyEquals(M.Key, Key))
				{
					return static_cast<int32>(Entry - 1);
				}
			}
		}
		for (int32 i = static_cast<int32>(O.Num) - 1; i >= 0; --i)
		{
			const FMember& M = O.Members[i];
			if (M.Key.Hash == Hash && KeyEquals(M.Key, Key))
			{
				return i; // scanning from the end: the last occurrence wins
			}
		}
		return INDEX_NONE;
	}

	FNode* FDocument::Find(const FNode& Object, FUtf8StringView Key)
	{
		const int32 i = FindMember(Object, Key, HashKey(Key));
		return i == INDEX_NONE ? nullptr : Object.Obj.Members[i].Value;
	}

	void FDocument::Set(FNode& Object, FUtf8StringView Key, FNode* Value)
	{
		const uint32 Hash = HashKey(Key);
		if (!Object.bDuplicateKeys)
		{
			const int32 Found = FindMember(Object, Key, Hash);
			if (Found != INDEX_NONE)
			{
				Object.Obj.Members[Found].Value = Value;
				return;
			}
			Append(Object, CopyStr(Key, true), Value);
			return;
		}

		// Parsed with duplicate keys (lookup sees the last). Spec 2.2: Set leaves exactly one member with this key, so
		// ToString never emits a duplicate it was asked to replace. The survivor keeps the FIRST occurrence's position,
		// where JSON.parse puts the key ({"a":1,"b":2,"a":3} becomes a:3, b:2).
		FNode::FObj& O = Object.Obj;
		int32 First = INDEX_NONE;
		uint32 Write = 0;
		for (uint32 Read = 0; Read < O.Num; ++Read)
		{
			const FMember M = O.Members[Read]; // by value: the write below may overwrite the slot a reference would still read
			if (M.Key.Hash == Hash && KeyEquals(M.Key, Key))
			{
				if (First != INDEX_NONE)
				{
					continue; // a later occurrence: dropped
				}
				First = static_cast<int32>(Write);
			}
			O.Members[Write++] = M;
		}
		if (First == INDEX_NONE)
		{
			Append(Object, CopyStr(Key, true), Value);
			return;
		}
		O.Members[First].Value = Value;
		if (Write != O.Num)
		{
			O.Num = Write;
			Reindex(Object);
		}
	}

	void FDocument::Append(FNode& Object, const FStr& Key, FNode* Value)
	{
		check(Object.Type == EVaRestJsonType::Object);
		checkSlow(FindMember(Object, Key.View(), Key.Hash) == INDEX_NONE); // the bDuplicateKeys invariant
		FNode::FObj& O = Object.Obj;
		if (O.Num == O.Max)
		{
			O.Members = DocumentPrivate::GrowVector(Arena, O.Members, O.Num, O.Max);
		}
		O.Members[O.Num] = FMember{ Key, Value };
		++O.Num;

		if (O.Index)
		{
			if (O.Num * 2 > O.Index->Capacity)
			{
				BuildIndex(Object);
			}
			else
			{
				IndexInsert(Object, O.Num - 1);
			}
		}
		else if (O.Num > IndexThreshold)
		{
			BuildIndex(Object);
		}
	}

	bool FDocument::IndexInsert(FNode& Object, uint32 MemberIndex)
	{
		FIndex& Index = *Object.Obj.Index;
		const FMember& M = Object.Obj.Members[MemberIndex];
		const uint32 Mask = Index.Capacity - 1;
		for (uint32 Slot = M.Key.Hash & Mask;; Slot = (Slot + 1) & Mask)
		{
			uint32& Entry = Index.Slots[Slot];
			if (Entry == 0)
			{
				Entry = MemberIndex + 1;
				return false;
			}
			const FMember& Other = Object.Obj.Members[Entry - 1];
			if (Other.Key.Hash == M.Key.Hash && KeyEquals(Other.Key, M.Key.View()))
			{
				Entry = MemberIndex + 1; // a later duplicate takes the slot: last occurrence wins
				return true;
			}
		}
	}

	bool FDocument::BuildIndex(FNode& Object)
	{
		FNode::FObj& O = Object.Obj;
		const uint32 Capacity = FMath::RoundUpToPowerOfTwo(FMath::Max<uint32>(16, O.Num * 2));
		FIndex* Index = Arena.New<FIndex>();
		Index->Capacity = Capacity;
		Index->Slots = Arena.NewArray<uint32>(Capacity);
		FMemory::Memzero(Index->Slots, sizeof(uint32) * Capacity);
		O.Index = Index;
		bool bDuplicates = false;
		for (uint32 i = 0; i < O.Num; ++i)
		{
			if (IndexInsert(Object, i))
			{
				bDuplicates = true;
			}
		}
		return bDuplicates;
	}

	void FDocument::Reindex(FNode& Object)
	{
		// Members shifted, so every index slot after the first removal is stale. The arena never frees, so the existing
		// slot array is cleared and refilled while it is large enough instead of allocating a new one.
		FNode::FObj& O = Object.Obj;
		if (O.Num <= IndexThreshold)
		{
			O.Index = nullptr;
		}
		else if (O.Index && O.Index->Capacity >= O.Num * 2)
		{
			FMemory::Memzero(O.Index->Slots, sizeof(uint32) * O.Index->Capacity);
			for (uint32 i = 0; i < O.Num; ++i)
			{
				IndexInsert(Object, i);
			}
		}
		else
		{
			BuildIndex(Object);
		}
	}

	int32 FDocument::Remove(FNode& Object, FUtf8StringView Key)
	{
		check(Object.Type == EVaRestJsonType::Object);
		FNode::FObj& O = Object.Obj;
		const uint32 Hash = HashKey(Key);
		uint32 Write = 0;
		for (uint32 Read = 0; Read < O.Num; ++Read)
		{
			const FMember& M = O.Members[Read];
			if (!(M.Key.Hash == Hash && KeyEquals(M.Key, Key)))
			{
				O.Members[Write++] = M;
			}
		}
		const int32 Removed = static_cast<int32>(O.Num - Write);
		O.Num = Write;
		if (Removed > 0)
		{
			Reindex(Object);
		}
		return Removed;
	}

	void FDocument::Append(FNode& Array, FNode* Value)
	{
		check(Array.Type == EVaRestJsonType::Array);
		FNode::FArr& A = Array.Arr;
		if (A.Num == A.Max)
		{
			A.Items = DocumentPrivate::GrowVector(Arena, A.Items, A.Num, A.Max);
		}
		A.Items[A.Num++] = Value;
	}

	bool FDocument::SetAt(FNode& Array, uint32 Index, FNode* Value)
	{
		check(Array.Type == EVaRestJsonType::Array);
		if (Index >= Array.Arr.Num)
		{
			return false;
		}
		Array.Arr.Items[Index] = Value;
		return true;
	}

	bool FDocument::RemoveAt(FNode& Array, uint32 Index)
	{
		check(Array.Type == EVaRestJsonType::Array);
		FNode::FArr& A = Array.Arr;
		if (Index >= A.Num)
		{
			return false;
		}
		FMemory::Memmove(A.Items + Index, A.Items + Index + 1, sizeof(FNode*) * (A.Num - Index - 1));
		--A.Num;
		return true;
	}

	void FDocument::AssignArray(FNode& Array, TConstArrayView<FNode*> Items)
	{
		const uint32 Num = static_cast<uint32>(Items.Num());
		FNode** Data = Arena.NewArray<FNode*>(Num);
		if (Num)
		{
			FMemory::Memcpy(Data, Items.GetData(), sizeof(FNode*) * Num);
		}
		Array.Arr = FNode::FArr{ Data, Num, Num };
	}

	void FDocument::AssignObject(FNode& Object, TConstArrayView<FMember> Members)
	{
		const uint32 Num = static_cast<uint32>(Members.Num());
		FMember* Data = Arena.NewArray<FMember>(Num);
		if (Num)
		{
			FMemory::Memcpy(Data, Members.GetData(), sizeof(FMember) * Num);
		}
		Object.Obj = FNode::FObj{ Data, Num, Num, nullptr };
		// Recorded once, here at parse time, so Set pays for duplicate handling only on objects that have duplicates
		// (spec 2.2): above IndexThreshold the index build sees every collision, below it a pairwise check does.
		Object.bDuplicateKeys = Num > IndexThreshold ? BuildIndex(Object) : DocumentPrivate::HasDuplicateKeys(Data, Num);
	}

	FNode* FDocument::DeepCopy(const FNode& Src)
	{
		return DeepCopyAt(Src, 0);
	}

	FNode* FDocument::DeepCopyAt(const FNode& Src, int32 Depth)
	{
		switch (Src.Type)
		{
		case EVaRestJsonType::None:
		case EVaRestJsonType::Null:
			return NewNull();
		case EVaRestJsonType::Bool:
			return NewBool(Src.Bool);
		case EVaRestJsonType::Number:
			switch (Src.NumKind)
			{
			case ENumKind::Int:    return NewInt(Src.Int);
			case ENumKind::UInt:   return NewUInt(Src.UInt);
			case ENumKind::Double: return NewDouble(Src.Double);
			}
			return NewNull();
		case EVaRestJsonType::String:
			return NewString(Src.Str.View());
		case EVaRestJsonType::Array:
		{
			if (Depth >= MaxDepth)
			{
				return nullptr;
			}
			const uint32 Count = Src.Arr.Num;
			FNode* Copy = NewArray(Count);
			for (uint32 i = 0; i < Count; ++i)
			{
				FNode* Item = DeepCopyAt(*Src.Arr.Items[i], Depth + 1);
				if (!Item)
				{
					return nullptr;
				}
				Copy->Arr.Items[i] = Item;
			}
			Copy->Arr.Num = Count;
			return Copy;
		}
		case EVaRestJsonType::Object:
		{
			if (Depth >= MaxDepth)
			{
				return nullptr;
			}
			const uint32 Count = Src.Obj.Num;
			FNode* Copy = NewObject(Count);
			for (uint32 i = 0; i < Count; ++i)
			{
				const FMember& M = Src.Obj.Members[i];
				FNode* Item = DeepCopyAt(*M.Value, Depth + 1);
				if (!Item)
				{
					return nullptr;
				}
				FStr Key = CopyStr(M.Key.View(), false);
				Key.Hash = M.Key.Hash;
				Copy->Obj.Members[i] = FMember{ Key, Item };
			}
			Copy->Obj.Num = Count;
			Copy->bDuplicateKeys = Src.bDuplicateKeys;
			if (Count > IndexThreshold)
			{
				BuildIndex(*Copy);
			}
			return Copy;
		}
		}
		return NewNull();
	}

	bool FDocument::Equals(const FNode& A, const FNode& B, bool* bOutTooDeep)
	{
		bool bTooDeep = false;
		const bool bEqual = EqualsAt(A, B, 0, bTooDeep);
		if (bOutTooDeep)
		{
			*bOutTooDeep = bTooDeep;
		}
		return bEqual && !bTooDeep;
	}

	bool FDocument::EqualsAt(const FNode& A, const FNode& B, int32 Depth, bool& bTooDeep)
	{
		if (A.Type != B.Type)
		{
			return false;
		}
		switch (A.Type)
		{
		case EVaRestJsonType::None:
		case EVaRestJsonType::Null:
			return true;
		case EVaRestJsonType::Bool:
			return A.Bool == B.Bool;
		case EVaRestJsonType::Number:
			return DocumentPrivate::NumbersEqual(A, B);
		case EVaRestJsonType::String:
			return KeyEquals(A.Str, B.Str.View());
		case EVaRestJsonType::Array:
			if (A.Arr.Num != B.Arr.Num)
			{
				return false;
			}
			if (Depth >= MaxDepth)
			{
				bTooDeep = true;
				return false;
			}
			for (uint32 i = 0; i < A.Arr.Num; ++i)
			{
				if (!EqualsAt(*A.Arr.Items[i], *B.Arr.Items[i], Depth + 1, bTooDeep))
				{
					return false;
				}
			}
			return true;
		case EVaRestJsonType::Object:
		{
			if (Depth >= MaxDepth)
			{
				bTooDeep = true;
				return false;
			}
			// Distinct keys, each by its last occurrence -- the same view Find gives.
			uint32 DistinctA = 0;
			for (uint32 i = 0; i < A.Obj.Num; ++i)
			{
				const FMember& M = A.Obj.Members[i];
				if (FindMember(A, M.Key.View(), M.Key.Hash) != static_cast<int32>(i))
				{
					continue; // an earlier duplicate
				}
				++DistinctA;
				const int32 j = FindMember(B, M.Key.View(), M.Key.Hash);
				if (j == INDEX_NONE || !EqualsAt(*M.Value, *B.Obj.Members[j].Value, Depth + 1, bTooDeep))
				{
					return false;
				}
			}
			uint32 DistinctB = 0;
			for (uint32 i = 0; i < B.Obj.Num; ++i)
			{
				const FMember& M = B.Obj.Members[i];
				DistinctB += FindMember(B, M.Key.View(), M.Key.Hash) == static_cast<int32>(i);
			}
			return DistinctA == DistinctB;
		}
		}
		return false;
	}
}
