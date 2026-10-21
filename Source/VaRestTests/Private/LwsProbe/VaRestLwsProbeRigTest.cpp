// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// The rig's own checks (Probe.VaRest.Lws.Rig.*): before any fact is believed, the harness that records it must work

#include "VaRestLwsProbe.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "VaRestHttpTesting.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeRigEchoTest, "Probe.VaRest.Lws.Rig.Echo",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeRigEchoTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	// The engine's ::FEvent (HAL/Event.h:21) is visible here too, so the directive alone makes FEvent ambiguous
	using VaRest::LwsProbe::FEvent;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	FRig Rig(Site);
	if (!TestTrue(TEXT("the probe's context and service thread exist"), Rig.IsReady()))
	{
		return false;
	}
	const FString Key = NewKey();
	FConnectSpec Spec;
	Spec.Path = TEXT("/ws/echo?key=") + Key;
	const int32 Conn = Rig.Connect(MoveTemp(Spec));
	if (!TestTrue(TEXT("the connection opens"), Rig.WaitReason(Conn, LWS_CALLBACK_CLIENT_ESTABLISHED)))
	{
		return false;
	}
	Rig.WriteText(Conn, TEXT("hello"));
	TestTrue(TEXT("the echo arrives"), Rig.WaitFor([Conn](const TArray<FEvent>& All)
	{
		return All.ContainsByPredicate([Conn](const FEvent& Each)
		{
			return Each.Conn == Conn && Each.Reason == LWS_CALLBACK_CLIENT_RECEIVE && EventText(Each).Equals(TEXT("hello"), ESearchCase::CaseSensitive);
		});
	}));
	Rig.Close(Conn, 1000, TEXT("bye"));
	if (!TestTrue(TEXT("the connection ends"), Rig.WaitEnded(Conn)))
	{
		return false;
	}
	const FVaRestJson Journal = FetchJournal(*this, Site, Key);
	if (!TestTrue(TEXT("the server's journal has ended"), Journal[TEXT("ended")].AsBool()))
	{
		return false;
	}
	const TArray<FVaRestJson> Messages = JournalEvents(Journal, TEXT("message"));
	TestTrue(TEXT("the server got hello"), Messages.Num() == 1 && Messages[0][TEXT("text")].AsString().Equals(TEXT("hello"), ESearchCase::CaseSensitive));
	const TArray<FVaRestJson> Closes = JournalEvents(Journal, TEXT("close"));
	TestTrue(TEXT("the server got our 1000 bye"), Closes.Num() == 1 && Closes[0][TEXT("code")].AsInt() == 1000
		&& Closes[0][TEXT("reason")].AsString().Equals(TEXT("bye"), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeRigFactLineTest, "Probe.VaRest.Lws.Rig.FactLine",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeRigFactLineTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	// The contract with Tools/lws_probe_passport.py: one JSON object per FACT line, whatever the observed text holds
	VaRest::Http::Testing::FLogCapture Capture;
	Fact(*this, TEXT("rig.fact_line"), EVerdict::Recorded, TEXT("a \"quoted\" | piped value"), TEXT("none"), TEXT("the rig's own check"));
	const TArray<FString> Lines = Capture.GetLines();
	const FString* Line = Lines.FindByPredicate([](const FString& Each) { return Each.StartsWith(TEXT("LogVaRestLwsProbe: FACT {")); });
	if (!TestNotNull(TEXT("one FACT line"), Line))
	{
		return false;
	}
	FString Json;
	Line->Split(TEXT("FACT "), nullptr, &Json);
	TValueOrError<FVaRestJson, FVaRestJsonError> Parsed = FVaRestJson::Parse(FStringView(Json));
	if (!TestTrue(TEXT("the line is JSON"), Parsed.HasValue()))
	{
		return false;
	}
	const FVaRestJson& Value = Parsed.GetValue();
	TestTrue(TEXT("id, verdict and observed survive"), Value[TEXT("id")].AsString().Equals(TEXT("rig.fact_line"), ESearchCase::CaseSensitive)
		&& Value[TEXT("verdict")].AsString().Equals(TEXT("recorded"), ESearchCase::CaseSensitive)
		&& Value[TEXT("observed")].AsString().Equals(TEXT("a \"quoted\" | piped value"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("the test is named"), Value[TEXT("test")].AsString().Contains(TEXT("Probe.VaRest.Lws.Rig.FactLine")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeRigSequentialTest, "Probe.VaRest.Lws.Rig.SequentialConnections",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeRigSequentialTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	// The engine's ::FEvent (HAL/Event.h:21) is visible here too, so the directive alone makes FEvent ambiguous
	using VaRest::LwsProbe::FEvent;
	// Review focus: LWS may hand a freed wsi's address to the next connection; the rig matches the userdata first
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	FRig Rig(Site);
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	TArray<int32> Ids;
	for (int32 Round = 0; Round < 5; ++Round)
	{
		FConnectSpec Spec;
		const int32 Conn = Rig.Connect(MoveTemp(Spec));
		Ids.Add(Conn);
		if (!TestTrue(FString::Printf(TEXT("round %d opens"), Round), Rig.WaitReason(Conn, LWS_CALLBACK_CLIENT_ESTABLISHED)))
		{
			return false;
		}
		const FString Hello = FString::Printf(TEXT("round %d"), Round);
		Rig.WriteText(Conn, Hello);
		TestTrue(FString::Printf(TEXT("round %d echoes"), Round), Rig.WaitFor([Conn, Hello](const TArray<FEvent>& All)
		{
			return All.ContainsByPredicate([Conn, &Hello](const FEvent& Each)
			{
				return Each.Conn == Conn && Each.Reason == LWS_CALLBACK_CLIENT_RECEIVE && EventText(Each).Equals(Hello, ESearchCase::CaseSensitive);
			});
		}));
		Rig.Close(Conn, 1000, TEXT("next"));
		TestTrue(FString::Printf(TEXT("round %d ends"), Round), Rig.WaitEnded(Conn));
	}
	const TArray<FEvent> Everything = Rig.Events();
	for (const int32 Id : Ids)
	{
		const int32 Opens = Everything.FilterByPredicate([Id](const FEvent& Each) { return Each.Conn == Id && Each.Reason == LWS_CALLBACK_CLIENT_ESTABLISHED; }).Num();
		const int32 Ends = Everything.FilterByPredicate([Id](const FEvent& Each) { return Each.Conn == Id && Each.Reason == LWS_CALLBACK_WSI_DESTROY; }).Num();
		TestTrue(FString::Printf(TEXT("connection %d: one ESTABLISHED and one WSI_DESTROY (saw %d and %d)"), Id, Opens, Ends), Opens == 1 && Ends == 1);
	}
	TestFalse(TEXT("no open or receive is left without its connection"), Everything.ContainsByPredicate([](const FEvent& Each)
	{
		return Each.Conn == 0 && (Each.Reason == LWS_CALLBACK_CLIENT_ESTABLISHED || Each.Reason == LWS_CALLBACK_CLIENT_RECEIVE);
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeRigTeardownTest, "Probe.VaRest.Lws.Rig.TeardownWithOpenConnection",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeRigTeardownTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	// The engine's ::FEvent (HAL/Event.h:21) is visible here too, so the directive alone makes FEvent ambiguous
	using VaRest::LwsProbe::FEvent;
	// Review focus: a probe whose connection never ends still gets its rig torn down, promptly
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	FRig Rig(Site);
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	const FString Key = NewKey();
	FConnectSpec Spec;
	Spec.Path = TEXT("/ws/silent?key=") + Key;
	const int32 Conn = Rig.Connect(MoveTemp(Spec));
	if (!TestTrue(TEXT("the connection opens"), Rig.WaitReason(Conn, LWS_CALLBACK_CLIENT_ESTABLISHED)))
	{
		return false;
	}
	// The service thread parks in its 10 s wait (FRigOptions::ServiceTimeoutMs) before the teardown starts
	FPlatformProcess::Sleep(0.2f);
	const double Before = FPlatformTime::Seconds();
	Rig.Shutdown();
	const double Took = FPlatformTime::Seconds() - Before;
	TestTrue(FString::Printf(TEXT("the teardown returns within 5 s with a connection open (took %.2f s)"), Took), Took < 5.0);
	FEvent Destroyed;
	TestTrue(TEXT("the open connection ended inside the teardown"), Rig.Find(Conn, LWS_CALLBACK_WSI_DESTROY, Destroyed) && Destroyed.Time >= Before);
	TestTrue(TEXT("the server saw the connection end"), FetchJournal(*this, Site, Key)[TEXT("ended")].AsBool());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
