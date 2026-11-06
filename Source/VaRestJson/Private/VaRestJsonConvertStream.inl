// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
// Included only by VaRestJsonConvertDecode.cpp: scalar conversion and its logical account remain shared.

namespace VaRest::Json::Convert::StreamPrivate
{
using namespace DecodePrivate;
using FReader = rapidjson::GenericReader<rapidjson::UTF8<>, rapidjson::UTF8<>, UE::Json::Private::FAllocatorImpl>;
constexpr unsigned StreamFlags = rapidjson::kParseIterativeFlag | rapidjson::kParseValidateEncodingFlag | rapidjson::kParseFullPrecisionFlag;
static_assert((StreamFlags & rapidjson::kParseIterativeFlag) != 0);

struct FPlan
{
	const FVaRestJsonConvertOptions& Options;
	TSharedRef<const FVaRestJsonRuleSnapshot, ESPMode::ThreadSafe> Rules;
	TSet<const UScriptStruct*> Visiting, Ready;
	TMap<const UScriptStruct*, FFieldMapRef> Maps;
	bool Struct(const UScriptStruct* Type, int32 Depth = 0)
	{
		// Native layouts have the caller's ordinary immutable lifetime contract. Reflected GTK qualification stays separate.
		if (!Type || Type->IsA<UUserDefinedStruct>() || !Type->GetCppStructOps() || Depth >= MaxDepth || ClassifyStruct(Type) == ESpecialStruct::Json) return false;
		if (ClassifyStruct(Type) != ESpecialStruct::None || Ready.Contains(Type)) return true;
		if (Visiting.Contains(Type)) return false;
		Visiting.Add(Type); const auto Fields = GetFieldMap(Type, Options.Naming, Rules); Maps.Add(Type, Fields);
		bool bValid = Fields->Error.IsEmpty();
		for (const auto& Field : Fields->Fields) if (bValid) bValid = Property(Field.Property, Depth + 1);
		Visiting.Remove(Type); if (bValid) Ready.Add(Type); return bValid;
	}
	bool Property(const FProperty* Value, int32 Depth)
	{
		if (!Value || Value->ArrayDim != 1 || Depth >= MaxDepth) return false;
		if (const auto* Item = CastField<FStructProperty>(Value)) return Struct(Item->Struct, Depth);
		if (const auto* Array = CastField<FArrayProperty>(Value)) return Property(Array->Inner, Depth + 1);
		if (const auto* Map = CastField<FMapProperty>(Value))
		{
			const auto* StructValue = CastField<FStructProperty>(Map->ValueProp);
			return Map->KeyProp->IsA<FStrProperty>() && Map->ValueProp->ArrayDim == 1
				&& !Map->ValueProp->IsA<FArrayProperty>() && !Map->ValueProp->IsA<FSetProperty>()
				&& !Map->ValueProp->IsA<FMapProperty>() && !Map->ValueProp->IsA<FOptionalProperty>()
				&& (!StructValue || ClassifyStruct(StructValue->Struct) != ESpecialStruct::None)
				&& Property(Map->ValueProp, Depth + 1);
		}
		// Set/map/optional semantics continue to use their existing decoder, including enum-key collision rules.
		if (Value->IsA<FSetProperty>() || Value->IsA<FMapProperty>() || Value->IsA<FOptionalProperty>()) return false;
		FString Detail; return IsSupported(Value, Detail);
	}
};
const UScriptStruct* OrdinaryStruct(const FProperty* Property)
{
	const auto* Field = CastField<FStructProperty>(Property);
	return Field && ClassifyStruct(Field->Struct) == ESpecialStruct::None ? Field->Struct.Get() : nullptr;
}
int32 MatchNextField(const FFieldMap& Fields,FUtf8StringView Name,int32 Next,FUtf8Scratch& Scratch)
{
	if (Fields.Fields.IsValidIndex(Next))
	{
		const auto Expected=Fields.Fields[Next].NameView();
		if (Expected.Len()==Name.Len() && (!Name.Len() || FMemory::Memcmp(Expected.GetData(),Name.GetData(),Name.Len())==0)) return Next;
	}
	uint8 Tier=0;
	return Fields.Match(Name,HashKey(Name),Scratch,Tier);
}

/** A syntax/type-only pass. It writes no destination memory. Field order/duplicates that need DOM selection
 * fall back before conversion; array counts let the real walk reserve once, before constructing elements. */
struct FPreflight
{
	struct FFrame
	{
		const FFieldMap* Fields = nullptr; // Plan owns every immutable map through both walks.
		const FArrayProperty* Array = nullptr;
		const FMapProperty* Map = nullptr;
		const FProperty* Pending = nullptr;
		int32 LastField = INDEX_NONE;
		int32 CountIndex = INDEX_NONE;
		bool bObject = false;
	};
	FPlan& Plan;
	const UScriptStruct* Root;
	FJsonBudget Budget;
	TArray<FFrame, TInlineAllocator<16>> Frames;
	TArray<uint32> ContainerCounts;
	FUtf8Scratch Scratch;
	bool bRoot = true, bFallback = false;
	FPreflight(FPlan& InPlan, const UScriptStruct* InRoot, int64 Bytes)
		: Plan(InPlan), Root(InRoot), Budget({0, InPlan.Options.MaxValues, InPlan.Options.MaxWorkUnits})
	{ Budget.Charge(0, 0, Bytes); }
	const FProperty* Slot()
	{
		if (Frames.IsEmpty()) return nullptr;
		auto& Parent = Frames.Last();
		if (Parent.Array) return Parent.Array->Inner;
		if (Parent.Map) return Parent.Map->ValueProp;
		const auto* Out = Parent.Pending; Parent.Pending = nullptr; return Out;
	}
	bool Scalar() { Slot(); bRoot = false; return Budget.Charge(0, 1, 1); }
	bool Null() { return Scalar(); }
	bool Bool(bool) { return Scalar(); }
	bool Int(int) { return Scalar(); }
	bool Uint(unsigned) { return Scalar(); }
	bool Int64(int64) { return Scalar(); }
	bool Uint64(uint64) { return Scalar(); }
	bool Double(double) { return Scalar(); }
	bool RawNumber(const char*, rapidjson::SizeType, bool) { return Scalar(); }
	bool String(const char*, rapidjson::SizeType, bool) { return Scalar(); }
	bool Key(const char* Text, rapidjson::SizeType Length, bool)
	{
		if (!Budget.Charge(0, 1, 1) || Frames.IsEmpty()) return false;
		auto& Frame = Frames.Last(); if (!Frame.Fields) return true;
		const FUtf8StringView Name(reinterpret_cast<const UTF8CHAR*>(Text), static_cast<int32>(Length));
		const int32 Index = MatchNextField(*Frame.Fields,Name,Frame.LastField+1,Scratch);
		if (Index != INDEX_NONE)
		{
			if (Index <= Frame.LastField) bFallback = true; // exact/tier/last-occurrence selection stays in the DOM path
			Frame.LastField = Index; Frame.Pending = Frame.Fields->Fields[Index].Property;
		}
		return true;
	}
	bool Begin(bool bObject)
	{
		if (!Budget.Charge(0, 1, 1) || Frames.Num() >= MaxDepth) return false;
		const bool bWasRoot = bRoot; const auto* Property = Slot(); bRoot = false;
		FFrame Frame; Frame.bObject = bObject;
		if (bObject)
		{
			const auto* Type = bWasRoot ? Root : OrdinaryStruct(Property);
			if (Type && ClassifyStruct(Type) == ESpecialStruct::None) Frame.Fields = &Plan.Maps.FindChecked(Type).Get();
			else Frame.Map = CastField<FMapProperty>(Property);
		}
		else Frame.Array = CastField<FArrayProperty>(Property);
		// Preorder slots match the second walk's container starts, including nested arrays.
		if (Frame.Map || Frame.Array) Frame.CountIndex=ContainerCounts.Add(0);
		Frames.Add(MoveTemp(Frame)); return true;
	}
	bool StartObject() { return Begin(true); }
	bool StartArray() { return Begin(false); }
	bool EndObject(rapidjson::SizeType Count)
	{
		if (Frames.IsEmpty()) return false;
		if (Frames.Last().CountIndex!=INDEX_NONE) ContainerCounts[Frames.Last().CountIndex]=Count;
		Frames.Pop(EAllowShrinking::No); return Budget.Charge(0, 0, 1);
	}
	bool EndArray(rapidjson::SizeType Count)
	{
		if (Frames.IsEmpty()) return false;
		if (Frames.Last().CountIndex!=INDEX_NONE) ContainerCounts[Frames.Last().CountIndex]=Count;
		Frames.Pop(EAllowShrinking::No); return Budget.Charge(0, 0, 1);
	}
};

struct FDecoder
{
	struct FMapScratch
	{
		const FMapProperty* Property;
		void* Address;
		void* Key;
		void* Value;
		FMapScratch(const FMapProperty* InProperty, void* InAddress) : Property(InProperty), Address(InAddress),
			Key(InProperty->KeyProp->AllocateAndInitializeValue()), Value(InProperty->ValueProp->AllocateAndInitializeValue()) {}
		~FMapScratch() { Property->KeyProp->DestroyAndFreeValue(Key); Property->ValueProp->DestroyAndFreeValue(Value); }
		void Reset()
		{
			Property->KeyProp->DestroyValue(Key); Property->KeyProp->InitializeValue(Key);
			Property->ValueProp->DestroyValue(Value); Property->ValueProp->InitializeValue(Value);
		}
	};
	struct FFrame
	{
		const FFieldMap* Fields = nullptr; // Borrowed only while the owning Plan remains alive.
		const FArrayProperty* Array = nullptr;
		FMapScratch* Map = nullptr;
		void* Address = nullptr;
		const FFieldInfo* Pending = nullptr;
		FUtf8StringView Key;
		uint32 Index = 0, Count = 0;
		int32 NextField = 0;
		bool bPath = false, bEntered = false;
	};
	struct FSlot
	{
		const FProperty* Property = nullptr;
		void* Address = nullptr;
		bool bRoot = false, bPath = false, bNullable = false;
		FMapScratch* Map = nullptr;
	};
	FDecodeContext& Context;
	const UScriptStruct* Root;
	void* RootAddress;
	const TArray<uint32>& Counts;
	const FPlan& Plan;
	/** Reuse storage by the actual property, never by a JSON key or destination address. */
	TArray<TUniquePtr<FMapScratch>, TInlineAllocator<4>> MapScratch;
	TArray<FFrame, TInlineAllocator<16>> Frames;
	int32 NextCount = 0;
	bool bRoot = true, bRootStored = true;
	bool ContainerCount(uint32& Out)
	{
		if (!Counts.IsValidIndex(NextCount)) return Context.Abort(TEXT("streaming container has no validated count"));
		Out=Counts[NextCount++];
		return Out<=MAX_int32 || Context.Abort(TEXT("streaming container cannot fit its element count"));
	}
	FSlot Slot()
	{
		FSlot Out; Out.bRoot = bRoot; bRoot = false;
		if (Out.bRoot) { Out.Address = RootAddress; return Out; }
		if (Frames.IsEmpty()) return Out;
		auto& Parent = Frames.Last();
		if (Parent.Array)
		{
			if (Parent.Index >= Parent.Count) { Context.Abort(TEXT("streaming array count changed after validation")); return Out; }
			FScriptArrayHelper Helper(Parent.Array, Parent.Address);
			Out.Property = Parent.Array->Inner; Out.Address = Helper.GetRawPtr(Parent.Index);
			Context.Path.Add(FSegment{FUtf8StringView(), static_cast<int32>(Parent.Index++)}); Out.bPath = true;
		}
		else if (Parent.Map)
		{
			Parent.Map->Reset(); Out.Map = Parent.Map;
			Context.Path.Add(FSegment{Parent.Key, -1}); Out.bPath = true;
			if (!Context.Charge((int64(Parent.Key.Len()) + 1) * sizeof(TCHAR) * 2, 1, Parent.Key.Len())
				|| !DecodeMapKey(Parent.Key, Out.Map->Property->KeyProp, Out.Map->Key, Context)) return Out;
			Out.Property = Out.Map->Property->ValueProp; Out.Address = Out.Map->Value;
		}
		else if (Parent.Pending)
		{
			Out.Property = Parent.Pending->Property; Out.Address = Out.Property->ContainerPtrToValuePtr<void>(Parent.Address);
			Out.bNullable = Parent.Pending->Rule.bNullable; Parent.Pending = nullptr;
			Context.Path.Add(FSegment{Parent.Key, -1}); Out.bPath = true;
		}
		return Out;
	}
	void EndSlot(const FSlot& Value) { if (Value.bPath) Context.Path.Pop(EAllowShrinking::No); }
	void Read(const FSlot& SlotValue, const FNode& Value)
	{
		if (SlotValue.bRoot) bRootStored = DecodeStruct(Value, Root, SlotValue.Address, Context);
		else if (SlotValue.Property && !(SlotValue.bNullable && Value.Type == EVaRestJsonType::Null))
		{
			const bool bStored = DecodeField(Value, SlotValue.Property, SlotValue.Address, Context);
			if (bStored && SlotValue.Map)
			{ FScriptMapHelper Helper(SlotValue.Map->Property, SlotValue.Map->Address); Helper.AddPair(SlotValue.Map->Key, SlotValue.Map->Value); }
		}
	}
	bool Scalar(FNode Value)
	{
		const auto Into = Slot(); Read(Into, Value); EndSlot(Into); return !Context.bAborted;
	}
	bool Null() { return Scalar(FNode()); }
	bool Bool(bool Value) { FNode Node; Node.Type = EVaRestJsonType::Bool; Node.Bool = Value; return Scalar(Node); }
	bool Int(int Value) { return Int64(Value); }
	bool Uint(unsigned Value) { return Uint64(Value); }
	bool Int64(int64 Value) { FNode Node; Node.Type = EVaRestJsonType::Number; Node.Int = Value; return Scalar(Node); }
	bool Uint64(uint64 Value)
	{
		if (Value <= static_cast<uint64>(MAX_int64)) return Int64(static_cast<int64>(Value));
		FNode Node; Node.Type = EVaRestJsonType::Number; Node.NumKind = ENumKind::UInt; Node.UInt = Value; return Scalar(Node);
	}
	bool Double(double Value) { FNode Node; Node.Type = EVaRestJsonType::Number; Node.NumKind = ENumKind::Double; Node.Double = Value; return Scalar(Node); }
	bool RawNumber(const char*, rapidjson::SizeType, bool) { return Context.Abort(TEXT("unexpected raw streaming number")); }
	bool String(const char* Value, rapidjson::SizeType Length, bool)
	{
		FNode Node; Node.Type = EVaRestJsonType::String; Node.Str = {reinterpret_cast<const UTF8CHAR*>(Value), Length, 0}; return Scalar(Node);
	}
	bool Key(const char* Text, rapidjson::SizeType Length, bool)
	{
		if (Frames.IsEmpty()) return false;
		auto& Frame = Frames.Last();
		if (Frame.Map) { Frame.Key = FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text), static_cast<int32>(Length)); return true; }
		if (!Frame.Fields) return true;
		if (!Context.Charge(Length, 1, int64(Length) + 1)) return false;
		Frame.Key = FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text), static_cast<int32>(Length));
		const int32 Index = MatchNextField(*Frame.Fields,Frame.Key,Frame.NextField,Context.Scratch);
		if (Index!=INDEX_NONE) Frame.NextField=Index+1;
		Frame.Pending = Index == INDEX_NONE ? nullptr : &Frame.Fields->Fields[Index]; return !Context.bAborted;
	}
	bool Begin(bool bObject)
	{
		const auto Into = Slot(); FFrame Frame; Frame.bPath = Into.bPath;
		const auto* Struct = Into.bRoot ? Root : OrdinaryStruct(Into.Property);
		if (bObject && Struct)
		{
			FNode Value; Value.Type = EVaRestJsonType::Object;
			if ((!Into.bRoot && !Context.Visit(Value, Into.Property->GetElementSize())) || !Context.Visit(Value, Struct->GetStructureSize()) || !Context.Enter()) return false;
			Frame.bEntered = true; Frame.Address = Into.Address; Frame.Fields = &Plan.Maps.FindChecked(Struct).Get();
			const int32 Fields = Frame.Fields->Fields.Num();
			if (!Context.Elements(Fields, sizeof(int32) + sizeof(uint8)) || !Context.Charge(0, Fields, Fields)) return false;
		}
		else if (bObject && Into.Property && Into.Property->IsA<FMapProperty>())
		{
			FNode Value; Value.Type = EVaRestJsonType::Object;
			const auto* Map = CastFieldChecked<FMapProperty>(Into.Property);
			uint32 Count=0; if (!ContainerCount(Count)) return false;
			const int64 EntryBytes = int64(Map->KeyProp->GetElementSize()) + Map->ValueProp->GetElementSize();
			if (!Context.Visit(Value, Into.Property->GetElementSize()) || !Context.Enter()
				|| !Context.Elements(Count, EntryBytes) || !Context.Elements(1, EntryBytes)) return false;
			Frame.bEntered = true;
			for (const auto& Scratch : MapScratch)
			{
				if (Scratch->Property == Map && !Scratch->Address)
				{
					Frame.Map = Scratch.Get();
					Frame.Map->Address = Into.Address;
					break;
				}
			}
			if (!Frame.Map)
			{
				Frame.Map = MapScratch.Add_GetRef(MakeUnique<FMapScratch>(Map, Into.Address)).Get();
			}
			FScriptMapHelper Helper(Map, Into.Address); Helper.EmptyValues(static_cast<int32>(Count));
		}
		else if (!bObject && Into.Property && Into.Property->IsA<FArrayProperty>())
		{
			FNode Value; Value.Type = EVaRestJsonType::Array;
			Frame.Array = CastFieldChecked<FArrayProperty>(Into.Property); Frame.Address = Into.Address;
			if (!ContainerCount(Frame.Count)) return false;
			if (!Context.Visit(Value, Into.Property->GetElementSize()) || !Context.Enter()
				|| !Context.Elements(Frame.Count, Frame.Array->Inner->GetElementSize())) return false;
			Frame.bEntered = true;
			FScriptArrayHelper Helper(Frame.Array, Frame.Address); Helper.EmptyValues(Frame.Count); Helper.AddValues(Frame.Count);
		}
		else
		{
			FNode Value; Value.Type = bObject ? EVaRestJsonType::Object : EVaRestJsonType::Array; Read(Into, Value);
		}
		Frames.Add(MoveTemp(Frame)); return !Context.bAborted;
	}
	bool StartObject() { return Begin(true); }
	bool StartArray() { return Begin(false); }
	bool EndObject(rapidjson::SizeType) { return End(); }
	bool EndArray(rapidjson::SizeType) { return End(); }
	bool End()
	{
		if (Frames.IsEmpty()) return false;
		const auto Frame = Frames.Pop(EAllowShrinking::No);
		if (Frame.Map)
		{
			// Retire the last entry's values at the same container boundary as before reuse.
			Frame.Map->Reset();
			Frame.Map->Address = nullptr;
		}
		if (Frame.bEntered) Context.Leave();
		if (Frame.bPath) Context.Path.Pop(EAllowShrinking::No);
		return !Context.bAborted;
	}
};
}

bool FVaRestJsonConvert::Utf8ToStructStreaming(TConstArrayView<uint8> Utf8, const UScriptStruct* Struct, void* Data,
	const FVaRestJsonConvertOptions& Options, FVaRestJsonDecodeReport* OutReport, bool* OutUsedStreaming)
{
	using namespace VaRest::Json;
	using namespace VaRest::Json::Convert;
	using namespace VaRest::Json::Convert::StreamPrivate;
	if (OutUsedStreaming) *OutUsedStreaming = false;
	FVaRestJsonConvertOptions Captured = Options;
	if (!Captured.Rules) Captured.Rules = FVaRestJsonRules::DefaultSnapshot();
	const auto Fallback = [&] { return Utf8ToStruct(Utf8, Struct, Data, Captured, OutReport); };
	if (!Struct || !Data || Utf8.Num() >= MAX_int32 || Options.BytesResolver || Options.MaxOutputBytes < 0 || Options.MaxValues < 0 || Options.MaxWorkUnits < 0)
		return Fallback();
	FPlan Plan{Captured, Captured.Rules.ToSharedRef(), {}, {}, {}};
	if (ClassifyStruct(Struct) != ESpecialStruct::None || !Plan.Struct(Struct)) return Fallback();
	const int64 Start = BomLength(Utf8.GetData(), Utf8.Num());
	FPreflight Probe(Plan, Struct, Utf8.Num());
	if (Probe.Budget.HasFailed()) return Fallback();
	// Both walks use the same bounded owned buffer and the reader's in-situ string fast path.
	// No source views survive preflight, so restoring the bytes keeps the validating and writing walks identical.
	TArray<uint8> Input; Input.Append(Utf8.GetData(), Utf8.Num()); Input.Add(0);
	rapidjson::InsituStringStream ProbeStream(reinterpret_cast<char*>(Input.GetData() + Start));
	UE::Json::Private::FAllocatorImpl ProbeAllocator; FReader ProbeReader(&ProbeAllocator);
	const auto Validated = ProbeReader.Parse<StreamFlags | rapidjson::kParseInsituFlag>(ProbeStream, Probe);
	if (Validated.IsError() || Start + static_cast<int64>(ProbeStream.Tell()) != Utf8.Num() || Probe.bFallback || Probe.Budget.HasFailed())
	{ Input.Empty(); return Fallback(); }
	// Syntax failures cannot partially alter the destination: the complete read-only validation above precedes writes.
	if (!Utf8.IsEmpty()) FMemory::Memcpy(Input.GetData(), Utf8.GetData(), Utf8.Num());
	Input[Utf8.Num()] = 0;
	rapidjson::InsituStringStream Stream(reinterpret_cast<char*>(Input.GetData() + Start));
	const FVaRestJson::FDocumentRef NoDocument;
	FDecodeContext Context(Captured, NoDocument, OutReport);
	FDecoder Decoder{Context, Struct, Data, Probe.ContainerCounts, Plan, {}, {}, 0, true, true};
	UE::Json::Private::FAllocatorImpl Allocator; FReader Reader(&Allocator);
	if (OutUsedStreaming) *OutUsedStreaming = true;
	const auto Result = Reader.Parse<StreamFlags | rapidjson::kParseInsituFlag>(Stream, Decoder);
	if (Result.IsError() && !Context.bAborted) Context.Abort(TEXT("streaming conversion diverged from validated syntax"));
	return Finish(Context, Decoder.bRootStored && !Result.IsError(), Struct->GetName());
}
