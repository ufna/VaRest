// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "VaRestClientState.h"
#include "VaRestHttpTesting.h"
#include "VaRestNativeClientSlots.h"
#include "VaRestNativeScope.h"
#include "VaRestRequestSpec.h"

namespace VaRest::Http::Testing::StopReentrancyPrivate
{
struct FResetExternalOnDestroy
{
	FResetExternalOnDestroy(TSharedPtr<FNativeLifetimeControl, ESPMode::ThreadSafe>* InExternal, int32* InCalls, bool* InWrongThread)
		: External(InExternal)
		, Calls(InCalls)
		, WrongThread(InWrongThread)
	{
	}
	~FResetExternalOnDestroy()
	{
		++*Calls;
		*WrongThread |= !IsInGameThread();
		External->Reset();
	}
	TSharedPtr<FNativeLifetimeControl, ESPMode::ThreadSafe>* External;
	int32* Calls;
	bool* WrongThread;
};
} // namespace VaRest::Http::Testing::StopReentrancyPrivate

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestNativeStopAnchorReentrancyTest, "VaRest.Http.Managed.StopAnchorFinalOwnerReentrancy",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestNativeStopAnchorReentrancyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	TSharedPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> Control = FNativeLifetimeControl::Create();
	TWeakPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> Weak = Control;
	FNativeLifetimeControl* const Identity = Control.Get();
	auto Scope = FNativeScopeLifetime::Open({}, 0.0);
	FNativeClientReservation ClientReservation;
	FName Error;
	if (!TestTrue(TEXT("fixture reserves an actual Control-owned slot"), Control->ClientSlotsOnGT().TryReserve(Scope.ToSharedRef(), ClientReservation, Error)))
		return false;
	auto Registration = ClientReservation.InitialRegistration();
	FClientConfig Config = *FClientAccess::State(Rig.Client)->GetConfig();
	Config.ScopeLifetime = Scope;
	Config.ScopeId = Scope->GetId();
	auto Client = FClientState::Create(MoveTemp(Config));
	if (!TestTrue(TEXT("real scoped client commits its exclusive reservation"), ClientReservation.CommitClient(Client)))
		return false;
	auto Payload = FRequestSpecAccess::MakeNativeOwnershipPayload(Client, {});
	auto Sender = Control->AdoptInput(Payload);
	FClientWorkLease Work;
	if (!TestTrue(TEXT("initial counted lease is real"), Registration.Gate->TryAcquire(Work)))
		return false;
	FNativeOperationReservation Operation;
	if (!TestTrue(TEXT("fixture owns a real unpublished cell"), Registration.Operations->TryClaim(Work, Sender, Operation)))
		return false;
	int32 AnchorCalls = 0;
	bool WrongThread = false;
	auto InputAnchor = Operation.ForkInputAnchor();
	Operation.SetReleaseAnchor([Input = MoveTemp(InputAnchor), &Control, &AnchorCalls, &WrongThread](bool Discarded) mutable {
		++AnchorCalls;
		WrongThread |= !IsInGameThread();
		Input.Finish(Discarded);
		Input.Reset();
		Control.Reset(); // actual last EXTERNAL receiver owner drops inside Stop's nested store-sweep callout
	});
	auto Initial = Operation.Publish();
	Registration.Operations->MarkTerminalOnGT(Initial.Reference());
	Initial.Reset();
	Registration.Control.Reset(); // no inert registration/token copy is allowed to hide the original lifetime defect
	Control->StopInputsOnGT();
	TestFalse(TEXT("anchor actually reset the external caller's shared pointer"), Control.IsValid());
	TestEqual(TEXT("real ready cell anchor ran once during Stop"), AnchorCalls, 1);
	TestFalse(TEXT("reentrant cleanup remains on GT"), WrongThread);
	auto Kept = Weak.Pin();
	if (!TestTrue(TEXT("Stop survived long enough to install the same Control's occupied-client exit hold"), Kept.IsValid()))
		return false;
	TestTrue(TEXT("retained Control is the original receiver, not a replacement owner"), Kept.Get() == Identity);
	TestTrue(TEXT("actual occupied client requests process-exit retention"), Kept->IsExitRetentionRequested());
	TestEqual(TEXT("actual input collector completed before Stop's final hold decision"), Kept->NumPendingInputs(), uint64(0));
	Registration.Control = Kept;
	TestTrue(TEXT("fixture has no non-operation obligations; final row can now quiesce"), Kept->ClientSlotsOnGT().TryReleaseQuiescent(Registration));
	TestTrue(TEXT("test-only teardown removes its now-quiescent hold without production policy changes"), Kept->DropQuiescentExitHoldForTests());
	Registration.Control.Reset();
	Kept.Reset();
	TestFalse(TEXT("repeated automation retains no fixture Control allocation"), Weak.IsValid());
	Scope->Close(0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestNativeStopInputReentrancyTest, "VaRest.Http.Managed.StopInputFinalOwnerReentrancy",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestNativeStopInputReentrancyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	TSharedPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> Control = FNativeLifetimeControl::Create();
	TWeakPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> Weak = Control;
	int32 Destroyed = 0;
	bool WrongThread = false;
	TSharedPtr<StopReentrancyPrivate::FResetExternalOnDestroy, ESPMode::ThreadSafe> Trigger =
		MakeShared<StopReentrancyPrivate::FResetExternalOnDestroy, ESPMode::ThreadSafe>(&Control, &Destroyed, &WrongThread);
	auto Payload = FRequestSpecAccess::MakeNativeOwnershipPayload(FClientAccess::State(Rig.Client),
		[Held = MoveTemp(Trigger)](const FString&) {});
	auto Sender = Control->AdoptInput(Payload);
	Sender.PublishReady();
	Sender.Finish(true);
	Sender.Reset();
	Control->StopInputsOnGT(); // actual whole-spec destructor drops the last external Control inside CollectInputsOnGT
	TestFalse(TEXT("whole-spec capture actually reset the external caller owner"), Control.IsValid());
	TestEqual(TEXT("real capture destructor ran once"), Destroyed, 1);
	TestFalse(TEXT("capture destruction remains GT"), WrongThread);
	TestFalse(TEXT("safe empty Stop return retains no unnecessary self-hold"), Weak.IsValid());
	return true;
}


namespace VaRest::Http::Testing::StopCensusPrivate
{
struct FCounts
{
 int32 Anchors=0,Inputs=0,Calls=0,Stops=0;
 bool WrongThread=false;
};
struct FOnDestroy
{
 explicit FOnDestroy(TUniqueFunction<void()>&& InAction):Action(MoveTemp(InAction)) {}
 ~FOnDestroy() { if (Action) Action(); }
 TUniqueFunction<void()> Action;
};
TUniquePtr<FRequestSpec> CountedInput(const TSharedPtr<FClientState,ESPMode::ThreadSafe>& Client,
 TSharedRef<FCounts,ESPMode::ThreadSafe> Counts,TUniqueFunction<void()> After={})
{
 TUniqueFunction<void()> Action=[Counts,After=MoveTemp(After)]() mutable {
  Counts->WrongThread|=!IsInGameThread();++Counts->Inputs;if (After) After();
 };
 const auto Capture=MakeShared<FOnDestroy,ESPMode::ThreadSafe>(MoveTemp(Action));
 return FRequestSpecAccess::MakeNativeOwnershipPayload(Client,[Capture,Counts](const FString&) { ++Counts->Calls; });
}
void Ready(FNativeLifetimeControl& Control,TUniquePtr<FRequestSpec>& Input)
{
 auto Sender=Control.AdoptInput(Input);Sender.PublishReady();Sender.Finish();Sender.Reset();
}
bool RunCells(FAutomationTestBase& Test,uint32 ReadyCount,bool Held,int32 Reentry)
{
 FTestRig Rig;
 const auto Control=FNativeLifetimeControl::Create();
 const auto Counts=MakeShared<FCounts,ESPMode::ThreadSafe>();
 TWeakPtr<FNativeLifetimeControl,ESPMode::ThreadSafe> Weak=Control;
 auto Scope=FNativeScopeLifetime::Open({},0.0);
 FNativeClientReservation ClientReservation;FName Error;
 if (!Test.TestTrue(TEXT("census owns an actual client slot"),Control->ClientSlotsOnGT().TryReserve(Scope.ToSharedRef(),ClientReservation,Error))) return false;
 const auto Registration=ClientReservation.InitialRegistration();
 FClientConfig Config=*FClientAccess::State(Rig.Client)->GetConfig();Config.ScopeLifetime=Scope;Config.ScopeId=Scope->GetId();
 const auto Client=FClientState::Create(MoveTemp(Config),Registration,false);
 if (!Test.TestTrue(TEXT("census commits the matching managed client"),ClientReservation.CommitClient(Client))) return false;
 FNativeLogicalTailLease HeldTail;bool Ok=true;
 const uint32 Total=ReadyCount+uint32(Held);
 for (uint32 Index=0;Index<Total;++Index)
 {
  auto Input=CountedInput(Client,Counts);auto Sender=Control->AdoptInput(Input);FClientWorkLease Work;
  FNativeOperationReservation Operation;
  if (!Registration.Gate->TryAcquire(Work) || !Registration.Operations->TryClaim(Work,Sender,Operation))
  {
   Test.AddError(TEXT("owned census operation unexpectedly refused"));Ok=false;Work.Release();
   if (Sender.IsValid()) { Sender.PublishReady();Sender.Finish(true);Sender.Reset(); }break;
  }
  auto Anchor=Operation.ForkInputAnchor();
  const bool Reenter=Reentry!=0 && Index==0;
  Operation.SetReleaseAnchor([Input=MoveTemp(Anchor),Counts,Weak,Reenter](bool Discarded) mutable {
   Counts->WrongThread|=!IsInGameThread();++Counts->Anchors;
   if (Reenter) { ++Counts->Stops;if (const auto Kept=Weak.Pin()) Kept->StopInputsOnGT(); }
   Input.Finish(Discarded);Input.Reset();
  });
  auto Initial=Operation.Publish();Registration.Operations->MarkTerminalOnGT(Initial.Reference());
  if (Held && Index==ReadyCount) HeldTail=MoveTemp(Initial);else Initial.Reset();
 }
 Test.AddInfo(FString::Printf(TEXT("stop census: ready=%u held=%d reentry=%d"),ReadyCount,int32(Held),Reentry));
 Ok=Test.TestEqual(TEXT("all actual whole inputs await GT destruction"),Control->NumPendingInputs(),uint64(Total))&&Ok;
 Ok=Test.TestEqual(TEXT("real managed gate still counts every input before Stop"),Registration.Gate->NumOwnedInputs(),uint64(Total))&&Ok;
 Control->ClientSlotsOnGT().BeginRetirement(Registration);
 // This fixture owns no queue/auth/transport work; only its real input/cell barrier remains.
 Ok=Test.TestFalse(TEXT("recorded non-operation barrier cannot bypass pending inputs/cells"),Control->ClientSlotsOnGT().TryReleaseQuiescent(Registration))&&Ok;
 if (Reentry==1) Registration.Operations->SweepOnGT(1);
 else if (Reentry==2) Control->ClientSlotsOnGT().SweepOperations(1);
 else Control->StopInputsOnGT();
 Ok=Test.TestTrue(TEXT("actual stopped Control was reached"),Control->IsStopped())&&Ok;
 if (Reentry) Ok=Test.TestEqual(TEXT("actual anchor reentered Stop exactly once"),Counts->Stops,1)&&Ok;
 Ok=Test.TestEqual(TEXT("every ready anchor ran once on GT"),Counts->Anchors,int32(ReadyCount))&&Ok;
 Ok=Test.TestEqual(TEXT("every ready whole input was actually destroyed"),Counts->Inputs,int32(ReadyCount))&&Ok;
 Ok=Test.TestEqual(TEXT("only an actually held input remains"),Control->NumPendingInputs(),uint64(Held))&&Ok;
 Ok=Test.TestEqual(TEXT("only an actually held cell remains"),Registration.Operations->NumOccupied(),uint32(Held))&&Ok;
 Ok=Test.TestEqual(TEXT("slot reclaims AFTER last input when its genuine barrier permits"),Control->ClientSlotsOnGT().NumOccupied(),uint32(Held))&&Ok;
 Ok=Test.TestEqual(TEXT("exit retention distinguishes held authority from ready backlog"),Control->IsExitRetentionRequested(),Held)&&Ok;
 Ok=Test.TestFalse(TEXT("all affine disposal remained on GT"),Counts->WrongThread)&&Ok;
 Ok=Test.TestEqual(TEXT("native cleanup invokes no user callback"),Counts->Calls,0)&&Ok;
 Ok=Test.TestEqual(TEXT("disposal-core fixture starts no fake HTTP"),Rig.Fake->NumCalls(),0)&&Ok;
 // Test-only teardown also works against a failing predecessor; every capture owns heap state, never Test/stack refs.
 HeldTail.Reset();Registration.Operations->SweepOnGT(FNativeOperationStore::Capacity);
 Control->CollectInputsOnGT(FNativeOperationStore::Capacity);
 Control->ClientSlotsOnGT().TryReleaseQuiescent(Registration);
 Ok=Test.TestEqual(TEXT("owned teardown leaves no input"),Control->NumPendingInputs(),uint64(0))&&Ok;
 Ok=Test.TestEqual(TEXT("owned teardown leaves no client slot"),Control->ClientSlotsOnGT().NumOccupied(),uint32(0))&&Ok;
 if (Control->IsExitRetentionRequested()) Ok=Test.TestTrue(TEXT("only genuinely quiescent test hold may clear"),Control->DropQuiescentExitHoldForTests())&&Ok;
 Scope->Close(0.0);return Ok;
}
} // namespace VaRest::Http::Testing::StopCensusPrivate

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestNativeStopReadyCensusTest,"VaRest.Http.Managed.StopReadyCensus",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestNativeStopReadyCensusTest::RunTest(const FString& Parameters)
{
 using namespace VaRest::Http::Testing::StopCensusPrivate;
 const bool One=RunCells(*this,1,false,0); // input-last client-slot order, below the ordinary live scan budget
 const bool Backlog=RunCells(*this,257,false,0);
 const bool Held=RunCells(*this,257,true,0); // a real remaining tail cannot be converted into a success
 return One && Backlog && Held;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestNativeStopOperationReentryTest,"VaRest.Http.Managed.StopOperationSweepReentry",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestNativeStopOperationReentryTest::RunTest(const FString& Parameters)
{
 using namespace VaRest::Http::Testing::StopCensusPrivate;
 const bool Store=RunCells(*this,1,false,1);
 const bool Clients=RunCells(*this,1,false,2);
 return Store && Clients;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestNativeStopInputSweepReentryTest,"VaRest.Http.Managed.StopInputSweepReentry",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestNativeStopInputSweepReentryTest::RunTest(const FString& Parameters)
{
 using namespace VaRest::Http;
 using namespace VaRest::Http::Testing;
 using namespace VaRest::Http::Testing::StopCensusPrivate;
 FTestRig Rig;const auto Control=FNativeLifetimeControl::Create();
 const auto Counts=MakeShared<FCounts,ESPMode::ThreadSafe>();TWeakPtr<FNativeLifetimeControl,ESPMode::ThreadSafe> Weak=Control;
 for (int32 Index=0;Index<256;++Index) { auto Input=CountedInput(FClientAccess::State(Rig.Client),Counts);Ready(*Control,Input); }
 TUniqueFunction<void()> Stop=[Weak,Counts] { ++Counts->Stops;if (const auto Kept=Weak.Pin()) Kept->StopInputsOnGT(); };
 auto Last=CountedInput(FClientAccess::State(Rig.Client),Counts,MoveTemp(Stop));Ready(*Control,Last);
 TestEqual(TEXT("outer live collector visits its one requested record"),Control->CollectInputsOnGT(1),uint32(1));
 TestTrue(TEXT("payload destructor actually stopped the captured Control"),Control->IsStopped());
 TestEqual(TEXT("Stop reentered once before the outer row finished"),Counts->Stops,1);
 TestEqual(TEXT("outer barrier then finite stop census destroys all257 ready inputs"),Counts->Inputs,257);
 TestEqual(TEXT("no whole input remains"),Control->NumPendingInputs(),uint64(0));
 TestFalse(TEXT("outer in-progress ready input does not force permanent retention"),Control->IsExitRetentionRequested());
 TestFalse(TEXT("all captures die on GT"),Counts->WrongThread);TestEqual(TEXT("no user callback"),Counts->Calls,0);
 Control->CollectInputsOnGT(512); // owned failure teardown, never a production drain
 if (Control->IsExitRetentionRequested()) TestTrue(TEXT("clear only an actually empty fixture hold"),Control->DropQuiescentExitHoldForTests());
 return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestNativeStopLateInputCensusTest,"VaRest.Http.Managed.StopLateInputCensus",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestNativeStopLateInputCensusTest::RunTest(const FString& Parameters)
{
 using namespace VaRest::Http;
 using namespace VaRest::Http::Testing;
 using namespace VaRest::Http::Testing::StopCensusPrivate;
 FTestRig Rig;const auto Control=FNativeLifetimeControl::Create();
 const auto Counts=MakeShared<FCounts,ESPMode::ThreadSafe>();TWeakPtr<FNativeLifetimeControl,ESPMode::ThreadSafe> Weak=Control;
 auto Late=CountedInput(FClientAccess::State(Rig.Client),Counts);
 TUniqueFunction<void()> AdoptLate=[Weak,Late=MoveTemp(Late)]() mutable {
  if (const auto Kept=Weak.Pin()) Ready(*Kept,Late); // actual stopped adoption, not a fabricated pending count
 };
 auto First=CountedInput(FClientAccess::State(Rig.Client),Counts,MoveTemp(AdoptLate));Ready(*Control,First);
 Control->StopInputsOnGT();
 TestEqual(TEXT("only the captured initial input is destroyed by this stop census"),Counts->Inputs,1);
 TestEqual(TEXT("late ready adoption was not swept into an unbounded continuation"),Control->NumPendingInputs(),uint64(1));
 TestTrue(TEXT("same-Control retention remains for genuine post-Stop adoption"),Control->IsExitRetentionRequested());
 TestFalse(TEXT("owned destruction stays on GT"),Counts->WrongThread);TestEqual(TEXT("no user delivery"),Counts->Calls,0);
 Control->CollectInputsOnGT(1);
 TestEqual(TEXT("test separately reclaims its actual late input"),Counts->Inputs,2);
 TestTrue(TEXT("only genuinely quiescent test teardown clears its own hold"),Control->DropQuiescentExitHoldForTests());
 return true;
}


namespace VaRest::Http::Testing::StopCensusPrivate
{
class FStopOnScopeClosed final : public IVaRestTraceSink
{
 uint64 ScopeId;
 TWeakPtr<FNativeLifetimeControl,ESPMode::ThreadSafe> Control;
public:
 int32 Calls=0;
 bool WrongThread=false,WasStoppedBefore=false,Returned=false;
 FStopOnScopeClosed(uint64 InScopeId,TWeakPtr<FNativeLifetimeControl,ESPMode::ThreadSafe> InControl)
  :ScopeId(InScopeId),Control(MoveTemp(InControl)) {}
 FVaRestTraceDetailSettings GetDetail() const override
 { FVaRestTraceDetailSettings Detail;Detail.Flags=EVaRestTraceDetail::Metadata;return Detail; }
 void OnAttached(const FVaRestTraceSnapshot&) override {}
 void OnEvent(const FVaRestTraceEvent& Event) override
 {
  if (Event.Kind!=EVaRestTraceKind::Scope) return;
  const auto* Info=Event.Payload.TryGet<FVaRestTraceScope>();
  if (!Info || Info->ScopeId!=ScopeId || Info->bOpened) return;
  WrongThread|=!IsInGameThread();if (!IsInGameThread()) return;
  ++Calls;
  if (const auto Kept=Control.Pin())
  { WasStoppedBefore=Kept->IsStopped();Kept->StopInputsOnGT();Returned=true; }
 }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestNativeStopQueueSweepReentryTest,"VaRest.Http.Managed.StopQueueSweepReentry",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestNativeStopQueueSweepReentryTest::RunTest(const FString& Parameters)
{
 using namespace VaRest::Http;
 using namespace VaRest::Http::Testing;
 using namespace VaRest::Http::Testing::StopCensusPrivate;
 FVaRestScopedSinkIsolation Isolation;
 FTestRig Rig;const auto Control=FNativeLifetimeControl::Create();
 const auto Counts=MakeShared<FCounts,ESPMode::ThreadSafe>();
 auto Scope=FNativeScopeLifetime::Open({},0.0);FNativeClientReservation Reservation;FName Error;
 if (!TestTrue(TEXT("queue-sweep case reserves an actual scoped client"),Control->ClientSlotsOnGT().TryReserve(Scope.ToSharedRef(),Reservation,Error))) return false;
 const auto Registration=Reservation.InitialRegistration();
 FClientConfig Config=*FClientAccess::State(Rig.Client)->GetConfig();Config.ScopeLifetime=Scope;Config.ScopeId=Scope->GetId();
 const auto Client=FClientState::Create(MoveTemp(Config),Registration,false);
 if (!TestTrue(TEXT("queue-sweep case commits its actual managed registration"),Reservation.CommitClient(Client))) return false;
 auto Input=CountedInput(Client,Counts);Ready(*Control,Input);
 Control->ClientSlotsOnGT().BeginRetirement(Registration);
 TestFalse(TEXT("last actual counted input holds the recorded non-operation barrier"),Control->ClientSlotsOnGT().TryReleaseQuiescent(Registration));
 Scope->RequestRetirementOnGT();
 TestTrue(TEXT("owned scope stays open while its actual client row exists"),Scope->IsOpen());
 TestEqual(TEXT("ordinary input collection destroys the last actual payload"),Control->CollectInputsOnGT(1),uint32(1));
 TestEqual(TEXT("ready client now awaits the real queue-sweeper reclaim"),Control->ClientSlotsOnGT().NumOccupied(),uint32(1));
 TWeakPtr<FNativeLifetimeControl,ESPMode::ThreadSafe> Weak=Control;
 const auto Sink=MakeShared<FStopOnScopeClosed,ESPMode::ThreadSafe>(Scope->GetId(),Weak);
 FVaRestTrace::AddSink(Sink);
 ON_SCOPE_EXIT { FVaRestTrace::RemoveSink(Sink); }; // only this fixture's sink; isolation restores prior membership
 // No synthetic queue flags: TryReclaim -> real ScopeClosed -> inline owned sink reenters Stop under bSweeping.
 Control->ClientSlotsOnGT().SweepQueueDrains();
 TestEqual(TEXT("the matching real ScopeClosed event reentered Stop once"),Sink->Calls,1);
 TestFalse(TEXT("the queue-sweeper entered with a live Control"),Sink->WasStoppedBefore);
 TestTrue(TEXT("nested Stop returned through the real scope trace sink"),Sink->Returned);
 TestFalse(TEXT("scope close and Stop remain on GT"),Sink->WrongThread);
 TestTrue(TEXT("actual Control is stopped after the outer queue sweep"),Control->IsStopped());
 TestFalse(TEXT("the genuine scope close already occurred before Stop"),Scope->IsOpen());
 TestEqual(TEXT("queue scan restores its row guard before final stopped reclamation"),Control->ClientSlotsOnGT().NumOccupied(),uint32(0));
 TestEqual(TEXT("actual counted input is gone"),Control->NumPendingInputs(),uint64(0));
 TestEqual(TEXT("actual capture died once"),Counts->Inputs,1);
 TestFalse(TEXT("fully quiescent queue-sweep reentry does not quarantine"),Control->IsExitRetentionRequested());
 TestEqual(TEXT("this core reclaim schedule performs no HTTP"),Rig.Fake->NumCalls(),0);
 if (!Control->IsStopped()) Control->StopInputsOnGT(); // owned failure-only cleanup, not a success oracle
 Control->CollectInputsOnGT(1);Control->ClientSlotsOnGT().TryReleaseQuiescent(Registration);
 if (Control->IsExitRetentionRequested()) TestTrue(TEXT("clear only a genuinely empty test hold"),Control->DropQuiescentExitHoldForTests());
 return true;
}

#endif
