// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestDynamicRequest.h"
#include "VaRestDynamicBridge.h"
#include "VaRestDynamicResultData.h"
#include "VaRestFrameDispatcher.h"
#include "VaRestClientState.h"
#include "VaRestDynamicClosure.h"
#include "VaRestAuth.h"
#include "VaRestDynamicRequestAccess.h"
#include "VaRestDecode.h"
#include "VaRestHttpTesting.h"
#include "VaRestHttpStats.h"
#include "VaRestTraceExcerpts.h"
#include "HAL/FileManager.h"
#include "Tasks/Task.h"

namespace VaRest::Http
{
/** Worker-visible fields are plain native data ONLY; reflected configuration is validated and not retained here. */
struct FDynamicHandlerData
{
	FString Problem=TEXT("dynamic activation is unavailable"),ProblemCode=TEXT("DynamicUnavailable");
	FVaRestDynamicLimits Limits;
	FVaRestDynamicBudgetLease Lease;
	TSharedPtr<FDynamicCompletion,ESPMode::ThreadSafe> Completion;
	TSharedRef<FDynamicResultData,ESPMode::ThreadSafe> Parsed=MakeShared<FDynamicResultData,ESPMode::ThreadSafe>();
	bool bPrepared=false,bDecoded=false,bDiscarded=false,bBudgetRefused=false,bFinalized=false;
	bool bResponseJson=false,bTypedResponse=false,bTypedError=false;
};
namespace DynamicRequestPrivate
{
FName TypeName(EVaRestTypeKind Kind)
{
	switch (Kind)
	{
	case EVaRestTypeKind::Json:return TEXT("Json");
	case EVaRestTypeKind::String:return TEXT("String");
	case EVaRestTypeKind::Bytes:return TEXT("Bytes");
	default:return NAME_None;
	}
}
bool RawType(const FVaRestTypeRef& Type)
{
	return !Type.bJsonValue && Type.Container==EVaRestTypeContainer::None && (Type.Kind==EVaRestTypeKind::None ||
		Type.Kind==EVaRestTypeKind::Json || Type.Kind==EVaRestTypeKind::String || Type.Kind==EVaRestTypeKind::Bytes);
}
void RefuseBody(FDynamicHandlerData& Data,FFinish& Finish)
{
	Data.bBudgetRefused=true;
	Finish.Body.Reset();
	Finish.Response.Body.Reset();
	if (!Finish.Error.IsError())
	{
		Finish.Error.Kind=EVaRestErrorKind::Config;
		Finish.Error.Code=TEXT("DecodeBudget");
		Finish.Error.Message=TEXT("the dynamic response exceeds its admitted decode budget");
	}
	// A primary HTTP/Server/Auth error remains primary; unavailable typed/error body is explicit in the shell.
}
void DecodeNative(FDynamicHandlerData& Native,FFinish& Finish)
{
	if (Native.bResponseJson) { Native.bDecoded=DecodeJson(Finish,Native.Parsed->Json); return; }
	switch (Native.Parsed->Type.Kind)
	{
	case EVaRestTypeKind::Json:Native.bDecoded=DecodeJson(Finish,Native.Parsed->Json);break;
	case EVaRestTypeKind::String:DecodeText(Finish,Native.Parsed->Text);Native.bDecoded=!Finish.Error.IsError();break;
	case EVaRestTypeKind::Bytes:Native.Parsed->Bytes=MoveTemp(Finish.Body);Native.bDecoded=true;break;
	case EVaRestTypeKind::None:
		CheckEnvelopePredicate(Finish);
		if (Finish.Error.IsError()) Finish.Response.Body=MoveTemp(Finish.Body);else ReleaseBody(Finish);
		Native.bDecoded=!Finish.Error.IsError();break;
	default:break;
	}
}
void RecordNativeBudget(FDynamicHandlerData& Native,FFinish& Finish)
{
	if (!Finish.bLogicalDecodeBudgetRefused) return;
	Native.bBudgetRefused=true;
	if (!Finish.Error.IsError() || Finish.Error.Kind==EVaRestErrorKind::Decode)
	{
		Finish.Error.Kind=EVaRestErrorKind::Config; Finish.Error.Code=TEXT("DecodeBudget");
		Finish.Error.Message=TEXT("the dynamic JSON parser exceeded its admitted node/work limits");
	}
}
/** Actual worker owns ONLY this native payload, never the original handler/callbacks/clock/config/exchange. */
struct FNativeDynamicWork
{
	TSharedPtr<FDynamicHandlerData,ESPMode::ThreadSafe> Data;
	TSharedPtr<std::atomic<bool>,ESPMode::ThreadSafe> Abort;
	FFinish Finish; // Clock and all DecodeOptions callbacks/rules are removed on GT before publication
	bool bDidRun=false,bCleanupRan=false;
	~FNativeDynamicWork() { check(!Finish.Clock && !Finish.DecodeOptions.BytesWriter && !Finish.DecodeOptions.BytesResolver); }
	void Run()
	{
		LLM_SCOPE_BYTAG(VaRest);
		bDidRun=true;
		const auto IsCanceled=[this] { return Abort->load(std::memory_order_relaxed) || Data->Completion->IsStopped(); };
		const bool bCanceled=IsCanceled();
		if (!Finish.TempFile.IsEmpty() && (bCanceled || Finish.Error.IsError()))
		{
			if (Finish.Error.Kind==EVaRestErrorKind::Http && Finish.bBodyInFile) LoadFilePrefix(Finish.TempFile,Finish.Body);
			IFileManager::Get().Delete(*Finish.TempFile,false,false,true);
		}
		if (!Finish.Error.IsError() && !bCanceled)
		{
			if (Finish.SaveTo.IsEmpty() || FinalizeSaveTo(Finish,IsCanceled)) DecodeNative(*Data,Finish);
		}
		else if (Finish.Error.Kind==EVaRestErrorKind::Http && !Data->Completion->IsStopped()) ProcessErrorBody(Finish);
		RecordNativeBudget(*Data,Finish);
#if !UE_BUILD_SHIPPING
		++Testing::Counters().DecodeTasksHandedBack;
#endif
	}
	void CleanupDroppedFile()
	{
		// This task was selected and published on GTK together with the decoder, before any observed Stop. Its genuine
		// decoder prerequisite separates file ownership; no GTK IO, destructor IO or new late worker is introduced.
		bCleanupRan=true;
		if (!bDidRun && !Finish.TempFile.IsEmpty()) IFileManager::Get().Delete(*Finish.TempFile,false,false,true);
	}
};
struct FNativeDynamicWorker
{
	TSharedPtr<FNativeDynamicWork,ESPMode::ThreadSafe> Work;
	TSharedPtr<FDynamicCompletion,ESPMode::ThreadSafe> Completion;
	~FNativeDynamicWorker()
	{
		// Last worker payload use precedes release. GT acquire then reads bDidRun/result, even if the task was dropped.
		Work.Reset();
		Completion->NotifyReadyFromWorker();
	}
};
struct FOrdinaryDynamicPending
{
	~FOrdinaryDynamicPending() { check(IsInGameThread()); if (!bConsumed && Abandon) Abandon(); }
	TUniquePtr<IResultHandler> Handler;
	TSharedPtr<IVaRestClock,ESPMode::ThreadSafe> Clock;
	FVaRestJsonConvertOptions DecodeOptions;
	TUniqueFunction<void()> Abandon;
	double StartedAt=-1;
	bool bTimeDecode=false,bConsumed=false;
};
}

TResultHandler<FVaRestDynamicResult>::TResultHandler() : Data(MakeShared<FDynamicHandlerData,ESPMode::ThreadSafe>()) {}
TResultHandler<FVaRestDynamicResult>::TResultHandler(FVaRestDynamicResultConfig&& Config) : TResultHandler()
{
	if (!IsInGameThread()) { Data->Problem=TEXT("RequestDynamic requires game-thread activation");Data->ProblemCode=TEXT("GameThreadRequired");return; }
	if (FClientState::IsShuttingDown()) { Data->Problem=TEXT("dynamic activation is unavailable during HTTP shutdown");Data->ProblemCode=TEXT("LifecycleClosed");return; }
	FVaRestApiValidationContext Validation;
	if (!Config.ResponseType.Validate(Validation,TEXTVIEW("ResponseType")) || !Config.ErrorType.Validate(Validation,TEXTVIEW("ErrorType")))
	{
		Data->Problem=TEXT("the dynamic response/error descriptor is invalid");
		Data->ProblemCode=TEXT("InvalidDynamicType");
		return;
	}
	Data->Parsed->Type.Kind=Config.ResponseType.Kind;
	Data->Parsed->Type.Container=Config.ResponseType.Container;
	Data->Parsed->Type.bJsonValue=Config.ResponseType.bJsonValue;
	Data->Parsed->Type.Name=DynamicRequestPrivate::TypeName(Config.ResponseType.Kind);
	Data->Parsed->ErrorType.Kind=Config.ErrorType.Kind;
	Data->Parsed->ErrorType.Container=Config.ErrorType.Container;
	Data->Parsed->ErrorType.bJsonValue=Config.ErrorType.bJsonValue;
	Data->Parsed->ErrorType.Name=DynamicRequestPrivate::TypeName(Config.ErrorType.Kind);
	const auto Budget=Config.Budget ? Config.Budget : DefaultDynamicBudgetOnGT();
	if (!Budget || !Budget->TryReserve(Data->Lease)) { Data->Problem=TEXT("dynamic request admission is unavailable");Data->ProblemCode=TEXT("DynamicCapacity");return; }
	Data->Limits=Budget->GetLimits();
	Data->Completion=FDynamicCompletion::ReserveOnGT();
	if (!Data->Completion)
	{
		Data->Lease.Reset();
		Data->Problem=TEXT("dynamic frame admission is unavailable");
		Data->ProblemCode=TEXT("DynamicCapacity");
		return;
	}
	Data->bResponseJson=Config.ResponseType.Kind==EVaRestTypeKind::Json;
	Data->bTypedResponse=!DynamicRequestPrivate::RawType(Config.ResponseType);
	Data->bTypedError=!DynamicRequestPrivate::RawType(Config.ErrorType);
	if (Data->bTypedResponse || Data->bTypedError)
	{
		const auto Types=FDynamicTypedState::CreateOnGT(Config,Data->Problem,&Data->ProblemCode);
		if (!Types || !Data->Completion->SetTypesOnGT(Types.ToSharedRef()))
		{
			if (Data->Problem.IsEmpty()) Data->Problem=TEXT("dynamic GT type admission is unavailable");
			Data->Completion->CancelOnGT(); Data->Completion.Reset(); Data->Lease.Reset(); return;
		}
		Data->Parsed->Type.Name=Types->ResultNameOnGT();
		Data->Parsed->ErrorType.Name=Types->ErrorNameOnGT();
		Data->bResponseJson=Types->NeedsResponseJsonOnGT();
	}
	Data->Problem.Reset();
	Data->ProblemCode.Reset();
}
TResultHandler<FVaRestDynamicResult>::~TResultHandler() = default;
bool TResultHandler<FVaRestDynamicResult>::SetPreparationOnGT(TUniqueFunction<void(FDynamicPrepareToken&)>&& Callback)
{
	check(IsInGameThread());
	const auto Types=Data->Completion ? Data->Completion->TypesOnGT() : nullptr;
	if (!Types || !Callback) return false;
	return Types->SetPreparationOnGT(MoveTemp(Callback));
}
bool TResultHandler<FVaRestDynamicResult>::SetActionLifetimeOnGT(TUniqueFunction<void()>&& Cleanup)
{
	check(IsInGameThread());
	return Data->Problem.IsEmpty() && Data->Completion && Data->Completion->SetActionLifetimeOnGT(MoveTemp(Cleanup));
}
TWeakPtr<FDynamicTypedState,ESPMode::ThreadSafe> TResultHandler<FVaRestDynamicResult>::GetTypesOnGT() const
{
	check(IsInGameThread());
	return Data->Completion ? Data->Completion->TypesOnGT() : nullptr;
}
FName TResultHandler<FVaRestDynamicResult>::GetResultType() const { return Data->Parsed->Type.Name; }
bool TResultHandler<FVaRestDynamicResult>::DecodesBody() const { return Data->bResponseJson || (Data->Parsed->Type.Kind!=EVaRestTypeKind::Bytes && Data->Parsed->Type.Kind!=EVaRestTypeKind::None); }
bool TResultHandler<FVaRestDynamicResult>::DecodesJson() const { return Data->bResponseJson; }
bool TResultHandler<FVaRestDynamicResult>::AppliesEnvelope() const { return DecodesJson() || Data->Parsed->Type.Kind==EVaRestTypeKind::None; }
EResultCompletionPolicy TResultHandler<FVaRestDynamicResult>::GetCompletionPolicy() const
{
	return Data->Completion ? EResultCompletionPolicy::LaterFrame : EResultCompletionPolicy::Immediate;
}
bool TResultHandler<FVaRestDynamicResult>::DeferCompletionOnGT(TUniqueFunction<void()>& Kernel)
{
	check(IsInGameThread());
	return Data->Completion && Data->Completion->DeferOnGT(Kernel);
}
int64 TResultHandler<FVaRestDynamicResult>::GetDynamicMaxInputBytes() const { return Data->Problem.IsEmpty() ? Data->Limits.MaxInputBytes : 0; }
void TResultHandler<FVaRestDynamicResult>::ConfigureParseOptions(FVaRestJsonParseOptions& Options) const
{
	if (!Data->Problem.IsEmpty()) return;
	Options.MaxValues=Options.MaxValues>0 ? FMath::Min(Options.MaxValues,Data->Limits.MaxParseValues) : Data->Limits.MaxParseValues;
	Options.MaxWorkUnits=Options.MaxWorkUnits>0 ? FMath::Min(Options.MaxWorkUnits,Data->Limits.MaxParseWorkUnits) : Data->Limits.MaxParseWorkUnits;
}
bool TResultHandler<FVaRestDynamicResult>::ValidateConfiguration(FString& Error) const
{
	Error=Data->Problem;
	if (!Error.IsEmpty()) return false;
	if (IsInGameThread() && (Data->bTypedResponse || Data->bTypedError))
	{
		const auto Types=Data->Completion ? Data->Completion->TypesOnGT() : nullptr;
		if (!Types) { Error=TEXT("dynamic GT type state is unavailable"); Data->ProblemCode=TEXT("DynamicTypeChanged"); return false; }
		if (!Types->ValidateActivationOnGT(Error)) { Data->ProblemCode=TEXT("DynamicTypeChanged"); return false; }
	}
	return true;
}
FString TResultHandler<FVaRestDynamicResult>::GetConfigurationFailureCode() const { return Data->ProblemCode; }
bool TResultHandler<FVaRestDynamicResult>::PrepareDecodeOnGT(FFinish& Finish)
{
	check(IsInGameThread());
	if (Data->bPrepared) return !Data->bBudgetRefused;
	Data->bPrepared=true;
	if (Finish.Error.Code==TEXT("DecodeBudget")) Data->bBudgetRefused=true;
	const int64 Bytes=Finish.Body.Num();
	Data->Parsed->bHasBody=Bytes!=0;
	if (!Data->Problem.IsEmpty() || !Data->Lease.IsValid())
	{
		DynamicRequestPrivate::RefuseBody(*Data,Finish);
		return false;
	}
	if (!Finish.SaveTo.IsEmpty() || Finish.bBodyInFile) return true; // streaming native finalizer stays separate
	const int64 Retained=Bytes*(Finish.bKeepBody || Finish.Error.IsError() ? 2LL : 1LL); // TArray's int32 count keeps multiplication in signed64
	const bool bParse=Bytes>0 && (DecodesJson() || Finish.Error.Kind==EVaRestErrorKind::Http ||
		(AppliesEnvelope() && Finish.Envelope && Finish.Envelope->HasPredicate()));
	const int64 Estimate=bParse ? 8LL*Bytes+2LL*1024*1024 : 0; // named admission ESTIMATE, never actual arena/RSS guarantee
	const int64 Output=!Finish.Error.IsError() && Data->Parsed->Type.Kind==EVaRestTypeKind::String && !Data->Parsed->Type.bJsonValue ? (Bytes+1)*int64(sizeof(TCHAR)) : 0;
	if (!Data->Lease.SetInputBytes(Retained) || !Data->Lease.SetParseEstimateBytes(Estimate) || !Data->Lease.SetOutputBytes(Output))
	{
		DynamicRequestPrivate::RefuseBody(*Data,Finish);
		return false;
	}
	return true;
}
void TResultHandler<FVaRestDynamicResult>::Decode(FFinish& Finish)
{
	DynamicRequestPrivate::DecodeNative(*Data,Finish);
	DynamicRequestPrivate::RecordNativeBudget(*Data,Finish);
}
void TResultHandler<FVaRestDynamicResult>::Finalize(FFinish& Finish)
{
	check(IsInGameThread());
	const auto Native=Data;
	DynamicRequestPrivate::RecordNativeBudget(*Native,Finish);
	Native->Parsed->ErrorJson=Finish.Error.Body;
	Native->Parsed->bHasBody=Native->Parsed->Json.IsValid() || Native->Parsed->ErrorJson.IsValid() ||
		!Native->Parsed->Text.IsEmpty() || !Native->Parsed->Bytes.IsEmpty();
	Native->Parsed->Type.Availability=Native->bBudgetRefused ? EVaRestDynamicAvailability::BudgetRefused :
		(Native->bDecoded && !Finish.Error.IsError() && Native->Parsed->Type.Kind!=EVaRestTypeKind::None ? EVaRestDynamicAvailability::Available : EVaRestDynamicAvailability::None);
	Native->Parsed->ErrorType.Availability=!Finish.Error.IsError() || Native->Parsed->ErrorType.Kind==EVaRestTypeKind::None ? EVaRestDynamicAvailability::None :
		(Native->bBudgetRefused ? EVaRestDynamicAvailability::BudgetRefused :
		 (Finish.Error.Body.IsValid() ? EVaRestDynamicAvailability::Available : EVaRestDynamicAvailability::Unavailable));
	if ((Native->bTypedResponse || Native->bTypedError) && !Native->bBudgetRefused)
	{
		const auto Types=Native->Completion ? Native->Completion->TypesOnGT() : nullptr;
		if (Types) Types->FinalizeOnGT(Finish,Native->Parsed->Json,Native->Parsed->Type,Native->Parsed->ErrorType,Native->Limits,Native->Lease);
		else if (!Finish.Error.IsError())
		{
			Finish.Error.Kind=EVaRestErrorKind::Config; Finish.Error.Code=TEXT("DynamicTypeChanged");
			Finish.Error.Message=TEXT("the admitted dynamic type state is unavailable");
			Native->Parsed->Type.Availability=EVaRestDynamicAvailability::TypeChanged;
		}
		Native->Parsed->ErrorJson=Finish.Error.Body;
	}
	Native->Parsed->bHasBody=Native->Parsed->Json.IsValid() || Native->Parsed->ErrorJson.IsValid() ||
		!Native->Parsed->Text.IsEmpty() || !Native->Parsed->Bytes.IsEmpty();
	Value.Emplace(FDynamicResultAccess::Publish(Native->Parsed));
	Native->bFinalized=true;
	Native->Lease.Reset(); // published native shell retention is caller-owned; no reflected/GT obligation travels with it
	if (Finish.bTimeDecode && Finish.Clock) Finish.DecodeEndAt=Finish.Clock->Now();
}
void TResultHandler<FVaRestDynamicResult>::ValidateTerminalOnGT(FFinish& Finish)
{
	check(IsInGameThread());
	const auto Types=Data->Completion ? Data->Completion->TypesOnGT() : nullptr;
	if (Types) Types->ValidateTerminalOnGT(Finish,Data->Parsed->Type,Data->Parsed->ErrorType);
}
void TResultHandler<FVaRestDynamicResult>::DiscardValue()
{
	check(IsInGameThread());
	Data->bDiscarded=true;
	if (const auto Types=Data->Completion ? Data->Completion->TypesOnGT() : nullptr) Types->DiscardOnGT();
	if (Data->bFinalized) return; // published shell is plain diagnostics; no reflected payload/permit survives Finalize
	Data->Parsed->Json=FVaRestJson();Data->Parsed->ErrorJson=FVaRestJson();Data->Parsed->Text.Reset();Data->Parsed->Bytes.Reset();
	Data->Parsed->bHasBody=false;
	Data->Lease.Reset();
	Value.Reset();
}
void TResultHandler<FVaRestDynamicResult>::Deliver(FFinish& Finish)
{
	check(IsInGameThread());
	if (Finish.Error.IsError()) { if (OnError) OnError(Finish.Error); }
	else if (OnSuccess && Value.IsSet()) OnSuccess(Value.GetValue(),Finish.Response);
	if (OnComplete)
	{
		TVaRestResult<FVaRestDynamicResult> Result;
		Result.Value=MoveTemp(Value);Result.Error=Finish.Error;Result.Response=MoveTemp(Finish.Response);
		OnComplete(Result);
	}
}

void FDynamicRequestAccess::BeginNativeDecodeOnGT(const FVaRestExchangeRef& Exchange,FFinish&& Finish)
{
	check(IsInGameThread());
	using namespace DynamicRequestPrivate;
	auto& Ex=Internals(*Exchange);
	auto& Handler=static_cast<TResultHandler<FVaRestDynamicResult>&>(*Ex.Handler);
	const auto Completion=Handler.Data->Completion;
	const auto Work=MakeShared<FNativeDynamicWork,ESPMode::ThreadSafe>();
	Work->Data=Handler.Data; Work->Abort=Ex.PersistenceAbort;
	const auto Pending=MakeShared<FOrdinaryDynamicPending,ESPMode::ThreadSafe>();
	Pending->Handler=MoveTemp(Ex.Handler);
	Pending->Clock=MoveTemp(Finish.Clock); Pending->DecodeOptions=MoveTemp(Finish.DecodeOptions);
	Finish.DecodeOptions=FVaRestJsonConvertOptions();
	Pending->bTimeDecode=Finish.bTimeDecode;
	Pending->Abandon=[Exchange] { if (const auto State=Internals(*Exchange).State) State->DropDeferredDynamicCompletion(Exchange); };
	Work->Finish=MoveTemp(Finish);
	Ex.Phase=EVaRestExchangePhase::Decoding; Ex.bCompletionQueued=true; Ex.bLaterFramePending=true;
	TUniqueFunction<void()> Resume=[Exchange,Pending,Work]() mutable {
		Pending->bConsumed=true; Pending->Abandon.Reset();
		auto& Returned=Internals(*Exchange);
		Returned.Handler=MoveTemp(Pending->Handler);
		FFinish Done=MoveTemp(Work->Finish);
		Done.Clock=MoveTemp(Pending->Clock); Done.DecodeOptions=MoveTemp(Pending->DecodeOptions);
		Done.DecodeStartAt=Pending->StartedAt;
		if (!Work->bDidRun)
		{
			if (!Done.Error.IsError())
			{
				Done.Error.Kind=EVaRestErrorKind::Config;
				Done.Error.Code=!Done.TempFile.IsEmpty() && !Work->bCleanupRan ? TEXT("DecodeCleanupTaskDiscarded") : TEXT("DecodeTaskDiscarded");
				Done.Error.Message=TEXT("the dynamic native decoder was discarded before execution");
			}
		}
		Returned.bLaterFramePending=false; Returned.bLaterFrameReady=true;
		if (Returned.State) Returned.State->Complete(Exchange,MoveTemp(Done));
	};
	if (!Completion || !Completion->BindWorkerOnGT(Resume)) return; // Pending's real GTK destructor performs silent bookkeeping
	Ex.State->EndWaits(Ex); // callback ownership was installed BEFORE reentrant cancellation/shutdown
	if (Completion->IsStopped() || FClientState::IsShuttingDown() || Ex.bFinished) return;
	Excerpts::EmitOnDecode(Work->Finish); // raw entity is still intact; no worker service lookup/late callback publication
	if (Pending->bTimeDecode && Pending->Clock) Pending->StartedAt=Pending->Clock->Now();
	if (Completion->IsStopped() || FClientState::IsShuttingDown() || Ex.bFinished) return;
	const auto Worker=MakeShared<FNativeDynamicWorker,ESPMode::ThreadSafe>();
	Worker->Work=Work; Worker->Completion=Completion;
#if !UE_BUILD_SHIPPING
	++Testing::Counters().DecodeTasksLaunched;
#endif
	const bool bFileCleanup=!Work->Finish.TempFile.IsEmpty(); // before the first worker can mutate native Finish
	const auto DecodeTask=UE::Tasks::Launch(TEXT("VaRest.DynamicDecode"),[Worker] { Worker->Work->Run(); });
	if (bFileCleanup)
		UE::Tasks::Launch(TEXT("VaRest.DynamicDecodeFileCleanup"),[Worker] { Worker->Work->CleanupDroppedFile(); },UE::Tasks::Prerequisites(DecodeTask));
	// Worker is retained by both native tasks and this GTK publication stack. Only their actual last use signals the frame.
}
}

TVaRestRequest<FVaRestDynamicResult> FVaRestClient::RequestDynamic(FStringView Verb,FStringView Path,FVaRestDynamicResultConfig&& Config) const
{
	TUniquePtr<VaRest::Http::FRequestSpec> Spec(new VaRest::Http::FRequestSpec(State,Verb,Path));
	if (State && !State->ManagedAdmissionIsClosed())
		Spec->Handler=MakeUnique<VaRest::Http::TResultHandler<FVaRestDynamicResult>>(MoveTemp(Config));
	else Spec->Handler=MakeUnique<VaRest::Http::TResultHandler<FVaRestDynamicResult>>();
	FString Error;
	if (!Spec->Handler->ValidateConfiguration(Error)) Spec->Fail(MoveTemp(Error));
	return TVaRestRequest<FVaRestDynamicResult>(MoveTemp(Spec));
}

TVaRestRequest<FVaRestDynamicResult> FVaRestAuthRefreshContext::RequestDynamic(FStringView Verb,FStringView Path,
	FVaRestDynamicResultConfig&& Config,FName TraceEndpoint) const
{
	const uint64 Preset=IdTaken->exchange(true) ? 0 : RefreshId;
	return GetClient().RequestDynamic(Verb,Path,MoveTemp(Config)).MarkAuthRefresh(Preset,TraceEndpoint);
}
