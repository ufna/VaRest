// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "VaRestFlushQueueAction.h"
#include "VaRestActionAuthTestTypes.h"
#include "VaRestHub.h"
#include "VaRestApiClient.h"
#include "VaRestTrace.h"
#include "CoreGlobals.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

namespace VaRest::Http::Editor::FlushTest
{
/** Uses actual Native.FlushQueue false admission: an ordinary unnamed managed client has no queue to flush. */
class FRefusalCases final : public IAutomationLatentCommand
{
public:
	explicit FRefusalCases(FAutomationTestBase& InTest) : Test(InTest) {}
	virtual ~FRefusalCases() override { Close(); }
	virtual bool Update() override
	{
		if (!Action)
		{
			if (Case==3) return true;
			StartedAt=FPlatformTime::Seconds(); SentFrame=GFrameCounter; FString Error;
			Isolation=MakeUnique<FVaRestTrace::FScopedSinkIsolation>();
			Hub.Reset(NewObject<UVaRestHub>()); Owner.Reset(NewObject<UVaRestActionAuthTestProbe>());
			if (!Test.TestTrue(TEXT("real engine-only Hub initializes"),Hub->InitializeOnGT(nullptr,true,Error)) ||
				!Test.TestTrue(TEXT("separate fixture profile"),Hub->SetStorageProfile(FGuid::NewGuid().ToString(EGuidFormats::Digits),Error))) return true;
			Client.Reset(Hub->MakeClient(TEXT("https://flush.invalid")));
			if (!Test.TestNotNull(TEXT("real managed client has an unnamed default queue"),Client.Get())) return true;
			Action.Reset(UVaRestFlushQueueAction::FlushQueue(Owner.Get(),Client.Get(),Error));
			if (!Test.TestNotNull(*FString::Printf(TEXT("admitted later-frame action: %s"),*Error),Action.Get())) return true;
			Action->Activate(); Action->Activate();
			Test.TestFalse(TEXT("actual native false admission is reported immediately"),Action->WasAdmitted());
			Test.TestFalse(TEXT("false admission never notifies in the Send frame"),Action->HasResult());
			if (Case==1) Hub->DeinitializeOnGT();
			if (Case==2) Owner->MarkAsGarbage();
			return false;
		}
		if (FPlatformTime::Seconds()-StartedAt>30)
		{
			Test.AddError(TEXT("FlushQueue false admission kept waiting for a callback that cannot arrive")); return true;
		}
		if (!Action->IsFinished() || GFrameCounter<=SentFrame+1) return false;
		if (Case==0)
		{
			Test.TestTrue(TEXT("a refused flush has a real later-frame result without a native callback"),Action->HasResult());
			Test.TestEqual(TEXT("native false maps to Refused"),Action->GetResult(),EVaRestFlushQueueOutcome::Refused);
			Test.TestFalse(TEXT("duplicate Activate never changes native admission"),Action->WasAdmitted());
		}
		else Test.TestFalse(TEXT("Hub/owner loss releases the action without a fabricated result"),Action->HasResult());
		Close(); ++Case; return false;
	}
private:
	void Close()
	{
		if (Hub) Hub->DeinitializeOnGT();
		Action.Reset(); Client.Reset(); Owner.Reset(); Hub.Reset(); // all original aliases/pins released on GT
		Isolation.Reset();
	}
	FAutomationTestBase& Test;
	TStrongObjectPtr<UVaRestHub> Hub;
	TStrongObjectPtr<UVaRestApiClient> Client;
	TStrongObjectPtr<UVaRestFlushQueueAction> Action;
	TStrongObjectPtr<UObject> Owner;
	TUniquePtr<FVaRestTrace::FScopedSinkIsolation> Isolation;
	int32 Case=0;
	uint64 SentFrame=0;
	double StartedAt=0;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestFlushQueueActionTest,"VaRest.Api.Actions.Flush.RefusedAndSuppressed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestFlushQueueActionTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(VaRest::Http::Editor::FlushTest::FRefusalCases(*this)); return true;
}
#endif
