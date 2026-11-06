// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonRules.h"

#include "Misc/ScopeLock.h"
#include "Misc/ScopeRWLock.h"
#include "UObject/Class.h"
#include "VaRestJsonConvert.h"
#include "VaRestJsonConvertPrivate.h"
#include "VaRestJsonLog.h"
#include "VaRestJsonUtf8.h"

namespace VaRest::Json::RulesPrivate
{
struct FOwner
{
	FString Path;
	TMap<FObjectKey, FVaRestJsonResolvedRules> Types;
	TArray<FString> Folders;
	uint64 Generation = 0;
};

struct FRegistry
{
	FRWLock Lock;
	TMap<FObjectKey, FOwner> Owners;
	TSharedPtr<const FVaRestJsonRuleSnapshot, ESPMode::ThreadSafe> Default;
	TMap<FObjectKey, TSharedRef<const FVaRestJsonRuleSnapshot, ESPMode::ThreadSafe>> Snapshots;
	uint64 Generation = 0;
	uint64 NextSnapshotId = 1;
	bool bChanging = false; // GT only; mutation from a notification is refused, never interleaved
	FVaRestJsonRules::FRulesChanged Changing;
	FVaRestJsonRules::FRulesChanged Changed;
};

FRegistry& Registry()
{
	static FRegistry Value;
	return Value;
}

bool Refuse(FString* OutError, const FString& Error)
{
	if (OutError)
	{
		*OutError = Error;
	}
	else
	{
		UE_LOG(LogVaRestJson, Warning, TEXT("JSON rules: %s"), *Error);
	}
	return false;
}

bool Normalize(const FVaRestJsonTypeRuleRegistration& Input, FVaRestJsonResolvedRules& Out, FString* OutError)
{
	if (!Input.Type || (!Input.Type->IsA<UScriptStruct>() && !Input.Type->IsA<UEnum>()) || (Input.Type->IsA<UScriptStruct>() ? !Input.Enums.IsEmpty() : !Input.Fields.IsEmpty()))
	{
		return Refuse(OutError, TEXT("a registration must contain fields for a struct or enumerators for an enum"));
	}
	Out.Fields = Input.Fields;
	Out.Enums = Input.Enums;
	for (const FVaRestJsonFieldRule& Rule : Out.Fields)
	{
		if (!VaRest::Json::Convert::StableWireText(Rule.Json))
			return Refuse(OutError, TEXT("field wire keys must be well-formed UTF-16 within the JSON string limit"));
	}
	for (FVaRestJsonEnumRule& Rule : Out.Enums)
	{
		if (!VaRest::Json::Convert::StableWireText(Rule.Json))
			return Refuse(OutError, TEXT("enum wire values must be well-formed UTF-16 within the JSON string limit"));
		const UEnum* Enum = CastChecked<UEnum>(Input.Type);
		int32 Matches = 0;
		FName Canonical;
		for (int32 Index = 0; Index < Enum->NumEnums() - (Enum->ContainsExistingMax() ? 1 : 0); ++Index)
		{
			if (Rule.Enumerator == Enum->GetNameByIndex(Index) || Rule.Enumerator.ToString().Equals(Enum->GetNameStringByIndex(Index), ESearchCase::IgnoreCase))
			{
				++Matches;
				Canonical = Enum->GetNameByIndex(Enum->GetIndexByValue(Enum->GetValueByIndex(Index)));
			}
		}
		if (Matches != 1)
		{
			return Refuse(OutError, TEXT("an enum rule must name exactly one existing enumerator"));
		}
		Rule.Enumerator = Canonical;
	}
	Out.Fields.Sort([](const FVaRestJsonFieldRule& A, const FVaRestJsonFieldRule& B) { return A.Field.ToString() < B.Field.ToString(); });
	Out.Enums.Sort([](const FVaRestJsonEnumRule& A, const FVaRestJsonEnumRule& B) { return A.Enumerator.LexicalLess(B.Enumerator); });
	TArray<FString> WireNames;
	for (int32 i = 0; i < Out.Fields.Num(); ++i)
	{
		const FVaRestJsonFieldRule& Rule = Out.Fields[i];
		if (!Rule.Field.IsValid() || (i && Rule.Field == Out.Fields[i - 1].Field) || static_cast<uint8>(Rule.Presence) > static_cast<uint8>(EVaRestJsonPresence::Ignore) || (Rule.Presence != EVaRestJsonPresence::Ignore && WireNames.ContainsByPredicate([&](const FString& Wire) { return VaRest::Json::EqualWireText(Wire, Rule.Json); })))
		{
			return Refuse(OutError, TEXT("field rules require distinct valid GUIDs and distinct wire keys"));
		}
		if (Rule.Presence != EVaRestJsonPresence::Ignore)
		{
			WireNames.Add(Rule.Json);
		}
	}
	for (int32 i = 0; i < Out.Enums.Num(); ++i)
	{
		const FVaRestJsonEnumRule& Rule = Out.Enums[i];
		if ((i && Rule.Enumerator == Out.Enums[i - 1].Enumerator) || (!Rule.bUnset && WireNames.ContainsByPredicate([&](const FString& Wire) { return VaRest::Json::EqualWireText(Wire, Rule.Json); })))
		{
			return Refuse(OutError, TEXT("enum rules require distinct enumerators and distinct non-unset wire values"));
		}
		if (!Rule.bUnset)
		{
			WireNames.Add(Rule.Json);
		}
	}
	if (const UEnum* Enum = Cast<UEnum>(Input.Type))
	{
		FString Error;
		if (!VaRest::Json::Convert::ValidateEnumRules(Enum, &Out, Error))
		{
			return Refuse(OutError, Error);
		}
	}
	return true;
}

void MergeDefaults(const TMap<FObjectKey, FOwner>& Owners, TMap<FObjectKey, FVaRestJsonResolvedRules>& Types, TArray<FString>& Folders)
{
	TArray<const FOwner*> Ordered;
	for (const auto& Pair : Owners)
	{
		Ordered.Add(&Pair.Value);
	}
	Ordered.Sort([](const FOwner& A, const FOwner& B) { return A.Path < B.Path; });
	TMap<FObjectKey, FString> FirstOwner;
	for (const FOwner* Owner : Ordered)
	{
		for (const FString& Folder : Owner->Folders)
		{
			Folders.AddUnique(Folder);
		}
		for (const auto& Pair : Owner->Types)
		{
			if (FVaRestJsonResolvedRules* Found = Types.Find(Pair.Key))
			{
				if (Found->Error.IsEmpty() && (Found->Fields != Pair.Value.Fields || Found->Enums != Pair.Value.Enums))
				{
					Found->Error = FString::Printf(TEXT("conflicting JSON rules from %s and %s"), *FirstOwner.FindChecked(Pair.Key), *Owner->Path);
				}
			}
			else
			{
				Types.Add(Pair.Key, Pair.Value);
				FirstOwner.Add(Pair.Key, Owner->Path);
			}
		}
	}
	Folders.Sort();
}

bool CopyOwner(const UObject* Owner, FOwner& Out, FString* OutError)
{
	if (!IsInGameThread() || !Owner || Registry().bChanging)
	{
		return Refuse(OutError, TEXT("rule registration requires a nonnull owner on GT, outside a rule-change notification"));
	}
	FReadScopeLock Lock(Registry().Lock);
	if (const FOwner* Existing = Registry().Owners.Find(FObjectKey(Owner)))
	{
		Out = *Existing;
	}
	Out.Path = Owner->GetPathName();
	return true;
}
} // namespace VaRest::Json::RulesPrivate

FVaRestJsonRuleSnapshot::FVaRestJsonRuleSnapshot(uint64 InId, TMap<FObjectKey, FVaRestJsonResolvedRules>&& InTypes, TArray<FString>&& InFolders)
	: Id(InId)
	, Types(MoveTemp(InTypes))
	, Folders(MoveTemp(InFolders))
{
}

const FVaRestJsonResolvedRules* FVaRestJsonRuleSnapshot::Find(const UObject* Type) const
{
	return Type ? Types.Find(FObjectKey(Type)) : nullptr;
}

bool FVaRestJsonRuleSnapshot::ValidateRequiredTypeOnGT(const UObject* Type, FString& Error) const
{
	check(IsInGameThread());
	if (!Type) return true;
	if (const auto* Found = Find(Type))
	{
		if (Found->Error.IsEmpty()) return true;
		Error = Found->Error;
		return false;
	}
	const FString Path = Type->GetPathName();
	for (const FString& Folder : Folders)
	{
		if (Path.StartsWith(Folder + TEXT("/"), ESearchCase::CaseSensitive))
		{
			Error = FString::Printf(TEXT("required JSON rules are not registered for %s"), *Path);
			return false;
		}
	}
	return true;
}

void FVaRestJsonRuleSnapshot::WarnIfMissing(const UObject* Type) const
{
#if !UE_BUILD_SHIPPING
	if (!Type || Find(Type) || Folders.IsEmpty())
	{
		return;
	}
	const FString Path = Type->GetPathName();
	for (const FString& Folder : Folders)
	{
		if (Path.StartsWith(Folder + TEXT("/"), ESearchCase::CaseSensitive))
		{
			static FCriticalSection WarningLock;
			static TSet<FObjectKey> Warned;
			bool bWarn = false;
			{
				FScopeLock Lock(&WarningLock);
				bWarn = !Warned.Contains(FObjectKey(Type));
				Warned.Add(FObjectKey(Type));
			}
			if (bWarn)
			{
				UE_LOG(LogVaRestJson, Warning, TEXT("No JSON rules registered for %s under TypesFolder %s"), *Path, *Folder);
			}
			return;
		}
	}
#endif
}

bool FVaRestJsonRules::ReplaceOwnerRules(const UObject* Owner, TConstArrayView<FVaRestJsonTypeRuleRegistration> Registrations,
	TConstArrayView<FString> TypesFolders, FString* OutError)
{
	using namespace VaRest::Json::RulesPrivate;
	FOwner Previous;
	if (!CopyOwner(Owner, Previous, OutError))
	{
		return false;
	}
	FOwner Next;
	Next.Path = Previous.Path;
	for (const auto& Registration : Registrations)
	{
		FVaRestJsonResolvedRules Rules;
		if (!Normalize(Registration, Rules, OutError))
		{
			return false;
		}
		if (Next.Types.Contains(FObjectKey(Registration.Type)))
		{
			return Refuse(OutError, TEXT("one owner batch must not declare a type twice"));
		}
		Next.Types.Add(FObjectKey(Registration.Type), MoveTemp(Rules));
	}
	for (FString Folder : TypesFolders)
	{
		Folder.ReplaceInline(TEXT("\\"), TEXT("/"));
		Folder.RemoveFromEnd(TEXT("/"));
		if (!Folder.StartsWith(TEXT("/")) || Folder.Contains(TEXT("//")) || Folder.Contains(TEXT("..")) || Folder.Len() < 2)
		{
			return Refuse(OutError, TEXT("TypesFolders must be absolute content package folders"));
		}
		Next.Folders.AddUnique(MoveTemp(Folder));
	}
	FRegistry& Store = Registry();
	TGuardValue<bool> Changing(Store.bChanging, true);
	Store.Changing.Broadcast(); // before publication, and outside the lock: observers may take old snapshots
	{
		FWriteScopeLock Lock(Store.Lock);
		Next.Generation = ++Store.Generation;
		Store.Owners.Add(FObjectKey(Owner), MoveTemp(Next));
		Store.Default.Reset();
		Store.Snapshots.Reset();
	}
	FVaRestJsonConvert::ResetCache();
	Store.Changed.Broadcast();
	return true;
}

namespace VaRest::Json::RulesPrivate
{
bool UpdateOne(const UObject* Owner, const FVaRestJsonTypeRuleRegistration* Registration, const FString* Folder, FString* OutError)
{
	FOwner Existing;
	if (!CopyOwner(Owner, Existing, OutError))
	{
		return false;
	}
	TArray<FVaRestJsonTypeRuleRegistration> Batch;
	for (const auto& Pair : Existing.Types)
	{
		const UObject* Type = Pair.Key.ResolveObjectPtr(); // GT, never used by a worker snapshot
		if (Type && (!Registration || Type != Registration->Type))
		{
			Batch.Add({Type, Pair.Value.Fields, Pair.Value.Enums});
		}
	}
	if (Registration)
	{
		Batch.Add(*Registration);
	}
	if (Folder)
	{
		Existing.Folders.AddUnique(*Folder);
	}
	return FVaRestJsonRules::ReplaceOwnerRules(Owner, Batch, Existing.Folders, OutError);
}
} // namespace VaRest::Json::RulesPrivate

bool FVaRestJsonRules::RegisterFields(const UObject* Owner, const UScriptStruct* Struct, TConstArrayView<FVaRestJsonFieldRule> Rules, FString* OutError)
{
	FVaRestJsonTypeRuleRegistration Registration;
	Registration.Type = Struct;
	Registration.Fields.Append(Rules.GetData(), Rules.Num());
	return VaRest::Json::RulesPrivate::UpdateOne(Owner, &Registration, nullptr, OutError);
}

bool FVaRestJsonRules::RegisterEnum(const UObject* Owner, const UEnum* Enum, TConstArrayView<FVaRestJsonEnumRule> Rules, FString* OutError)
{
	FVaRestJsonTypeRuleRegistration Registration;
	Registration.Type = Enum;
	Registration.Enums.Append(Rules.GetData(), Rules.Num());
	return VaRest::Json::RulesPrivate::UpdateOne(Owner, &Registration, nullptr, OutError);
}

bool FVaRestJsonRules::RegisterTypesFolder(const UObject* Owner, const FString& Folder, FString* OutError)
{
	return VaRest::Json::RulesPrivate::UpdateOne(Owner, nullptr, &Folder, OutError);
}

void FVaRestJsonRules::UnregisterOwner(const UObject* Owner)
{
	using namespace VaRest::Json::RulesPrivate;
	FOwner Previous;
	if (!CopyOwner(Owner, Previous, nullptr))
	{
		return;
	}
	FRegistry& Store = Registry();
	{
		FReadScopeLock Lock(Store.Lock);
		if (!Store.Owners.Contains(FObjectKey(Owner)))
		{
			return;
		}
	}
	TGuardValue<bool> Changing(Store.bChanging, true);
	Store.Changing.Broadcast();
	{
		FWriteScopeLock Lock(Store.Lock);
		Store.Owners.Remove(FObjectKey(Owner));
		++Store.Generation;
		Store.Default.Reset();
		Store.Snapshots.Reset();
	}
	FVaRestJsonConvert::ResetCache();
	Store.Changed.Broadcast();
}

TSharedRef<const FVaRestJsonRuleSnapshot, ESPMode::ThreadSafe> FVaRestJsonRules::DefaultSnapshot()
{
	using namespace VaRest::Json::RulesPrivate;
	FRegistry& Store = Registry();
	{
		FReadScopeLock Lock(Store.Lock);
		if (Store.Default)
		{
			return Store.Default.ToSharedRef();
		}
	}
	FWriteScopeLock Lock(Store.Lock);
	if (!Store.Default)
	{
		TMap<FObjectKey, FVaRestJsonResolvedRules> Types;
		TArray<FString> Folders;
		MergeDefaults(Store.Owners, Types, Folders);
		Store.Default = MakeShareable(new FVaRestJsonRuleSnapshot(Store.NextSnapshotId++, MoveTemp(Types), MoveTemp(Folders)));
	}
	return Store.Default.ToSharedRef();
}

TSharedRef<const FVaRestJsonRuleSnapshot, ESPMode::ThreadSafe> FVaRestJsonRules::Snapshot(const UObject* Owner)
{
	using namespace VaRest::Json::RulesPrivate;
	if (!Owner)
	{
		return DefaultSnapshot();
	}
	FRegistry& Store = Registry();
	const auto Default = DefaultSnapshot();
	FWriteScopeLock Lock(Store.Lock);
	if (const auto* Cached = Store.Snapshots.Find(FObjectKey(Owner)))
	{
		return *Cached;
	}
	if (!Store.Owners.Contains(FObjectKey(Owner)))
	{
		return Default;
	}
	TMap<FObjectKey, FVaRestJsonResolvedRules> Types;
	TArray<FString> Folders;
	MergeDefaults(Store.Owners, Types, Folders);
	if (const FOwner* Explicit = Store.Owners.Find(FObjectKey(Owner)))
	{
		for (const auto& Pair : Explicit->Types)
		{
			Types.Add(Pair.Key, Pair.Value);
		}
	}
	TSharedRef<const FVaRestJsonRuleSnapshot, ESPMode::ThreadSafe> Result = MakeShareable(
		new FVaRestJsonRuleSnapshot(Store.NextSnapshotId++, MoveTemp(Types), MoveTemp(Folders)));
	Store.Snapshots.Add(FObjectKey(Owner), Result);
	return Result;
}

uint64 FVaRestJsonRules::GetGeneration(const UObject* Owner)
{
	using namespace VaRest::Json::RulesPrivate;
	FReadScopeLock Lock(Registry().Lock);
	const FOwner* Found = Owner ? Registry().Owners.Find(FObjectKey(Owner)) : nullptr;
	return Found ? Found->Generation : 0;
}

uint64 FVaRestJsonRules::GetDefaultGeneration()
{
	using namespace VaRest::Json::RulesPrivate;
	FReadScopeLock Lock(Registry().Lock);
	return Registry().Generation;
}

FVaRestJsonRules::FRulesChanged& FVaRestJsonRules::OnRulesChanging()
{
	return VaRest::Json::RulesPrivate::Registry().Changing;
}
FVaRestJsonRules::FRulesChanged& FVaRestJsonRules::OnRulesChanged()
{
	return VaRest::Json::RulesPrivate::Registry().Changed;
}
