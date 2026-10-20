// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonConvertPrivate.h"

#include "Misc/ScopeRWLock.h"
#include "Misc/StringBuilder.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/EnumProperty.h"
#include "UObject/ObjectKey.h"
#include "UObject/PropertyOptional.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/TextProperty.h"
#include "VaRestJsonLog.h"

namespace VaRest::Json::Convert
{
	namespace NamesPrivate
	{
		/**
		 * The process-wide field-map cache and rename registry. Keys are FObjectKey (index + serial number,
		 * ObjectKey.h:29-38), so a struct that dies and a new one at the same address never share a map. Reads take the
		 * read lock, so concurrent decodes on worker threads share maps; maps are immutable once inserted. Making the key
		 * off the game thread is safe: a first serial number is claimed by compare-exchange (UObjectArray.cpp:549).
		 */
		struct FCache
		{
			FRWLock Lock;
			/**
			 * Bumped by ResetCache, Register and Unregister. Build runs outside the lock, so a map built from the
			 * renames (or struct) of before a reset could otherwise be inserted after it and outlive it.
			 */
			uint64 Epoch = 0;
			TMap<TPair<FObjectKey, uint8>, FFieldMapRef> Maps;
			TMap<FObjectKey, TMap<FName, FString>> Renames;
		};

		FCache& Cache()
		{
			static FCache Instance; // a function-local static: its first use is thread-safe
			return Instance;
		}

		bool IsSeparator(TCHAR C)
		{
			return C == TEXT('_') || C == TEXT('-') || C == TEXT(' ');
		}

		/** UE names bools bSomething (coding standard); the b is a C++ convention, not part of the JSON name */
		bool HasBoolPrefix(FStringView Name)
		{
			return Name.Len() >= 2 && Name[0] == TEXT('b') && FChar::IsUpper(Name[1]);
		}

		/** "URLPath" -> URL|Path, "UserId" -> User|Id, "user_id" -> user|id, "HTTPStatus2" -> HTTP|Status2, "Vector3D" -> Vector3|D */
		TArray<FString> SplitWords(const FString& Name)
		{
			TArray<FString> Words;
			FString Current;
			for (int32 i = 0; i < Name.Len(); ++i)
			{
				const TCHAR C = Name[i];
				if (IsSeparator(C))
				{
					if (!Current.IsEmpty())
					{
						Words.Add(Current);
						Current.Reset();
					}
					continue;
				}
				if (!Current.IsEmpty())
				{
					const TCHAR Prev = Current[Current.Len() - 1];
					const bool bLowerToUpper = (FChar::IsLower(Prev) || FChar::IsDigit(Prev)) && FChar::IsUpper(C);
					const bool bAcronymEnds = FChar::IsUpper(Prev) && FChar::IsUpper(C) && i + 1 < Name.Len() && FChar::IsLower(Name[i + 1]);
					if (bLowerToUpper || bAcronymEnds)
					{
						Words.Add(Current);
						Current.Reset();
					}
				}
				Current.AppendChar(C);
			}
			if (!Current.IsEmpty())
			{
				Words.Add(Current);
			}
			return Words;
		}

		FString Capitalized(const FString& Word)
		{
			FString Result = Word.ToLower();
			if (Result.Len() > 0)
			{
				Result[0] = FChar::ToUpper(Result[0]);
			}
			return Result;
		}

		FUtf8StringView View(const TArray<UTF8CHAR>& Bytes)
		{
			return FUtf8StringView(Bytes.GetData(), Bytes.Num());
		}

		void AssignUtf8(TArray<UTF8CHAR>& Out, const FString& Text)
		{
			FUtf8Scratch Scratch;
			const FUtf8StringView Utf8 = ConvertForDocument(Text, Scratch);
			Out.Reset(Utf8.Len());
			Out.Append(Utf8.GetData(), Utf8.Len());
		}

		int32 FindIn(const TMultiMap<uint32, int32>& Table, const TArray<FFieldInfo>& Fields, TArray<UTF8CHAR> FFieldInfo::* Spelling, FUtf8StringView Key, uint32 Hash)
		{
			for (auto It = Table.CreateConstKeyIterator(Hash); It; ++It)
			{
				const TArray<UTF8CHAR>& Candidate = Fields[It.Value()].*Spelling;
				if (Candidate.Num() == Key.Len() && (Key.Len() == 0 || FMemory::Memcmp(Candidate.GetData(), Key.GetData(), Key.Len()) == 0))
				{
					return It.Value();
				}
			}
			return INDEX_NONE;
		}

		/** A spelling two fields share stays out of the tolerant tables: only an exact key reaches either field */
		void AddUnambiguous(TMultiMap<uint32, int32>& Table, const TArray<FFieldInfo>& Fields, TArray<UTF8CHAR> FFieldInfo::* Spelling)
		{
			for (int32 i = 0; i < Fields.Num(); ++i)
			{
				const FUtf8StringView Mine = View(Fields[i].*Spelling);
				bool bShared = false;
				for (int32 j = 0; j < Fields.Num() && !bShared; ++j)
				{
					bShared = j != i && View(Fields[j].*Spelling).Equals(Mine, ESearchCase::CaseSensitive);
				}
				if (!bShared)
				{
					Table.Add(HashKey(Mine), i);
				}
			}
		}

		/** OutEpoch: the cache epoch the renames were read at (read together, under one lock) */
		FFieldMapRef Build(const UScriptStruct* Struct, EVaRestJsonNaming Naming, uint64& OutEpoch)
		{
			TMap<FName, FString> StructRenames;
			{
				FReadScopeLock ReadLock(Cache().Lock);
				OutEpoch = Cache().Epoch;
				if (const TMap<FName, FString>* Found = Cache().Renames.Find(FObjectKey(Struct)))
				{
					StructRenames = *Found;
				}
			}

			const TSharedRef<FFieldMap, ESPMode::ThreadSafe> Map = MakeShared<FFieldMap, ESPMode::ThreadSafe>();
			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				const FProperty* Property = *It;
				// The flags FJsonObjectConverter skips by default (JsonObjectConverter.cpp:406-410), plus editor-only data:
				// such a field exists in PIE and not in a cooked game, the divergence spec 2.4 rules out for BP struct names
				if (Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient | CPF_EditorOnly))
				{
					continue;
				}
				FString Why;
				if (!IsSupported(Property, Why))
				{
					UE_LOG(LogVaRestJson, Warning, TEXT("FVaRestJsonConvert: %s.%s is skipped: %s"), *Struct->GetName(), *Property->GetName(), *Why);
					continue;
				}
				const bool bBool = Property->IsA<FBoolProperty>();
				const FString Authored = AuthoredFieldName(Struct, Property);
				const FString* Renamed = StructRenames.Find(Property->GetFName());
				const FString JsonName = Renamed ? *Renamed : ApplyNaming(Authored, bBool, Naming);
				// Normalised from the camelCase spelling, so a bool's 'b' prefix never takes part (bIsActive ~ is_active)
				const FString Loose = Renamed ? *Renamed : ApplyNaming(Authored, bBool, EVaRestJsonNaming::CamelCase);

				FFieldInfo Field;
				Field.Property = Property;
				AssignUtf8(Field.Name, JsonName);
				if (FindIn(Map->ByName, Map->Fields, &FFieldInfo::Name, View(Field.Name), HashKey(View(Field.Name))) != INDEX_NONE)
				{
					// One key twice would write a duplicate and make decode depend on member order
					UE_LOG(LogVaRestJson, Warning, TEXT("FVaRestJsonConvert: %s.%s is skipped: its JSON name \"%s\" is already used by another field"), *Struct->GetName(), *Property->GetName(), *JsonName);
					continue;
				}
				FUtf8Scratch Scratch;
				FoldUtf8(View(Field.Name), Scratch);
				Field.Folded.Append(Scratch.GetData(), Scratch.Num());
				TArray<UTF8CHAR> LooseUtf8;
				AssignUtf8(LooseUtf8, Loose);
				NormalizeUtf8(View(LooseUtf8), Scratch);
				Field.Normalized.Append(Scratch.GetData(), Scratch.Num());

				const uint32 NameHash = HashKey(View(Field.Name));
				const int32 FieldIndex = Map->Fields.Add(MoveTemp(Field));
				Map->ByName.Add(NameHash, FieldIndex);
			}
			AddUnambiguous(Map->ByFolded, Map->Fields, &FFieldInfo::Folded);
			AddUnambiguous(Map->ByNormalized, Map->Fields, &FFieldInfo::Normalized);
			return Map;
		}
	}

	FFieldMapRef GetFieldMap(const UScriptStruct* Struct, EVaRestJsonNaming Naming)
	{
		NamesPrivate::FCache& Shared = NamesPrivate::Cache();
		const TPair<FObjectKey, uint8> Key(FObjectKey(Struct), static_cast<uint8>(Naming));
		{
			FReadScopeLock ReadLock(Shared.Lock);
			if (const FFieldMapRef* Found = Shared.Maps.Find(Key))
			{
				return *Found;
			}
		}
		// Built outside the lock: Build reads the rename registry under the same lock, and FRWLock is not recursive
		uint64 BuiltEpoch = 0;
		const FFieldMapRef Built = NamesPrivate::Build(Struct, Naming, BuiltEpoch);
		FWriteScopeLock WriteLock(Shared.Lock);
		if (const FFieldMapRef* Raced = Shared.Maps.Find(Key))
		{
			return *Raced; // another thread built it first: every caller shares one map
		}
		if (Shared.Epoch != BuiltEpoch)
		{
			return Built; // reset or renamed while this map was built: used for this call, never cached
		}
		Shared.Maps.Add(Key, Built);
		return Built;
	}

	int32 FFieldMap::Match(FUtf8StringView Key, uint32 KeyHash, FUtf8Scratch& Scratch, uint8& OutTier) const
	{
		int32 Found = NamesPrivate::FindIn(ByName, Fields, &FFieldInfo::Name, Key, KeyHash);
		if (Found != INDEX_NONE)
		{
			OutTier = 0;
			return Found;
		}
		FoldUtf8(Key, Scratch);
		const FUtf8StringView FoldedKey(Scratch.GetData(), Scratch.Num());
		Found = NamesPrivate::FindIn(ByFolded, Fields, &FFieldInfo::Folded, FoldedKey, HashKey(FoldedKey));
		if (Found != INDEX_NONE)
		{
			OutTier = 1;
			return Found;
		}
		NormalizeUtf8(Key, Scratch);
		const FUtf8StringView NormalizedKey(Scratch.GetData(), Scratch.Num());
		Found = NamesPrivate::FindIn(ByNormalized, Fields, &FFieldInfo::Normalized, NormalizedKey, HashKey(NormalizedKey));
		if (Found != INDEX_NONE)
		{
			OutTier = 2;
			return Found;
		}
		// A bool's AsIs spelling under any other policy: bIsActive for the field the camelCase map calls isActive.
		// FJsonObjectConverter writes that spelling: a field name only gets its first character lowered
		// (JsonObjectConverter.cpp:449-453, :41).
		if (NormalizedKey.Len() > 1 && static_cast<uint8>(NormalizedKey[0]) == 'b')
		{
			const FUtf8StringView WithoutPrefix = NormalizedKey.RightChop(1);
			Found = NamesPrivate::FindIn(ByNormalized, Fields, &FFieldInfo::Normalized, WithoutPrefix, HashKey(WithoutPrefix));
			if (Found != INDEX_NONE && Fields[Found].Property->IsA<FBoolProperty>())
			{
				// Only a field whose b the policy dropped: a bool named Old does not answer to "bold"
				TStringBuilder<128> PropertyName;
				Fields[Found].Property->GetFName().ToString(PropertyName);
				if (NamesPrivate::HasBoolPrefix(PropertyName.ToView()))
				{
					OutTier = 2;
					return Found;
				}
			}
		}
		return INDEX_NONE;
	}

	FString StripGuidSuffix(const FString& PropertyName)
	{
		// UUserDefinedStruct::GetAuthoredNameForField, cooked branch (UserDefinedStruct.cpp:300-313), line for line
		const int32 GuidStrLen = 32;
		const int32 MinimalPostfixLen = GuidStrLen + 3;
		if (PropertyName.Len() > MinimalPostfixLen)
		{
			const FString DisplayName = PropertyName.LeftChop(GuidStrLen + 1);
			int32 FirstCharToRemove = -1;
			if (DisplayName.FindLastChar(TCHAR('_'), FirstCharToRemove) && FirstCharToRemove > 0)
			{
				return DisplayName.Mid(0, FirstCharToRemove);
			}
		}
		return PropertyName;
	}

	FString AuthoredFieldName(const UScriptStruct* Owner, const FProperty* Property)
	{
		// Never Property->GetAuthoredName(): for a Blueprint struct it is the editor-only friendly name in editor builds
		// (UserDefinedStruct.cpp:289-298) and the stripped name in cooked ones, so PIE and a packaged game would write
		// different JSON (spec 2.4). The cooked algorithm runs everywhere instead.
		return Owner && Owner->IsA<UUserDefinedStruct>() ? StripGuidSuffix(Property->GetName()) : Property->GetName();
	}

	FString ApplyNaming(const FString& AuthoredName, bool bBoolField, EVaRestJsonNaming Naming)
	{
		if (Naming == EVaRestJsonNaming::AsIs)
		{
			return AuthoredName;
		}
		const bool bBoolPrefix = bBoolField && NamesPrivate::HasBoolPrefix(AuthoredName);
		const TArray<FString> Words = NamesPrivate::SplitWords(bBoolPrefix ? AuthoredName.RightChop(1) : AuthoredName);
		if (Words.Num() == 0)
		{
			return AuthoredName;
		}
		FString Result;
		for (int32 i = 0; i < Words.Num(); ++i)
		{
			switch (Naming)
			{
			case EVaRestJsonNaming::CamelCase:
				Result += i == 0 ? Words[i].ToLower() : NamesPrivate::Capitalized(Words[i]);
				break;
			case EVaRestJsonNaming::SnakeCase:
				if (i > 0)
				{
					Result.AppendChar(TEXT('_'));
				}
				Result += Words[i].ToLower();
				break;
			case EVaRestJsonNaming::PascalCase:
				Result += NamesPrivate::Capitalized(Words[i]);
				break;
			case EVaRestJsonNaming::AsIs:
				break;
			}
		}
		return Result;
	}

	void FoldUtf8(FUtf8StringView Text, FUtf8Scratch& Out)
	{
		Out.Reset();
		for (const UTF8CHAR C : Text)
		{
			const uint8 Byte = static_cast<uint8>(C);
			Out.Add(static_cast<UTF8CHAR>(Byte >= 'A' && Byte <= 'Z' ? Byte + ('a' - 'A') : Byte));
		}
	}

	void NormalizeUtf8(FUtf8StringView Text, FUtf8Scratch& Out)
	{
		Out.Reset();
		for (const UTF8CHAR C : Text)
		{
			const uint8 Byte = static_cast<uint8>(C);
			if (Byte == '_' || Byte == '-' || Byte == ' ')
			{
				continue;
			}
			Out.Add(static_cast<UTF8CHAR>(Byte >= 'A' && Byte <= 'Z' ? Byte + ('a' - 'A') : Byte));
		}
	}

	bool IsSupported(const FProperty* Property, FString& OutWhy)
	{
		if (Property->IsA<FBoolProperty>() || Property->IsA<FNumericProperty>() || Property->IsA<FEnumProperty>()
			|| Property->IsA<FStrProperty>() || Property->IsA<FNameProperty>() || Property->IsA<FTextProperty>()
			|| Property->IsA<FStructProperty>() || Property->IsA<FSoftObjectProperty>()) // FSoftClassProperty is one (UnrealType.h:3550)
		{
			return true;
		}
		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			return IsSupported(ArrayProperty->Inner, OutWhy);
		}
		if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
		{
			return IsSupported(SetProperty->ElementProp, OutWhy);
		}
		if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		{
			const FProperty* KeyProperty = MapProperty->KeyProp;
			const FNumericProperty* NumericKey = CastField<FNumericProperty>(KeyProperty);
			const bool bKeyFits = KeyProperty->IsA<FStrProperty>() || KeyProperty->IsA<FNameProperty>() || KeyProperty->IsA<FEnumProperty>()
				|| (NumericKey && (NumericKey->IsInteger() || NumericKey->IsEnum()));
			if (!bKeyFits)
			{
				OutWhy = TEXT("map keys must be strings, names, integers or enums (they become JSON object keys)");
				return false;
			}
			return IsSupported(MapProperty->ValueProp, OutWhy);
		}
		if (const FOptionalProperty* OptionalProperty = CastField<FOptionalProperty>(Property))
		{
			return IsSupported(OptionalProperty->GetValueProperty(), OutWhy);
		}
		if (Property->IsA<FObjectPropertyBase>() || Property->IsA<FInterfaceProperty>())
		{
			OutWhy = TEXT("object references are not converted");
			return false;
		}
		OutWhy = FString::Printf(TEXT("%s properties are not converted"), *Property->GetClass()->GetName());
		return false;
	}

	ESpecialStruct ClassifyStruct(const UScriptStruct* Struct)
	{
		if (Struct == FVaRestJson::StaticStruct())
		{
			return ESpecialStruct::Json;
		}
		if (Struct == TBaseStructure<FDateTime>::Get())
		{
			return ESpecialStruct::DateTime;
		}
		if (Struct == TBaseStructure<FGuid>::Get())
		{
			return ESpecialStruct::Guid;
		}
		if (Struct == FSoftObjectPath::StaticStruct() || Struct == FSoftClassPath::StaticStruct())
		{
			return ESpecialStruct::SoftObjectPath;
		}
		// FTimespan has no TBaseStructure specialisation (UObject/Class.h declares FDateTime's at :5645, none for
		// FTimespan); its reflection is the noexport struct /Script/CoreUObject.Timespan (NoExportTypes.h:2252-2253).
		static const FName TimespanName(TEXT("Timespan"));
		static const FName CoreUObjectPackage(TEXT("/Script/CoreUObject"));
		if (Struct->GetFName() == TimespanName && Struct->GetPackage()->GetFName() == CoreUObjectPackage)
		{
			return ESpecialStruct::Timespan;
		}
		return ESpecialStruct::None;
	}

	bool IsUnsignedInteger(const FNumericProperty* Property)
	{
		return Property->IsA<FByteProperty>() || Property->IsA<FUInt16Property>() || Property->IsA<FUInt32Property>() || Property->IsA<FUInt64Property>();
	}

	bool IsPlainByteArray(const FArrayProperty* Property)
	{
		const FByteProperty* Inner = CastField<FByteProperty>(Property->Inner);
		return Inner && !Inner->Enum;
	}
}

void FVaRestJsonNames::Register(const UScriptStruct* Struct, TConstArrayView<FVaRestJsonRename> Renames)
{
	using namespace VaRest::Json::Convert;
	if (!Struct)
	{
		return;
	}
	TMap<FName, FString> ByField;
	for (const FVaRestJsonRename& Rename : Renames)
	{
		ByField.Add(Rename.Field, Rename.Json);
	}
	NamesPrivate::FCache& Shared = NamesPrivate::Cache();
	FWriteScopeLock WriteLock(Shared.Lock);
	Shared.Renames.Add(FObjectKey(Struct), MoveTemp(ByField));
	Shared.Maps.Reset(); // maps built before the rename are stale
	++Shared.Epoch;
}

void FVaRestJsonNames::Unregister(const UScriptStruct* Struct)
{
	using namespace VaRest::Json::Convert;
	NamesPrivate::FCache& Shared = NamesPrivate::Cache();
	FWriteScopeLock WriteLock(Shared.Lock);
	Shared.Renames.Remove(FObjectKey(Struct));
	Shared.Maps.Reset();
	++Shared.Epoch;
}

void FVaRestJsonConvert::ResetCache()
{
	using namespace VaRest::Json::Convert;
	NamesPrivate::FCache& Shared = NamesPrivate::Cache();
	FWriteScopeLock WriteLock(Shared.Lock);
	Shared.Maps.Reset();
	++Shared.Epoch;
}

int32 FVaRestJsonConvert::NumCachedFieldMapsInternal()
{
	using namespace VaRest::Json::Convert;
	NamesPrivate::FCache& Shared = NamesPrivate::Cache();
	FReadScopeLock ReadLock(Shared.Lock);
	return Shared.Maps.Num();
}
