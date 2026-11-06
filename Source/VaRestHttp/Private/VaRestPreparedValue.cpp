// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestPreparedValue.h"
#include "VaRestDynamicBridge.h"
#include "VaRestDynamicClosure.h"
#include "VaRestFrameDispatcher.h"
#include "Misc/ScopeExit.h"

namespace
{
	FVaRestError PreparedValueFailure(const TCHAR* Code, const TCHAR* Message)
	{ FVaRestError Error; Error.Kind = EVaRestErrorKind::Config; Error.Code = Code; Error.Message = Message; return Error; }
}
struct FVaRestPreparedValue::FImpl
{
	~FImpl() { check(IsInGameThread()); }
	TSharedPtr<VaRest::Http::FDynamicCompletion, ESPMode::ThreadSafe> Completion;
	FVaRestDynamicBudgetLease Lease;
	FVaRestDynamicLimits Limits;
	FVaRestDynamicResultConfig FactoryConfig;
	TSharedPtr<const FVaRestJsonRuleSnapshot, ESPMode::ThreadSafe> FactoryRules;
	bool bStarted = false, bPrepared = false, bRetired = false;
};
FVaRestPreparedValue::FVaRestPreparedValue(TSharedRef<FImpl> InState) : State(MoveTemp(InState)) {}
FVaRestPreparedValue::~FVaRestPreparedValue() { check(IsInGameThread()); RetireOnGT(); }

TUniquePtr<FVaRestPreparedValue> FVaRestPreparedValue::CreateOnGT(FVaRestDynamicResultConfig Config,
	TSharedPtr<const FVaRestJsonRuleSnapshot, ESPMode::ThreadSafe> Rules, FVaRestError& Error)
{
	check(IsInGameThread()); Error = {};
	// A protocol value is already JSON; String/Bytes denote a JSON string or binary placeholder here, never a raw body.
	if ((Config.ResponseType.Kind == EVaRestTypeKind::String || Config.ResponseType.Kind == EVaRestTypeKind::Bytes)
		&& Config.ResponseType.Container == EVaRestTypeContainer::None) Config.ResponseType.bJsonValue = true;
	Config.ErrorType = {};
	FVaRestApiValidationContext Validation;
	if (!Config.ResponseType.Validate(Validation, TEXTVIEW("ValueType")) || Config.ResponseType.Kind == EVaRestTypeKind::None
		|| (Config.ResponseType.Kind == EVaRestTypeKind::Json && Config.ResponseType.Container == EVaRestTypeContainer::None))
	{ Error = PreparedValueFailure(TEXT("InvalidPreparedType"), TEXT("A prepared reflected value requires a valid typed descriptor")); return {}; }
	const auto Budget = Config.Budget ? Config.Budget : VaRest::Http::DefaultDynamicBudgetOnGT();
	auto Local = MakeShared<FImpl>();
	if (!Budget || !Budget->TryReserve(Local->Lease))
	{ Error = PreparedValueFailure(TEXT("DynamicCapacity"), TEXT("Prepared value admission is unavailable")); return {}; }
	Local->Limits = Budget->GetLimits();
	Local->Completion = VaRest::Http::FDynamicCompletion::ReserveOnGT();
	if (!Local->Completion)
	{ Error = PreparedValueFailure(TEXT("DynamicCapacity"), TEXT("Prepared value frame admission is unavailable")); return {}; }
	FString Detail, Code;
	const auto Types = VaRest::Http::FDynamicTypedState::CreateOnGT(Config, Detail, &Code, MoveTemp(Rules));
	if (!Types || !Local->Completion->SetTypesOnGT(Types.ToSharedRef()))
	{
		Local->Completion->CancelOnGT();
		Error.Kind = EVaRestErrorKind::Config; Error.Code = Code.IsEmpty() ? TEXT("DynamicTypeUnavailable") : Code;
		Error.Message = Detail.IsEmpty() ? TEXT("Prepared type closure admission is unavailable") : MoveTemp(Detail); return {};
	}
	Local->FactoryConfig = Config; Local->FactoryConfig.Budget = Budget;
	Local->FactoryRules = Types->RulesOnGT();
	return TUniquePtr<FVaRestPreparedValue>(new FVaRestPreparedValue(Local));
}
bool FVaRestPreparedValue::IsCurrentForActivationOnGT() const
{
	check(IsInGameThread()); const auto Local = State;
	if (!Local || Local->bStarted || Local->bRetired || !Local->Completion || Local->Completion->IsStopped()) return false;
	const auto Types = Local->Completion->TypesOnGT(); FString Detail;
	return Types && Types->ValidateActivationOnGT(Detail);
}
TUniquePtr<FVaRestPreparedValue> FVaRestPreparedValue::CloneOnGT(FVaRestError& Error) const
{
	check(IsInGameThread());
	if (!IsCurrentForActivationOnGT())
	{ Error = PreparedValueFailure(TEXT("DynamicTypeChanged"), TEXT("The activation type or JSON rules changed")); return {}; }
	// The admitted root still pins every reflected descriptor and owner while the factory inputs are read.
	const auto Local = State; return CreateOnGT(Local->FactoryConfig, Local->FactoryRules, Error);
}
FVaRestError FVaRestPreparedValue::PrepareOnGT(const FVaRestJson& Value, const FVaRestJsonConvertOptions& Options,
	TFunctionRef<void(FVaRestPreparedValueToken&)> Prepare, TArray<FVaRestJsonError>& Warnings)
{
	check(IsInGameThread()); const auto Local = State;
	if (!Local || Local->bRetired || Local->bStarted || !Local->Completion || Local->Completion->IsStopped())
		return PreparedValueFailure(TEXT("DynamicValueUnavailable"), TEXT("The prepared value was retired or already consumed"));
	Local->bStarted = true;
	if (!Value.IsValid())
	{
		if (Options.bStrict)
		{ FVaRestError Error; Error.Kind = EVaRestErrorKind::Decode; Error.Path = TEXT("[0]"); Error.Message = TEXT("missing protocol argument"); return Error; }
		FVaRestJsonError Warning; Warning.Path = TEXT("[0]"); Warning.Message = TEXT("missing protocol argument; kept its authored default"); Warnings.Add(MoveTemp(Warning));
	}
	const auto Types = Local->Completion->TypesOnGT();
	if (!Types) return PreparedValueFailure(TEXT("DynamicTypeChanged"), TEXT("The prepared type closure is unavailable"));
	ON_SCOPE_EXIT { Types->DiscardOnGT(); Local->Lease.SetOutputBytes(0); };
	if (!Types->SetPreparationOnGT([&Prepare](VaRest::Http::FDynamicPrepareToken& Native)
	{
		FVaRestPreparedValueToken Token(&Native); Prepare(Token);
	})) return PreparedValueFailure(TEXT("DynamicOutputPreparationFailed"), TEXT("The prepared destination was not admitted"));
	VaRest::Http::FFinish Finish; Finish.DecodeOptions = Options;
	FVaRestDynamicTypeInfo ResponseInfo, ErrorInfo;
	Types->FinalizeOnGT(Finish, Value, ResponseInfo, ErrorInfo, Local->Limits, Local->Lease);
	Types->ValidateTerminalOnGT(Finish, ResponseInfo, ErrorInfo);
	Warnings.Append(MoveTemp(Finish.Response.DecodeWarnings));
	if (Local->bRetired) return PreparedValueFailure(TEXT("DynamicValueRetired"), TEXT("The prepared value scope closed during conversion"));
	Local->bPrepared = !Finish.Error.IsError() && ResponseInfo.Availability == EVaRestDynamicAvailability::Available;
	return MoveTemp(Finish.Error);
}
bool FVaRestPreparedValue::IsCurrentForDeliveryOnGT() const
{
	check(IsInGameThread()); const auto Local = State;
	if (!Local || Local->bRetired || !Local->bPrepared || !Local->Completion || Local->Completion->IsStopped()) return false;
	const auto Types = Local->Completion->TypesOnGT(); return Types && Types->IsCurrentForDeliveryOnGT();
}
void FVaRestPreparedValue::RetireOnGT()
{
	check(IsInGameThread()); const auto Local = State; if (!Local || Local->bRetired) return;
	Local->bRetired = true; Local->bPrepared = false;
	Local->FactoryConfig = {}; Local->FactoryRules.Reset();
	if (Local->Completion)
	{
		if (const auto Types = Local->Completion->TypesOnGT()) Types->InvalidateOnGT();
		Local->Completion->CancelOnGT(); Local->Completion.Reset();
	}
	Local->Lease.Reset();
}
bool FVaRestPreparedValueToken::Take(const FProperty* Destination, void* Address)
{
	check(IsInGameThread()); return Token && static_cast<VaRest::Http::FDynamicPrepareToken*>(Token)->Take(Destination, Address);
}
bool FVaRestPreparedValueToken::ConfirmPrepared()
{
	check(IsInGameThread()); return Token && static_cast<VaRest::Http::FDynamicPrepareToken*>(Token)->ConfirmPrepared();
}
