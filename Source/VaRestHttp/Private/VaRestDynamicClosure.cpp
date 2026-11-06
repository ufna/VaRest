// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestDynamicClosure.h"
#include "VaRestDynamicTypes.h"
#include "VaRestHttpTesting.h"
#include "CoreGlobals.h"
#include "Misc/CoreDelegates.h"
#include "Misc/ScopeExit.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/EnumProperty.h"
#include "UObject/ObjectKey.h"
#include "UObject/PropertyOptional.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"
#include <array>

namespace VaRest::Http
{
namespace DynamicClosurePrivate
{
constexpr int32 MaxClosureItems = 4096;
const FName ValueName(TEXT("Value"));
// One stable field identity. Descriptor hashing also includes kind, type object and container; never NewGuid per request.
const FGuid ValueId(0x76617265, 0x73746479, 0x6e616d69, 0x6376616c);
bool Typed(const FVaRestTypeRef& Type)
{
	return Type.bJsonValue || Type.Container != EVaRestTypeContainer::None || (Type.Kind != EVaRestTypeKind::None &&
		Type.Kind != EVaRestTypeKind::Json && Type.Kind != EVaRestTypeKind::String && Type.Kind != EVaRestTypeKind::Bytes);
}
bool Intrinsic(const UScriptStruct* Type)
{
	return Type == FVaRestJson::StaticStruct() || Type == TBaseStructure<FDateTime>::Get() ||
		Type == TBaseStructure<FGuid>::Get() || Type == TBaseStructure<FVector>::Get() ||
		Type == TBaseStructure<FVector2D>::Get() || Type == TBaseStructure<FRotator>::Get() ||
		Type == TBaseStructure<FQuat>::Get() || Type == TBaseStructure<FTransform>::Get() ||
		Type == TBaseStructure<FColor>::Get() || Type == TBaseStructure<FLinearColor>::Get();
}
FString Name(const FVaRestTypeRef& Type)
{
	FString Result;
	switch (Type.Kind)
	{
	case EVaRestTypeKind::Struct: case EVaRestTypeKind::Enum: Result = Type.Type->GetName(); break;
	case EVaRestTypeKind::None: return FString();
	case EVaRestTypeKind::Json: Result=TEXT("Json"); break;
	case EVaRestTypeKind::String: Result=TEXT("String"); break;
	case EVaRestTypeKind::Bytes: Result=TEXT("Bytes"); break;
	case EVaRestTypeKind::Bool: Result=TEXT("Bool"); break;
	case EVaRestTypeKind::Int: Result=TEXT("Int"); break;
	case EVaRestTypeKind::Int64: Result=TEXT("Int64"); break;
	case EVaRestTypeKind::Double: Result=TEXT("Double"); break;
	case EVaRestTypeKind::DateTime: Result=TEXT("DateTime"); break;
	}
	if (Type.Container==EVaRestTypeContainer::Array) Result=TEXT("Array<")+Result+TEXT(">");
	if (Type.Container==EVaRestTypeContainer::Map) Result=TEXT("Map<String,")+Result+TEXT(">");
	if (Type.bJsonValue) Result=TEXT("Json<")+Result+TEXT(">");
	return Result;
}
UScriptStruct* StructType(const FVaRestTypeRef& Type)
{
	if (Type.Kind==EVaRestTypeKind::Struct) return Cast<UScriptStruct>(Type.Type);
	if (Type.Kind==EVaRestTypeKind::DateTime) return TBaseStructure<FDateTime>::Get();
	if (Type.Kind==EVaRestTypeKind::Json) return FVaRestJson::StaticStruct();
	return nullptr;
}
EPropertyBagPropertyType BagKind(EVaRestTypeKind Kind)
{
	switch (Kind)
	{
	case EVaRestTypeKind::Bool:return EPropertyBagPropertyType::Bool;
	case EVaRestTypeKind::Int:return EPropertyBagPropertyType::Int32;
	case EVaRestTypeKind::Int64:return EPropertyBagPropertyType::Int64;
	case EVaRestTypeKind::Double:return EPropertyBagPropertyType::Double;
	case EVaRestTypeKind::String:return EPropertyBagPropertyType::String;
	case EVaRestTypeKind::Bytes:return EPropertyBagPropertyType::Byte;
	case EVaRestTypeKind::Enum:return EPropertyBagPropertyType::Enum;
	case EVaRestTypeKind::Json:case EVaRestTypeKind::Struct:case EVaRestTypeKind::DateTime:return EPropertyBagPropertyType::Struct;
	default:return EPropertyBagPropertyType::None;
	}
}
}

struct FDynamicTypeRuntimeData
{
	struct FNativeContract
	{
		FName Name;
		TArray<TWeakObjectPtr<const UObject>> Dependencies;
	};
	struct FBagCacheEntry
	{
		EVaRestTypeKind Kind=EVaRestTypeKind::None;
		EVaRestTypeContainer Container=EVaRestTypeContainer::None;
		FObjectKey Type;
		uint64 Schema=0;
		TWeakObjectPtr<const UPropertyBag> Bag;
	};
	std::array<TWeakPtr<FDynamicTypedState,ESPMode::ThreadSafe>,256> States;
	TArray<FBagCacheEntry> Bags; // LRU, maximum64 weak entries; strong bag pins belong to admitted GT states
	TMap<TWeakObjectPtr<const UScriptStruct>,FNativeContract> NativeContracts; // maximum256 explicitly declared native types
	uint64 Schema=1,SettleSchema=0,SettleFrame=0;
	bool bStopped=false,bMutating=false,bSettlePending=false,bExhausted=false;
	bool bEditorHooksAvailable=!WITH_EDITOR;
	TFunction<bool()> CurrentEditorBindings;
	FDelegateHandle FrameHandle,RulesChangingHandle,RulesChangedHandle;

	void Invalidate()
	{
		check(IsInGameThread());
		bMutating=true; bSettlePending=false;
		if (Schema==MAX_uint64) bExhausted=true; else ++Schema;
		Bags.Reset();
		for (auto& Weak:States) if (const auto State=Weak.Pin()) State->InvalidateOnGT();
	}
	void Settle()
	{
		check(IsInGameThread());
		SettleSchema=Schema; SettleFrame=GFrameCounter; bSettlePending=true;
	}
	bool Available() const
	{
		return !bStopped && !bMutating && !bExhausted && bEditorHooksAvailable &&
			(!WITH_EDITOR || (CurrentEditorBindings && CurrentEditorBindings()));
	}
	bool Track(const TSharedRef<FDynamicTypedState,ESPMode::ThreadSafe>& State)
	{
		for (auto& Weak:States) if (!Weak.IsValid()) { Weak=State; return true; }
		return false;
	}
	const UPropertyBag* BagFor(const FVaRestTypeRef& Type)
	{
		using namespace DynamicClosurePrivate;
		const FObjectKey Key(Type.Type.Get());
		for (int32 Index=0;Index<Bags.Num();++Index)
		{
			const auto Entry=Bags[Index];
			if (Entry.Kind==Type.Kind && Entry.Container==Type.Container && Entry.Type==Key && Entry.Schema==Schema && Entry.Bag.IsValid())
			{
				Bags.RemoveAt(Index); Bags.Add(Entry); return Entry.Bag.Get();
			}
		}
		const UObject* Object=StructType(Type);
		if (Type.Kind==EVaRestTypeKind::Enum) Object=Type.Type.Get();
		const auto Container=Type.Container==EVaRestTypeContainer::Array || Type.Kind==EVaRestTypeKind::Bytes ? EPropertyBagContainerType::Array :
			Type.Container==EVaRestTypeContainer::Map ? EPropertyBagContainerType::Map : EPropertyBagContainerType::None;
		FPropertyBagPropertyDesc Desc(ValueName,Container,BagKind(Type.Kind),Object,CPF_Edit,
			Container==EPropertyBagContainerType::Map ? EPropertyBagPropertyType::String : EPropertyBagPropertyType::None);
		Desc.ID=ValueId;
		const auto* Bag=UPropertyBag::GetOrCreateFromDescs(MakeArrayView(&Desc,1));
		if (Bag)
		{
			if (Bags.Num()==64) Bags.RemoveAt(0);
			Bags.Add({Type.Kind,Type.Container,Key,Schema,Bag});
		}
		return Bag;
	}
};
namespace DynamicClosurePrivate
{
// GT-only module pointer, cleared at Stop before module state is freed. Native worker data never looks it up.
FDynamicTypeRuntimeData* Active=nullptr;
struct FClosureWalk
{
	FDynamicTypeRuntimeData& Runtime;
	const FVaRestJsonRuleSnapshot& Rules;
	TArray<TStrongObjectPtr<const UObject>>& Pins;
	TSet<const UObject*> Seen;
	int32 Work=0;
	FString& Error;
	bool Fail(const TCHAR* Reason) { Error=Reason; return false; }
	bool Pin(const UObject* Object)
	{
		if (!Object || !IsValid(Object)) return Fail(TEXT("dynamic type closure contains an unavailable type"));
		if (Seen.Contains(Object)) return true;
		if (++Work>MaxClosureItems) return Fail(TEXT("dynamic type closure exceeds its 4096-item admission limit"));
		if (!Rules.ValidateRequiredTypeOnGT(Object,Error)) return false;
		Seen.Add(Object); Pins.Emplace(Object); return true;
	}
	bool Struct(const UScriptStruct* Type,int32 Depth=0)
	{
		if (!Type || Depth>128) return Fail(TEXT("dynamic struct closure is missing or too deep"));
		if (Seen.Contains(Type)) return true;
		if (!Pin(Type)) return false;
		const auto* UserType=Cast<UUserDefinedStruct>(Type);
		if (UserType && UserType->Status!=UDSS_UpToDate) return Fail(TEXT("dynamic user-defined struct is not up to date"));
		if (!UserType && !Intrinsic(Type) && !Runtime.NativeContracts.Contains(TWeakObjectPtr<const UScriptStruct>(Type)))
			return Fail(TEXT("dynamic native type has no qualified game-thread lifecycle contract"));
		if (const auto* Contract=Runtime.NativeContracts.Find(TWeakObjectPtr<const UScriptStruct>(Type)))
		{
			for (const auto& Dependency:Contract->Dependencies)
			{
				if (!Dependency.IsValid()) return Fail(TEXT("qualified dynamic native type dependency is unavailable"));
				if (const auto* Nested=Cast<UScriptStruct>(Dependency.Get())) { if (!Struct(Nested,Depth+1)) return false; }
				else if (!Pin(Dependency.Get())) return false;
			}
		}
		if (const auto* Super=Cast<UScriptStruct>(Type->GetSuperStruct()); Super && !Struct(Super,Depth+1)) return false;
		for (TFieldIterator<FProperty> It(Type);It;++It) if (!Property(*It,Depth+1)) return false;
		return true;
	}
	bool Property(const FProperty* Prop,int32 Depth)
	{
		if (!Prop || Depth>128 || ++Work>MaxClosureItems) return Fail(TEXT("dynamic property closure exceeds its admission limit"));
		if (const auto* Value=CastField<FStructProperty>(Prop)) return Struct(Value->Struct,Depth+1);
		if (const auto* Value=CastField<FArrayProperty>(Prop)) return Property(Value->Inner,Depth+1);
		if (const auto* Value=CastField<FSetProperty>(Prop)) return Property(Value->ElementProp,Depth+1);
		if (const auto* Value=CastField<FMapProperty>(Prop))
		{
			const auto* Numeric=CastField<FNumericProperty>(Value->KeyProp);
			if (!(Value->KeyProp->IsA<FStrProperty>() || Value->KeyProp->IsA<FNameProperty>() ||
				Value->KeyProp->IsA<FEnumProperty>() || (Numeric && (Numeric->IsInteger() || Numeric->IsEnum()))))
				return Fail(TEXT("dynamic map has an unsupported key category"));
			return Property(Value->KeyProp,Depth+1) && Property(Value->ValueProp,Depth+1);
		}
		if (const auto* Value=CastField<FOptionalProperty>(Prop)) return Property(Value->GetValueProperty(),Depth+1);
		if (const auto* Value=CastField<FEnumProperty>(Prop)) return Pin(Value->GetEnum());
		if (const auto* Value=CastField<FNumericProperty>(Prop)) return !Value->IsEnum() || Pin(Value->GetIntPropertyEnum());
		if (Prop->IsA<FBoolProperty>() || Prop->IsA<FStrProperty>() || Prop->IsA<FNameProperty>() ||
			Prop->IsA<FTextProperty>() || Prop->IsA<FSoftObjectProperty>()) return true;
		return Fail(TEXT("dynamic type closure contains an unsupported property category"));
	}
};
}

TUniquePtr<FDynamicTypeRuntime> FDynamicTypeRuntime::CreateOnGT()
{
	check(IsInGameThread());
	using namespace DynamicClosurePrivate;
	if (Active) return nullptr;
	TUniquePtr<FDynamicTypeRuntime> Runtime(new FDynamicTypeRuntime());
	Runtime->Impl=MakeUnique<FDynamicTypeRuntimeData>(); Active=Runtime->Impl.Get();
	Active->RulesChangingHandle=FVaRestJsonRules::OnRulesChanging().AddLambda([] { DynamicTypes::InvalidateOnGT(); });
	Active->RulesChangedHandle=FVaRestJsonRules::OnRulesChanged().AddLambda([] { DynamicTypes::ScheduleSettleOnGT(); });
	Active->FrameHandle=FCoreDelegates::OnEndFrame.AddLambda([] {
		if (Active && Active->bSettlePending && Active->SettleSchema==Active->Schema && Active->SettleFrame<GFrameCounter)
			{ Active->bSettlePending=false; Active->bMutating=false; }
	});
	return Runtime;
}
FDynamicTypeRuntime::~FDynamicTypeRuntime() { StopOnGT(); }
void FDynamicTypeRuntime::StopOnGT()
{
	check(IsInGameThread());
	using namespace DynamicClosurePrivate;
	if (!Impl || Impl->bStopped) return;
	Impl->bStopped=true; Impl->Invalidate();
	Impl->CurrentEditorBindings=nullptr;
	FVaRestJsonRules::OnRulesChanging().Remove(Impl->RulesChangingHandle);
	FVaRestJsonRules::OnRulesChanged().Remove(Impl->RulesChangedHandle);
	FCoreDelegates::OnEndFrame.Remove(Impl->FrameHandle);
	if (Active==Impl.Get()) Active=nullptr;
	Impl->NativeContracts.Reset();
}
void DynamicTypes::InvalidateOnGT() { check(IsInGameThread()); if (DynamicClosurePrivate::Active) DynamicClosurePrivate::Active->Invalidate(); }
void DynamicTypes::ScheduleSettleOnGT() { check(IsInGameThread()); if (DynamicClosurePrivate::Active) DynamicClosurePrivate::Active->Settle(); }
void DynamicTypes::SetEditorHooksAvailableOnGT(bool bAvailable,TFunction<bool()>&& CurrentBindings)
{
	check(IsInGameThread());
	if (auto* Runtime=DynamicClosurePrivate::Active)
	{
		const bool bChanged=Runtime->bEditorHooksAvailable!=bAvailable;
		Runtime->bEditorHooksAvailable=bAvailable;
		Runtime->CurrentEditorBindings=MoveTemp(CurrentBindings);
		if (bChanged) { Runtime->Invalidate(); if (bAvailable) Runtime->Settle(); }
	}
}

void FDynamicTypedState::FCarrier::Reset()
{
	check(IsInGameThread()); bReady=false;
#if WITH_DEV_AUTOMATION_TESTS
	const bool bWasCounted=bCounted; bCounted=false;
#endif
	// Remove storage from tracked state BEFORE running native destruction; a reentrant cancellation cannot reset it twice.
	auto OldStruct=MoveTemp(Struct); auto OldBag=MoveTemp(Bag);
	OldStruct.Reset(); OldBag.Reset();
#if WITH_DEV_AUTOMATION_TESTS
	if (bWasCounted) --Testing::LiveDynamicCarriersOnGT();
#endif
}
FDynamicTypedState::~FDynamicTypedState() { check(IsInGameThread()); DiscardOnGT(); }
TSharedPtr<FDynamicTypedState,ESPMode::ThreadSafe> FDynamicTypedState::CreateOnGT(const FVaRestDynamicResultConfig& Config,FString& Error,FString* Code,
	TSharedPtr<const FVaRestJsonRuleSnapshot, ESPMode::ThreadSafe> RulesOverride)
{
	check(IsInGameThread());
	using namespace DynamicClosurePrivate;
	auto* Runtime=Active;
	if (Code) *Code=TEXT("DynamicTypeUnavailable");
	if (!Runtime || !Runtime->Available())
	{
		const bool bMissingHooks=Runtime && (!Runtime->bEditorHooksAvailable || (WITH_EDITOR &&
			(!Runtime->CurrentEditorBindings || !Runtime->CurrentEditorBindings())));
		if (Code) *Code=bMissingHooks ? TEXT("EditorMutationHooksUnavailable") : TEXT("DynamicTypeChanged");
		Error=bMissingHooks ? TEXT("dynamic materialization requires current editor mutation and transactor hooks") :
			TEXT("dynamic types are unavailable or a mutation is still settling");
		return nullptr;
	}
	const auto State=MakeShared<FDynamicTypedState,ESPMode::ThreadSafe>();
	State->ResponseCarrier.Type=Config.ResponseType; State->ErrorCarrier.Type=Config.ErrorType;
	if (Config.bPositionalResponse && (Config.ResponseType.Kind!=EVaRestTypeKind::Struct || Config.ResponseType.Container!=EVaRestTypeContainer::None))
	{ Error=TEXT("positional output requires a scalar struct descriptor"); return nullptr; }
	State->bPositionalResponse=Config.bPositionalResponse;
	State->ResponseCarrier.bTyped=Typed(Config.ResponseType);
	State->ErrorCarrier.bTyped=Typed(Config.ErrorType);
	State->RuleOwner=Config.RuleOwner; State->bHadRuleOwner=Config.RuleOwner!=nullptr;
	if (Config.RuleOwner && !IsValid(Config.RuleOwner)) { Error=TEXT("dynamic JSON rule owner is unavailable"); return nullptr; }
	State->Rules=RulesOverride ? MoveTemp(RulesOverride) : FVaRestJsonRules::Snapshot(Config.RuleOwner);
	State->Schema=Runtime->Schema;
	State->OwnerRules=FVaRestJsonRules::GetGeneration(Config.RuleOwner);
	State->DefaultRules=FVaRestJsonRules::GetDefaultGeneration();
	State->NativeNames=FVaRestJsonNames::GetGeneration();
	if (Config.RuleOwner) State->Pins.Emplace(Config.RuleOwner);
	FClosureWalk Walk{*Runtime,*State->Rules,State->Pins,{},0,Error};
	for (auto* Carrier:{&State->ResponseCarrier,&State->ErrorCarrier})
	{
		if (const auto* Struct=StructType(Carrier->Type); Struct && !Walk.Struct(Struct)) return nullptr;
		if (Carrier->Type.Kind==EVaRestTypeKind::Enum && !Walk.Pin(Carrier->Type.Type)) return nullptr;
		if (Carrier->bTyped && !(Carrier->Type.Kind==EVaRestTypeKind::Struct && Carrier->Type.Container==EVaRestTypeContainer::None))
		{
			Carrier->BagType.Reset(Runtime->BagFor(Carrier->Type));
			if (!Carrier->BagType.IsValid()) { Error=TEXT("dynamic property-bag descriptor creation failed"); return nullptr; }
		}
	}
	if (!State->CurrentOnGT() || !Runtime->Track(State)) { Error=TEXT("dynamic GT state admission is unavailable"); return nullptr; }
	if (Code) Code->Reset();
	return State;
}
FName FDynamicTypedState::ResultNameOnGT() const { check(IsInGameThread()); return FName(*DynamicClosurePrivate::Name(ResponseCarrier.Type)); }
FName FDynamicTypedState::ErrorNameOnGT() const { check(IsInGameThread()); return FName(*DynamicClosurePrivate::Name(ErrorCarrier.Type)); }
bool FDynamicTypedState::HasTypedResponseOnGT() const { check(IsInGameThread()); return ResponseCarrier.bTyped; }
bool FDynamicTypedState::HasTypedErrorOnGT() const { check(IsInGameThread()); return ErrorCarrier.bTyped; }
bool FDynamicTypedState::NeedsResponseJsonOnGT() const { return HasTypedResponseOnGT() || ResponseCarrier.Type.Kind==EVaRestTypeKind::Json; }
bool FDynamicTypedState::ValidateActivationOnGT(FString& Error) const
{
	check(IsInGameThread());
	if (!bFinalized && CurrentOnGT()) return true;
	Error=TEXT("DynamicTypeChanged: rebuild the request after editor/type/rule changes settle"); return false;
}
bool FDynamicTypedState::CurrentOnGT() const
{
	check(IsInGameThread());
	const auto* Runtime=DynamicClosurePrivate::Active;
	return !bStale && Runtime && Runtime->Available() && Runtime->Schema==Schema && (!bHadRuleOwner || RuleOwner.IsValid()) &&
		FVaRestJsonRules::GetGeneration(RuleOwner.Get())==OwnerRules && FVaRestJsonRules::GetDefaultGeneration()==DefaultRules &&
		NativeNames!=MAX_uint64 && FVaRestJsonNames::GetGeneration()==NativeNames;
}
void FDynamicTypedState::InvalidateOnGT()
{
	check(IsInGameThread());
	if (bStale) return;
	bStale=true;
	DiscardOnGT();
	ResponseCarrier.BagType.Reset(); ErrorCarrier.BagType.Reset();
	Pins.Reset(); Rules.Reset();
}
void FDynamicTypedState::DiscardOnGT()
{
	check(IsInGameThread()); bPreparing=false;
	auto DroppedPreparation=MoveTemp(Preparation);
	ResponseCarrier.Reset(); ErrorCarrier.Reset(); DroppedPreparation.Reset();
}
bool FDynamicTypedState::SetPreparationOnGT(TUniqueFunction<void(FDynamicPrepareToken&)>&& Callback)
{
	check(IsInGameThread());
	if (bFinalized || bStale || bPreparationBound || !Callback) return false;
	bPreparationBound=true; Preparation=MoveTemp(Callback); return true;
}
bool FDynamicTypedState::MaterializeOnGT(FCarrier& Carrier,const FVaRestJson& Json,const FVaRestJsonConvertOptions& Options,FVaRestJsonDecodeReport& Report)
{
	check(IsInGameThread());
	if (!CurrentOnGT()) return false;
	bool bValid=false;
	const UScriptStruct* AllocationType=Carrier.Type.Kind==EVaRestTypeKind::Struct && Carrier.Type.Container==EVaRestTypeContainer::None ?
		DynamicClosurePrivate::StructType(Carrier.Type) : static_cast<const UScriptStruct*>(Carrier.BagType.Get());
	if (!AllocationType || AllocationType->GetStructureSize()>Options.MaxOutputBytes)
	{
		Report.bBudgetExceeded=true;
		return false; // minimum carrier storage preflight, BEFORE InitializeAs/default code
	}
	if (Carrier.Type.Kind==EVaRestTypeKind::Struct && Carrier.Type.Container==EVaRestTypeContainer::None)
	{
		const auto* Type=DynamicClosurePrivate::StructType(Carrier.Type);
		Carrier.Struct.InitializeAs(Type);
#if WITH_DEV_AUTOMATION_TESTS
		Carrier.bCounted=Carrier.Struct.IsValid(); if (Carrier.bCounted) ++Testing::LiveDynamicCarriersOnGT();
#endif
		bValid=bPositionalResponse && &Carrier==&ResponseCarrier
			? FVaRestJsonConvert::JsonArrayToStructInternal(Json,Type,Carrier.Struct.GetMutableMemory(),Options,&Report)
			: !Json.IsValid() || FVaRestJsonConvert::JsonToStruct(Json,Type,Carrier.Struct.GetMutableMemory(),Options,&Report);
	}
	else
	{
		Carrier.Bag.InitializeFromBagStruct(Carrier.BagType.Get());
#if WITH_DEV_AUTOMATION_TESTS
		Carrier.bCounted=Carrier.Bag.IsValid(); if (Carrier.bCounted) ++Testing::LiveDynamicCarriersOnGT();
#endif
		const auto* Desc=Carrier.Bag.FindPropertyDescByName(DynamicClosurePrivate::ValueName);
		const auto* Property=Desc ? Desc->CachedProperty : nullptr;
		void* Memory=Property ? Property->ContainerPtrToValuePtr<void>(Carrier.Bag.GetMutableValue().GetMemory()) : nullptr;
		bValid=Memory && (!Json.IsValid() || FVaRestJsonConvert::JsonToProperty(Json,Property,Memory,Options,&Report));
	}
	Carrier.bReady=bValid && CurrentOnGT();
	if (!Carrier.bReady) Carrier.Reset();
	return Carrier.bReady;
}
void FDynamicTypedState::FinalizeOnGT(FFinish& Finish,const FVaRestJson& Json,FVaRestDynamicTypeInfo& Response,FVaRestDynamicTypeInfo& Error,
	const FVaRestDynamicLimits& Limits,FVaRestDynamicBudgetLease& Lease)
{
	check(IsInGameThread());
	if (bFinalized) return;
	bFinalized=true;
	const bool bPrimaryError=Finish.Error.IsError();
	bHadPrimaryError=bPrimaryError;
	auto& Info=bPrimaryError ? Error : Response;
	auto& Carrier=bPrimaryError ? ErrorCarrier : ResponseCarrier;
	const bool bNeedsTyped=Carrier.bTyped;
	ON_SCOPE_EXIT
	{
		DiscardOnGT(); // cleanup can run qualified native destruction; resample BEFORE terminal acceptance
		if (bNeedsTyped && !CurrentOnGT())
		{
			Info.Availability=EVaRestDynamicAvailability::TypeChanged;
			if (!bPrimaryError)
			{
				Finish.Error.Kind=EVaRestErrorKind::Config; Finish.Error.Code=TEXT("DynamicTypeChanged");
				Finish.Error.Message=TEXT("the dynamic type or JSON rules changed; retry after editor changes settle");
			}
		}
	};
	if (!bNeedsTyped) return;
	Info.Availability=EVaRestDynamicAvailability::Unavailable;
	if (!CurrentOnGT())
	{
		Info.Availability=EVaRestDynamicAvailability::TypeChanged;
		if (!bPrimaryError) { Finish.Error.Kind=EVaRestErrorKind::Config; Finish.Error.Code=TEXT("DynamicTypeChanged"); Finish.Error.Message=TEXT("the dynamic type or JSON rules changed; retry after editor changes settle"); }
		return;
	}
	// Reserve the converter's whole logical allowance before initialization/conversion, then release at GT cleanup.
	// This excludes arbitrary authored/native default allocations and engine-owned destination copies.
	if (!Lease.SetOutputBytes(Limits.MaxOutputBytes))
	{
		Info.Availability=EVaRestDynamicAvailability::BudgetRefused;
		if (!bPrimaryError) { Finish.Error.Kind=EVaRestErrorKind::Config; Finish.Error.Code=TEXT("DecodeBudget"); Finish.Error.Message=TEXT("dynamic typed output admission is unavailable"); }
		return;
	}
	if (!bPrimaryError && Finish.bEnvelopeDataMissing)
	{
		Finish.Error.Kind=EVaRestErrorKind::Decode; Finish.Error.Path=Finish.Envelope->DataPath;
		Finish.Error.Message=TEXT("the envelope data path is not in the dynamic response"); Finish.Error.Body=Finish.EnvelopeBody; return;
	}
	if (bPrimaryError && !Finish.Error.Body.IsValid()) return;
	auto Options=Finish.DecodeOptions; Options.Rules=Rules;
	Options.MaxOutputBytes=Limits.MaxOutputBytes; Options.MaxValues=Limits.MaxValues; Options.MaxWorkUnits=Limits.MaxWorkUnits;
	FVaRestJsonDecodeReport Report;
	const bool bValid=MaterializeOnGT(Carrier,bPrimaryError ? Finish.Error.Body : Json,Options,Report);
	Finish.Response.DecodeWarnings.Append(MoveTemp(Report.Warnings));
	if (bValid)
	{
		Info.Availability=EVaRestDynamicAvailability::Available;
		if (Preparation)
		{
			auto Prepare=MoveTemp(Preparation); // invalidation must not destroy an executing callback target
			bPreparing=true; FDynamicPrepareToken Token(*this); Prepare(Token); bPreparing=false; Prepare.Reset();
			if (!bPreparationConfirmed)
			{
				Info.Availability=EVaRestDynamicAvailability::Unavailable;
				if (!bPrimaryError)
				{
					Finish.Error.Kind=EVaRestErrorKind::Config; Finish.Error.Code=TEXT("DynamicOutputPreparationFailed");
					Finish.Error.Message=TEXT("the internal dynamic output destination was not prepared");
				}
			}
		}
	}
	else
	{
		Info.Availability=Report.bBudgetExceeded ? EVaRestDynamicAvailability::BudgetRefused : EVaRestDynamicAvailability::Unavailable;
		if (bPrimaryError) return; // optional error-body conversion never replaces the original primary failure
		Finish.Error.Kind=Report.bBudgetExceeded ? EVaRestErrorKind::Config : EVaRestErrorKind::Decode;
		if (Report.bBudgetExceeded) Finish.Error.Code=TEXT("DecodeBudget");
		Finish.Error.Message=Report.Errors.IsEmpty() ? TEXT("the JSON value does not fit the declared dynamic type") : Report.Errors[0].Message;
		Finish.Error.Path=Report.Errors.IsEmpty() ? FString() : Report.Errors[0].Path; Finish.Error.Body=Json;
	}
}
void FDynamicTypedState::ValidateTerminalOnGT(FFinish& Finish,FVaRestDynamicTypeInfo& Response,FVaRestDynamicTypeInfo& Error) const
{
	check(IsInGameThread());
	if (!bFinalized || !(bHadPrimaryError ? ErrorCarrier.bTyped : ResponseCarrier.bTyped) || CurrentOnGT()) return;
	(bHadPrimaryError ? Error : Response).Availability=EVaRestDynamicAvailability::TypeChanged;
	if (!bHadPrimaryError)
	{
		Finish.Error.Kind=EVaRestErrorKind::Config; Finish.Error.Code=TEXT("DynamicTypeChanged");
		Finish.Error.Message=TEXT("the dynamic type or JSON rules changed before terminal acceptance");
	}
}
bool FDynamicPrepareToken::Take(const FProperty* Destination,void* Address,bool bError) { return State.TakeOnGT(Destination,Address,bError); }
bool FDynamicPrepareToken::IsError() const { check(IsInGameThread() && State.bPreparing); return State.bHadPrimaryError; }
bool FDynamicPrepareToken::ConfirmPrepared()
{
	check(IsInGameThread());
	if (!State.bPreparing || !State.bPreparationTaken || State.bPreparationConfirmed || !State.CurrentOnGT()) return false;
	State.bPreparationConfirmed=true; return true;
}
bool FDynamicTypedState::TakeOnGT(const FProperty* Destination,void* Address,bool bError)
{
	check(IsInGameThread());
	auto& Carrier=bError ? ErrorCarrier : ResponseCarrier;
	ON_SCOPE_EXIT { Carrier.Reset(); };
	if (!bPreparing || !CurrentOnGT() || !Carrier.bReady || !Destination || !Address) return false;
	Carrier.bReady=false; // one-shot BEFORE native copy code; no address/property escapes this stack
	if (Carrier.Struct.IsValid())
	{
		const auto* Struct=CastField<FStructProperty>(Destination);
		if (!Struct || Struct->ArrayDim!=1 || Struct->Struct!=Carrier.Struct.GetScriptStruct()) return false;
		Struct->CopyCompleteValue(Address,Carrier.Struct.GetMemory());
	}
	else
	{
		const auto* Desc=Carrier.Bag.FindPropertyDescByName(DynamicClosurePrivate::ValueName);
		const auto* Source=Desc ? Desc->CachedProperty : nullptr;
		if (!Source || Destination->ArrayDim!=1 || !Destination->SameType(Source)) return false;
		Destination->CopyCompleteValue(Address,Source->ContainerPtrToValuePtr<void>(Carrier.Bag.GetValue().GetMemory()));
	}
	bPreparationTaken=CurrentOnGT();
	return bPreparationTaken;
}
}

bool FVaRestDynamicNativeTypes::RegisterGameThreadType(const UScriptStruct* Type,FName Contract,TConstArrayView<const UObject*> NativeTypeDependencies)
{
	check(IsInGameThread());
	auto* Runtime=VaRest::Http::DynamicClosurePrivate::Active;
	if (!Runtime || Runtime->bStopped || !IsValid(Type) || Contract.IsNone() || !(Type->StructFlags & STRUCT_Native)) return false;
	if (NativeTypeDependencies.Num()>VaRest::Http::DynamicClosurePrivate::MaxClosureItems) return false;
	VaRest::Http::FDynamicTypeRuntimeData::FNativeContract Declaration;
	Declaration.Name=Contract;
	for (const auto* Dependency:NativeTypeDependencies)
	{
		if (!IsValid(Dependency) || !(Dependency->IsA<UScriptStruct>() || Dependency->IsA<UEnum>())) return false;
		Declaration.Dependencies.AddUnique(TWeakObjectPtr<const UObject>(Dependency));
	}
	for (auto It=Runtime->NativeContracts.CreateIterator();It;++It) if (!It.Key().IsValid()) It.RemoveCurrent();
	const TWeakObjectPtr<const UScriptStruct> Key(Type);
	if (const auto* Existing=Runtime->NativeContracts.Find(Key)) return Existing->Name==Contract && Existing->Dependencies==Declaration.Dependencies;
	if (Runtime->NativeContracts.Num()==256) return false;
	Runtime->Invalidate(); Runtime->NativeContracts.Add(Key,MoveTemp(Declaration)); Runtime->Settle(); return true;
}
void FVaRestDynamicNativeTypes::UnregisterGameThreadType(const UScriptStruct* Type)
{
	check(IsInGameThread());
	if (auto* Runtime=VaRest::Http::DynamicClosurePrivate::Active; Runtime && Runtime->NativeContracts.Contains(TWeakObjectPtr<const UScriptStruct>(Type)))
		{ Runtime->Invalidate(); Runtime->NativeContracts.Remove(TWeakObjectPtr<const UScriptStruct>(Type)); Runtime->Settle(); }
}
