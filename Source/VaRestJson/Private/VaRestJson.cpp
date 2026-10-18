// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJson.h"

#include "VaRestJsonDocument.h"
#include "VaRestJsonLog.h"

using namespace VaRest::Json;

namespace
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
	return MakeShared<FDocument, ESPMode::ThreadSafe>(ArenaFirstChunk);
}

FVaRestJson FVaRestJson::Object()
{
	FDocumentRef NewDoc = NewDocument();
	FNode* Root = NewDoc->NewObject();
	NewDoc->Root = Root;
	return FVaRestJson(MoveTemp(NewDoc), Root);
}

FVaRestJson FVaRestJson::Array()
{
	FDocumentRef NewDoc = NewDocument();
	FNode* Root = NewDoc->NewArray();
	NewDoc->Root = Root;
	return FVaRestJson(MoveTemp(NewDoc), Root);
}

FVaRestJson FVaRestJson::Null()
{
	FDocumentRef NewDoc = NewDocument(256);
	FNode* Root = NewDoc->NewNull();
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
	if (!IsNumber())
	{
		return {};
	}
	switch (Node->NumKind)
	{
	case ENumKind::Int:
		return Node->Int;
	case ENumKind::UInt:
		return {}; // only values above INT64_MAX are stored as UInt
	case ENumKind::Double:
	{
		const double D = Node->Double;
		// [-2^63, 2^63) is the int64 range; both bounds are exact doubles
		if (FMath::IsFinite(D) && FMath::TruncToDouble(D) == D && D >= -9223372036854775808.0 && D < 9223372036854775808.0)
		{
			return static_cast<int64>(D);
		}
		return {};
	}
	}
	return {};
}

TOptional<uint64> FVaRestJson::TryGetUInt64() const
{
	if (!IsNumber())
	{
		return {};
	}
	switch (Node->NumKind)
	{
	case ENumKind::Int:
		return Node->Int >= 0 ? TOptional<uint64>(static_cast<uint64>(Node->Int)) : TOptional<uint64>();
	case ENumKind::UInt:
		return Node->UInt;
	case ENumKind::Double:
	{
		const double D = Node->Double;
		if (FMath::IsFinite(D) && FMath::TruncToDouble(D) == D && D >= 0.0 && D < 18446744073709551616.0)
		{
			return static_cast<uint64>(D);
		}
		return {};
	}
	}
	return {};
}

TOptional<double> FVaRestJson::TryGetDouble() const
{
	if (!IsNumber())
	{
		return {};
	}
	switch (Node->NumKind)
	{
	case ENumKind::Int:    return static_cast<double>(Node->Int);
	case ENumKind::UInt:   return static_cast<double>(Node->UInt);
	case ENumKind::Double: return Node->Double;
	}
	return {};
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
		return {};
	}
	FNode* Found = FDocument::Find(*Node, Key);
	return Found ? FVaRestJson(Doc, Found) : FVaRestJson();
}

FVaRestJson FVaRestJson::Get(FStringView Key) const
{
	if (!IsObject())
	{
		return {};
	}
	const auto Utf8 = StringCast<UTF8CHAR>(Key.GetData(), Key.Len());
	return Get(FUtf8StringView(Utf8.Get(), Utf8.Length()));
}

FDocument* FVaRestJson::PrepareContainer(EVaRestJsonType Kind, const TCHAR* Operation)
{
	if (!Node)
	{
		*this = Kind == EVaRestJsonType::Object ? Object() : Array();
		return Doc.Get();
	}
	if (Node->Type == Kind)
	{
		return Doc.Get();
	}
	UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson: cannot %s on a %s"), Operation, TypeName(Node->Type));
	return nullptr;
}

void FVaRestJson::SetNode(FStringView Key, FNode* Value)
{
	const auto Utf8 = StringCast<UTF8CHAR>(Key.GetData(), Key.Len());
	Doc->Set(*Node, FUtf8StringView(Utf8.Get(), Utf8.Length()), Value);
}

void FVaRestJson::SetNodeUtf8(FUtf8StringView Key, FNode* Value)
{
	Doc->Set(*Node, Key, Value);
}

void FVaRestJson::AddNode(FNode* Value)
{
	Doc->Append(*Node, Value);
}

void FVaRestJson::SetAtNode(int32 Index, FNode* Value)
{
	if (Index >= 0)
	{
		Doc->SetAt(*Node, static_cast<uint32>(Index), Value);
	}
}

bool FVaRestJson::Remove(FStringView Key)
{
	if (!IsObject())
	{
		return false;
	}
	const auto Utf8 = StringCast<UTF8CHAR>(Key.GetData(), Key.Len());
	return Doc->Remove(*Node, FUtf8StringView(Utf8.Get(), Utf8.Length())) > 0;
}

TArray<FString> FVaRestJson::GetKeys() const
{
	TArray<FString> Keys;
	if (IsObject())
	{
		Keys.Reserve(Node->Obj.Num);
		for (uint32 i = 0; i < Node->Obj.Num; ++i)
		{
			const FStr& Key = Node->Obj.Members[i].Key;
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
	const FMember& M = Node->Obj.Members[Index];
	return FVaRestJsonMember{ M.Key.View(), FVaRestJson(Doc, M.Value) };
}

FVaRestJson FVaRestJson::At(int32 Index) const
{
	if (!IsArray() || Index < 0 || Index >= static_cast<int32>(Node->Arr.Num))
	{
		return {};
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
	if (!Node)
	{
		return {};
	}
	FDocumentRef NewDoc = NewDocument(FMath::Max<SIZE_T>(4096, Doc->Arena.GetUsedBytes()));
	FNode* Root = NewDoc->DeepCopy(*Node);
	NewDoc->Root = Root;
	return FVaRestJson(MoveTemp(NewDoc), Root);
}

bool FVaRestJson::Equals(const FVaRestJson& Other) const
{
	if (!Node || !Other.Node)
	{
		return !Node && !Other.Node;
	}
	return Node == Other.Node || FDocument::Equals(*Node, *Other.Node);
}

FNode* FVaRestJson::MakeNull(FDocument& InDoc) { return InDoc.NewNull(); }
FNode* FVaRestJson::MakeBool(FDocument& InDoc, bool bValue) { return InDoc.NewBool(bValue); }
FNode* FVaRestJson::MakeInt(FDocument& InDoc, int64 Value) { return InDoc.NewInt(Value); }
FNode* FVaRestJson::MakeUInt(FDocument& InDoc, uint64 Value) { return InDoc.NewUInt(Value); }
FNode* FVaRestJson::MakeDouble(FDocument& InDoc, double Value) { return InDoc.NewDouble(Value); }
FNode* FVaRestJson::MakeUtf8String(FDocument& InDoc, FUtf8StringView Value) { return InDoc.NewString(Value); }

FNode* FVaRestJson::MakeString(FDocument& InDoc, FStringView Value)
{
	const auto Utf8 = StringCast<UTF8CHAR>(Value.GetData(), Value.Len());
	return InDoc.NewString(FUtf8StringView(Utf8.Get(), Utf8.Length()));
}

FNode* FVaRestJson::MakeCopy(FDocument& InDoc, const FVaRestJson& Value)
{
	// A node has exactly one parent, so even a value from this same document is copied.
	return Value.Node ? InDoc.DeepCopy(*Value.Node) : InDoc.NewNull();
}
