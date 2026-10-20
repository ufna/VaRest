// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonConvert.h"

#include "Misc/Base64.h"
#include "UObject/EnumProperty.h"
#include "UObject/PropertyOptional.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"
#include "VaRestJsonConvertPrivate.h"
#include "VaRestJsonDocument.h"
#include "VaRestJsonLog.h"
#include "VaRestJsonWriter.h"

namespace VaRest::Json::Convert::DecodePrivate
{
	struct FSegment
	{
		/** Object key when Index < 0 */
		FUtf8StringView Key;
		int32 Index = -1;
	};

	const TCHAR* TypeName(const FNode& Value)
	{
		switch (Value.Type)
		{
		case EVaRestJsonType::Null:   return TEXT("null");
		case EVaRestJsonType::Bool:   return TEXT("a bool");
		case EVaRestJsonType::Number: return TEXT("a number");
		case EVaRestJsonType::String: return TEXT("a string");
		case EVaRestJsonType::Array:  return TEXT("an array");
		case EVaRestJsonType::Object: return TEXT("an object");
		case EVaRestJsonType::None:   break;
		}
		return TEXT("nothing");
	}

	FString TextOf(const FNode& Value)
	{
		return FString::ConstructFromPtrSize(Value.Str.Data, static_cast<int32>(Value.Str.Len));
	}

	/**
	 * Issues a report keeps per decode; later ones are only counted, and one closing entry says how many. Input can hold
	 * an issue every two bytes ([0,0,...] into strings), and each kept issue formats its path.
	 */
	inline constexpr int32 MaxRecordedIssues = 100;

	/**
	 * Whether Text may become an FName (or an object path, whose parts become FNames, SoftObjectPath.cpp:224-245).
	 * NAME_SIZE (NameTypes.h:57) characters or more abort FName construction in every build with checks: a checkf in
	 * FindOrStoreString (UnrealNames.cpp:3271-3281). Text from a response must never get there, so it is a mismatch.
	 */
	bool FitsName(const FString& Text)
	{
		return Text.Len() < NAME_SIZE;
	}

	struct FDecodeContext
	{
		const FVaRestJsonConvertOptions& Options;
		/** The document being read, handed to BytesResolver */
		const FVaRestJson::FDocumentRef& Source;
		FVaRestJsonDecodeReport* Report;
		/** Where the walk is; formatted only when an issue is recorded */
		TArray<FSegment, TInlineAllocator<16>> Path;
		int32 Depth = 0;
		/** Every issue, recorded or not */
		int32 NumWarnings = 0;
		int32 NumErrors = 0;
		/** Errors past MaxRecordedIssues: the closing entry goes to Errors when there are any */
		int32 NumUnrecordedErrors = 0;
		/** A failure that stops the whole decode (depth): nothing more is read */
		bool bAborted = false;
		/** Without a report: the issue the summary line names */
		FVaRestJsonError FirstIssue;
		FUtf8Scratch Scratch;

		FDecodeContext(const FVaRestJsonConvertOptions& InOptions, const FVaRestJson::FDocumentRef& InSource, FVaRestJsonDecodeReport* InReport)
			: Options(InOptions)
			, Source(InSource)
			, Report(InReport)
		{
		}

		/**
		 * "items[3].name", the syntax FVaRestJson::Path reads. A key longer than LongKeyBytes is cut there (at a character
		 * boundary) and marked "...", so a hostile key costs its length once, not once per issue beneath it.
		 */
		FString PathString() const
		{
			FString Out;
			for (const FSegment& Segment : Path)
			{
				if (Segment.Index >= 0)
				{
					Out += FString::Printf(TEXT("[%d]"), Segment.Index);
					continue;
				}
				if (!Out.IsEmpty())
				{
					Out.AppendChar(TEXT('.'));
				}
				if (Segment.Key.Len() <= LongKeyBytes)
				{
					Out += FString::ConstructFromPtrSize(Segment.Key.GetData(), Segment.Key.Len());
					continue;
				}
				int32 Cut = LongKeyBytes;
				while (Cut > 0 && (static_cast<uint8>(Segment.Key[Cut]) & 0xC0) == 0x80)
				{
					--Cut; // Segment.Key[Cut] continues a UTF-8 sequence: cut before the sequence starts
				}
				Out += FString::ConstructFromPtrSize(Segment.Key.GetData(), Cut);
				Out += TEXT("...");
			}
			return Out;
		}

		void Record(bool bError, FString Message)
		{
			++(bError ? NumErrors : NumWarnings);
			const int32 NumIssues = NumErrors + NumWarnings;
			if (Report ? NumIssues > MaxRecordedIssues : NumIssues > 1)
			{
				// Counted only, and the path is never formatted (Finish adds the closing entry)
				NumUnrecordedErrors += bError ? 1 : 0;
				return;
			}
			FVaRestJsonError Issue;
			Issue.Message = MoveTemp(Message);
			Issue.Path = PathString();
			if (Report)
			{
				(bError ? Report->Errors : Report->Warnings).Add(MoveTemp(Issue));
			}
			else
			{
				FirstIssue = MoveTemp(Issue);
			}
		}

		/** The value does not fit its field, which keeps its value. Spec 2.4: a warning; an error in strict mode. */
		bool Mismatch(const FNode& Value, const TCHAR* Expected)
		{
			Record(Options.bStrict, FString::Printf(TEXT("expected %s, got %s"), Expected, TypeName(Value)));
			return false;
		}

		/** A lenient coercion that worked ("42" -> 42): recorded like a mismatch (strict mode never coerces) */
		void Coerced(const TCHAR* What)
		{
			Record(Options.bStrict, FString(What));
		}

		/** A value nested past MaxDepth (spec 2.2): the whole decode fails, nothing more is read */
		bool TooDeep()
		{
			Record(true, FString::Printf(TEXT("value nested deeper than %d levels"), MaxDepth));
			bAborted = true;
			return false;
		}

		bool Enter()
		{
			if (Depth >= MaxDepth)
			{
				return TooDeep();
			}
			++Depth;
			return true;
		}

		void Leave()
		{
			--Depth;
		}
	};

	bool DecodeValue(const FNode& Value, const FProperty* Property, void* Address, FDecodeContext& Context);
	bool DecodeStruct(const FNode& Value, const UScriptStruct* Struct, void* Data, FDecodeContext& Context);

	/** The number Value holds; lenient decode also takes the number a string spells ("42"). Holder keeps that parse alive. */
	const FNode* NumberOf(const FNode& Value, const FDecodeContext& Context, FVaRestJson& Holder)
	{
		if (Value.Type == EVaRestJsonType::Number)
		{
			return &Value;
		}
		if (Value.Type != EVaRestJsonType::String || Context.Options.bStrict)
		{
			return nullptr;
		}
		if (Value.Str.Len > 64)
		{
			return nullptr; // no number worth coercing is that long; bounds the parse on hostile input
		}
		TValueOrError<FVaRestJson, FVaRestJsonError> Parsed = FVaRestJson::Parse(Value.Str.View());
		if (!Parsed.HasValue() || !Parsed.GetValue().IsNumber())
		{
			return nullptr;
		}
		Holder = Parsed.StealValue();
		return Holder.GetNodeInternal();
	}

	bool DecodeBool(const FNode& Value, const FBoolProperty* BoolProperty, void* Address, FDecodeContext& Context)
	{
		if (Value.Type == EVaRestJsonType::Bool)
		{
			BoolProperty->SetPropertyValue(Address, Value.Bool);
			return true;
		}
		if (!Context.Options.bStrict)
		{
			int64 Number = 0;
			if (NodeToInt64(Value, Number) && (Number == 0 || Number == 1))
			{
				BoolProperty->SetPropertyValue(Address, Number == 1);
				Context.Coerced(TEXT("a bool was read from 0/1"));
				return true;
			}
			if (Value.Type == EVaRestJsonType::String)
			{
				const FUtf8StringView Text = Value.Str.View();
				const bool bTrue = Text.Equals(UTF8TEXT("true"), ESearchCase::CaseSensitive);
				if (bTrue || Text.Equals(UTF8TEXT("false"), ESearchCase::CaseSensitive))
				{
					BoolProperty->SetPropertyValue(Address, bTrue);
					Context.Coerced(TEXT("a bool was read from a string"));
					return true;
				}
			}
		}
		return Context.Mismatch(Value, TEXT("a bool"));
	}

	bool DecodeInteger(const FNode& Value, const FNumericProperty* Numeric, void* Address, FDecodeContext& Context)
	{
		FVaRestJson Holder;
		const FNode* Number = NumberOf(Value, Context, Holder);
		bool bStored = false;
		if (Number && IsUnsignedInteger(Numeric))
		{
			uint64 Unsigned = 0;
			if (NodeToUInt64(*Number, Unsigned) && Numeric->CanHoldValue(Unsigned))
			{
				Numeric->SetIntPropertyValue(Address, Unsigned);
				bStored = true;
			}
		}
		else if (Number)
		{
			int64 Signed = 0;
			if (NodeToInt64(*Number, Signed) && Numeric->CanHoldValue(Signed))
			{
				Numeric->SetIntPropertyValue(Address, Signed);
				bStored = true;
			}
		}
		if (!bStored)
		{
			return Context.Mismatch(Value, TEXT("an integer in the field's range"));
		}
		if (Number != &Value)
		{
			Context.Coerced(TEXT("an integer was read from a string"));
		}
		return true;
	}

	bool DecodeFloatingPoint(const FNode& Value, const FNumericProperty* Numeric, void* Address, FDecodeContext& Context)
	{
		FVaRestJson Holder;
		const FNode* Number = NumberOf(Value, Context, Holder);
		double D = 0.0;
		if (!Number || !NodeToDouble(*Number, D))
		{
			return Context.Mismatch(Value, TEXT("a number"));
		}
		if (Numeric->IsA<FFloatProperty>() && FMath::Abs(D) >= FloatRoundsToInfinity)
		{
			return Context.Mismatch(Value, TEXT("a number in float range"));
		}
		Numeric->SetFloatingPointPropertyValue(Address, D);
		if (Number != &Value)
		{
			Context.Coerced(TEXT("a number was read from a string"));
		}
		return true;
	}

	/** Text for a string/name/text field: a string; lenient decode also takes a number's or bool's JSON text */
	bool TextFor(const FNode& Value, FDecodeContext& Context, FString& Out)
	{
		if (Value.Type == EVaRestJsonType::String)
		{
			Out = TextOf(Value);
			return true;
		}
		if (!Context.Options.bStrict && (Value.Type == EVaRestJsonType::Number || Value.Type == EVaRestJsonType::Bool))
		{
			TArray<uint8> Bytes;
			Write(Value, EVaRestJsonFormat::Condensed, Bytes); // a scalar: never too deep
			Out = FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
			Context.Coerced(TEXT("a string was read from a number or bool"));
			return true;
		}
		return Context.Mismatch(Value, TEXT("a string"));
	}

	/** By authored name (as encode writes it), exact first; the autogenerated _MAX entry is not a value anyone sends */
	bool FindEnumerator(const UEnum* Enum, const FString& Name, bool bAllowCase, int64& OutValue, bool& bOutCaseOnly)
	{
		const int32 Count = Enum->NumEnums() - (Enum->ContainsExistingMax() ? 1 : 0);
		for (int32 i = 0; i < Count; ++i)
		{
			if (Enum->GetAuthoredNameStringByIndex(i).Equals(Name, ESearchCase::CaseSensitive))
			{
				OutValue = Enum->GetValueByIndex(i);
				bOutCaseOnly = false;
				return true;
			}
		}
		if (bAllowCase)
		{
			for (int32 i = 0; i < Count; ++i)
			{
				if (Enum->GetAuthoredNameStringByIndex(i).Equals(Name, ESearchCase::IgnoreCase))
				{
					OutValue = Enum->GetValueByIndex(i);
					bOutCaseOnly = true;
					return true;
				}
			}
		}
		return false;
	}

	bool DecodeEnum(const FNode& Value, const UEnum* Enum, const FNumericProperty* Underlying, void* Address, FDecodeContext& Context)
	{
		int64 EnumValue = 0;
		if (Value.Type == EVaRestJsonType::String)
		{
			bool bCaseOnly = false;
			if (!FindEnumerator(Enum, TextOf(Value), !Context.Options.bStrict, EnumValue, bCaseOnly))
			{
				return Context.Mismatch(Value, *FString::Printf(TEXT("an enumerator of %s"), *Enum->GetName()));
			}
			Underlying->SetIntPropertyValue(Address, EnumValue);
			if (bCaseOnly)
			{
				Context.Coerced(TEXT("an enumerator name was matched ignoring case"));
			}
			return true;
		}
		if (NodeToInt64(Value, EnumValue) && Enum->IsValidEnumValue(EnumValue))
		{
			Underlying->SetIntPropertyValue(Address, EnumValue);
			return true;
		}
		return Context.Mismatch(Value, *FString::Printf(TEXT("an enumerator of %s"), *Enum->GetName()));
	}

	/**
	 * The whole key is a JSON integer: optional '-', digits, no leading zero, within 64 bits. Not a JSON parse: that
	 * would allocate a document per key and accept " 5", "5.0" and "1e0", collapsing them silently with "5".
	 */
	bool ParseIntegerKey(FUtf8StringView Text, bool& bNegative, uint64& Magnitude)
	{
		bNegative = Text.Len() > 0 && Text[0] == UTF8CHAR('-');
		int32 i = bNegative ? 1 : 0;
		if (i >= Text.Len() || (Text[i] == UTF8CHAR('0') && Text.Len() > i + 1))
		{
			return false;
		}
		Magnitude = 0;
		for (; i < Text.Len(); ++i)
		{
			const uint32 C = static_cast<uint8>(Text[i]);
			if (C < '0' || C > '9')
			{
				return false;
			}
			const uint64 Digit = C - '0';
			if (Magnitude > (MAX_uint64 - Digit) / 10)
			{
				return false;
			}
			Magnitude = Magnitude * 10 + Digit;
		}
		return !bNegative || Magnitude <= 9223372036854775808ull;
	}

	/** A JSON object key into a map key: string, name, integer (the whole key a JSON integer), enumerator name or value */
	bool DecodeMapKey(FUtf8StringView Text, const FProperty* KeyProperty, void* Key, FDecodeContext& Context)
	{
		const FString KeyText = FString::ConstructFromPtrSize(Text.GetData(), Text.Len());
		if (const FStrProperty* StrKey = CastField<FStrProperty>(KeyProperty))
		{
			StrKey->SetPropertyValue(Key, KeyText);
			return true;
		}
		if (const FNameProperty* NameKey = CastField<FNameProperty>(KeyProperty))
		{
			if (!FitsName(KeyText))
			{
				Context.Record(Context.Options.bStrict, FString::Printf(TEXT("map key of %d characters is too long for a name"), KeyText.Len()));
				return false;
			}
			NameKey->SetPropertyValue(Key, FName(*KeyText));
			return true;
		}
		const FNumericProperty* Underlying = nullptr;
		const UEnum* Enum = nullptr;
		if (const FEnumProperty* EnumKey = CastField<FEnumProperty>(KeyProperty))
		{
			Underlying = EnumKey->GetUnderlyingProperty();
			Enum = EnumKey->GetEnum();
		}
		else if (const FNumericProperty* NumericKey = CastField<FNumericProperty>(KeyProperty))
		{
			Underlying = NumericKey;
			Enum = NumericKey->GetIntPropertyEnum();
		}
		if (Underlying)
		{
			int64 EnumValue = 0;
			bool bCaseOnly = false;
			if (Enum && FindEnumerator(Enum, KeyText, !Context.Options.bStrict, EnumValue, bCaseOnly))
			{
				Underlying->SetIntPropertyValue(Key, EnumValue);
				if (bCaseOnly)
				{
					Context.Coerced(TEXT("an enumerator name was matched ignoring case"));
				}
				return true;
			}
			bool bNegative = false;
			uint64 Magnitude = 0;
			if (ParseIntegerKey(Text, bNegative, Magnitude)) // "5", "-5" yes; "05", "5.0", "1e0", " 5", "5x" no
			{
				if (!Enum && IsUnsignedInteger(Underlying))
				{
					// Only through CanHoldValue(uint64): CanHoldValue(int64) on a uint64 property accepts -1
					// ((int64)(uint64)-1 == -1, UnrealType.h:2172-2175), which would store 18446744073709551615 and merge
					// the pair with that key's
					if (!bNegative && Underlying->CanHoldValue(Magnitude))
					{
						Underlying->SetIntPropertyValue(Key, Magnitude);
						return true;
					}
				}
				else if (bNegative || Magnitude <= static_cast<uint64>(MAX_int64))
				{
					const int64 Signed = !bNegative ? static_cast<int64>(Magnitude)
						: Magnitude == 9223372036854775808ull ? MIN_int64 : -static_cast<int64>(Magnitude);
					if (Enum ? Enum->IsValidEnumValue(Signed) : Underlying->CanHoldValue(Signed))
					{
						Underlying->SetIntPropertyValue(Key, Signed);
						return true;
					}
				}
			}
		}
		// The key itself is in the path (shortened there when long), not repeated here
		Context.Record(Context.Options.bStrict, FString::Printf(TEXT("map key does not fit %s"), *KeyProperty->GetClass()->GetName()));
		return false;
	}

	void AssignBytes(FScriptArrayHelper& Helper, const TArray<uint8>& Bytes)
	{
		Helper.EmptyValues(Bytes.Num());
		Helper.AddValues(Bytes.Num());
		if (Bytes.Num() > 0)
		{
			FMemory::Memcpy(Helper.GetRawPtr(0), Bytes.GetData(), Bytes.Num());
		}
	}

	bool DecodeArray(const FNode& Value, const FArrayProperty* ArrayProperty, void* Address, FDecodeContext& Context)
	{
		FScriptArrayHelper Helper(ArrayProperty, Address);
		const bool bBytes = IsPlainByteArray(ArrayProperty);
		if (bBytes && Context.Options.BytesResolver)
		{
			// WebSocket spec §19 item 10: the caller decides first (Socket.IO attachments); declining keeps the built-in forms
			TArray<uint8> Resolved;
			if (Context.Options.BytesResolver(FVaRestJson::MakeInternal(Context.Source, const_cast<FNode*>(&Value)), Context.PathString(), Resolved))
			{
				AssignBytes(Helper, Resolved);
				return true;
			}
		}
		if (Value.Type == EVaRestJsonType::Null)
		{
			Helper.EmptyValues(); // spec 2.4: null clears a container
			return true;
		}
		if (Value.Type == EVaRestJsonType::String && bBytes)
		{
			TArray<uint8> Decoded;
			if (!FBase64::Decode(TextOf(Value), Decoded))
			{
				return Context.Mismatch(Value, TEXT("base64 text or an array of bytes"));
			}
			AssignBytes(Helper, Decoded);
			return true;
		}
		if (Value.Type != EVaRestJsonType::Array)
		{
			return Context.Mismatch(Value, TEXT("an array"));
		}
		if (!Context.Enter())
		{
			return false;
		}
		// Replaced, not appended: every element starts from the inner type's defaults
		const int32 Count = static_cast<int32>(Value.Arr.Num);
		Helper.EmptyValues(Count);
		Helper.AddValues(Count);
		for (int32 i = 0; i < Count && !Context.bAborted; ++i)
		{
			Context.Path.Add(FSegment{ FUtf8StringView(), i });
			DecodeValue(*Value.Arr.Items[i], ArrayProperty->Inner, Helper.GetRawPtr(i), Context);
			Context.Path.Pop(EAllowShrinking::No);
		}
		Context.Leave();
		return !Context.bAborted;
	}

	bool DecodeSet(const FNode& Value, const FSetProperty* SetProperty, void* Address, FDecodeContext& Context)
	{
		FScriptSetHelper Helper(SetProperty, Address);
		if (Value.Type == EVaRestJsonType::Null)
		{
			Helper.EmptyElements();
			return true;
		}
		if (Value.Type != EVaRestJsonType::Array)
		{
			return Context.Mismatch(Value, TEXT("an array"));
		}
		if (!Context.Enter())
		{
			return false;
		}
		const FProperty* ElementProperty = SetProperty->ElementProp;
		Helper.EmptyElements(static_cast<int32>(Value.Arr.Num));
		void* Element = ElementProperty->AllocateAndInitializeValue();
		for (uint32 i = 0; i < Value.Arr.Num && !Context.bAborted; ++i)
		{
			ElementProperty->DestroyValue(Element);
			ElementProperty->InitializeValue(Element);
			Context.Path.Add(FSegment{ FUtf8StringView(), static_cast<int32>(i) });
			if (DecodeValue(*Value.Arr.Items[i], ElementProperty, Element, Context))
			{
				Helper.AddElement(Element); // hashes and compares: duplicates collapse (UnrealType.h:6208-6236)
			}
			Context.Path.Pop(EAllowShrinking::No);
		}
		ElementProperty->DestroyAndFreeValue(Element);
		Context.Leave();
		return !Context.bAborted;
	}

	bool DecodeMap(const FNode& Value, const FMapProperty* MapProperty, void* Address, FDecodeContext& Context)
	{
		FScriptMapHelper Helper(MapProperty, Address);
		if (Value.Type == EVaRestJsonType::Null)
		{
			Helper.EmptyValues();
			return true;
		}
		if (Value.Type != EVaRestJsonType::Object)
		{
			return Context.Mismatch(Value, TEXT("an object"));
		}
		if (!Context.Enter())
		{
			return false;
		}
		const FProperty* KeyProperty = MapProperty->KeyProp;
		const FProperty* ValueProperty = MapProperty->ValueProp;
		Helper.EmptyValues(static_cast<int32>(Value.Obj.Num));
		void* Key = KeyProperty->AllocateAndInitializeValue();
		void* Item = ValueProperty->AllocateAndInitializeValue();
		for (uint32 m = 0; m < Value.Obj.Num && !Context.bAborted; ++m)
		{
			const FMember& M = Value.Obj.Members[m];
			KeyProperty->DestroyValue(Key);
			KeyProperty->InitializeValue(Key);
			ValueProperty->DestroyValue(Item);
			ValueProperty->InitializeValue(Item);
			Context.Path.Add(FSegment{ M.Key.View(), -1 });
			if (DecodeMapKey(M.Key.View(), KeyProperty, Key, Context) && DecodeValue(*M.Value, ValueProperty, Item, Context))
			{
				// AddPair overwrites an equal key (UnrealType.h:5402-5440): case-variant FString keys collapse, the last value wins
				Helper.AddPair(Key, Item);
			}
			Context.Path.Pop(EAllowShrinking::No);
		}
		KeyProperty->DestroyAndFreeValue(Key);
		ValueProperty->DestroyAndFreeValue(Item);
		Context.Leave();
		return !Context.bAborted;
	}

	bool DecodeSpecial(const FNode& Value, ESpecialStruct Special, void* Data, FDecodeContext& Context)
	{
		if (Special == ESpecialStruct::Json)
		{
			// Its own copy: a decoded struct never shares a document with the response it came from (spec 2.2 thread
			// contract). The containers open around the field count, as encode counts them (EmitJson): a value that nests
			// the struct past MaxDepth fails the decode, not the struct's next encode. A parsed document never gets there
			// (the parser has the same limit); one built through Set/Add can.
			FVaRestJson::FDocumentRef Copy = MakeShared<FDocument, ESPMode::ThreadSafe>();
			FNode* Root = Copy->DeepCopy(Value, Context.Depth);
			if (!Root)
			{
				return Context.TooDeep();
			}
			Copy->Root = Root;
			*static_cast<FVaRestJson*>(Data) = FVaRestJson::MakeInternal(MoveTemp(Copy), Root);
			return true;
		}
		if (Value.Type == EVaRestJsonType::Null)
		{
			return true; // spec 2.4: null keeps a struct's value
		}
		switch (Special)
		{
		case ESpecialStruct::DateTime:
		{
			FDateTime Parsed;
			if (Value.Type == EVaRestJsonType::String && FDateTime::ParseIso8601(*TextOf(Value), Parsed))
			{
				*static_cast<FDateTime*>(Data) = Parsed;
				return true;
			}
			return Context.Mismatch(Value, TEXT("an ISO 8601 date-time"));
		}
		case ESpecialStruct::Timespan:
		{
			FVaRestJson Holder;
			const FNode* Number = NumberOf(Value, Context, Holder);
			double Seconds = 0.0;
			if (Number && NodeToDouble(*Number, Seconds) && Seconds >= FTimespan::MinValue().GetTotalSeconds() && Seconds <= FTimespan::MaxValue().GetTotalSeconds())
			{
				// FromSeconds's own arithmetic, but not its unchecked cast to int64 ticks (Timespan.h:673-676): at the top of
				// the range the seconds round to 2^63 ticks, one past int64 (MaxValue's own seconds, which encode writes, do).
				// That one rounding step is MaxValue; the range check keeps every other value inside int64.
				const double Ticks = FMath::FloorToDouble(Seconds * ETimespan::TicksPerSecond + 0.5);
				*static_cast<FTimespan*>(Data) = Ticks < 9223372036854775808.0 ? FTimespan(static_cast<int64>(Ticks)) : FTimespan::MaxValue();
				if (Number != &Value)
				{
					Context.Coerced(TEXT("seconds were read from a string"));
				}
				return true;
			}
			return Context.Mismatch(Value, TEXT("a number of seconds"));
		}
		case ESpecialStruct::Guid:
		{
			FGuid Parsed;
			if (Value.Type == EVaRestJsonType::String && FGuid::Parse(TextOf(Value), Parsed))
			{
				*static_cast<FGuid*>(Data) = Parsed;
				return true;
			}
			return Context.Mismatch(Value, TEXT("a GUID"));
		}
		case ESpecialStruct::SoftObjectPath:
			if (Value.Type == EVaRestJsonType::String)
			{
				const FString Text = TextOf(Value);
				if (!FitsName(Text))
				{
					return Context.Mismatch(Value, TEXT("an object path shorter than 1024 characters"));
				}
				static_cast<FSoftObjectPath*>(Data)->SetPath(FStringView(Text));
				return true;
			}
			return Context.Mismatch(Value, TEXT("an object path"));
		case ESpecialStruct::None:
		case ESpecialStruct::Json:
			break;
		}
		return true;
	}

	/** A field value, including C-style array members (a JSON array of up to ArrayDim elements) */
	bool DecodeField(const FNode& Value, const FProperty* Property, void* Address, FDecodeContext& Context)
	{
		if (Property->ArrayDim == 1)
		{
			return DecodeValue(Value, Property, Address, Context);
		}
		if (Value.Type != EVaRestJsonType::Array)
		{
			return Context.Mismatch(Value, TEXT("an array"));
		}
		if (!Context.Enter())
		{
			return false;
		}
		const int32 Count = FMath::Min<int32>(static_cast<int32>(Value.Arr.Num), Property->ArrayDim);
		if (static_cast<int32>(Value.Arr.Num) > Property->ArrayDim)
		{
			Context.Record(Context.Options.bStrict, FString::Printf(TEXT("%u elements for a fixed array of %d; the rest are ignored"), Value.Arr.Num, Property->ArrayDim));
		}
		for (int32 i = 0; i < Count && !Context.bAborted; ++i)
		{
			Context.Path.Add(FSegment{ FUtf8StringView(), i });
			DecodeValue(*Value.Arr.Items[i], Property, static_cast<uint8*>(Address) + static_cast<SIZE_T>(Property->GetElementSize()) * i, Context);
			Context.Path.Pop(EAllowShrinking::No);
		}
		Context.Leave();
		return !Context.bAborted;
	}

	bool DecodeStruct(const FNode& Value, const UScriptStruct* Struct, void* Data, FDecodeContext& Context)
	{
		const ESpecialStruct Special = ClassifyStruct(Struct);
		if (Special != ESpecialStruct::None)
		{
			return DecodeSpecial(Value, Special, Data, Context);
		}
		if (Value.Type == EVaRestJsonType::Null)
		{
			return true; // spec 2.4: null keeps a struct's values
		}
		if (Value.Type != EVaRestJsonType::Object)
		{
			return Context.Mismatch(Value, TEXT("an object"));
		}
		if (!Context.Enter())
		{
			return false;
		}
		const FFieldMapRef Fields = GetFieldMap(Struct, Context.Options.Naming);
		const int32 FieldCount = Fields->Fields.Num();
		// Per field, the member that feeds it: the best tier wins (exact beats case-insensitive beats normalised, in any
		// member order); within a tier the last occurrence wins, as lookups do (spec 2.1).
		TArray<int32, TInlineAllocator<32>> Chosen;
		TArray<uint8, TInlineAllocator<32>> ChosenTier;
		Chosen.Init(INDEX_NONE, FieldCount);
		ChosenTier.Init(MAX_uint8, FieldCount);
		const FNode::FObj& Obj = Value.Obj;
		for (uint32 m = 0; m < Obj.Num; ++m)
		{
			const FMember& M = Obj.Members[m];
			uint8 Tier = 0;
			const int32 Field = Fields->Match(M.Key.View(), M.Key.Hash, Context.Scratch, Tier);
			if (Field != INDEX_NONE && Tier <= ChosenTier[Field])
			{
				Chosen[Field] = static_cast<int32>(m);
				ChosenTier[Field] = Tier;
			}
		}
		for (int32 f = 0; f < FieldCount && !Context.bAborted; ++f)
		{
			if (Chosen[f] == INDEX_NONE)
			{
				continue; // spec 2.4: an absent field keeps its value
			}
			const FMember& M = Obj.Members[Chosen[f]];
			const FProperty* Property = Fields->Fields[f].Property;
			Context.Path.Add(FSegment{ M.Key.View(), -1 });
			DecodeField(*M.Value, Property, Property->ContainerPtrToValuePtr<void>(Data), Context);
			Context.Path.Pop(EAllowShrinking::No);
		}
		Context.Leave();
		return !Context.bAborted;
	}

	bool DecodeValue(const FNode& Value, const FProperty* Property, void* Address, FDecodeContext& Context)
	{
		if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			return DecodeBool(Value, BoolProperty, Address, Context);
		}
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			return DecodeEnum(Value, EnumProperty->GetEnum(), EnumProperty->GetUnderlyingProperty(), Address, Context);
		}
		if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
		{
			if (const UEnum* Enum = Numeric->GetIntPropertyEnum())
			{
				return DecodeEnum(Value, Enum, Numeric, Address, Context);
			}
			return Numeric->IsFloatingPoint() ? DecodeFloatingPoint(Value, Numeric, Address, Context) : DecodeInteger(Value, Numeric, Address, Context);
		}
		if (const FStrProperty* StrProperty = CastField<FStrProperty>(Property))
		{
			FString Text;
			if (!TextFor(Value, Context, Text))
			{
				return false;
			}
			StrProperty->SetPropertyValue(Address, Text);
			return true;
		}
		if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			FString Text;
			if (!TextFor(Value, Context, Text))
			{
				return false;
			}
			if (!FitsName(Text))
			{
				return Context.Mismatch(Value, TEXT("a name shorter than 1024 characters"));
			}
			NameProperty->SetPropertyValue(Address, FName(*Text));
			return true;
		}
		if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
		{
			FString Text;
			if (!TextFor(Value, Context, Text))
			{
				return false;
			}
			TextProperty->SetPropertyValue(Address, FText::AsCultureInvariant(MoveTemp(Text))); // spec 2.4: culture-invariant
			return true;
		}
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			return DecodeStruct(Value, StructProperty->Struct, Address, Context);
		}
		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			return DecodeArray(Value, ArrayProperty, Address, Context);
		}
		if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
		{
			return DecodeSet(Value, SetProperty, Address, Context);
		}
		if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		{
			return DecodeMap(Value, MapProperty, Address, Context);
		}
		if (const FOptionalProperty* Optional = CastField<FOptionalProperty>(Property))
		{
			if (Value.Type == EVaRestJsonType::Null)
			{
				Optional->MarkUnset(Address); // spec 2.4: null <-> unset
				return true;
			}
			const bool bWasSet = Optional->IsSet(Address);
			// The existing value when set (PropertyOptional.h:35-56), a fresh one otherwise
			void* Inner = Optional->MarkSetAndGetInitializedValuePointerToReplace(Address);
			if (!DecodeValue(Value, Optional->GetValueProperty(), Inner, Context))
			{
				// A mismatch keeps the field as it was (spec 2.4): unset stays unset, a set value is left untouched
				if (!bWasSet)
				{
					Optional->MarkUnset(Address);
				}
				return false;
			}
			return true;
		}
		if (const FSoftObjectProperty* SoftProperty = CastField<FSoftObjectProperty>(Property))
		{
			if (Value.Type == EVaRestJsonType::Null)
			{
				SoftProperty->SetPropertyValue(Address, FSoftObjectPtr());
				return true;
			}
			if (Value.Type == EVaRestJsonType::String)
			{
				const FString Text = TextOf(Value);
				if (!FitsName(Text))
				{
					return Context.Mismatch(Value, TEXT("an object path shorter than 1024 characters"));
				}
				SoftProperty->SetPropertyValue(Address, FSoftObjectPtr(FSoftObjectPath(Text)));
				return true;
			}
			return Context.Mismatch(Value, TEXT("an object path"));
		}
		Context.Record(true, FString::Printf(TEXT("%s properties are not converted"), *Property->GetClass()->GetName()));
		return false;
	}

	bool Refuse(FVaRestJsonDecodeReport* Report, const FString& Message)
	{
		FVaRestJsonError Error;
		Error.Message = Message;
		if (Report)
		{
			Report->Errors.Add(MoveTemp(Error));
		}
		else
		{
			UE_LOG(LogVaRestJson, Warning, TEXT("FVaRestJsonConvert: %s"), *Message);
		}
		return false;
	}

	bool Finish(const FDecodeContext& Context, bool bStored, const FString& What)
	{
		const int32 NumIssues = Context.NumWarnings + Context.NumErrors;
		if (Context.Report && NumIssues > MaxRecordedIssues)
		{
			FVaRestJsonError More;
			More.Message = FString::Printf(TEXT("%d more issue(s) not recorded"), NumIssues - MaxRecordedIssues);
			(Context.NumUnrecordedErrors > 0 ? Context.Report->Errors : Context.Report->Warnings).Add(MoveTemp(More));
		}
		if (!Context.Report && NumIssues > 0)
		{
			// One line per decode, not per issue: a big response with a systematic mismatch must not flood the log
			UE_LOG(LogVaRestJson, Warning, TEXT("FVaRestJsonConvert: decoding %s: %d warning(s), %d error(s); first at '%s': %s"),
				*What, Context.NumWarnings, Context.NumErrors, *Context.FirstIssue.Path, *Context.FirstIssue.Message);
		}
		return bStored && !Context.bAborted && Context.NumErrors == 0;
	}
}

bool FVaRestJsonConvert::JsonToStruct(const FVaRestJson& Json, const UScriptStruct* Struct, void* Data, const FVaRestJsonConvertOptions& Options, FVaRestJsonDecodeReport* OutReport)
{
	using namespace VaRest::Json::Convert::DecodePrivate;
	if (!Struct || !Data)
	{
		return Refuse(OutReport, TEXT("no struct to decode into"));
	}
	if (!Json.IsValid())
	{
		return Refuse(OutReport, FString::Printf(TEXT("decoding %s: the value is missing"), *Struct->GetName()));
	}
	FDecodeContext Context(Options, Json.GetDocumentInternal(), OutReport);
	const bool bStored = DecodeStruct(*Json.GetNodeInternal(), Struct, Data, Context);
	return Finish(Context, bStored, Struct->GetName());
}

bool FVaRestJsonConvert::Utf8ToStruct(TConstArrayView<uint8> Utf8, const UScriptStruct* Struct, void* Data, const FVaRestJsonConvertOptions& Options, FVaRestJsonDecodeReport* OutReport)
{
	using namespace VaRest::Json::Convert::DecodePrivate;
	TValueOrError<FVaRestJson, FVaRestJsonError> Parsed = FVaRestJson::Parse(Utf8);
	if (!Parsed.HasValue())
	{
		if (OutReport)
		{
			OutReport->Errors.Add(Parsed.GetError());
			return false;
		}
		return Refuse(nullptr, FString::Printf(TEXT("cannot decode %s: %s"), Struct ? *Struct->GetName() : TEXT("a null struct"), *Parsed.GetError().ToString()));
	}
	return JsonToStruct(Parsed.GetValue(), Struct, Data, Options, OutReport);
}

bool FVaRestJsonConvert::JsonToProperty(const FVaRestJson& Json, const FProperty* Property, void* Value, const FVaRestJsonConvertOptions& Options, FVaRestJsonDecodeReport* OutReport)
{
	using namespace VaRest::Json::Convert::DecodePrivate;
	if (!Property || !Value)
	{
		return Refuse(OutReport, TEXT("no property to decode into"));
	}
	if (!Json.IsValid())
	{
		return Refuse(OutReport, FString::Printf(TEXT("decoding %s: the value is missing"), *Property->GetName()));
	}
	FString Why;
	if (!VaRest::Json::Convert::IsSupported(Property, Why))
	{
		// Refused as PropertyToJson refuses it. A struct's field map never holds such a property; walking one would write
		// what it could before failing ([] empties an array of object references).
		return Refuse(OutReport, FString::Printf(TEXT("cannot decode %s: %s"), *Property->GetName(), *Why));
	}
	FDecodeContext Context(Options, Json.GetDocumentInternal(), OutReport);
	const bool bStored = DecodeField(*Json.GetNodeInternal(), Property, Value, Context);
	return Finish(Context, bStored, Property->GetName());
}

bool VaRest::Json::DecodeStructForTryGet(const FVaRestJson& Json, const UScriptStruct* Struct, void* Data)
{
	// TryGet asks "is this a T". A missing value or null is not one, not even for a struct with a scalar form, whose
	// field decode keeps the value on null (spec 2.4) and would hand back a default FDateTime/FGuid as if one was sent.
	if (!Json.IsValid() || Json.IsNull())
	{
		return false;
	}
	// Anything but an object is not an ordinary struct; a value struct (FGuid, FDateTime...) takes its scalar form
	if (Convert::ClassifyStruct(Struct) == Convert::ESpecialStruct::None && !Json.IsObject())
	{
		return false;
	}
	FVaRestJsonDecodeReport Report; // a report keeps TryGet quiet; lenient warnings are not failures
	return FVaRestJsonConvert::JsonToStruct(Json, Struct, Data, FVaRestJsonConvertOptions(), &Report);
}
