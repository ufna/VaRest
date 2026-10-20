// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonConvert.h"

#include "Misc/Base64.h"
#include "UObject/EnumProperty.h"
#include "UObject/PropertyOptional.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"
#include "VaRestJsonBuilder.h"
#include "VaRestJsonConvertPrivate.h"
#include "VaRestJsonLog.h"
#include "VaRestJsonSax.h"
#include "VaRestJsonUtf8.h"
#include "VaRestJsonWriterInternal.h"

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
		static constexpr double Pow10[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
			1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
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
		const char EmptyText[1] = { 0 };

		struct FEncodeContext
		{
			const FVaRestJsonConvertOptions& Options;
			/** Containers open around the value being written: the count the parser and EmitNode limit */
			int32 Depth = 0;
			/** First failure; any failure fails the whole encode (spec 2.2) */
			FString Error;
			int32 NonFinite = 0;
			FUtf8Scratch Scratch;

			explicit FEncodeContext(const FVaRestJsonConvertOptions& InOptions)
				: Options(InOptions)
			{
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

		template <typename SinkType>
		void WriteUtf8Text(SinkType& Sink, FUtf8StringView Text)
		{
			Sink.String(Text.Len() ? reinterpret_cast<const char*>(Text.GetData()) : EmptyText, static_cast<rapidjson::SizeType>(Text.Len()), true);
		}

		template <typename SinkType>
		void WriteText(SinkType& Sink, FStringView Text, FEncodeContext& Context)
		{
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
				Out = StrKey->GetPropertyValue(Key);
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
		bool IsOmitted(const FProperty* Property, const void* Value)
		{
			if (Property->ArrayDim != 1)
			{
				return false; // a C-style array member is always written; an invalid element is null
			}
			if (const FOptionalProperty* Optional = CastField<FOptionalProperty>(Property))
			{
				return !Optional->IsSet(Value);
			}
			const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
			return StructProperty && StructProperty->Struct == FVaRestJson::StaticStruct() && !static_cast<const FVaRestJson*>(Value)->IsValid();
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
				if (!EncodeValue(Sink, Property, static_cast<const uint8*>(Value) + static_cast<SIZE_T>(Property->GetElementSize()) * i, Context))
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
		bool EncodeMembers(SinkType& Sink, const FFieldMap& Fields, const void* Data, FEncodeContext& Context)
		{
			for (const FFieldInfo& Field : Fields.Fields)
			{
				const void* Value = Field.Property->ContainerPtrToValuePtr<void>(Data);
				if (IsOmitted(Field.Property, Value))
				{
					continue;
				}
				WriteKey(Sink, Field.NameView());
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
			const ESpecialStruct Special = ClassifyStruct(Struct);
			if (Special != ESpecialStruct::None)
			{
				return EncodeSpecial(Sink, Special, Data, Context);
			}
			if (!Context.Enter())
			{
				return false;
			}
			const FFieldMapRef Fields = GetFieldMap(Struct, Context.Options.Naming);
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
			if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
			{
				Sink.Bool(BoolProperty->GetPropertyValue(Value));
				return true;
			}
			if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
			{
				WriteEnum(Sink, EnumProperty->GetEnum(), EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(Value), Context);
				return true;
			}
			if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
			{
				WriteNumber(Sink, Numeric, Value, Context);
				return true;
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
				if (Context.Options.bBytesAsBase64 && IsPlainByteArray(ArrayProperty))
				{
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
				for (FScriptMapHelper::FIterator It(Helper); It; ++It)
				{
					if (!MapKeyText(MapProperty->KeyProp, Helper.GetKeyPtr(It), Context, KeyText))
					{
						return false;
					}
					WriteKey(Sink, ConvertForDocument(KeyText, Context.Scratch));
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
		FVaRestJson BuildDocument(BodyType&& Body)
		{
			FVaRestJson::FDocumentRef NewDoc = MakeShared<FDocument, ESPMode::ThreadSafe>();
			FDomBuilder Builder(*NewDoc, MaxDepth);
			if (!Body(Builder))
			{
				return FVaRestJson();
			}
			NewDoc->Root = Builder.Result;
			FNode* Root = NewDoc->Root;
			return FVaRestJson::MakeInternal(MoveTemp(NewDoc), Root);
		}

		template <typename WriterType>
		bool WriteMembers(WriterType& Writer, const UScriptStruct* Struct, const void* Data, const FVaRestJsonConvertOptions& Options,
			int32 OpenContainers, FVaRestJsonError* OutError)
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
			return Finish(EncodeMembers(Writer, *GetFieldMap(Struct, Options.Naming), Data, Context), Context, Struct->GetName(), OutError);
		}
	}
}

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
	return Finish(EncodeStruct(Writer, Struct, Data, Context), Context, Struct->GetName(), OutError);
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
	return Finish(EncodeField(Writer, Property, Value, Context), Context, Property->GetName(), OutError);
}

TArray<const FProperty*> VaRest::Json::GetConvertedFieldsInternal(const UScriptStruct* Struct, const FVaRestJsonConvertOptions& Options)
{
	using namespace Convert;
	TArray<const FProperty*> Result;
	if (!Struct || ClassifyStruct(Struct) != ESpecialStruct::None)
	{
		return Result;
	}
	// The very map EncodeMembers walks (and DecodeStruct matches against): one filter, in one place (NamesPrivate::Build)
	const FFieldMapRef Fields = GetFieldMap(Struct, Options.Naming);
	Result.Reserve(Fields->Fields.Num());
	for (const FFieldInfo& Field : Fields->Fields)
	{
		Result.Add(Field.Property);
	}
	return Result;
}

bool FVaRestJsonConvert::StructToUtf8(const UScriptStruct* Struct, const void* Data, TArray<uint8>& Out,
	const FVaRestJsonConvertOptions& Options, EVaRestJsonFormat Format, FVaRestJsonError* OutError)
{
	using namespace VaRest::Json;
	using namespace VaRest::Json::Convert;
	using namespace VaRest::Json::Convert::EncodePrivate;
	FEncodeContext Context(Options);
	if (!Struct || !Data)
	{
		Context.Fail(TEXT("no struct to encode"));
		return Finish(false, Context, TEXT("a null struct"), OutError);
	}
	const int32 Start = Out.Num();
	FUtf8Sink Sink{ Out };
	FWriterAllocator StackAllocator;
	bool bEncoded = false;
	if (Format == EVaRestJsonFormat::Pretty)
	{
		FPrettyWriter Writer(Sink, &StackAllocator);
		Writer.SetIndent(' ', 2);
		bEncoded = EncodeStruct(Writer, Struct, Data, Context);
	}
	else
	{
		FCondensedWriter Writer(Sink, &StackAllocator);
		bEncoded = EncodeStruct(Writer, Struct, Data, Context);
	}
	if (!bEncoded)
	{
		Out.SetNum(Start, EAllowShrinking::No);
	}
	return Finish(bEncoded, Context, Struct->GetName(), OutError);
}

FString FVaRestJsonConvert::StructToString(const UScriptStruct* Struct, const void* Data, const FVaRestJsonConvertOptions& Options, EVaRestJsonFormat Format)
{
	TArray<uint8> Bytes;
	if (!StructToUtf8(Struct, Data, Bytes, Options, Format))
	{
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
	FVaRestJson Result = BuildDocument([&](FDomBuilder& Builder) { return EncodeStruct(Builder, Struct, Data, Context); });
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
	FVaRestJson Result = BuildDocument([&](FDomBuilder& Builder) { return EncodeField(Builder, Property, Value, Context); });
	return Finish(Result.IsValid(), Context, Property->GetName(), nullptr) ? Result : FVaRestJson();
}
