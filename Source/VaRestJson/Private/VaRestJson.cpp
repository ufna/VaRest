// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJson.h"

#include "VaRestJsonDocument.h"
#include "VaRestJsonLog.h"
#include "VaRestJsonUtf8.h"

// The module builds in unity blobs, where a file-scope using-directive or an anonymous namespace leaks into every
// later .cpp of the same blob. File-local helpers live in a namespace named after the file; member functions spell
// VaRest::Json:: in their signatures and open a using-directive inside their body when they need many internals.
namespace VaRest::Json::HandlePrivate
{
	const TCHAR* TypeName(EVaRestJsonType Type)
	{
		switch (Type)
		{
		case EVaRestJsonType::None:   return TEXT("invalid handle");
		case EVaRestJsonType::Null:   return TEXT("Null");
		case EVaRestJsonType::Bool:   return TEXT("Bool");
		case EVaRestJsonType::Number: return TEXT("Number");
		case EVaRestJsonType::String: return TEXT("String");
		case EVaRestJsonType::Array:  return TEXT("Array");
		case EVaRestJsonType::Object: return TEXT("Object");
		}
		return TEXT("?");
	}
}

FString FVaRestJsonError::ToString() const
{
	FString Result = Message;
	if (Line > 0)
	{
		Result += FString::Printf(TEXT(" (line %d, column %d)"), Line, Column);
	}
	else if (Offset >= 0)
	{
		Result += FString::Printf(TEXT(" (byte %lld)"), Offset);
	}
	if (!Path.IsEmpty())
	{
		Result += FString::Printf(TEXT(" at %s"), *Path);
	}
	return Result;
}

FVaRestJson::FDocumentRef FVaRestJson::NewDocument(SIZE_T ArenaFirstChunk)
{
	static_assert(DefaultArenaChunk == VaRest::Json::FArena::MinChunkSize, "an API-built document starts with one minimum-size arena chunk");
	return MakeShared<VaRest::Json::FDocument, ESPMode::ThreadSafe>(ArenaFirstChunk);
}

FVaRestJson FVaRestJson::Object()
{
	FDocumentRef NewDoc = NewDocument();
	VaRest::Json::FNode* Root = NewDoc->NewObject();
	NewDoc->Root = Root;
	return FVaRestJson(MoveTemp(NewDoc), Root);
}

FVaRestJson FVaRestJson::Array()
{
	FDocumentRef NewDoc = NewDocument();
	VaRest::Json::FNode* Root = NewDoc->NewArray();
	NewDoc->Root = Root;
	return FVaRestJson(MoveTemp(NewDoc), Root);
}

FVaRestJson FVaRestJson::Null()
{
	FDocumentRef NewDoc = NewDocument();
	VaRest::Json::FNode* Root = NewDoc->NewNull();
	NewDoc->Root = Root;
	return FVaRestJson(MoveTemp(NewDoc), Root);
}

EVaRestJsonType FVaRestJson::GetType() const
{
	return Node ? Node->Type : EVaRestJsonType::None;
}

int32 FVaRestJson::Num() const
{
	switch (GetType())
	{
	case EVaRestJsonType::Array:  return static_cast<int32>(Node->Arr.Num);
	case EVaRestJsonType::Object: return static_cast<int32>(Node->Obj.Num);
	default:                      return 0;
	}
}

TOptional<bool> FVaRestJson::TryGetBool() const
{
	return IsBool() ? TOptional<bool>(Node->Bool) : TOptional<bool>();
}

TOptional<int64> FVaRestJson::TryGetInt64() const
{
	int64 Value = 0;
	return Node && VaRest::Json::NodeToInt64(*Node, Value) ? TOptional<int64>(Value) : TOptional<int64>();
}

TOptional<uint64> FVaRestJson::TryGetUInt64() const
{
	uint64 Value = 0;
	return Node && VaRest::Json::NodeToUInt64(*Node, Value) ? TOptional<uint64>(Value) : TOptional<uint64>();
}

TOptional<double> FVaRestJson::TryGetDouble() const
{
	double Value = 0.0;
	return Node && VaRest::Json::NodeToDouble(*Node, Value) ? TOptional<double>(Value) : TOptional<double>();
}

TOptional<FString> FVaRestJson::TryGetString() const
{
	if (!IsString())
	{
		return {};
	}
	return FString::ConstructFromPtrSize(Node->Str.Data, static_cast<int32>(Node->Str.Len));
}

FString FVaRestJson::AsString(const FString& Default) const
{
	TOptional<FString> S = TryGetString();
	return S ? MoveTemp(*S) : Default;
}

FUtf8StringView FVaRestJson::AsUtf8View() const
{
	return IsString() ? Node->Str.View() : FUtf8StringView();
}

FVaRestJson FVaRestJson::Get(FUtf8StringView Key) const
{
	if (!IsObject())
	{
		return Miss();
	}
	// Set stores an invalid key repaired, so a lookup by the same bytes must repair the same way (quietly: it stores nothing)
	VaRest::Json::FUtf8Scratch Scratch;
	if (!VaRest::Json::IsValidUtf8(Key))
	{
		VaRest::Json::AppendRepairedUtf8(Key, Scratch);
		Key = FUtf8StringView(Scratch.GetData(), Scratch.Num());
	}
	VaRest::Json::FNode* Found = VaRest::Json::FDocument::Find(*Node, Key);
	return Found ? FVaRestJson(Doc, Found) : Miss();
}

FVaRestJson FVaRestJson::Get(FStringView Key) const
{
	if (!IsObject())
	{
		return Miss();
	}
	// The same conversion Set uses, so a key with an unpaired surrogate finds what Set stored; a lookup stores nothing, so no warning
	VaRest::Json::FUtf8Scratch Scratch;
	return Get(VaRest::Json::ConvertForDocument(Key, Scratch, false));
}

VaRest::Json::FDocument* FVaRestJson::PrepareContainer(EVaRestJsonType Kind, const TCHAR* Operation)
{
	if (!Node)
	{
		if (Doc)
		{
			UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson: cannot %s on a missing value (a lookup missed); set the field on its parent first, e.g. Parent.Set(Key, FVaRestJson::Object())"), Operation);
			return nullptr;
		}
		*this = Kind == EVaRestJsonType::Object ? Object() : Array();
		return Doc.Get();
	}
	if (Node->Type == Kind)
	{
		return Doc.Get();
	}
	UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson: cannot %s on a %s"), Operation, VaRest::Json::HandlePrivate::TypeName(Node->Type));
	return nullptr;
}

void FVaRestJson::SetNode(FStringView Key, VaRest::Json::FNode* Value)
{
	VaRest::Json::FUtf8Scratch Scratch;
	Doc->Set(*Node, VaRest::Json::ConvertForDocument(Key, Scratch), Value);
}

void FVaRestJson::SetNodeUtf8(FUtf8StringView Key, VaRest::Json::FNode* Value)
{
	VaRest::Json::FUtf8Scratch Scratch;
	Doc->Set(*Node, VaRest::Json::ValidateForDocument(Key, Scratch), Value);
}

void FVaRestJson::AddNode(VaRest::Json::FNode* Value)
{
	Doc->Append(*Node, Value);
}

void FVaRestJson::SetAtNode(int32 Index, VaRest::Json::FNode* Value)
{
	if (Index >= 0)
	{
		Doc->SetAt(*Node, static_cast<uint32>(Index), Value);
	}
}

bool FVaRestJson::Remove(FUtf8StringView Key)
{
	if (!IsObject())
	{
		return false;
	}
	VaRest::Json::FUtf8Scratch Scratch; // as in Get: the key Set stored is the repaired one
	if (!VaRest::Json::IsValidUtf8(Key))
	{
		VaRest::Json::AppendRepairedUtf8(Key, Scratch);
		Key = FUtf8StringView(Scratch.GetData(), Scratch.Num());
	}
	return Doc->Remove(*Node, Key) > 0;
}

bool FVaRestJson::Remove(FStringView Key)
{
	if (!IsObject())
	{
		return false;
	}
	VaRest::Json::FUtf8Scratch Scratch;
	return Remove(VaRest::Json::ConvertForDocument(Key, Scratch, false));
}

VaRest::Json::FDocument* FVaRestJson::PrepareSetAt(int32 Index)
{
	// One message per cause: "an invalid handle of 0 elements" (M1) named neither the missed lookup nor the type.
	if (!Node)
	{
		if (Doc)
		{
			UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson: cannot SetAt index %d on a missing value (a lookup missed)"), Index);
		}
		else
		{
			UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson: cannot SetAt index %d on an invalid handle"), Index);
		}
		return nullptr;
	}
	if (Node->Type != EVaRestJsonType::Array)
	{
		UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson: cannot SetAt index %d on a %s"), Index, VaRest::Json::HandlePrivate::TypeName(Node->Type));
		return nullptr;
	}
	if (Index < 0 || Index >= static_cast<int32>(Node->Arr.Num))
	{
		UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson: cannot SetAt index %d on an array of %d elements"), Index, static_cast<int32>(Node->Arr.Num));
		return nullptr;
	}
	return Doc.Get();
}

TArray<FString> FVaRestJson::GetKeys() const
{
	TArray<FString> Keys;
	if (IsObject())
	{
		Keys.Reserve(Node->Obj.Num);
		for (uint32 i = 0; i < Node->Obj.Num; ++i)
		{
			const VaRest::Json::FStr& Key = Node->Obj.Members[i].Key;
			Keys.Add(FString::ConstructFromPtrSize(Key.Data, static_cast<int32>(Key.Len)));
		}
	}
	return Keys;
}

FVaRestJsonMemberRange FVaRestJson::Members() const
{
	return FVaRestJsonMemberRange(*this);
}

FVaRestJsonMember FVaRestJson::MemberAt(int32 Index) const
{
	if (!IsObject() || Index < 0 || Index >= static_cast<int32>(Node->Obj.Num))
	{
		return FVaRestJsonMember{};
	}
	const VaRest::Json::FMember& M = Node->Obj.Members[Index];
	return FVaRestJsonMember{ M.Key.View(), FVaRestJson(Doc, M.Value) };
}

FVaRestJson FVaRestJson::At(int32 Index) const
{
	if (!IsArray() || Index < 0 || Index >= static_cast<int32>(Node->Arr.Num))
	{
		return Miss();
	}
	return FVaRestJson(Doc, Node->Arr.Items[Index]);
}

bool FVaRestJson::RemoveAt(int32 Index)
{
	return IsArray() && Index >= 0 && Doc->RemoveAt(*Node, static_cast<uint32>(Index));
}

FVaRestJsonElementRange FVaRestJson::Elements() const
{
	return FVaRestJsonElementRange(*this);
}

FVaRestJson FVaRestJson::Clone() const
{
	using VaRest::Json::FArena;
	if (!Node)
	{
		return {};
	}
	// A root clone needs about what the source used; a subtree clone starts small and grows.
	FDocumentRef NewDoc = NewDocument(Node == Doc->Root ? FMath::Max<SIZE_T>(FArena::MinChunkSize, Doc->Arena.GetUsedBytes()) : FArena::MinChunkSize);
	VaRest::Json::FNode* Root = NewDoc->DeepCopy(*Node);
	if (!Root)
	{
		UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson::Clone: cannot copy a value nested deeper than %d levels"), VaRest::Json::MaxDepth);
		return {};
	}
	NewDoc->Root = Root;
	return FVaRestJson(MoveTemp(NewDoc), Root);
}

bool FVaRestJson::Equals(const FVaRestJson& Other) const
{
	if (!Node || !Other.Node)
	{
		return !Node && !Other.Node;
	}
	if (Node == Other.Node)
	{
		return true;
	}
	bool bTooDeep = false;
	const bool bEqual = VaRest::Json::FDocument::Equals(*Node, *Other.Node, &bTooDeep);
	if (bTooDeep)
	{
		UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson::Equals: cannot compare values nested deeper than %d levels; treated as not equal"), VaRest::Json::MaxDepth);
	}
	return bEqual;
}

VaRest::Json::FNode* FVaRestJson::MakeNull(VaRest::Json::FDocument& InDoc) { return InDoc.NewNull(); }
VaRest::Json::FNode* FVaRestJson::MakeBool(VaRest::Json::FDocument& InDoc, bool bValue) { return InDoc.NewBool(bValue); }
VaRest::Json::FNode* FVaRestJson::MakeInt(VaRest::Json::FDocument& InDoc, int64 Value) { return InDoc.NewInt(Value); }
VaRest::Json::FNode* FVaRestJson::MakeUInt(VaRest::Json::FDocument& InDoc, uint64 Value) { return InDoc.NewUInt(Value); }
VaRest::Json::FNode* FVaRestJson::MakeDouble(VaRest::Json::FDocument& InDoc, double Value) { return InDoc.NewDouble(Value); }
VaRest::Json::FNode* FVaRestJson::MakeUtf8String(VaRest::Json::FDocument& InDoc, FUtf8StringView Value)
{
	VaRest::Json::FUtf8Scratch Scratch;
	return InDoc.NewString(VaRest::Json::ValidateForDocument(Value, Scratch));
}

VaRest::Json::FNode* FVaRestJson::MakeString(VaRest::Json::FDocument& InDoc, FStringView Value)
{
	VaRest::Json::FUtf8Scratch Scratch;
	return InDoc.NewString(VaRest::Json::ConvertForDocument(Value, Scratch));
}

VaRest::Json::FNode* FVaRestJson::MakeCopy(VaRest::Json::FDocument& InDoc, const FVaRestJson& Value)
{
	if (!Value.Node)
	{
		return InDoc.NewNull();
	}
	// A node has exactly one parent, so even a value from this same document is copied.
	VaRest::Json::FNode* Copy = InDoc.DeepCopy(*Value.Node);
	if (!Copy)
	{
		UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson: cannot copy a value nested deeper than %d levels; nothing was written"), VaRest::Json::MaxDepth);
	}
	return Copy;
}
