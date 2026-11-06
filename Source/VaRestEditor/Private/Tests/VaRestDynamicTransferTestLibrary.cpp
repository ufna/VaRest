// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestDynamicTransferTestLibrary.h"
#include "VaRestRequestAction.h"
#include "VaRestApiClient.h"
#include "VaRestRequestHandle.h"
#include "VaRestHttpTesting.h"
#include "CoreGlobals.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

void UVaRestDynamicTransferTestLibrary::RecompileTarget(UVaRestRequestAction* Action,UObject* Target,UVaRestDynamicTransferTestProbe* Probe,int32 Value,FString Cookie,int32 ContinuationCount)
{
	check(IsInGameThread()); if (!IsValid(Probe)) return;
	const TStrongObjectPtr<UVaRestDynamicTransferTestProbe> Observation(Probe); Probe->bCalled=true;
	// Native owned values before the compiler call. No address/reference into a Blueprint frame survives that call.
	Probe->ObservedValue=Value; Probe->ObservedCookie=MoveTemp(Cookie); Probe->ContinuationCount=ContinuationCount;
	if (!IsValid(Action) || !IsValid(Target)) return;
	const auto Diagnostics=Action->GetTransferDiagnostics();
	Probe->bNoNativeCarrier=Diagnostics.Continuations==1 && Diagnostics.AddressBorrows==0;
#if WITH_DEV_AUTOMATION_TESTS
	Probe->bNoNativeCarrier &= VaRest::Http::Testing::LiveDynamicCarriersOnGT()==0;
#endif
	if (!Probe->bNoNativeCarrier) return;
	const auto* Class=Cast<UBlueprintGeneratedClass>(Target->GetClass());
	const TStrongObjectPtr<UBlueprint> Blueprint(Class ? Cast<UBlueprint>(Class->ClassGeneratedBy) : nullptr);
	if (!Blueprint) return;
	// Recompiling this same Blueprint synchronously empties the bytecode array still executing below
	// this native call (KismetCompiler.cpp:719). Keep the observations made inside the real user callback,
	// then let the latent harness compile at a later engine frame after the VM has returned.
	Probe->PendingRecompile=Blueprint.Get(); Probe->RecompileRequestedFrame=GFrameCounter; Probe->bRecompileRequested=true;
}
void UVaRestDynamicTransferTestLibrary::ReenterTarget(UVaRestRequestAction* Action,UObject* Target,UVaRestDynamicTransferTestProbe* Probe,int32 Value,FString Cookie,int32 ContinuationCount)
{
	check(IsInGameThread()); if (!IsValid(Probe)) return;
	const TStrongObjectPtr<UVaRestDynamicTransferTestProbe> Observation(Probe);
	++Probe->ReentrantEntries;
	if (Probe->bCalled) return; // second user continuation never creates an unbounded third activation
	Probe->bCalled=true; Probe->ObservedValue=Value; Probe->ObservedCookie=MoveTemp(Cookie); Probe->ContinuationCount=ContinuationCount;
	if (!IsValid(Action) || !IsValid(Target) || !IsValid(Probe->ReentrantClient)) { Probe->ReentrantError=TEXT("MissingReentrantFixture"); return; }
	const auto Diagnostics=Action->GetTransferDiagnostics();
	Probe->bNoNativeCarrier=Diagnostics.Continuations==1 && Diagnostics.AddressBorrows==0;
#if WITH_DEV_AUTOMATION_TESTS
	Probe->bNoNativeCarrier &= VaRest::Http::Testing::LiveDynamicCarriersOnGT()==0;
#endif
	if (!Probe->bNoNativeCarrier) { Probe->ReentrantError=TEXT("RetainedNativeCarrier"); return; }
	const TStrongObjectPtr<UObject> PinnedTarget(Target);
	FVaRestTypeRef Type; Type.Kind=EVaRestTypeKind::Int;
	Probe->ReentrantAction=UVaRestRequestAction::CreateRequest(Target,Probe->ReentrantClient.Get(),TEXT("GET"),TEXT("/value"),Type,FVaRestTypeRef(),Probe->ReentrantError);
	if (!Probe->ReentrantAction) return;
	{
		const auto* Property=FindFProperty<FObjectPropertyBase>(Target->GetClass(),TEXT("Action"));
		if (!Property) { Probe->ReentrantError=TEXT("MissingGeneratedAction"); return; }
		Property->SetObjectPropertyValue_InContainer(Target,Probe->ReentrantAction.Get());
	} // no property/frame pointer survives the following real user graph call
	auto* Start=Target->FindFunction(FName(TEXT("Start")));
	if (!Start || Start->NumParms!=0) { Probe->ReentrantError=TEXT("MissingGeneratedStart"); return; }
	Probe->ReentrantSendFrame=GFrameCounter;
	Target->ProcessEvent(Start,nullptr); // SAME generated Bind/Activate path and shared output/cookie storage
	Probe->bSecondSent=IsValid(Probe->ReentrantAction) && Probe->ReentrantAction->GetHandle() && Probe->ReentrantAction->GetHandle()->GetId()!=0;
}
