// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "VaRestRequestNodeTestUtils.h"
#include "VaRestDynamicTransferTestLibrary.h"
#include "VaRestRequestAction.h"
#include "VaRestRequestHandle.h"
#include "VaRestHub.h"
#include "VaRestActionAuthTestTypes.h"
#include "VaRestApiClient.h"
#include "VaRestFakeTransport.h"
#include "VaRestHttpTesting.h"
#include "VaRestJsonRules.h"
#include "VaRestTrace.h"
#include "VaRestDynamicResult.h"
#include "CoreGlobals.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/StructureEditorUtils.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "HAL/PlatformTime.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Tasks/Task.h"

namespace VaRest::Http::Editor::TransferTest
{
namespace
{
enum class ECase : uint8 { Success,Overlap,RuleAfterCompleted,CompileAfterCompleted,Superseded,HubAfterCompleted,CancelBeforeAcceptance,
	StructSuccess,StructAfterCompleted,BagAfterCompleted,StructBeforeAcceptance,MultipleMetadata,MissingBinding,RetypedBinding,MissingSharedFrame,UserContinuationRecompile,
	TypedError,TypedErrorRuleAfterCompleted,OwnerBeforeAcceptance,OwnerAfterCompleted,RulesBeforeAcceptance,Reentrant };
struct FWorkerReleaseBatch { std::atomic<bool> bDone{false},bPlainReads{true}; };
class FSink final : public IVaRestTraceSink
{
public:
	TFunction<void(const FVaRestTraceCompleted&)> Observe;
	TFunction<void(const FVaRestTraceStarted&)> ObserveStart;
	virtual FVaRestTraceDetailSettings GetDetail() const override { FVaRestTraceDetailSettings Detail; Detail.Flags=EVaRestTraceDetail::Metadata; return Detail; }
	virtual void OnAttached(const FVaRestTraceSnapshot&) override {}
	virtual void OnEvent(const FVaRestTraceEvent& Event) override
	{
		if (Event.Kind==EVaRestTraceKind::Completed && Observe) Observe(Event.Payload.Get<FVaRestTraceCompleted>());
		else if (Event.Kind==EVaRestTraceKind::Started && ObserveStart) ObserveStart(Event.Payload.Get<FVaRestTraceStarted>());
	}
};
struct FCase : public TSharedFromThis<FCase,ESPMode::ThreadSafe>
{
	FAutomationTestBase& Test;
	ECase Mode;
	FGraph Graph;
	TStrongObjectPtr<UBlueprint> Blueprint;
	TStrongObjectPtr<UObject> Instance,RuleOwner;
	TWeakObjectPtr<UObject> LostOwner;
	TStrongObjectPtr<UUserDefinedStruct> Struct;
	TStrongObjectPtr<UPackage> StructPackage;
	TStrongObjectPtr<UVaRestDynamicTransferTestProbe> UserProbe;
	TStrongObjectPtr<UVaRestActionAuthTestProbe> ErrorProbe;
	FDelegateHandle Replaced;
	TStrongObjectPtr<UVaRestHub> Hub;
	TStrongObjectPtr<UVaRestApiClient> Client;
	TArray<TStrongObjectPtr<UVaRestRequestAction>> Actions;
	TSharedRef<FVaRestVirtualClock,ESPMode::ThreadSafe> Clock=MakeShared<FVaRestVirtualClock,ESPMode::ThreadSafe>();
	TSharedRef<FVaRestFakeTransport,ESPMode::ThreadSafe> Fake=MakeShared<FVaRestFakeTransport,ESPMode::ThreadSafe>(Clock);
	TSharedRef<FSink,ESPMode::ThreadSafe> Sink=MakeShared<FSink,ESPMode::ThreadSafe>();
	TUniquePtr<FVaRestTrace::FScopedSinkIsolation> Isolation;
	TArray<FVaRestDynamicResult> WorkerShells;
	TArray<TSharedRef<FVaRestDynamicReleaseProbe,ESPMode::ThreadSafe>> ReleaseProbes;
	TMap<uint64,FVaRestTraceInitiator> Initiators;
	TMap<uint64,uint64> StartFrames;
	FString ExpectedScriptObject,ExpectedStartFunction,ExpectedUbergraphFunction;
	uint64 ExpectedClientId=0;
	TSharedRef<FWorkerReleaseBatch,ESPMode::ThreadSafe> WorkerRelease=MakeShared<FWorkerReleaseBatch,ESPMode::ThreadSafe>();
	uint64 BuiltFrame=0,SentFrame=0;
	int32 Completed=0,CarrierBaseline=0;
	int32 ContinuationsBeforeOwnerLoss=0,UnavailableBeforeOwnerLoss=0;
	bool bStarted=false,bSinkInstalled=false,bBuilt=false,bVerified=false,bShellsTaken=false,bWorkerLaunched=false,bPreChanged=false;
	explicit FCase(FAutomationTestBase& InTest,ECase InMode) : Test(InTest),Mode(InMode) {}
	bool HasTypedError() const { return Mode==ECase::TypedError || Mode==ECase::TypedErrorRuleAfterCompleted; }
	bool ExpectsDeliveredOutput() const
	{
		return Mode==ECase::Success || Mode==ECase::Overlap || Mode==ECase::StructSuccess || Mode==ECase::MultipleMetadata ||
			Mode==ECase::UserContinuationRecompile || Mode==ECase::TypedError || Mode==ECase::Reentrant;
	}
	void ObserveAction(UVaRestRequestAction* Action)
	{
		Actions.Emplace(Action);
		Action->OnSuccess.AddDynamic(ErrorProbe.Get(),&UVaRestActionAuthTestProbe::MetadataCompleted);
		Action->OnError.AddDynamic(ErrorProbe.Get(),&UVaRestActionAuthTestProbe::Failed);
		Action->OnTypedOutputUnavailable.AddDynamic(ErrorProbe.Get(),&UVaRestActionAuthTestProbe::TypedUnavailable);
	}
	void RegisterReentrantAction()
	{
		if (Mode==ECase::Reentrant && UserProbe && UserProbe->ReentrantAction && Actions.Num()==1) ObserveAction(UserProbe->ReentrantAction.Get());
	}
	void InvalidateOwner()
	{
		LostOwner=Instance.Get();
		ContinuationsBeforeOwnerLoss=ReadInt(Instance.Get(),TEXT("ContinuationCount"));
		UnavailableBeforeOwnerLoss=ReadInt(Instance.Get(),TEXT("UnavailableCount"));
		Instance->MarkAsGarbage(); Instance.Reset(); // no later reflection/read of a garbage BP object, even if actual GC runs
	}
	~FCase()
	{
		check(IsInGameThread());
		FCoreUObjectDelegates::OnObjectsReplaced.Remove(Replaced);
		if (bSinkInstalled) FVaRestTrace::RemoveSink(Sink);
		Sink->Observe={};
		Sink->ObserveStart={};
		if (Hub) Hub->DeinitializeOnGT();
		for (int32 Index=0;Index<Fake->NumCalls();++Index) Fake->ReleaseCompletedClock(Index);
		if (RuleOwner) FVaRestJsonRules::UnregisterOwner(RuleOwner.Get());
	}
	bool Build()
	{
		Isolation=MakeUnique<FVaRestTrace::FScopedSinkIsolation>();
		UE_LOG(LogTemp,Display,TEXT("G-BP-TRANSFER construction schedule %d"),int32(Mode));
		FEdGraphPinType ResponsePin;
		if (Mode>=ECase::StructSuccess && Mode<=ECase::StructBeforeAcceptance)
		{
			// BlueprintEditorUtils removes non-native RF_Transient struct member types. Use a real asset-shaped
			// struct in an unsaved transient package; no asset files or production metadata are changed.
			StructPackage.Reset(CreatePackage(*(FString(TEXT("/Temp/VaRestTransferQualification_"))+FGuid::NewGuid().ToString(EGuidFormats::Digits))));
			if (!Test.TestNotNull(TEXT("owned unsaved UDS fixture package"),StructPackage.Get())) return false;
			StructPackage->SetFlags(RF_Transient);
			Struct.Reset(FStructureEditorUtils::CreateUserDefinedStruct(StructPackage.Get(),
				MakeUniqueObjectName(StructPackage.Get(),UUserDefinedStruct::StaticClass(),TEXT("VaRestTransferStruct")),RF_Public));
			if (!Test.TestNotNull(TEXT("real UDS fixture"),Struct.Get())) return false;
			const FEdGraphPinType Int(UEdGraphSchema_K2::PC_Int,NAME_None,nullptr,EPinContainerType::None,false,FEdGraphTerminalType());
			if (!Test.TestTrue(TEXT("real UDS integer field added"),FStructureEditorUtils::AddVariable(Struct.Get(),Int))) return false;
			FStructureEditorUtils::RenameVariable(Struct.Get(),FStructureEditorUtils::GetVarDesc(Struct.Get())[0].VarGuid,TEXT("Active"));
			FStructureEditorUtils::RenameVariable(Struct.Get(),FStructureEditorUtils::GetVarDesc(Struct.Get())[1].VarGuid,TEXT("Value"));
			if (!Test.TestTrue(TEXT("real UDS is compiled and remains a legal non-transient Blueprint member type"),Struct->Status==UDSS_UpToDate && !Struct->HasAnyFlags(RF_Transient))) return false;
			ResponsePin=FEdGraphPinType(UEdGraphSchema_K2::PC_Struct,NAME_None,Struct.Get(),
				Mode==ECase::BagAfterCompleted ? EPinContainerType::Array : EPinContainerType::None,false,FEdGraphTerminalType());
		}
		Graph=MakeGraph(Struct ? &ResponsePin : nullptr,Mode==ECase::MultipleMetadata,Mode==ECase::RetypedBinding,Mode==ECase::UserContinuationRecompile,HasTypedError(),Mode==ECase::Reentrant); Blueprint.Reset(Graph.Blueprint);
		if (!Test.TestTrue(*FString::Printf(TEXT("preparation exec ends at internal Confirm without a user branch (schedule %d)"),int32(Mode)),Graph.bPreparationIsClosed)) return false;
		FCompilerResultsLog Log;
		if (!Test.TestTrue(TEXT("actual shared transfer graph compiles"),Compile(Blueprint.Get(),Log))) return false;
		auto* Class=Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
		if (!Test.TestNotNull(TEXT("generated Blueprint class"),Class)) return false;
		if (!Test.TestNotNull(TEXT("actual compiled ubergraph function"),Class->UberGraphFunction.Get())) return false;
		Instance.Reset(NewObject<UObject>(GetTransientPackage(),Class)); RuleOwner.Reset(NewObject<UVaRestDynamicTransferTestProbe>());
		ExpectedScriptObject=Instance->GetPathName(); ExpectedStartFunction=Graph.Start.ToString(); ExpectedUbergraphFunction=Class->UberGraphFunction->GetName();
		if (Mode==ECase::UserContinuationRecompile || Mode==ECase::Reentrant)
		{
			UserProbe.Reset(NewObject<UVaRestDynamicTransferTestProbe>());
			if (!Test.TestTrue(TEXT("stable native probe injected into the real graph"),SetTransferProbe(Instance.Get(),UserProbe.Get()))) return false;
			const TWeakPtr<FCase,ESPMode::ThreadSafe> Weak=AsShared();
			Replaced=FCoreUObjectDelegates::OnObjectsReplaced.AddLambda([Weak](const FCoreUObjectDelegates::FReplacementObjectMap& Map) {
				if (!IsInGameThread()) return; // unrelated replacements do not acquire GTK-owned fixture state
				if (const auto Case=Weak.Pin())
					if (auto* const* Replacement=Map.Find(Case->Instance.Get())) Case->Instance.Reset(*Replacement);
			});
		}
		for (const FName Function:{Graph.Prepare,Graph.Continue,Graph.Unavailable})
		{
			const auto* Compiled=Instance->FindFunction(Function);
			if (!Test.TestTrue(TEXT("real generated callback has script and belongs to the current class"),Compiled && Compiled->GetOuter()==Class && !Compiled->Script.IsEmpty())) return false;
		}
		if (!Test.TestNotNull(TEXT("real persistent event-graph storage exists"),Class->GetPersistentUberGraphFrame(Instance.Get(),Class->UberGraphFunction.Get()))) return false;
		Hub.Reset(NewObject<UVaRestHub>()); FString Error;
		if (!Test.TestTrue(TEXT("explicit engine-only Hub initializes without a fabricated GI"),Hub->InitializeOnGT(nullptr,true,Error))) return false;
		if (!Test.TestTrue(TEXT("fixture owns a distinct storage profile"),Hub->SetStorageProfile(FGuid::NewGuid().ToString(EGuidFormats::Digits),Error))) return false;
		Client.Reset(Hub->MakeClient(TEXT("https://transfer.invalid")));
		ErrorProbe.Reset(NewObject<UVaRestActionAuthTestProbe>());
		if (Client) Client->OnError.AddDynamic(ErrorProbe.Get(),&UVaRestActionAuthTestProbe::WrapperFailed);
		if (!Test.TestNotNull(TEXT("real managed client wrapper"),Client.Get())) return false;
		ExpectedClientId=Client->GetNativeClient().GetId();
		if (Mode==ECase::Reentrant) UserProbe->ReentrantClient=Client.Get();
		Fake->bCompleteOnWorker=false;
		auto Native=Client->GetNativeClient();
		Native.WithClock(Clock).WithTransport(Fake).WithLogLevel(EVaRestLogLevel::None).WithConnectivity(FVaRestConnectivity::Make(Clock));
		CarrierBaseline=Testing::LiveDynamicCarriersOnGT(); BuiltFrame=GFrameCounter; bBuilt=true; return true;
	}
	bool Start()
	{
		const TWeakPtr<FCase,ESPMode::ThreadSafe> Weak=AsShared();
		Sink->Observe=[Weak](const FVaRestTraceCompleted& Event) { if (const auto Case=Weak.Pin()) Case->OnCompleted(Event); };
		Sink->ObserveStart=[Weak](const FVaRestTraceStarted& Event) {
			if (const auto Case=Weak.Pin(); Case && Event.ClientId==Case->ExpectedClientId)
			{
				// Started is synchronous inside Send, before the public handle assignment returns. Key by its native ID.
				Case->Initiators.Add(Event.RequestId,Event.Initiator); Case->StartFrames.Add(Event.RequestId,GFrameCounter);
			}
		};
		FVaRestTrace::AddSink(Sink); bSinkInstalled=true;
		FVaRestTypeRef Response; Response.Kind=Struct ? EVaRestTypeKind::Struct : EVaRestTypeKind::Int; Response.Type=Struct.Get();
		if (Mode==ECase::BagAfterCompleted) Response.Container=EVaRestTypeContainer::Array;
		const int32 Count=Mode==ECase::Overlap ? 2 : 1; SentFrame=GFrameCounter;
		for (int32 Index=0;Index<Count;++Index)
		{
			Fake->Enqueue(Mode==ECase::StructBeforeAcceptance || Mode==ECase::RulesBeforeAcceptance ? FVaRestFakeResponse::Manual() : ResponseFor(Index));
			// UDS schedules isolate preterminal type refusal from unrelated owner reinstancing; owner policy has separate cases.
			FString Error; auto* Action=UVaRestRequestAction::CreateRequest(Struct ? nullptr : Instance.Get(),Client.Get(),TEXT("GET"),TEXT("/value"),Response,HasTypedError() ? Response : FVaRestTypeRef(),Error);
			if (!Test.TestNotNull(*FString::Printf(TEXT("admitted action: %s"),*Error),Action)) return false;
			ObserveAction(Action);
			if (!Test.TestTrue(TEXT("action injected into the actual graph instance"),SetAction(Instance.Get(),Action))) return false;
			if (Mode==ECase::MissingBinding || Mode==ECase::RetypedBinding || Mode==ECase::MissingSharedFrame)
			{
				FVaRestPrepareOutput Prepare; FVaRestContinueOutput Continue; FVaRestUnavailableOutput Unavailable;
				const FString Missing=FGuid::NewGuid().ToString(EGuidFormats::Digits);
				Prepare.BindUFunction(Instance.Get(),Mode==ECase::MissingBinding ? FName(FString(TEXT("__VaRest_Prepare_"))+Missing) : Graph.Prepare);
				Continue.BindUFunction(Instance.Get(),Mode==ECase::MissingBinding ? FName(FString(TEXT("__VaRest_Continue_"))+Missing) : Graph.Continue);
				Unavailable.BindUFunction(Instance.Get(),Mode==ECase::MissingBinding ? FName(FString(TEXT("__VaRest_Unavailable_"))+Missing) : Graph.Unavailable);
				if (Mode==ECase::MissingSharedFrame) CastChecked<UBlueprintGeneratedClass>(Instance->GetClass())->DestroyPersistentUberGraphFrame(Instance.Get());
				Test.TestFalse(TEXT("missing/retyped functions or actual absent shared storage refuse binding"),Action->BindOutput(Prepare,Continue,Unavailable));
				Action->Activate();
			}
			else Instance->ProcessEvent(Instance->FindFunctionChecked(Graph.Start),nullptr); // Bind then immediate Activate/Send
			Test.TestTrue(TEXT("Send already captured a real request ID"),Action->GetHandle()->GetId()!=0);
			Test.TestFalse(TEXT("no same-frame terminal delivery"),Action->HasTerminalMetadata());
			if (Mode==ECase::CancelBeforeAcceptance) Action->GetHandle()->Cancel();
			if (Mode==ECase::OwnerBeforeAcceptance) InvalidateOwner(); // after immediate Send, before acceptance
		}
		if (Mode==ECase::Reentrant) Fake->Enqueue(ResponseFor(1)); // second wire response belongs to the real user-started activation
		bStarted=true; return true;
	}
	FVaRestFakeResponse ResponseFor(int32 Index=0) const
	{
		if (HasTypedError()) return FVaRestFakeResponse::Json(422,FUtf8StringView(UTF8TEXT("17")));
		if (Mode==ECase::BagAfterCompleted) return FVaRestFakeResponse::Json(200,FUtf8StringView(UTF8TEXT("[{\"active\":true,\"value\":17}]")));
		if (Struct) return FVaRestFakeResponse::Json(200,FUtf8StringView(UTF8TEXT("{\"active\":true,\"value\":17}")));
		return FVaRestFakeResponse::Json(200,Index==0 ? FUtf8StringView(UTF8TEXT("17")) : FUtf8StringView(UTF8TEXT("29")));
	}
	void ChangeStruct()
	{
		const FEdGraphPinType String(UEdGraphSchema_K2::PC_String,NAME_None,nullptr,EPinContainerType::None,false,FEdGraphTerminalType());
		Test.TestTrue(TEXT("real pre-change/post-change UDS layout mutation"),FStructureEditorUtils::AddVariable(Struct.Get(),String));
	}
	void AdvanceHeldMutation()
	{
		if ((Mode!=ECase::StructBeforeAcceptance && Mode!=ECase::RulesBeforeAcceptance) || bPreChanged || Fake->NumCalls()!=1) return;
		bPreChanged=true;
		if (Mode==ECase::RulesBeforeAcceptance) Test.TestTrue(TEXT("actual rule publication after owned wire admission"),FVaRestJsonRules::ReplaceOwnerRules(RuleOwner.Get(),{}));
		else ChangeStruct();
		Fake->Complete(0,ResponseFor()); // release this exact held logical operation before any drain
	}
	void OnCompleted(const FVaRestTraceCompleted& Event)
	{
		RegisterReentrantAction(); // register even if actual engine tick order reaches Completed before the latent runner
		int32 Index=INDEX_NONE;
		for (int32 I=0;I<Actions.Num();++I) if (Actions[I]->GetHandle()->GetId()==Event.RequestId) { Index=I; break; }
		if (Index==INDEX_NONE) return;
		++Completed;
		const auto Diagnostics=Actions[Index]->GetTransferDiagnostics();
		Test.TestEqual(TEXT("all original native carriers reset before Completed"),Testing::LiveDynamicCarriersOnGT(),CarrierBaseline);
		Test.TestEqual(TEXT("no thunk address borrow survives into Completed"),Diagnostics.AddressBorrows,uint32(0));
		Test.TestEqual(TEXT("no typed user execution before Completed returns"),Diagnostics.Continuations,uint32(0));
		Test.TestTrue(TEXT("terminal acceptance crossed a real engine frame"),GFrameCounter>SentFrame);
		if (Mode==ECase::CancelBeforeAcceptance || Mode==ECase::OwnerBeforeAcceptance)
		{
			Test.TestEqual(TEXT("pre-cutoff cancel remains primary"),Event.Outcome,EVaRestTraceOutcome::Canceled); return;
		}
		if (Mode==ECase::StructBeforeAcceptance || Mode==ECase::RulesBeforeAcceptance)
		{
			Test.TestEqual(TEXT("pre-cutoff UDS/rule edit selects native Config"),Event.Error.Kind,EVaRestErrorKind::Config);
			Test.TestEqual(TEXT("pre-cutoff UDS/rule edit is explicit type change"),Event.Error.Code,FString(TEXT("DynamicTypeChanged"))); return;
		}
		if (Mode==ECase::MissingBinding || Mode==ECase::RetypedBinding || Mode==ECase::MissingSharedFrame)
		{
			Test.TestEqual(TEXT("required unavailable preparation selects native Config"),Event.Error.Kind,EVaRestErrorKind::Config);
			Test.TestEqual(TEXT("missing preparation never silently succeeds"),Event.Error.Code,FString(TEXT("DynamicOutputPreparationFailed"))); return;
		}
		if (HasTypedError())
		{
			Test.TestEqual(TEXT("native primary HTTP error is accepted before typed-error delivery mutations"),Event.Error.Kind,EVaRestErrorKind::Http);
			Test.TestEqual(TEXT("typed error preparation never changes HTTP status"),Event.Error.HttpStatus,422);
		}
		else Test.TestEqual(TEXT("native primary Success is accepted before delivery mutations"),Event.Outcome,EVaRestTraceOutcome::Success);
		Test.TestEqual(TEXT("one synchronous preparation"),Diagnostics.Preparations,uint32(1));
		Test.TestEqual(TEXT("one actual property copy"),Diagnostics.Copies,uint32(1));
		Test.TestEqual(TEXT("one confirmed invocation cookie"),Diagnostics.Confirmations,uint32(1));
		const auto Storage=InspectSharedStorage(Instance.Get(),Actions[Index]->GetHandle()->GetIdText(),Index==0 ? 17 : 29,Struct.Get(),HasTypedError());
		Test.TestTrue(TEXT("compiled shared frame owns the actual prepared cookie"),Storage.bFrame && Storage.bCookie);
		Test.TestTrue(TEXT("compiled shared frame owns the actual prepared response value"),Storage.bValue);
		if (Mode==ECase::RuleAfterCompleted || Mode==ECase::TypedErrorRuleAfterCompleted)
			Test.TestTrue(TEXT("real rule publication during Completed"),FVaRestJsonRules::ReplaceOwnerRules(RuleOwner.Get(),{}));
		else if (Mode==ECase::CompileAfterCompleted)
		{
			FCompilerResultsLog Log; Test.TestTrue(TEXT("synchronous target recompile in the actual Completed sink"),Compile(Blueprint.Get(),Log));
		}
		else if (Mode==ECase::Superseded)
			Test.TestTrue(TEXT("only engine-owned shared cookie is superseded"),SupersedeSharedCookie(Instance.Get(),Actions[Index]->GetHandle()->GetIdText()));
		else if (Mode==ECase::HubAfterCompleted) Hub->DeinitializeOnGT();
		else if (Mode==ECase::OwnerAfterCompleted) InvalidateOwner();
		else if (Mode==ECase::StructAfterCompleted || Mode==ECase::BagAfterCompleted) ChangeStruct();
	}
	void AdvanceUserRecompile()
	{
		if (Mode!=ECase::UserContinuationRecompile || !UserProbe || !UserProbe->bRecompileRequested || UserProbe->bRecompileFinished || GFrameCounter<=UserProbe->RecompileRequestedFrame) return;
		const TStrongObjectPtr<UBlueprint> Pending(UserProbe->PendingRecompile.Get());
		UserProbe->PendingRecompile.Reset(); UserProbe->bRecompileFinished=true;
		if (!Test.TestNotNull(TEXT("user-requested Blueprint remains alive until the safe recompile frame"),Pending.Get())) return;
		FCompilerResultsLog Results; Results.SetSilentMode(true);
		FKismetEditorUtilities::CompileBlueprint(Pending.Get(),EBlueprintCompileOptions::SkipGarbageCollection,&Results);
		UserProbe->bCompiled=Results.NumErrors==0 && Pending->Status!=BS_Error;
	}
	bool Done() const
	{
		if (Mode==ECase::Reentrant && Actions.Num()!=2) return false;
		if (Mode==ECase::UserContinuationRecompile && UserProbe && UserProbe->bRecompileRequested && !UserProbe->bRecompileFinished) return false;
		if (!bStarted || Completed!=Actions.Num() || Client->GetNativeClient().GetInFlightCount()!=0 || Fake->NumInFlight()!=0) return false;
		for (const auto& Action:Actions) if (Action->GetHandle()->IsActive()) return false;
		return true;
	}
	void Verify()
	{
		const bool bPrimaryError=Mode==ECase::CancelBeforeAcceptance || Mode==ECase::StructBeforeAcceptance ||
			Mode==ECase::MissingBinding || Mode==ECase::RetypedBinding || Mode==ECase::MissingSharedFrame || Mode==ECase::RulesBeforeAcceptance || HasTypedError();
		Test.TestEqual(TEXT("primary errors forward once; successful typed availability does not fabricate a client error"),ErrorProbe->WrapperErrors,bPrimaryError ? 1 : 0);
		if (HasTypedError())
		{
			Test.TestEqual(TEXT("typed primary error never emits metadata success"),ErrorProbe->MetadataSuccesses,0);
			Test.TestEqual(TEXT("typed error selects exactly one public primary/availability route"),ErrorProbe->Failures,Mode==ECase::TypedError ? 1 : 0);
			Test.TestEqual(TEXT("only the post-cutoff changed typed error reports unavailable"),ErrorProbe->UnavailableEvents,Mode==ECase::TypedError ? 0 : 1);
		}
		if (Mode==ECase::OwnerBeforeAcceptance || Mode==ECase::OwnerAfterCompleted)
		{
			Test.TestFalse(TEXT("real weak owner became invalid"),LostOwner.IsValid());
			Test.TestEqual(TEXT("no user execution before owner loss"),ContinuationsBeforeOwnerLoss,0);
			Test.TestEqual(TEXT("no unavailable callback before owner loss"),UnavailableBeforeOwnerLoss,0);
			Test.TestEqual(TEXT("native observer sees no later metadata success"),ErrorProbe->MetadataSuccesses,0);
			Test.TestEqual(TEXT("native observer sees no later action error"),ErrorProbe->Failures,0);
			Test.TestEqual(TEXT("native observer sees no later unavailable event"),ErrorProbe->UnavailableEvents,0);
			for (const auto& Action:Actions) Test.TestFalse(TEXT("owner suppression fabricates no action terminal metadata"),Action->HasTerminalMetadata());
		}
		for (const auto& Action:Actions)
		{
			const auto Id=Action->GetHandle()->GetId();
			if (const auto* Initiator=Initiators.Find(Id))
			{
				const bool bFromGraph=Mode!=ECase::MissingBinding && Mode!=ECase::RetypedBinding && Mode!=ECase::MissingSharedFrame;
				Test.TestTrue(TEXT("IN15 actual immediate native Send call site"),Initiator->SourceFile.Contains(TEXT("VaRestRequestAction.cpp"),ESearchCase::CaseSensitive) && Initiator->SourceLine>0);
				Test.TestEqual(TEXT("native Started is still in the activating engine frame"),StartFrames.FindChecked(Id),
					Mode==ECase::Reentrant && Action.Get()==UserProbe->ReentrantAction.Get() ? UserProbe->ReentrantSendFrame : SentFrame);
#if DO_BLUEPRINT_GUARD
				if (bFromGraph)
				{
					Test.TestEqual(TEXT("IN15 actual Blueprint initiator object at Send"),Initiator->ScriptObject,ExpectedScriptObject);
					Test.TestTrue(TEXT("IN15 actual generated script function at Send"),Initiator->ScriptFunction==ExpectedStartFunction || Initiator->ScriptFunction==ExpectedUbergraphFunction);
					Test.TestTrue(TEXT("IN15 actual script bytecode offset at Send"),Initiator->ScriptCodeOffset>=0);
				}
				else Test.TestTrue(TEXT("direct native negative admission has no fabricated Blueprint stack"),Initiator->ScriptFunction.IsEmpty() && Initiator->ScriptCodeOffset==INDEX_NONE);
#else
				(void)bFromGraph;
				Test.AddInfo(TEXT("IN15 Blueprint guard is disabled in this target; no Blueprint stack qualification"));
#endif
			}
			else Test.AddError(TEXT("missing actual Started initiator for an action's native ID"));
			const auto D=Action->GetTransferDiagnostics();
			Test.TestEqual(TEXT("no retained native addresses at action cleanup"),D.AddressBorrows,uint32(0));
			if (ExpectsDeliveredOutput())
			{
				Test.TestEqual(TEXT("typed continuation consumed exactly once"),D.Continuations,uint32(1));
				Test.TestEqual(TEXT("all later listeners still have only one native typed copy"),D.Copies,uint32(1));
				Test.TestEqual(TEXT("no unavailable route for current storage"),D.Unavailable,uint32(0));
				Test.TestTrue(TEXT("terminal metadata remains the original native primary"),Action->HasTerminalMetadata() &&
					Action->GetTerminalError().Kind==(HasTypedError() ? EVaRestErrorKind::Http : EVaRestErrorKind::None));
			}
			else
			{
				Test.TestEqual(TEXT("changed/canceled/suppressed output never enters typed user execution"),D.Continuations,uint32(0));
				if (Mode==ECase::RuleAfterCompleted || Mode==ECase::Superseded || Mode==ECase::StructAfterCompleted || Mode==ECase::BagAfterCompleted || Mode==ECase::TypedErrorRuleAfterCompleted)
					Test.TestEqual(TEXT("exactly one explicit unavailable route"),D.Unavailable,uint32(1));
				if (Mode==ECase::TypedErrorRuleAfterCompleted)
					Test.TestEqual(TEXT("post-cutoff unavailable typed error preserves primary HTTP"),Action->GetTerminalError().Kind,EVaRestErrorKind::Http);
			}
		}
		if (ExpectsDeliveredOutput())
		{
			if (Mode!=ECase::UserContinuationRecompile)
			{
				Test.TestEqual(TEXT("actual compiled user continuation read prepared values"),ReadResponseValue(Instance.Get()),Mode==ECase::Overlap || Mode==ECase::Reentrant ? 29 : 17);
				Test.TestEqual(TEXT("overlapping activations retain separate invocation cookies"),ReadString(Instance.Get(),TEXT("ObservedCookie")),Actions.Last()->GetHandle()->GetIdText());
				Test.TestEqual(TEXT("one user route per invocation of one node"),ReadInt(Instance.Get(),TEXT("ContinuationCount")),Actions.Num());
				Test.TestTrue(TEXT("compiled continuation observes selected typed body valid"),ReadBool(Instance.Get(),TEXT("ObservedValid")));
				Test.TestEqual(TEXT("compiled continuation keeps primary success/error discriminator"),ReadBool(Instance.Get(),TEXT("ObservedSuccess")),!HasTypedError());
			}
			if (Mode==ECase::MultipleMetadata)
			{
				Test.TestEqual(TEXT("both metadata subscribers ran without a second typed take"),ReadInt(Instance.Get(),TEXT("MetadataCount")),2);
				Test.TestEqual(TEXT("each actual postterminal thunk returned false"),ReadInt(Instance.Get(),TEXT("IllegalTakeCount")),0);
			}
			if (Mode==ECase::UserContinuationRecompile)
			{
				Test.TestTrue(TEXT("actual user continuation requested real compilation after its VM frame returned"),UserProbe->bCalled && UserProbe->bRecompileRequested && UserProbe->bRecompileFinished && UserProbe->bCompiled);
				Test.TestTrue(TEXT("user recompile observed zero native carrier/address ownership"),UserProbe->bNoNativeCarrier);
				Test.TestEqual(TEXT("user read the engine-owned typed value before recompile"),UserProbe->ObservedValue,17);
				Test.TestEqual(TEXT("user read the exact invocation cookie before recompile"),UserProbe->ObservedCookie,Actions.Last()->GetHandle()->GetIdText());
				Test.TestEqual(TEXT("user execution began exactly once"),UserProbe->ContinuationCount,1);
			}
			if (Mode==ECase::Reentrant)
			{
				Test.TestTrue(TEXT("actual first user continuation sent the second request"),UserProbe->bCalled && UserProbe->bSecondSent && UserProbe->ReentrantError.IsEmpty());
				Test.TestTrue(TEXT("reentrant Send began after actual native copy/reset"),UserProbe->bNoNativeCarrier);
				Test.TestEqual(TEXT("first invocation value was consumed before reentry"),UserProbe->ObservedValue,17);
				Test.TestEqual(TEXT("first invocation cookie was consumed before reentry"),UserProbe->ObservedCookie,Actions[0]->GetHandle()->GetIdText());
				Test.TestEqual(TEXT("only one reentrant activation; both user continuations observed"),UserProbe->ReentrantEntries,2);
				Test.TestEqual(TEXT("two separate native primary successes"),ErrorProbe->MetadataSuccesses,2);
			}
		}
		Test.TestEqual(TEXT("native carrier inventory returns to baseline"),Testing::LiveDynamicCarriersOnGT(),CarrierBaseline);
	}
	bool VerifyWorkerRelease()
	{
		if (!bShellsTaken)
		{
			bShellsTaken=true;
			for (int32 Index=0;Index<Fake->NumCalls();++Index) Fake->ReleaseCompletedClock(Index); // only after owned logical terminals
			for (const auto& Action:Actions)
			{
				auto Shell=Action->TakeNativeResultOnGT();
				if (Shell.GetNativeOwnerCountForTesting()==0) continue;
				const auto Probe=MakeShared<FVaRestDynamicReleaseProbe,ESPMode::ThreadSafe>();
				Test.TestTrue(TEXT("exact native payload receives a release probe"),Shell.AttachReleaseProbeForTesting(Probe));
				WorkerShells.Add(MoveTemp(Shell)); ReleaseProbes.Add(Probe);
			}
			if (ExpectsDeliveredOutput())
				Test.TestEqual(TEXT("every accepted result supplied a real shell"),WorkerShells.Num(),Actions.Num());
		}
		if (WorkerShells.IsEmpty() && !bWorkerLaunched) return true;
		if (!bWorkerLaunched)
		{
			// Wait for actual native handler/input/frame owners to release; never substitute a fixed frame delay for quiescence.
			for (const auto& Shell:WorkerShells) if (Shell.GetNativeOwnerCountForTesting()!=1) return false;
			bWorkerLaunched=true;
			const bool bExpectBody=Mode!=ECase::CancelBeforeAcceptance;
			const bool bExpectError=HasTypedError();
			const auto ExpectedKind=Struct ? EVaRestTypeKind::Struct : EVaRestTypeKind::Int;
			UE::Tasks::Launch(TEXT("VaRest.GBPTransfer.LastPlainShellRelease"),[Shells=MoveTemp(WorkerShells),Batch=WorkerRelease,bExpectBody,bExpectError,ExpectedKind]() mutable {
				for (auto& Shell:Shells)
				{
					if (Shell.GetType().Kind!=ExpectedKind || Shell.HasBody()!=bExpectBody ||
						(bExpectBody && !(bExpectError ? Shell.GetErrorJson() : Shell.GetJson()).IsValid()))
						Batch->bPlainReads.store(false,std::memory_order_relaxed);
					Shell=FVaRestDynamicResult(); // actual unique native payload destructor runs here, with no UObject/callback capture
				}
				Shells.Reset(); Batch->bDone.store(true,std::memory_order_release);
			});
			return false;
		}
		if (!WorkerRelease->bDone.load(std::memory_order_acquire)) return false;
		Test.TestTrue(TEXT("worker reads only native kind/body/immutable JSON"),WorkerRelease->bPlainReads.load(std::memory_order_relaxed));
		for (const auto& Probe:ReleaseProbes)
		{
			Test.TestTrue(TEXT("this exact last native payload was destroyed"),Probe->bDestroyed.load(std::memory_order_acquire));
			Test.TestFalse(TEXT("last native payload destruction needed no GT release"),Probe->bDestroyedOnGameThread.load(std::memory_order_relaxed));
		}
		Test.TestEqual(TEXT("worker last release owned no live reflected carrier"),Testing::LiveDynamicCarriersOnGT(),CarrierBaseline); return true;
	}
};
class FRunCases final : public IAutomationLatentCommand
{
public:
	explicit FRunCases(FAutomationTestBase& InTest) : Test(InTest) {}
	virtual bool Update() override
	{
		if (!Current)
		{
			if (Next>static_cast<uint8>(ECase::Reentrant)) { Test.TestEqual(TEXT("every transfer construction schedule reached verification and retired its owned release"),VerifiedCases,int32(ECase::Reentrant)+1); return true; }
			Current=MakeShared<FCase,ESPMode::ThreadSafe>(Test,static_cast<ECase>(Next++)); StartedAt=FPlatformTime::Seconds();
			if (!Current->Build()) { Current.Reset(); return true; }
			return false;
		}
		if (FPlatformTime::Seconds()-StartedAt>30)
		{
			Test.AddError(TEXT("transfer construction timed out waiting for owned operations across real engine frames")); Current.Reset(); return true;
		}
		// Do not advance GFrameCounter or simulate OnEndFrame. Actual editor frames settle types and deliver callbacks.
		if (!Current->bStarted)
		{
			if (GFrameCounter<=Current->BuiltFrame+1) return false;
			if (!Current->Start()) { Current.Reset(); return true; }
		}
		Current->Clock->Advance(0);
		Current->AdvanceHeldMutation();
		Current->AdvanceUserRecompile();
		Current->RegisterReentrantAction();
		if (Current->UserProbe && !Current->UserProbe->ReentrantError.IsEmpty())
		{
			Test.AddError(FString(TEXT("actual reentrant request failed: "))+Current->UserProbe->ReentrantError); Current.Reset(); return true;
		}
		if (!Current->Done()) return false;
		if (!Current->bVerified) { Current->Verify(); Current->bVerified=true; }
		if (!Current->VerifyWorkerRelease()) return false;
		++VerifiedCases; Current.Reset(); return false;
	}
private:
	FAutomationTestBase& Test;
	TSharedPtr<FCase,ESPMode::ThreadSafe> Current;
	uint8 Next=0;
	int32 VerifiedCases=0;
	double StartedAt=0;
};
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestDynamicTransferConstructionTest,"VaRest.Http.Dynamic.GBPTransfer.Construction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestDynamicTransferConstructionTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(VaRest::Http::Editor::TransferTest::FRunCases(*this)); return true;
}
#endif
