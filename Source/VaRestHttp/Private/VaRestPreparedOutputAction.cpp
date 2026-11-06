// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestPreparedOutputAction.h"
#include "VaRestPreparedValue.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "VaRestBlueprintFrame.h"
#include "Misc/ScopeExit.h"
#include "UObject/Stack.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace
{
	FVaRestError OutputFailure(const TCHAR* Code, const TCHAR* Message)
	{ FVaRestError Error; Error.Kind = EVaRestErrorKind::Config; Error.Code = Code; Error.Message = Message; return Error; }
	template <typename D> UFunction* ResolveOutput(const D& Delegate, FName Name)
	{
		auto* Target = Delegate.GetUObject();
		auto* Function = IsValid(Target) ? Target->FindFunction(Delegate.GetFunctionName()) : nullptr;
		const auto* Property = FindFProperty<FDelegateProperty>(UVaRestPreparedOutputAction::StaticClass(), Name);
		return Function && Property && Property->SignatureFunction && Function->IsSignatureCompatibleWith(Property->SignatureFunction) ? Function : nullptr;
	}
	struct FPreparedDelivery
	{
		TUniquePtr<FVaRestPreparedValue> Value;
		TFunction<bool()> Current;
		FVaRestPreparedValueToken* Preparation = nullptr;
		TArray<FVaRestJsonError> Warnings;
		bool bConfirmed = false, bContinuing = false, bConsumed = false;
		void Retire() { check(IsInGameThread()); if (Value) Value->RetireOnGT(); Current = {}; }
		~FPreparedDelivery() { check(IsInGameThread()); }
	};
}
struct FVaRestPreparedOutputState
{
	~FVaRestPreparedOutputState() { check(IsInGameThread()); }
	TUniquePtr<FVaRestPreparedValue> Root;
	TMap<FString, TSharedPtr<FPreparedDelivery>> Deliveries;
	TWeakObjectPtr<const UObject> Owner;
	TWeakObjectPtr<UObject> Target;
	TWeakObjectPtr<UClass> TargetClass;
	TWeakObjectPtr<UFunction> PrepareFunction, ContinueFunction, UnavailableFunction;
	FString Preparing, Continuing;
	bool bConfigured = false, bBound = false, bAdmitted = false, bRetired = false;
	bool bPositional = false;
};
UVaRestPreparedOutputAction::UVaRestPreparedOutputAction(const FObjectInitializer& Initializer) : Super(Initializer), PreparedState(MakeShared<FVaRestPreparedOutputState>()) {}
UVaRestPreparedOutputAction::UVaRestPreparedOutputAction(FVTableHelper& Helper) : Super(Helper), PreparedState(MakeShared<FVaRestPreparedOutputState>()) {}
UVaRestPreparedOutputAction::~UVaRestPreparedOutputAction() = default;
bool UVaRestPreparedOutputAction::ConfigureOutputType(const FVaRestTypeRef& Type)
{
	check(IsInGameThread()); const auto State = PreparedState;
	if (!State || State->bAdmitted || State->bRetired || State->bConfigured) return false;
	State->bConfigured = true; OutputType = Type; return true;
}
bool UVaRestPreparedOutputAction::BindOutput(FVaRestProtocolPrepare Prepare, FVaRestProtocolContinue Continue, FVaRestProtocolUnavailable Unavailable)
{
	check(IsInGameThread()); const auto State = PreparedState;
	if (!State || State->bAdmitted || State->bRetired || State->bBound) return false;
	auto* Target = Prepare.GetUObject();
	if (!IsValid(Target) || Continue.GetUObject() != Target || Unavailable.GetUObject() != Target) return false;
	const auto* Class = Cast<UBlueprintGeneratedClass>(Target->GetClass());
	if (!VaRest::Http::HasPreparedBlueprintFrameOnGT(Target,Class)) return false;
	const FString Prefix(TEXT("__VaRest_Prepare_")), Name = Prepare.GetFunctionName().ToString();
	if (!Name.StartsWith(Prefix, ESearchCase::CaseSensitive)) return false;
	const FString Identity = Name.Mid(Prefix.Len()); FGuid Guid;
	if (!FGuid::ParseExact(Identity, EGuidFormats::Digits, Guid) || !Guid.IsValid()
		|| Continue.GetFunctionName() != FName(FString(TEXT("__VaRest_Continue_")) + Identity)
		|| Unavailable.GetFunctionName() != FName(FString(TEXT("__VaRest_Unavailable_")) + Identity)) return false;
	auto* PrepareFn = ResolveOutput(Prepare, GET_MEMBER_NAME_CHECKED(UVaRestPreparedOutputAction, PrepareDelegate));
	auto* ContinueFn = ResolveOutput(Continue, GET_MEMBER_NAME_CHECKED(UVaRestPreparedOutputAction, ContinueDelegate));
	auto* UnavailableFn = ResolveOutput(Unavailable, GET_MEMBER_NAME_CHECKED(UVaRestPreparedOutputAction, UnavailableDelegate));
	if (!PrepareFn || !ContinueFn || !UnavailableFn) return false;
	State->bBound = true; State->Target = Target; State->TargetClass = Target->GetClass();
	State->PrepareFunction = PrepareFn; State->ContinueFunction = ContinueFn; State->UnavailableFunction = UnavailableFn;
	PrepareDelegate = MoveTemp(Prepare); ContinueDelegate = MoveTemp(Continue); UnavailableDelegate = MoveTemp(Unavailable); return true;
}
bool UVaRestPreparedOutputAction::TargetCurrentOnGT() const
{
	check(IsInGameThread()); const auto State = PreparedState; auto* Target = State ? State->Target.Get() : nullptr;
	return State && !State->bRetired && State->bBound && IsValid(Target) && Target->GetClass() == State->TargetClass.Get()
		&& State->PrepareFunction.IsValid() && State->ContinueFunction.IsValid() && State->UnavailableFunction.IsValid()
		&& ResolveOutput(PrepareDelegate, GET_MEMBER_NAME_CHECKED(UVaRestPreparedOutputAction, PrepareDelegate)) == State->PrepareFunction.Get()
		&& ResolveOutput(ContinueDelegate, GET_MEMBER_NAME_CHECKED(UVaRestPreparedOutputAction, ContinueDelegate)) == State->ContinueFunction.Get()
		&& ResolveOutput(UnavailableDelegate, GET_MEMBER_NAME_CHECKED(UVaRestPreparedOutputAction, UnavailableDelegate)) == State->UnavailableFunction.Get();
}
bool UVaRestPreparedOutputAction::AdmitPreparedOutputOnGT(const UObject* Owner, const UObject* RuleOwner,
	TSharedPtr<FVaRestDynamicBudget, ESPMode::ThreadSafe> Budget, const FVaRestJsonConvertOptions& Options, FVaRestError& Error)
{
	check(IsInGameThread()); const auto State = PreparedState;
	if (!State || State->bRetired || State->bAdmitted) { Error = OutputFailure(TEXT("DynamicScopeClosed"), TEXT("The output scope is closed")); return false; }
	State->bAdmitted = true; State->Owner = Owner;
	if (!State->bConfigured) return true;
	if (!IsValid(Owner) || State->Target.Get() != Owner || !TargetCurrentOnGT())
	{ Error = OutputFailure(TEXT("DynamicOutputNotBound"), TEXT("Typed output requires a generated binding on its context owner")); return false; }
	FVaRestDynamicResultConfig Config; Config.ResponseType = OutputType; Config.RuleOwner = RuleOwner; Config.Budget = MoveTemp(Budget);
	Config.bPositionalResponse = State->bPositional;
	State->Root = FVaRestPreparedValue::CreateOnGT(MoveTemp(Config), Options.Rules, Error); return bool(State->Root);
}
bool UVaRestPreparedOutputAction::HasPreparedOutputOnGT() const
{ check(IsInGameThread()); return PreparedState && PreparedState->bConfigured; }
TArray<FVaRestJsonError> UVaRestPreparedOutputAction::GetPreparedDecodeWarnings(UVaRestPreparedOutputAction* Action, const FString& RequestIdText)
{
	check(IsInGameThread()); const auto State = IsValid(Action) ? Action->PreparedState : nullptr;
	const auto Delivery = State && !State->bRetired ? State->Deliveries.FindRef(RequestIdText) : nullptr;
	return Delivery ? Delivery->Warnings : TArray<FVaRestJsonError>();
}
void UVaRestPreparedOutputAction::SetPositionalOutputOnGT(bool bPositional)
{
	check(IsInGameThread()); const auto State = PreparedState;
	if (State && !State->bAdmitted && !State->bRetired) State->bPositional = bPositional;
}
FString UVaRestPreparedOutputAction::PrepareOutputOnGT(const FVaRestJson& Value, const FVaRestJsonConvertOptions& Options,
	TFunction<bool()> Current, FVaRestError& Error)
{
	check(IsInGameThread()); Error = {}; const auto State = PreparedState;
	if (!HasPreparedOutputOnGT()) return {};
	if (!State || !State->Root || State->bRetired || !State->Owner.IsValid() || !CanDeliverPreparedOutputOnGT()
		|| !TargetCurrentOnGT() || !Current || !Current())
	{ Error = OutputFailure(TEXT("DynamicOutputUnavailable"), TEXT("The output target or protocol scope changed")); return {}; }
	if (State->Deliveries.Num() >= 64)
	{ Error = OutputFailure(TEXT("DynamicCapacity"), TEXT("Nested prepared output capacity was exhausted")); return {}; }
	auto Delivery = MakeShared<FPreparedDelivery>(); Delivery->Current = MoveTemp(Current);
	Delivery->Value = State->Root->CloneOnGT(Error); if (!Delivery->Value) return {};
	const TStrongObjectPtr<UVaRestPreparedOutputAction> Self(this);
	const FString Cookie = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	State->Deliveries.Add(Cookie, Delivery);
	Error = Delivery->Value->PrepareOnGT(Value, Options, [&](FVaRestPreparedValueToken& Token)
	{
		if (State->bRetired || !CanDeliverPreparedOutputOnGT() || !TargetCurrentOnGT() || !Delivery->Current || !Delivery->Current()) return;
		TGuardValue<FString> CookieGuard(State->Preparing, Cookie);
		TGuardValue<FVaRestPreparedValueToken*> TokenGuard(Delivery->Preparation, &Token);
		const TStrongObjectPtr<UObject> Target(State->Target.Get()); const auto Callback = PrepareDelegate;
		Callback.Execute(this, Cookie);
	}, Delivery->Warnings);
	if (!Error.IsError() && !Delivery->bConfirmed) Error = OutputFailure(TEXT("DynamicOutputPreparationFailed"), TEXT("The generated output preparation was not confirmed"));
	if (Error.IsError()) { Delivery->Retire(); State->Deliveries.Remove(Cookie); return {}; }
	return Cookie;
}
bool UVaRestPreparedOutputAction::TakeResponse(int32&) { checkNoEntry(); return false; }
DEFINE_FUNCTION(UVaRestPreparedOutputAction::execTakeResponse)
{
	FProperty* Chain = Stack.Code ? nullptr : CastField<FProperty>(Stack.PropertyChainForCompiledIn);
	uint8 Placeholder = 0; Stack.MostRecentProperty = nullptr; Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FProperty>(Stack.Code ? nullptr : &Placeholder);
	const FProperty* Property = Stack.MostRecentProperty ? Stack.MostRecentProperty : Chain;
	void* Address = Stack.MostRecentPropertyAddress; P_FINISH;
	P_NATIVE_BEGIN; *static_cast<bool*>(RESULT_PARAM) = P_THIS->TakeOnGT(Property, Address); P_NATIVE_END;
}
bool UVaRestPreparedOutputAction::TakeOnGT(const FProperty* Property, void* Address)
{
	check(IsInGameThread()); const auto State = PreparedState;
	const auto Delivery = State ? State->Deliveries.FindRef(State->Preparing) : nullptr;
	return Delivery && Delivery->Preparation && !Delivery->bConfirmed && CanDeliverPreparedOutputOnGT() && TargetCurrentOnGT()
		&& Delivery->Current && Delivery->Current() && Delivery->Preparation->Take(Property, Address);
}
bool UVaRestPreparedOutputAction::ConfirmPrepared(const FString& RequestIdText)
{
	check(IsInGameThread()); const auto State = PreparedState;
	const auto Delivery = State && State->Preparing == RequestIdText ? State->Deliveries.FindRef(RequestIdText) : nullptr;
	if (!Delivery || !Delivery->Preparation || Delivery->bConfirmed || !CanDeliverPreparedOutputOnGT() || !TargetCurrentOnGT()
		|| !Delivery->Current || !Delivery->Current() || !Delivery->Preparation->ConfirmPrepared()) return false;
	Delivery->bConfirmed = true; return true;
}
bool UVaRestPreparedOutputAction::AcceptPreparedOutput(const FString& StoredCookie, const FString& RequestIdText)
{
	check(IsInGameThread()); const auto State = PreparedState;
	const auto Delivery = State && StoredCookie == RequestIdText && State->Continuing == RequestIdText ? State->Deliveries.FindRef(RequestIdText) : nullptr;
	if (!Delivery || !Delivery->bContinuing || Delivery->bConsumed || !Delivery->bConfirmed || !CanDeliverPreparedOutputOnGT()
		|| !TargetCurrentOnGT() || !Delivery->Current || !Delivery->Current() || !Delivery->Value->IsCurrentForDeliveryOnGT()) return false;
	Delivery->bConsumed = true; return true;
}
bool UVaRestPreparedOutputAction::DeliverPreparedOutputOnGT(const FString& Cookie)
{
	check(IsInGameThread()); const auto State = PreparedState;
	const auto Delivery = State ? State->Deliveries.FindRef(Cookie) : nullptr;
	if (!Delivery) return false;
	const TStrongObjectPtr<UVaRestPreparedOutputAction> Self(this);
	ON_SCOPE_EXIT { Delivery->Retire(); State->Deliveries.Remove(Cookie); };
	if (!CanDeliverPreparedOutputOnGT() || State->bRetired || !State->Owner.IsValid()) return false;
	if (!TargetCurrentOnGT() || !Delivery->Current || !Delivery->Current() || !Delivery->Value->IsCurrentForDeliveryOnGT())
	{ UnavailableOnGT(Cookie, OutputFailure(TEXT("DynamicOutputChanged"), TEXT("The output type, target or protocol scope changed after preparation"))); return false; }
	const TStrongObjectPtr<UObject> Target(State->Target.Get()); const auto Callback = ContinueDelegate;
	{ TGuardValue<FString> CookieGuard(State->Continuing, Cookie); TGuardValue<bool> Guard(Delivery->bContinuing, true); Callback.Execute(this, Cookie, true, FVaRestError()); }
	if (!Delivery->bConsumed && CanDeliverPreparedOutputOnGT() && !State->bRetired)
		UnavailableOnGT(Cookie, OutputFailure(TEXT("DynamicOutputSuperseded"), TEXT("A nested output replaced the prepared engine storage")));
	return Delivery->bConsumed;
}
void UVaRestPreparedOutputAction::UnavailableOnGT(const FString& Cookie, FVaRestError Error)
{
	check(IsInGameThread());
	const auto Callback = UnavailableDelegate;
	if (ResolveOutput(Callback, GET_MEMBER_NAME_CHECKED(UVaRestPreparedOutputAction, UnavailableDelegate)))
	{ const TStrongObjectPtr<const UObject> Target(Callback.GetUObject()); Callback.Execute(this, Cookie, Error); }
	if (CanDeliverPreparedOutputOnGT()) OnTypedOutputUnavailable.Broadcast(Error);
}
void UVaRestPreparedOutputAction::RetirePreparedOutputOnGT()
{
	check(IsInGameThread()); const auto State = PreparedState; if (!State || State->bRetired) return;
	State->bRetired = true;
	for (const auto& Pair : State->Deliveries) Pair.Value->Retire(); State->Deliveries.Reset();
	if (State->Root) { State->Root->RetireOnGT(); State->Root.Reset(); }
	State->Owner.Reset(); State->Target.Reset(); State->TargetClass.Reset();
	State->PrepareFunction.Reset(); State->ContinueFunction.Reset(); State->UnavailableFunction.Reset();
	OutputType = {}; PrepareDelegate.Unbind(); ContinueDelegate.Unbind(); UnavailableDelegate.Unbind();
}
void UVaRestPreparedOutputAction::BeginDestroy() { RetirePreparedOutputOnGT(); Super::BeginDestroy(); }
