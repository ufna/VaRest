// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS && !WITH_EDITOR
#include "VaRestDynamicClosure.h"
#include "VaRestJson.h"
#include "VaRestJsonRules.h"
#include "CoreGlobals.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/Class.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestDynamicGamePublicationTest, "VaRest.Http.Dynamic.GameRulePublication",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestDynamicGamePublicationTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	TStrongObjectPtr<UEnum> Owner(NewObject<UEnum>());
	FVaRestDynamicResultConfig Config;
	Config.ResponseType.Kind=EVaRestTypeKind::Struct;
	Config.ResponseType.Type=FVaRestJson::StaticStruct(); // existing real intrinsic type; no reflected test type
	Config.RuleOwner=Owner.Get();
	FString Error,Code;
	auto Initial=FDynamicTypedState::CreateOnGT(Config,Error,&Code);
	if (!TestTrue(TEXT("initial real Game GT admission"),Initial.IsValid())) return false;
	int32 ChangingCalls=0,ChangedCalls=0;
	const auto Changing=FVaRestJsonRules::OnRulesChanging().AddLambda([&] {
		++ChangingCalls;
		TestTrue(TEXT("changing callback is inside registry transaction"),FVaRestJsonRules::IsChangingOnGameThread());
		TestFalse(TEXT("no admission before actual publication"),FDynamicTypedState::CreateOnGT(Config,Error,&Code).IsValid());
		TestEqual(TEXT("changing refusal retains real code"),Code,FString(TEXT("DynamicTypeChanged")));
	});
	const auto Changed=FVaRestJsonRules::OnRulesChanged().AddLambda([&] {
		++ChangedCalls;
		TestTrue(TEXT("changed callback is still inside registry transaction"),FVaRestJsonRules::IsChangingOnGameThread());
		TestFalse(TEXT("no early admission from changed notification"),FDynamicTypedState::CreateOnGT(Config,Error,&Code).IsValid());
		TestEqual(TEXT("changed refusal retains real code"),Code,FString(TEXT("DynamicTypeChanged")));
	});
	ON_SCOPE_EXIT {
		FVaRestJsonRules::OnRulesChanging().Remove(Changing);
		FVaRestJsonRules::OnRulesChanged().Remove(Changed);
		FVaRestJsonRules::UnregisterOwner(Owner.Get());
	};
	const uint64 Frame=GFrameCounter;
	TestTrue(TEXT("real owner publication completes"),FVaRestJsonRules::RegisterTypesFolder(Owner.Get(),TEXT("/Game/VaRestPublicationFixture")));
	TestFalse(TEXT("old captured schema remains invalid"),Initial->ValidateActivationOnGT(Error));
	auto Published=FDynamicTypedState::CreateOnGT(Config,Error,&Code);
	TestTrue(TEXT("completed Game publication admits without waiting for EndFrame"),Published.IsValid());
	if (Published) TestTrue(TEXT("new capture validates its current schema"),Published->ValidateActivationOnGT(Error));
	TestEqual(TEXT("actual pre-publication callback once"),ChangingCalls,1);
	TestEqual(TEXT("actual post-publication callback once"),ChangedCalls,1);
	FVaRestJsonRules::UnregisterOwner(Owner.Get());
	if (Published) TestFalse(TEXT("unregister invalidates that captured schema"),Published->ValidateActivationOnGT(Error));
	auto Removed=FDynamicTypedState::CreateOnGT(Config,Error,&Code);
	TestTrue(TEXT("completed unregister also admits a current capture"),Removed.IsValid());
	TestEqual(TEXT("all real changing callbacks observed"),ChangingCalls,2);
	TestEqual(TEXT("all real changed callbacks observed"),ChangedCalls,2);
	TestEqual(TEXT("no simulated frame advance or latent wait"),GFrameCounter,Frame);
	return true;
}
#endif
