// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonConvert.h"

#include "Misc/Base64.h"
#include "UObject/EnumProperty.h"
#include "UObject/PropertyOptional.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/StructOnScope.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"
#include "VaRestJsonBuilder.h"
#include "VaRestJsonConvertPrivate.h"
#include "VaRestJsonLog.h"
#include "VaRestJsonSax.h"
#include "VaRestJsonUtf8.h"
#include "VaRestJsonWriterInternal.h"

double VaRest::Json::FloatToJsonDoubleInternal(float Value) { return VaRest::Json::Convert::FloatToJsonDouble(Value); }

namespace VaRest::Json::Convert
{
double FloatToJsonDouble(float Value)
{
	const double Exact = static_cast<double>(Value);
	if (!FMath::IsFinite(Value) || Value == 0.0f)
	{
		return Exact;
	}
	// Powers of ten up to 10^22 are exact doubles, so Round(x * 10^k) / 10^k is the double nearest that decimal; the
	// check keeps only a candidate that converts back to the very same float.
	static constexpr double Pow10[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
		1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
	const int32 Exponent = FMath::FloorToInt32(FMath::LogX(10.0, FMath::Abs(Exact)));
	for (int32 Digits = 6; Digits <= 9; ++Digits) // 9 significant digits always round-trip a float
	{
		const int32 Shift = Digits - 1 - Exponent;
		if (Shift > 22 || Shift < -22)
		{
			break;
		}
		const double Scale = Pow10[FMath::Abs(Shift)];
		const double Candidate = Shift >= 0 ? FMath::RoundToDouble(Exact * Scale) / Scale : FMath::RoundToDouble(Exact / Scale) * Scale;
		if (static_cast<float>(Candidate) == Value)
		{
			return Candidate;
		}
	}
	return Exact;
}

namespace EncodePrivate
{
/** RapidJSON asserts on a null string pointer (writer.h:198, :205); an empty view may carry one */
const char EmptyText[1] = {0};

struct FEncodeContext
{
	const FVaRestJsonConvertOptions& Options;
	const TSharedRef<const FVaRestJsonRuleSnapshot, ESPMode::ThreadSafe> Rules;
	FJsonBudget Budget;
	const FProperty* Base64Property = nullptr;
	TSet<const UEnum*> CheckedEnums;
	/** Containers open around the value being written: the count the parser and EmitNode limit */
	int32 Depth = 0;
	/** First failure; any failure fails the whole encode (spec 2.2) */
	FString Error;
	int32 NonFinite = 0;
	FUtf8Scratch Scratch;

	explicit FEncodeContext(const FVaRestJsonConvertOptions& InOptions)
		: Options(InOptions)
		, Rules(InOptions.Rules ? InOptions.Rules.ToSharedRef() : FVaRestJsonRules::DefaultSnapshot())
		, Budget({InOptions.MaxOutputBytes, InOptions.MaxValues, InOptions.MaxWorkUnits})
	{
		if (Budget.HasFailed())
			Fail(Budget.GetFailureMessage());
	}

	bool Charge(int64 Bytes, int64 Values, int64 Work)
	{
		if (!Error.IsEmpty())
			return false;
		if (Budget.Charge(Bytes, Values, Work))
			return true;
		Fail(Budget.GetFailureMessage());
		return false;
	}

	bool Enter()
	{
		if (Depth >= MaxDepth)
		{
			Fail(FString::Printf(TEXT("value nested deeper than %d levels"), MaxDepth));
			return false;
		}
		++Depth;
		return true;
	}

	void Leave()
	{
		--Depth;
	}

	void Fail(FString Message)
	{
		if (Error.IsEmpty())
		{
			Error = MoveTemp(Message);
		}
	}
};

/** Every emitted node/key is charged before either the DOM builder or writer can grow. */
template <typename InnerType>
struct FBudgetSink
{
	InnerType& Inner;
	FEncodeContext& Context;
	// An array child is held in both the builder scratch and its final arena vector until the builder dies.
	static constexpr int64 NodeSlot = JsonNodeBudgetBytes + 2 * sizeof(FNode*);
	int32 Nesting;

	FBudgetSink(InnerType& InInner, FEncodeContext& InContext)
		: Inner(InInner)
		, Context(InContext)
		, Nesting(InContext.Depth)
	{
	}
	int64 PrefixBytes() const { return 2 * int64(Nesting) + 2; } // default pretty indentation, separator/newline

	template <typename CallType>
	bool Emit(int64 Bytes, int64 Values, int64 Work, CallType&& Call)
	{
		if (!Context.Charge(Bytes, Values, Work))
			return false;
		if (Call())
			return true;
		Context.Fail(TEXT("JSON sink refused the converted value"));
		return false;
	}
	bool Null()
	{
		return Emit(NodeSlot + 32 + PrefixBytes(), 1, 1, [&] { return Inner.Null(); });
	}
	bool Bool(bool Value)
	{
		return Emit(NodeSlot + 32 + PrefixBytes(), 1, 1, [&] { return Inner.Bool(Value); });
	}
	bool Int64(int64 Value)
	{
		return Emit(NodeSlot + 32 + PrefixBytes(), 1, 1, [&] { return Inner.Int64(Value); });
	}
	bool Uint64(uint64 Value)
	{
		return Emit(NodeSlot + 32 + PrefixBytes(), 1, 1, [&] { return Inner.Uint64(Value); });
	}
	bool Double(double Value)
	{
		return Emit(NodeSlot + 32 + PrefixBytes(), 1, 1, [&] { return Inner.Double(Value); });
	}
	bool String(const char* Text, rapidjson::SizeType Length, bool bCopy)
	{
		// Six bytes per input byte covers JSON escaping; also charge owned DOM text and its slot.
		return Emit(NodeSlot + int64(Length) * 7 + 3 + PrefixBytes(), 1, int64(Length) + 1, [&] { return Inner.String(Text, Length, bCopy); });
	}
	bool Key(const char* Text, rapidjson::SizeType Length, bool bCopy)
	{
		return Emit(2 * sizeof(FMember) + 4 * sizeof(uint32) + int64(Length) * 7 + 3 + PrefixBytes(), 1, int64(Length) + 1, [&] { return Inner.Key(Text, Length, bCopy); });
	}
	bool StartObject()
	{
		if (!Emit(NodeSlot + sizeof(FDomBuilder::FFrame) + sizeof(FIndex) + PrefixBytes(), 1, 1, [&] { return Inner.StartObject(); }))
			return false;
		++Nesting;
		return true;
	}
	bool StartArray()
	{
		if (!Emit(NodeSlot + sizeof(FDomBuilder::FFrame) + PrefixBytes(), 1, 1, [&] { return Inner.StartArray(); }))
			return false;
		++Nesting;
		return true;
	}
	bool EndObject(rapidjson::SizeType Count)
	{
		if (!Emit(PrefixBytes(), 0, 1, [&] { return Inner.EndObject(Count); }))
			return false;
		--Nesting;
		return true;
	}
	bool EndArray(rapidjson::SizeType Count)
	{
		if (!Emit(PrefixBytes(), 0, 1, [&] { return Inner.EndArray(Count); }))
			return false;
		--Nesting;
		return true;
	}
};

bool CheckEnum(const UEnum* Enum, FEncodeContext& Context)
{
	if (!Context.CheckedEnums.Contains(Enum))
	{
		const int64 Count = Enum->NumEnums();
		if (!Context.Charge(0, 0, Count * Count + 1))
			return false;
		FString Error;
		if (!ValidateEnumRules(Enum, Context.Rules->Find(Enum), Error))
		{
			Context.Fail(FString::Printf(TEXT("%s: %s"), *Enum->GetPathName(), *Error));
			return false;
		}
		Context.Rules->WarnIfMissing(Enum);
		Context.CheckedEnums.Add(Enum);
	}
	return true;
}

template <typename SinkType>
void WriteUtf8Text(SinkType& Sink, FUtf8StringView Text)
{
	Sink.String(Text.Len() ? reinterpret_cast<const char*>(Text.GetData()) : EmptyText, static_cast<rapidjson::SizeType>(Text.Len()), true);
}

template <typename SinkType>
void WriteText(SinkType& Sink, FStringView Text, FEncodeContext& Context)
{
	if (!Context.Charge(0, 0, Text.Len()))
		return;
	const int64 Length = Utf8Length(Text);
	if (Length >= MAX_int32 || !Context.Charge(Length + 1, 0, Length))
	{
		if (Length >= MAX_int32)
			Context.Fail(TEXT("converted string cannot fit its UTF-8 buffer"));
		return;
	}
	// The sink copies at once (writer output, or FDomBuilder::String with copy = true), so Scratch is free again after
	WriteUtf8Text(Sink, ConvertForDocument(Text, Context.Scratch));
}

template <typename SinkType>
void WriteKey(SinkType& Sink, FUtf8StringView Key)
{
	Sink.Key(Key.Len() ? reinterpret_cast<const char*>(Key.GetData()) : EmptyText, static_cast<rapidjson::SizeType>(Key.Len()), true);
}

template <typename SinkType>
void WriteEnum(SinkType& Sink, const UEnum* Enum, int64 Value, FEncodeContext& Context)
{
	if (!CheckEnum(Enum, Context))
	{
		return;
	}
	if (const FVaRestJsonEnumRule* Rule = FindEnumRule(Enum, Value, *Context.Rules))
	{
		if (Rule->bUnset)
		{
			Sink.Null(); // a standalone value; fields/container entries omit it before writing their key/value
		}
		else
		{
			WriteText(Sink, Rule->Json, Context);
		}
		return;
	}
	if (!Context.Options.bEnumsAsNumbers)
	{
		const int32 Index = Enum->GetIndexByValue(Value);
		if (Index != INDEX_NONE)
		{
			// The authored name, as FJsonObjectConverter writes it (JsonObjectConverter.cpp:109-114): a C++
			// enumerator's short name, a Blueprint enum's display text (UserDefinedEnum.cpp:208-220)
			WriteText(Sink, Enum->GetAuthoredNameStringByIndex(Index), Context);
			return;
		}
		// A value with no enumerator (a bitmask, a cast) keeps its data as a number
	}
	Sink.Int64(Value);
}

template <typename SinkType>
void WriteNumber(SinkType& Sink, const FNumericProperty* Numeric, const void* Value, FEncodeContext& Context)
{
	if (const UEnum* Enum = Numeric->GetIntPropertyEnum())
	{
		WriteEnum(Sink, Enum, Numeric->GetSignedIntPropertyValue(Value), Context);
		return;
	}
	if (Numeric->IsFloatingPoint())
	{
		const FFloatProperty* FloatProperty = CastField<FFloatProperty>(Numeric);
		const double D = FloatProperty ? FloatToJsonDouble(FloatProperty->GetPropertyValue(Value)) : Numeric->GetFloatingPointPropertyValue(Value);
		if (FMath::IsFinite(D))
		{
			Sink.Double(D);
		}
		else
		{
			Sink.Null(); // JSON has no NaN/Infinity; one warning per encode (Finish)
			++Context.NonFinite;
		}
		return;
	}
	if (IsUnsignedInteger(Numeric))
	{
		Sink.Uint64(Numeric->GetUnsignedIntPropertyValue(Value));
	}
	else
	{
		Sink.Int64(Numeric->GetSignedIntPropertyValue(Value));
	}
}

/** A map key as JSON object key text */
bool MapKeyText(const FProperty* KeyProperty, const void* Key, FEncodeContext& Context, FString& Out)
{
	if (const FStrProperty* StrKey = CastField<FStrProperty>(KeyProperty))
	{
		const FString& Text = StrKey->GetPropertyValue(Key);
		if (!Context.Charge((int64(Text.Len()) + 1) * sizeof(TCHAR), 0, Text.Len()))
			return false;
		Out = Text;
		return true;
	}
	if (const FNameProperty* NameKey = CastField<FNameProperty>(KeyProperty))
	{
		Out = NameKey->GetPropertyValue(Key).ToString();
		return true;
	}
	const UEnum* Enum = nullptr;
	int64 EnumValue = 0;
	if (const FEnumProperty* EnumKey = CastField<FEnumProperty>(KeyProperty))
	{
		Enum = EnumKey->GetEnum();
		EnumValue = EnumKey->GetUnderlyingProperty()->GetSignedIntPropertyValue(Key);
	}
	else if (const FNumericProperty* NumericKey = CastField<FNumericProperty>(KeyProperty))
	{
		if (const UEnum* ByteEnum = NumericKey->GetIntPropertyEnum())
		{
			Enum = ByteEnum;
			EnumValue = NumericKey->GetSignedIntPropertyValue(Key);
		}
		else
		{
			Out = IsUnsignedInteger(NumericKey) ? LexToString(NumericKey->GetUnsignedIntPropertyValue(Key)) : LexToString(NumericKey->GetSignedIntPropertyValue(Key));
			return true;
		}
	}
	if (Enum)
	{
		if (!CheckEnum(Enum, Context))
		{
			return false;
		}
		if (const FVaRestJsonResolvedRules* Rules = Context.Rules->Find(Enum); Rules && !Rules->Enums.IsEmpty())
		{
			const int32 DeclaredIndex = Enum->GetIndexByValue(EnumValue);
			const int32 DeclaredCount = Enum->NumEnums() - (Enum->ContainsExistingMax() ? 1 : 0);
			if (DeclaredIndex == INDEX_NONE || DeclaredIndex >= DeclaredCount)
			{
				Context.Fail(TEXT("an undeclared enum map key has no unambiguous registered reverse spelling"));
				return false;
			}
		}
		if (const FVaRestJsonEnumRule* Rule = FindEnumRule(Enum, EnumValue, *Context.Rules))
		{
			Out = Rule->Json;
			return true;
		}
		const int32 Index = Context.Options.bEnumsAsNumbers ? INDEX_NONE : Enum->GetIndexByValue(EnumValue);
		Out = Index != INDEX_NONE ? Enum->GetAuthoredNameStringByIndex(Index) : LexToString(EnumValue);
		return true;
	}
	Context.Fail(FString::Printf(TEXT("a map key of type %s cannot be a JSON key"), *KeyProperty->GetClass()->GetName()));
	return false;
}

template <typename SinkType>
bool EncodeValue(SinkType& Sink, const FProperty* Property, const void* Value, FEncodeContext& Context);

/** An FVaRestJson value through the sink at the current depth; a missing handle is null (spec 2.2) */
template <typename SinkType>
bool EmitJson(SinkType& Sink, const FVaRestJson& Json, FEncodeContext& Context)
{
	if (!Json.IsValid())
	{
		Sink.Null(); // inside a container; a missing struct FIELD is omitted before this point
		return true;
	}
	FEmitState State;
	const bool bEmitted = EmitNode(Sink, *Json.GetNodeInternal(), Context.Depth, State);
	Context.NonFinite += State.NonFinite;
	if (!bEmitted)
	{
		Context.Fail(FString::Printf(TEXT("value nested deeper than %d levels"), MaxDepth));
	}
	return bEmitted;
}

template <typename SinkType>
bool EncodeSpecial(SinkType& Sink, ESpecialStruct Special, const void* Data, FEncodeContext& Context)
{
	switch (Special)
	{
	case ESpecialStruct::Json:
		return EmitJson(Sink, *static_cast<const FVaRestJson*>(Data), Context);
	case ESpecialStruct::DateTime:
		WriteText(Sink, static_cast<const FDateTime*>(Data)->ToIso8601(), Context);
		return true;
	case ESpecialStruct::Timespan:
		Sink.Double(static_cast<const FTimespan*>(Data)->GetTotalSeconds());
		return true;
	case ESpecialStruct::Guid:
		WriteText(Sink, static_cast<const FGuid*>(Data)->ToString(EGuidFormats::DigitsWithHyphens), Context);
		return true;
	case ESpecialStruct::SoftObjectPath:
		WriteText(Sink, static_cast<const FSoftObjectPath*>(Data)->ToString(), Context);
		return true;
	case ESpecialStruct::None:
		break;
	}
	return true;
}

/** Spec 2.4 / 2.2: an unset TOptional and a missing FVaRestJson are a missing key, not null */
bool IsUnsetEnum(const FProperty* Property, const void* Value, FEncodeContext& Context)
{
	const UEnum* Enum = nullptr;
	int64 Number = 0;
	if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		Enum = EnumProperty->GetEnum();
		Number = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(Value);
	}
	else if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property); Numeric && Numeric->GetIntPropertyEnum())
	{
		Enum = Numeric->GetIntPropertyEnum();
		Number = Numeric->GetSignedIntPropertyValue(Value);
	}
	if (Enum)
	{
		if (!CheckEnum(Enum, Context))
		{
			return false;
		}
		const FVaRestJsonEnumRule* Rule = FindEnumRule(Enum, Number, *Context.Rules);
		return Rule && Rule->bUnset;
	}
	if (const FOptionalProperty* Optional = CastField<FOptionalProperty>(Property))
	{
		const void* Inner = Optional->GetValuePointerForReadIfSet(Value);
		return Inner && IsUnsetEnum(Optional->GetValueProperty(), Inner, Context);
	}
	return false;
}

bool IsOmitted(const FProperty* Property, const void* Value, FEncodeContext& Context)
{
	if (Property->ArrayDim != 1)
	{
		return false; // a C-style array member is always written; an invalid element is null
	}
	if (const FOptionalProperty* Optional = CastField<FOptionalProperty>(Property))
	{
		return !Optional->IsSet(Value) || IsUnsetEnum(Property, Value, Context);
	}
	const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
	return (StructProperty && StructProperty->Struct == FVaRestJson::StaticStruct() && !static_cast<const FVaRestJson*>(Value)->IsValid()) || IsUnsetEnum(Property, Value, Context);
}

template <typename SinkType>
bool EncodeField(SinkType& Sink, const FProperty* Property, const void* Value, FEncodeContext& Context)
{
	if (Property->ArrayDim == 1)
	{
		return EncodeValue(Sink, Property, Value, Context);
	}
	// A C-style array member (int32 Values[3]) is a JSON array of its ArrayDim elements
	if (!Context.Enter())
	{
		return false;
	}
	Sink.StartArray();
	for (int32 i = 0; i < Property->ArrayDim; ++i)
	{
		if (!Context.Charge(0, 1, 1))
			return false;
		const void* Element = static_cast<const uint8*>(Value) + static_cast<SIZE_T>(Property->GetElementSize()) * i;
		if (IsUnsetEnum(Property, Element, Context))
		{
			UE_LOG(LogVaRestJson, Warning, TEXT("Unset enum entry omitted from JSON fixed array"));
			continue;
		}
		if (!Context.Error.IsEmpty() || !EncodeValue(Sink, Property, Element, Context))
		{
			return false;
		}
	}
	Sink.EndArray(0);
	Context.Leave();
	return true;
}

/** The key/value pairs of Data's fields, in field-map order; no braces (the object is open around them) */
template <typename SinkType>
bool EncodeMembers(SinkType& Sink, const FFieldMap& Fields, const void* Data, FEncodeContext& Context,
	TConstArrayView<FString> ExcludedKeys = {}, bool* bSkipped = nullptr)
{
	if (!Fields.Error.IsEmpty())
	{
		Context.Fail(Fields.Error);
		return false;
	}
	auto Excluded = [&](const FFieldInfo& Field)
	{
		for (const FString& Key : ExcludedKeys)
		{
			const int64 Bytes = Utf8Length(Key);
			if (Bytes >= MAX_int32 || !Context.Charge(Bytes + 1, 0, int64(Key.Len()) + Bytes + Field.Name.Num() + 1))
			{ Context.Fail(TEXT("excluded member key exceeds the encode budget")); return false; }
			const FTCHARToUTF8 Utf8(*Key, Key.Len());
			if (Utf8.Length() == Field.Name.Num() && (Field.Name.IsEmpty() || FMemory::Memcmp(Utf8.Get(), Field.Name.GetData(), Field.Name.Num()) == 0))
			{ if (bSkipped) *bSkipped = true; return true; }
		}
		return false;
	};
	bool bNeedsDefaults = Fields.bNeedsDefaults;
	if (bNeedsDefaults && !ExcludedKeys.IsEmpty())
	{
		bNeedsDefaults = false;
		for (const FFieldInfo& Field : Fields.Fields)
			if ((Field.Rule.Presence == EVaRestJsonPresence::OmitWhenDefault || Field.Rule.Presence == EVaRestJsonPresence::NullWhenDefault) && !Excluded(Field))
			{ bNeedsDefaults = true; break; }
		if (!Context.Error.IsEmpty()) return false;
	}
	TUniquePtr<FStructOnScope> Defaults;
	if (bNeedsDefaults)
	{
		if (!Context.Charge(Fields.Struct->GetStructureSize(), 1, Fields.Fields.Num()))
			return false;
		Defaults = MakeUnique<FStructOnScope>(Fields.Struct);
	}
	for (const FFieldInfo& Field : Fields.Fields)
	{
		if (!Context.Charge(0, 1, 1))
			return false;
		if (Excluded(Field)) continue;
		if (!Context.Error.IsEmpty()) return false;
		if (Field.Rule.Presence == EVaRestJsonPresence::DecodeOnly)
		{
			continue;
		}
		const void* Value = Field.Property->ContainerPtrToValuePtr<void>(Data);
		bool bDefault = Defaults && (Field.Rule.Presence == EVaRestJsonPresence::OmitWhenDefault || Field.Rule.Presence == EVaRestJsonPresence::NullWhenDefault);
		for (int32 Index = 0; bDefault && Index < Field.Property->ArrayDim; ++Index)
		{
			if (!Context.Charge(0, 1, 1))
				return false;
			bDefault = Field.Property->Identical_InContainer(Data, Defaults->GetStructMemory(), Index);
		}
		const bool bUnset = Field.Property->ArrayDim == 1 && IsUnsetEnum(Field.Property, Value, Context);
		if (!Context.Error.IsEmpty())
		{
			return false;
		}
		if (Field.Rule.Presence == EVaRestJsonPresence::NullWhenDefault && (bDefault || bUnset))
		{
			WriteKey(Sink, Field.NameView());
			Sink.Null();
			continue;
		}
		if ((bDefault && Field.Rule.Presence == EVaRestJsonPresence::OmitWhenDefault) || IsOmitted(Field.Property, Value, Context))
		{
			continue;
		}
		WriteKey(Sink, Field.NameView());
		TGuardValue<const FProperty*> Base64(Context.Base64Property, Field.Rule.bBase64 ? Field.Property : nullptr);
		if (!EncodeField(Sink, Field.Property, Value, Context))
		{
			return false;
		}
	}
	return true;
}

template <typename SinkType>
bool EncodeStruct(SinkType& Sink, const UScriptStruct* Struct, const void* Data, FEncodeContext& Context)
{
	if (!Context.Charge(0, 0, 1))
		return false;
	const ESpecialStruct Special = ClassifyStruct(Struct);
	if (Special != ESpecialStruct::None)
	{
		return EncodeSpecial(Sink, Special, Data, Context);
	}
	if (!Context.Enter())
	{
		return false;
	}
	const FFieldMapRef Fields = GetFieldMap(Struct, Context.Options.Naming, Context.Rules);
	Sink.StartObject();
	if (!EncodeMembers(Sink, *Fields, Data, Context))
	{
		return false;
	}
	Sink.EndObject(0);
	Context.Leave();
	return true;
}

template <typename SinkType>
bool EncodeValue(SinkType& Sink, const FProperty* Property, const void* Value, FEncodeContext& Context)
{
	if (!Context.Charge(0, 0, 1))
		return false;
	if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
	{
		Sink.Bool(BoolProperty->GetPropertyValue(Value));
		return true;
	}
	if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		WriteEnum(Sink, EnumProperty->GetEnum(), EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(Value), Context);
		return Context.Error.IsEmpty();
	}
	if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
	{
		WriteNumber(Sink, Numeric, Value, Context);
		return Context.Error.IsEmpty();
	}
	if (const FStrProperty* StrProperty = CastField<FStrProperty>(Property))
	{
		WriteText(Sink, StrProperty->GetPropertyValue(Value), Context);
		return true;
	}
	if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
	{
		WriteText(Sink, NameProperty->GetPropertyValue(Value).ToString(), Context);
		return true;
	}
	if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
	{
		// Culture-invariant (spec 2.4): ToString() would give the current culture's display text
		WriteText(Sink, TextProperty->GetPropertyValue(Value).BuildSourceString(), Context);
		return true;
	}
	if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
	{
		return EncodeStruct(Sink, StructProperty->Struct, Value, Context);
	}
	if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
	{
		FScriptArrayHelper Helper(ArrayProperty, Value);
		if (Context.Options.BytesWriter && IsPlainByteArray(ArrayProperty))
		{
			// Socket.IO attachments (spec §18.2 J-1): the caller's replacement, written in place at this depth
			const TConstArrayView<uint8> Bytes(Helper.Num() ? Helper.GetRawPtr(0) : nullptr, Helper.Num());
			FVaRestJson Replacement;
			if (Context.Options.BytesWriter(Bytes, Replacement))
			{
				return EmitJson(Sink, Replacement, Context);
			}
		}
		if ((Context.Options.bBytesAsBase64 || Context.Base64Property == Property) && IsPlainByteArray(ArrayProperty))
		{
			const int64 EncodedLength = ((int64(Helper.Num()) + 2) / 3) * 4;
			if (EncodedLength >= MAX_int32 || !Context.Charge((EncodedLength + 1) * sizeof(TCHAR), 0, Helper.Num()))
			{
				if (EncodedLength >= MAX_int32)
					Context.Fail(TEXT("base64 output cannot fit its string buffer"));
				return false;
			}
			WriteText(Sink, Helper.Num() ? FBase64::Encode(Helper.GetRawPtr(0), static_cast<uint32>(Helper.Num())) : FString(), Context);
			return true;
		}
		if (!Context.Enter())
		{
			return false;
		}
		Sink.StartArray();
		for (int32 i = 0; i < Helper.Num(); ++i)
		{
			if (!Context.Charge(0, 1, 1))
				return false;
			if (IsUnsetEnum(ArrayProperty->Inner, Helper.GetRawPtr(i), Context))
			{
				UE_LOG(LogVaRestJson, Warning, TEXT("Unset enum entry omitted from JSON array"));
				continue;
			}
			if (!EncodeValue(Sink, ArrayProperty->Inner, Helper.GetRawPtr(i), Context))
			{
				return false;
			}
		}
		Sink.EndArray(0);
		Context.Leave();
		return true;
	}
	if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
	{
		FScriptSetHelper Helper(SetProperty, Value);
		if (!Context.Enter())
		{
			return false;
		}
		Sink.StartArray();
		for (FScriptSetHelper::FIterator It(Helper); It; ++It)
		{
			if (!Context.Charge(0, 1, 1))
				return false;
			if (IsUnsetEnum(SetProperty->ElementProp, Helper.GetElementPtr(It), Context))
			{
				UE_LOG(LogVaRestJson, Warning, TEXT("Unset enum entry omitted from JSON set"));
				continue;
			}
			if (!EncodeValue(Sink, SetProperty->ElementProp, Helper.GetElementPtr(It), Context))
			{
				return false;
			}
		}
		Sink.EndArray(0);
		Context.Leave();
		return true;
	}
	if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
	{
		FScriptMapHelper Helper(MapProperty, Value);
		if (!Context.Enter())
		{
			return false;
		}
		Sink.StartObject();
		FString KeyText;
		const FNumericProperty* NumericKey = CastField<FNumericProperty>(MapProperty->KeyProp);
		const bool bEnumKeys = MapProperty->KeyProp->IsA<FEnumProperty>() || (NumericKey && NumericKey->GetIntPropertyEnum());
		if (bEnumKeys)
		{
			const UEnum* Enum = NumericKey ? NumericKey->GetIntPropertyEnum() : CastFieldChecked<FEnumProperty>(MapProperty->KeyProp)->GetEnum();
			const int64 Count = Enum->NumEnums();
			if (!Context.Charge(0, 0, Count * Count + 1))
				return false;
			FString Error;
			if (!ValidateEnumRules(Enum, Context.Rules->Find(Enum), Error, Context.Options.bEnumsAsNumbers))
			{
				Context.Fail(FString::Printf(TEXT("%s: %s"), *Enum->GetPathName(), *Error));
				return false;
			}
		}
		TMap<uint32, TArray<TArray<UTF8CHAR>>> EmittedKeys;
		for (FScriptMapHelper::FIterator It(Helper); It; ++It)
		{
			if (!Context.Charge(0, 1, 1))
				return false;
			if (IsUnsetEnum(MapProperty->KeyProp, Helper.GetKeyPtr(It), Context) || IsUnsetEnum(MapProperty->ValueProp, Helper.GetValuePtr(It), Context))
			{
				UE_LOG(LogVaRestJson, Warning, TEXT("Unset enum entry omitted from JSON map"));
				continue;
			}
			if (!MapKeyText(MapProperty->KeyProp, Helper.GetKeyPtr(It), Context, KeyText))
			{
				return false;
			}
			const int64 KeyBytes = Utf8Length(KeyText);
			if (KeyBytes >= MAX_int32)
			{
				Context.Fail(TEXT("converted key cannot fit its UTF-8 buffer"));
				return false;
			}
			if (!Context.Charge(KeyBytes + 1, 0, KeyText.Len() + KeyBytes))
				return false;
			const FUtf8StringView Wire = ConvertForDocument(KeyText, Context.Scratch);
			if (bEnumKeys)
			{
				// Include cast/unknown enum values and the actual UTF-8 repair, not only declared enumerators.
				TArray<TArray<UTF8CHAR>>& Bucket = EmittedKeys.FindOrAdd(HashKey(Wire));
				if (Bucket.ContainsByPredicate([&](const TArray<UTF8CHAR>& Existing) {
						return Existing.Num() == Wire.Len() && (Wire.Len() == 0 || FMemory::Memcmp(Existing.GetData(), Wire.GetData(), Wire.Len()) == 0);
					}))
				{
					Context.Fail(TEXT("enum map keys have the same encoded wire spelling"));
					return false;
				}
				TArray<UTF8CHAR> OwnedKey;
				if (!Context.Charge(Wire.Len(), 0, Wire.Len()))
					return false;
				OwnedKey.Append(Wire.GetData(), Wire.Len());
				Bucket.Add(MoveTemp(OwnedKey));
			}
			WriteKey(Sink, Wire);
			if (!EncodeValue(Sink, MapProperty->ValueProp, Helper.GetValuePtr(It), Context))
			{
				return false;
			}
		}
		Sink.EndObject(0);
		Context.Leave();
		return true;
	}
	if (const FOptionalProperty* Optional = CastField<FOptionalProperty>(Property))
	{
		const void* Inner = Optional->GetValuePointerForReadIfSet(Value);
		if (!Inner)
		{
			Sink.Null(); // inside a container; an unset optional FIELD is omitted before this point
			return true;
		}
		return EncodeValue(Sink, Optional->GetValueProperty(), Inner, Context);
	}
	if (const FSoftObjectProperty* SoftProperty = CastField<FSoftObjectProperty>(Property))
	{
		WriteText(Sink, SoftProperty->GetPropertyValue(Value).ToSoftObjectPath().ToString(), Context);
		return true;
	}
	Context.Fail(FString::Printf(TEXT("%s properties are not converted"), *Property->GetClass()->GetName()));
	return false;
}

bool Finish(bool bEncoded, const FEncodeContext& Context, const FString& What, FVaRestJsonError* OutError)
{
	bEncoded = bEncoded && Context.Error.IsEmpty();
	if (!bEncoded)
	{
		const FString Message = FString::Printf(TEXT("FVaRestJsonConvert: cannot encode %s: %s"), *What, *Context.Error);
		if (OutError)
		{
			*OutError = FVaRestJsonError();
			OutError->Message = Message;
		}
		else
		{
			UE_LOG(LogVaRestJson, Error, TEXT("%s"), *Message);
		}
		return false;
	}
	if (Context.NonFinite > 0)
	{
		UE_LOG(LogVaRestJson, Warning, TEXT("FVaRestJsonConvert: %d non-finite number(s) written as null (JSON has no NaN/Infinity)"), Context.NonFinite);
	}
	return true;
}

/** Runs Body against a fresh document's DOM builder */
template <typename BodyType>
FVaRestJson BuildDocument(FEncodeContext& Context, BodyType&& Body)
{
	FVaRestJson::FDocumentRef NewDoc = MakeShared<FDocument, ESPMode::ThreadSafe>();
	FDomBuilder Builder(*NewDoc, MaxDepth);
	FBudgetSink<FDomBuilder> Sink{Builder, Context};
	if (!Body(Sink) || !Context.Error.IsEmpty())
	{
		return FVaRestJson();
	}
	NewDoc->Root = Builder.Result;
	FNode* Root = NewDoc->Root;
	return FVaRestJson::MakeInternal(MoveTemp(NewDoc), Root);
}

template <typename WriterType>
bool WriteMembers(WriterType& Writer, const UScriptStruct* Struct, const void* Data, const FVaRestJsonConvertOptions& Options,
	int32 OpenContainers, FVaRestJsonError* OutError, TConstArrayView<FString> ExcludedKeys = {}, bool* bSkipped = nullptr)
{
	FEncodeContext Context(Options);
	if (!Struct || !Data)
	{
		Context.Fail(TEXT("no struct to encode"));
		return Finish(false, Context, TEXT("a null struct"), OutError);
	}
	if (ClassifyStruct(Struct) != ESpecialStruct::None)
	{
		Context.Fail(TEXT("it is written as a single value and has no members"));
		return Finish(false, Context, Struct->GetName(), OutError);
	}
	Context.Depth = FMath::Max(0, OpenContainers);
	FBudgetSink<WriterType> Sink{Writer, Context};
	return Finish(EncodeMembers(Sink, *GetFieldMap(Struct, Options.Naming, Context.Rules), Data, Context, ExcludedKeys, bSkipped), Context, Struct->GetName(), OutError);
}
} // namespace EncodePrivate
} // namespace VaRest::Json::Convert

bool VaRest::Json::WriteStructMembersInternal(FCondensedWriter& Writer, const UScriptStruct* Struct, const void* Data,
	const FVaRestJsonConvertOptions& Options, int32 OpenContainers, FVaRestJsonError* OutError)
{
	return Convert::EncodePrivate::WriteMembers(Writer, Struct, Data, Options, OpenContainers, OutError);
}

bool VaRest::Json::WriteStructMembersInternal(FPrettyWriter& Writer, const UScriptStruct* Struct, const void* Data,
	const FVaRestJsonConvertOptions& Options, int32 OpenContainers, FVaRestJsonError* OutError)
{
	return Convert::EncodePrivate::WriteMembers(Writer, Struct, Data, Options, OpenContainers, OutError);
}

bool VaRest::Json::WriteStructMembersFilteredInternal(FCondensedWriter& Writer, const UScriptStruct* Struct, const void* Data,
	TConstArrayView<FString> ExcludedKeys, bool& bSkipped, const FVaRestJsonConvertOptions& Options, int32 OpenContainers, FVaRestJsonError* OutError)
{
	bSkipped = false;
	return Convert::EncodePrivate::WriteMembers(Writer, Struct, Data, Options, OpenContainers, OutError, ExcludedKeys, &bSkipped);
}

bool VaRest::Json::WriteJsonInternal(FCondensedWriter& Writer, const FVaRestJson& Value, int32 OpenContainers, FVaRestJsonError* OutError)
{
	using namespace Convert::EncodePrivate;
	const FVaRestJsonConvertOptions Defaults;
	FEncodeContext Context(Defaults);
	Context.Depth = FMath::Max(0, OpenContainers);
	return Finish(EmitJson(Writer, Value, Context), Context, TEXT("a VaRest Json value"), OutError);
}

bool VaRest::Json::WriteJsonMembersInternal(FCondensedWriter& Writer, const FVaRestJson& Object, int32 OpenContainers, FVaRestJsonError* OutError)
{
	using namespace Convert::EncodePrivate;
	const FVaRestJsonConvertOptions Defaults;
	FEncodeContext Context(Defaults);
	if (!Object.IsObject())
	{
		Context.Fail(TEXT("it is not an object"));
		return Finish(false, Context, TEXT("a VaRest Json value"), OutError);
	}
	Context.Depth = FMath::Max(0, OpenContainers);
	// The members as stored, duplicates included, exactly as EmitNode writes an object's body
	const FNode& Source = *Object.GetNodeInternal();
	bool bEmitted = true;
	for (uint32 i = 0; i < Source.Obj.Num && bEmitted; ++i)
	{
		const FMember& Member = Source.Obj.Members[i];
		WriteKey(Writer, Member.Key.View());
		FEmitState State;
		bEmitted = EmitNode(Writer, *Member.Value, Context.Depth, State);
		Context.NonFinite += State.NonFinite;
	}
	if (!bEmitted)
	{
		Context.Fail(FString::Printf(TEXT("value nested deeper than %d levels"), MaxDepth));
	}
	return Finish(bEmitted, Context, TEXT("a VaRest Json value"), OutError);
}

bool VaRest::Json::WriteStructInternal(FCondensedWriter& Writer, const UScriptStruct* Struct, const void* Data,
	const FVaRestJsonConvertOptions& Options, int32 OpenContainers, FVaRestJsonError* OutError)
{
	using namespace Convert::EncodePrivate;
	FEncodeContext Context(Options);
	if (!Struct || !Data)
	{
		Context.Fail(TEXT("no struct to encode"));
		return Finish(false, Context, TEXT("a null struct"), OutError);
	}
	Context.Depth = FMath::Max(0, OpenContainers);
	FBudgetSink<FCondensedWriter> Sink{Writer, Context};
	return Finish(EncodeStruct(Sink, Struct, Data, Context), Context, Struct->GetName(), OutError);
}

bool VaRest::Json::WritePropertyInternal(FCondensedWriter& Writer, const FProperty* Property, const void* Value,
	const FVaRestJsonConvertOptions& Options, int32 OpenContainers, FVaRestJsonError* OutError)
{
	using namespace Convert;
	using namespace Convert::EncodePrivate;
	FEncodeContext Context(Options);
	if (!Property || !Value)
	{
		Context.Fail(TEXT("no value to encode"));
		return Finish(false, Context, TEXT("a null property"), OutError);
	}
	FString Why;
	if (!IsSupported(Property, Why))
	{
		Context.Fail(Why);
		return Finish(false, Context, Property->GetName(), OutError);
	}
	Context.Depth = FMath::Max(0, OpenContainers);
	FBudgetSink<FCondensedWriter> Sink{Writer, Context};
	return Finish(EncodeField(Sink, Property, Value, Context), Context, Property->GetName(), OutError);
}

TArray<const FProperty*> VaRest::Json::GetConvertedFieldsInternal(const UScriptStruct* Struct, const FVaRestJsonConvertOptions& Options)
{
	TArray<const FProperty*> Fields;
	TryGetConvertedFieldsInternal(Struct, Fields, Options);
	return Fields;
}

bool VaRest::Json::TryGetConvertedFieldsInternal(const UScriptStruct* Struct, TArray<const FProperty*>& OutFields,
	const FVaRestJsonConvertOptions& Options, FVaRestJsonError* OutError)
{
	using namespace Convert;
	OutFields.Reset();
	if (!Struct || ClassifyStruct(Struct) != ESpecialStruct::None)
	{
		return true;
	}
	// The very map EncodeMembers walks (and DecodeStruct matches against): one filter, in one place (NamesPrivate::Build)
	const FFieldMapRef Fields = GetFieldMap(Struct, Options.Naming, Options.Rules ? Options.Rules.ToSharedRef() : FVaRestJsonRules::DefaultSnapshot());
	if (!Fields->Error.IsEmpty())
	{
		EncodePrivate::FEncodeContext Context(Options);
		Context.Fail(Fields->Error);
		return EncodePrivate::Finish(false, Context, Struct->GetName(), OutError);
	}
	EncodePrivate::FEncodeContext Context(Options);
	if (!Context.Charge(int64(Fields->Fields.Num()) * sizeof(const FProperty*), Fields->Fields.Num(), Fields->Fields.Num()))
		return EncodePrivate::Finish(false, Context, Struct->GetName(), OutError);
	OutFields.Reserve(Fields->Fields.Num());
	for (const FFieldInfo& Field : Fields->Fields)
	{
		if (Field.Rule.Presence != EVaRestJsonPresence::DecodeOnly)
		{
			OutFields.Add(Field.Property);
		}
	}
	return true;
}

namespace VaRest::Json::Convert::EncodePrivate
{
bool EncodeToUtf8(const UScriptStruct* Struct, const void* Data, TArray<uint8>& Out,
	FEncodeContext& Context, EVaRestJsonFormat Format, FVaRestJsonError* OutError)
{
	if (!Struct || !Data)
	{
		Context.Fail(TEXT("no struct to encode"));
		return Finish(false, Context, TEXT("a null struct"), OutError);
	}
	const int32 Start = Out.Num();
	FUtf8Sink Sink{Out};
	FWriterAllocator StackAllocator;
	bool bEncoded = false;
	if (Format == EVaRestJsonFormat::Pretty)
	{
		FPrettyWriter Writer(Sink, &StackAllocator);
		Writer.SetIndent(' ', 2);
		FBudgetSink<FPrettyWriter> BudgetSink{Writer, Context};
		bEncoded = EncodeStruct(BudgetSink, Struct, Data, Context);
	}
	else
	{
		FCondensedWriter Writer(Sink, &StackAllocator);
		FBudgetSink<FCondensedWriter> BudgetSink{Writer, Context};
		bEncoded = EncodeStruct(BudgetSink, Struct, Data, Context);
	}
	bEncoded = bEncoded && Context.Error.IsEmpty();
	if (!bEncoded)
	{
		Out.SetNum(Start, EAllowShrinking::No);
	}
	return Finish(bEncoded, Context, Struct->GetName(), OutError);
}
} // namespace VaRest::Json::Convert::EncodePrivate

bool FVaRestJsonConvert::StructToUtf8(const UScriptStruct* Struct, const void* Data, TArray<uint8>& Out,
	const FVaRestJsonConvertOptions& Options, EVaRestJsonFormat Format, FVaRestJsonError* OutError)
{
	VaRest::Json::Convert::EncodePrivate::FEncodeContext Context(Options);
	return VaRest::Json::Convert::EncodePrivate::EncodeToUtf8(Struct, Data, Out, Context, Format, OutError);
}

FString FVaRestJsonConvert::StructToString(const UScriptStruct* Struct, const void* Data, const FVaRestJsonConvertOptions& Options, EVaRestJsonFormat Format)
{
	using namespace VaRest::Json::Convert::EncodePrivate;
	FEncodeContext Context(Options);
	TArray<uint8> Bytes;
	if (!EncodeToUtf8(Struct, Data, Bytes, Context, Format, nullptr))
	{
		return FString();
	}
	if (!Context.Charge((int64(Bytes.Num()) + 1) * sizeof(TCHAR), 0, Bytes.Num()))
	{
		Finish(false, Context, Struct->GetName(), nullptr);
		return FString();
	}
	return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
}

FVaRestJson FVaRestJsonConvert::StructToJson(const UScriptStruct* Struct, const void* Data, const FVaRestJsonConvertOptions& Options)
{
	using namespace VaRest::Json;
	using namespace VaRest::Json::Convert;
	using namespace VaRest::Json::Convert::EncodePrivate;
	FEncodeContext Context(Options);
	if (!Struct || !Data)
	{
		Context.Fail(TEXT("no struct to encode"));
		Finish(false, Context, TEXT("a null struct"), nullptr);
		return FVaRestJson();
	}
	FVaRestJson Result = BuildDocument(Context, [&](auto& Builder) { return EncodeStruct(Builder, Struct, Data, Context); });
	return Finish(Result.IsValid(), Context, Struct->GetName(), nullptr) ? Result : FVaRestJson();
}

FVaRestJson FVaRestJsonConvert::PropertyToJson(const FProperty* Property, const void* Value, const FVaRestJsonConvertOptions& Options)
{
	using namespace VaRest::Json;
	using namespace VaRest::Json::Convert;
	using namespace VaRest::Json::Convert::EncodePrivate;
	FEncodeContext Context(Options);
	if (!Property || !Value)
	{
		Context.Fail(TEXT("no value to encode"));
		Finish(false, Context, TEXT("a null property"), nullptr);
		return FVaRestJson();
	}
	FString Why;
	if (!IsSupported(Property, Why))
	{
		Context.Fail(Why);
		Finish(false, Context, Property->GetName(), nullptr);
		return FVaRestJson();
	}
	FVaRestJson Result = BuildDocument(Context, [&](auto& Builder) { return EncodeField(Builder, Property, Value, Context); });
	return Finish(Result.IsValid(), Context, Property->GetName(), nullptr) ? Result : FVaRestJson();
}
