// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HttpManager.h"
#include "HttpModule.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/ScopeExit.h"
#include "Misc/URLRequestFilter.h"
#include "VaRestClassify.h"
#include "VaRestClient.h"
#include "VaRestHttpTesting.h"
#include "VaRestHttpTransportInternal.h"

namespace VaRest::Http::FidelityTestPrivate
{
	FVaRestAttemptOutcome Failed(EVaRestTransportFailure Failure, const TCHAR* Detail)
	{
		FVaRestAttemptOutcome Out;
		Out.Failure = Failure;
		Out.FailureDetail = Detail;
		return Out;
	}

	FVaRestAttemptOutcome Status(int32 Code)
	{
		FVaRestAttemptOutcome Out;
		Out.Status = Code;
		return Out;
	}

	FString Describe(const FAttemptClass& Class)
	{
		return FString::Printf(TEXT("%s%s"), LexToString(Class.Kind), Class.bRetryable ? TEXT("+retry") : TEXT(""));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpFidelityClassifyTest, "VaRest.Http.Fidelity.Classify",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpFidelityClassifyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::FidelityTestPrivate;
	struct FCase
	{
		FVaRestAttemptOutcome Outcome;
		const TCHAR* Expected;
	};
	FVaRestAttemptOutcome LargeError = Failed(EVaRestTransportFailure::Other, TEXT("ResponseTooLarge"));
	LargeError.Status = 503;
	const FCase Cases[] = {
		{ Status(200), TEXT("None") },
		{ Status(204), TEXT("None") },
		{ Status(404), TEXT("Http") },
		{ Status(503), TEXT("Http+retry") },
		{ Status(429), TEXT("Http+retry") },
		{ Failed(EVaRestTransportFailure::Connection, TEXT("ConnectionError")), TEXT("Connection+retry") },
		// Plan decision 15: the engine's other Other is a network failure on curl (a reset, an empty reply)
		{ Failed(EVaRestTransportFailure::Other, TEXT("Other")), TEXT("Connection+retry") },
		{ Failed(EVaRestTransportFailure::Timeout, TEXT("TimedOut")), TEXT("Timeout+retry") },
		{ Failed(EVaRestTransportFailure::Canceled, TEXT("Cancelled")), TEXT("Canceled") },
		{ Failed(EVaRestTransportFailure::Other, TEXT("TransportDropped")), TEXT("Connection") },
		{ Failed(EVaRestTransportFailure::Other, TEXT("InvalidHeader")), TEXT("Config") },
		{ Failed(EVaRestTransportFailure::Other, TEXT("HttpDisabled")), TEXT("Config") },
		{ Failed(EVaRestTransportFailure::Other, TEXT("DomainNotAllowed")), TEXT("Config") },
		// Ruling Q7: a built-in auth whose configuration cannot work is the request's fault, like an invalid header
		{ Failed(EVaRestTransportFailure::Other, TEXT("AuthInvalid")), TEXT("Config") },
		// Ruling I-7: a parked request fails as its refresh did -- Config is not retried, a network failure is
		{ Failed(EVaRestTransportFailure::Other, TEXT("AuthRefreshConfig")), TEXT("Config") },
		{ Failed(EVaRestTransportFailure::Connection, TEXT("AuthRefreshFailed")), TEXT("Connection+retry") },
		{ Failed(EVaRestTransportFailure::Timeout, TEXT("AuthRefreshFailed")), TEXT("Timeout+retry") },
		{ Failed(EVaRestTransportFailure::Other, TEXT("ResponseTooLarge")), TEXT("Decode") },
		{ LargeError, TEXT("Http+retry") },
		{ Failed(EVaRestTransportFailure::Other, TEXT("AuthRejected")), TEXT("Auth") },
		{ Failed(EVaRestTransportFailure::Other, TEXT("NoCredentials")), TEXT("Auth") },
		{ Failed(EVaRestTransportFailure::Other, TEXT("SessionCleared")), TEXT("Auth") },
		{ Failed(EVaRestTransportFailure::Timeout, TEXT("Deadline")), TEXT("Timeout") },
	};
	for (const FCase& Case : Cases)
	{
		TestEqualSensitive(FString::Printf(TEXT("status %d, %s %s"), Case.Outcome.Status, LexToString(Case.Outcome.Failure), *Case.Outcome.FailureDetail.ToString()),
			Describe(ClassifyAttempt(Case.Outcome)), FString(Case.Expected));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpFidelityBodyVerbTest, "VaRest.Http.Fidelity.BodyOnVerbNotSent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpFidelityBodyVerbTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	// Plan decision 13 (final review M1): curl sends a body only with POST, PUT, PATCH and DELETE (CurlHttp.cpp:941-993);
	// the fake mirrors that, so a body on another verb is a Config error and never reaches the transport
	FTestRig Rig;
	const TCHAR* Refused[] = { TEXT("HEAD"), TEXT("OPTIONS"), TEXT("QUERY"), TEXT("PROPFIND") };
	const TCHAR* Sent[] = { TEXT("POST"), TEXT("PUT"), TEXT("PATCH"), TEXT("DELETE"), TEXT("post") };
	TArray<TSharedRef<TTestOutcome<FVaRestJson>, ESPMode::ThreadSafe>> RefusedOutcomes;
	TArray<TSharedRef<TTestOutcome<FVaRestJson>, ESPMode::ThreadSafe>> SentOutcomes;
	for (const TCHAR* Verb : Refused)
	{
		RefusedOutcomes.Add(MakeOutcome<FVaRestJson>());
		Observe(Rig.Client.Request(Verb, TEXT("/r")).Body(TEXT("x")), RefusedOutcomes.Last()).Send();
	}
	for (const TCHAR* Verb : Sent)
	{
		SentOutcomes.Add(MakeOutcome<FVaRestJson>());
		Observe(Rig.Client.Request(Verb, TEXT("/s")).Body(TEXT("x")), SentOutcomes.Last()).Send();
	}
	auto NoBody = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Request(TEXT("OPTIONS"), TEXT("/n")), NoBody).Send();
	const auto AllDone = [&RefusedOutcomes, &SentOutcomes, NoBody]
	{
		for (const auto& One : RefusedOutcomes) { if (!One->IsDone()) { return false; } }
		for (const auto& One : SentOutcomes) { if (!One->IsDone()) { return false; } }
		return NoBody->IsDone();
	};
	if (!TestTrue(TEXT("all complete"), PumpUntil(AllDone)))
	{
		return false;
	}
	for (int32 Index = 0; Index < RefusedOutcomes.Num(); ++Index)
	{
		const FVaRestError& Error = RefusedOutcomes[Index]->Error;
		TestTrue(FString::Printf(TEXT("%s with a body: Kind Config, not retryable, naming the verb"), Refused[Index]),
			Error.Kind == EVaRestErrorKind::Config && !Error.bRetryable && Error.Message.Contains(Refused[Index]));
	}
	for (int32 Index = 0; Index < SentOutcomes.Num(); ++Index)
	{
		TestEqual(FString::Printf(TEXT("%s with a body: sent"), Sent[Index]), SentOutcomes[Index]->SuccessCalls.load(), 1);
	}
	TestEqual(TEXT("OPTIONS without a body: sent"), NoBody->SuccessCalls.load(), 1);
	TestEqual(TEXT("only the sendable ones reached the transport"), Rig.Fake->NumCalls(), 6);

	// A transport that says it sends any body (a custom one) gets it
	FTestRig Any;
	Any.Fake->bSendsBodyForAnyVerb = true;
	auto Query = MakeOutcome<FVaRestJson>();
	Observe(Any.Client.Request(TEXT("QUERY"), TEXT("/q")).Body(TEXT("x")), Query).Send();
	if (!TestTrue(TEXT("QUERY completes"), Any.Wait(Query)) || !TestEqual(TEXT("... sent through a transport that sends it"), Any.Fake->NumCalls(), 1))
	{
		return false;
	}
	TestEqual(TEXT("... with its body"), Any.Fake->ReadBody(0).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpFidelityPreCheckTest, "VaRest.Http.Fidelity.PreCheckRefusal",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpFidelityPreCheckTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	FVaRestClient Client = FVaRestClient::Make(TEXT("http://127.0.0.1:1"));
	Client.WithLogLevel(EVaRestLogLevel::None);
	{
		// Plan decision 15 (final review M9): with [HTTP] bEnableHttp false the engine's PreCheck refuses the request
		// (HttpRequestCommon.cpp:83-87) and reports EHttpFailureReason::Other, which VaRest read as a retryable Connection
		// error. UpdateConfigs re-reads the switch (HttpModule.cpp:58-68); the real transport, no server needed.
		bool bWasEnabled = true;
		GConfig->GetBool(TEXT("HTTP"), TEXT("bEnableHttp"), bWasEnabled, GEngineIni);
		GConfig->SetBool(TEXT("HTTP"), TEXT("bEnableHttp"), false, GEngineIni);
		FHttpModule::Get().UpdateConfigs();
		// At the end of this block: the domain check below needs HTTP on
		ON_SCOPE_EXIT
		{
			GConfig->SetBool(TEXT("HTTP"), TEXT("bEnableHttp"), bWasEnabled, GEngineIni);
			FHttpModule::Get().UpdateConfigs();
		};
		auto Outcome = MakeOutcome<FVaRestJson>();
		Observe(Client.Get(TEXT("/x")), Outcome).Send();
		if (!TestTrue(TEXT("completes"), PumpUntil([Outcome] { return Outcome->IsDone(); }, 10.0, true)))
		{
			return false;
		}
		TestTrue(TEXT("HTTP disabled: Kind Config, not retryable"), Outcome->Error.Kind == EVaRestErrorKind::Config && !Outcome->Error.bRetryable);
		TestTrue(TEXT("... naming why"), Outcome->Error.Message.Contains(TEXT("HttpDisabled")));
	}
	{
		// Spec review M-8: the domain filter IsDomainAllowed reads is the HTTP manager's own, loaded from
		// [Online.HttpManager http] / [Online.HttpManager https] AllowedDomains (HttpManager.cpp:324-326,
		// URLRequestFilter.h:36-46), not [HTTP]. The test sets one in code (HttpManager.h:314); UpdateConfigs on the manager
		// re-reads the project's own after. The engine refuses at PreCheck as Other too (HttpRequestCommon.cpp:109-113).
		UE::Core::FURLRequestFilter::FRequestMap Allowed;
		Allowed.Add(TEXT("http"), TArray<FString>{ TEXT("example.invalid") });
		FHttpModule::Get().GetHttpManager().SetURLRequestFilter(UE::Core::FURLRequestFilter(MoveTemp(Allowed)));
		ON_SCOPE_EXIT { FHttpModule::Get().GetHttpManager().UpdateConfigs(); };
		auto Outcome = MakeOutcome<FVaRestJson>();
		Observe(Client.Get(TEXT("/y")), Outcome).Send();
		if (!TestTrue(TEXT("completes"), PumpUntil([Outcome] { return Outcome->IsDone(); }, 10.0, true)))
		{
			return false;
		}
		TestTrue(TEXT("a domain the filter does not allow: Kind Config, not retryable"), Outcome->Error.Kind == EVaRestErrorKind::Config && !Outcome->Error.bRetryable);
		TestTrue(TEXT("... naming why"), Outcome->Error.Message.Contains(TEXT("DomainNotAllowed")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpFidelityHeaderCollectTest, "VaRest.Http.Fidelity.HeadersOneByOne",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpFidelityHeaderCollectTest::RunTest(const FString& Parameters)
{
	// Plan decision 14 (final review M2): the completion keeps each header line the delegate reports, repeats included
	VaRest::Http::FHttpTransportCompletion Completion(FVaRestTransportComplete(), MakeShared<VaRest::Http::FBodySink, ESPMode::ThreadSafe>(0),
		MakeShared<FVaRestTransportProbe, ESPMode::ThreadSafe>(), nullptr);
	Completion.AddHeader(TEXT("Set-Cookie"), TEXT("a=1; Expires=Wed, 21 Oct 2026 07:28:00 GMT"));
	Completion.AddHeader(TEXT("Content-Type"), TEXT("application/json"));
	Completion.AddHeader(TEXT("Set-Cookie"), TEXT("b=2"));
	const FVaRestHeaders Taken = Completion.TakeHeaders();
	TestEqual(TEXT("three lines, in order"), Taken.Num(), 3);
	int32 Cookies = 0;
	for (const FVaRestHeaders::FPair& Pair : Taken)
	{
		Cookies += Pair.Key.Equals(TEXT("Set-Cookie"), ESearchCase::IgnoreCase) ? 1 : 0;
	}
	TestEqual(TEXT("both Set-Cookie headers, apart"), Cookies, 2);
	TestEqual(TEXT("taken once"), Completion.TakeHeaders().Num(), 0);
	return true;
}

#endif
