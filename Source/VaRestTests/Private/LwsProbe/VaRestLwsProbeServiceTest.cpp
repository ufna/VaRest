// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// WebSocket spec 16.4 (W0): the environment, the service thread's command path, timers and forced closes, and the
// handshake timeouts. Two service waits (WS W0 decision 9, review C-1): LWS checks its own deadlines only when
// lws_service returns (lws-plat-unix.c.o _lws_plat_service_tsi: the hrtimer list caps poll()'s wait, nothing else
// does), so facts about LWS's timers take a 100 ms wait, and facts about the service shape the rig's 10 s one

#include "VaRestLwsProbe.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformProperties.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

#if PLATFORM_LINUX
#include <dlfcn.h>
#endif

namespace VaRest::LwsProbe::VaRestLwsProbeServiceTestPrivate
{
	/** The wait for facts about LWS's own timers: short enough that a deadline is noticed within 0.1 s of passing */
	constexpr int32 ShortWaitMs = 100;
	/** The rig's default, spec 2.1's long wait woken by lws_cancel_service */
	constexpr int32 LongWaitMs = 10000;

	struct FRoundTrip
	{
		bool bEstablished = false;
		bool bEchoed = false;
		bool bEnded = false;
		double WakeMs = -1.0;
	};

	/** Connect, a wake while the service thread waits, an echo, a close: every step goes through the command queue */
	FRoundTrip RoundTrip(const FServer& Site, bool bInsideCallback)
	{
		FRoundTrip Out;
		FRigOptions Options;
		Options.bCommandsInsideCallback = bInsideCallback;
		FRig Rig(Site, MoveTemp(Options));
		if (!Rig.IsReady())
		{
			return Out;
		}
		FConnectSpec Spec;
		Spec.Path = TEXT("/ws/echo?key=") + NewKey();
		const int32 Conn = Rig.Connect(MoveTemp(Spec));
		Out.bEstablished = Rig.WaitReason(Conn, LWS_CALLBACK_CLIENT_ESTABLISHED);
		if (Out.bEstablished)
		{
			// The service thread parks in its 10 s wait (FRigOptions::ServiceTimeoutMs) before the wake is timed
			FPlatformProcess::Sleep(0.2f);
			const double Queued = FPlatformTime::Seconds();
			Rig.Exec(TEXT("wake"), [](FRig&) {});
			FEvent Ran;
			if (Rig.WaitReason(0, CommandRan) && Rig.Find(0, CommandRan, Ran, TEXT("wake")))
			{
				Out.WakeMs = (Ran.Time - Queued) * 1000.0;
			}
			Rig.WriteText(Conn, TEXT("round trip"));
			Out.bEchoed = Rig.WaitFor([Conn](const TArray<FEvent>& All)
			{
				return All.ContainsByPredicate([Conn](const FEvent& Each)
				{
					return Each.Conn == Conn && Each.Reason == LWS_CALLBACK_CLIENT_RECEIVE && EventText(Each).Equals(TEXT("round trip"), ESearchCase::CaseSensitive);
				});
			});
			Rig.Close(Conn, 1000, TEXT("done"));
		}
		Out.bEnded = Rig.WaitEnded(Conn);
		return Out;
	}

	FString Describe(const FRoundTrip& Trip)
	{
		return FString::Printf(TEXT("established=%s echoed=%s ended=%s, wake %.1f ms"), YesNo(Trip.bEstablished), YesNo(Trip.bEchoed), YesNo(Trip.bEnded), Trip.WakeMs);
	}

	struct FHang
	{
		bool bEnded = false;
		double Seconds = -1.0;
		FString Error;
		FString Sequence;
		TArray<FEvent> Seen;
	};

	/**
	 * A connection whose TCP connects and whose upgrade is never answered (/ws/hang), or a TCP connect to Address, timed
	 * from the connect call to its end, with the short wait: these ask what LWS's own deadline is (review C-1)
	 */
	FHang Hang(const FServer& Site, uint32 TimeoutSecs, const FString& Address, int32 Port, double WaitSeconds)
	{
		FHang Out;
		FRigOptions Options;
		Options.TimeoutSecs = TimeoutSecs;
		Options.ServiceTimeoutMs = ShortWaitMs;
		FRig Rig(Site, MoveTemp(Options));
		if (!Rig.IsReady())
		{
			return Out;
		}
		FConnectSpec Spec;
		Spec.Path = TEXT("/ws/hang?ms=60000&key=") + NewKey();
		Spec.Address = Address;
		Spec.Port = Port;
		const int32 Conn = Rig.Connect(MoveTemp(Spec));
		Out.bEnded = Rig.WaitEnded(Conn, WaitSeconds);
		Out.Seen = Rig.Events(Conn);
		FEvent Start;
		FEvent End;
		const bool bStarted = Rig.Find(Conn, ConnectCalling, Start);
		if (Out.bEnded && bStarted && (Rig.Find(Conn, LWS_CALLBACK_CLIENT_CONNECTION_ERROR, End) || Rig.Find(Conn, LWS_CALLBACK_WSI_DESTROY, End)))
		{
			Out.Seconds = End.Time - Start.Time;
		}
		Out.Error = ErrorOf(Out.Seen);
		Out.Sequence = CallSequence(Out.Seen);
		return Out;
	}

	FString Describe(const FHang& Run, double WaitSeconds)
	{
		return Run.bEnded ? FString::Printf(TEXT("%.1f s, then \"%s\" (%s)"), Run.Seconds, *Run.Error, *Run.Sequence)
			: FString::Printf(TEXT("still pending after %.0f s (%s)"), WaitSeconds, *Run.Sequence);
	}

	/** TEST-NET-1 (RFC 5737): never assigned, so a SYN to it usually goes unanswered */
	constexpr const TCHAR* BlackholeAddress = TEXT("192.0.2.1");
	/** An end this soon after the connect call is a failure of the connect, not one of LWS's timeouts (whole seconds) */
	constexpr double AtOnceSeconds = 0.5;

	/**
	 * A blackhole fact. Recorded with the measured time only when the connect really hung; unverified when it did not
	 * black-hole here, so Task 11 never applies a value that measured something else (controller ruling, Task 5). Read
	 * from the rig's events: APPEND_HANDSHAKE_HEADER means the TCP connect completed, because LWS compiles the handshake
	 * only to write it to the socket (client.c.o lws_client_socket_service: lws_generate_client_handshake at 0x3d2, its
	 * one call, then lws_ssl_capable_write at 0x40e). A transparent proxy accepts every connect that way
	 */
	void BlackholeFact(FAutomationTestBase& Test, const TCHAR* Id, const FHang& Run, double WaitSeconds, const FString& Spec, const FString& Evidence)
	{
		const bool bNullConnect = ConnectResult(Run.Seen).Equals(TEXT("null"), ESearchCase::CaseSensitive);
		const bool bConnected = Run.Seen.ContainsByPredicate([](const FEvent& Each)
		{
			return Each.Reason == LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER || Each.Reason == LWS_CALLBACK_CLIENT_FILTER_PRE_ESTABLISH
				|| Each.Reason == LWS_CALLBACK_CLIENT_ESTABLISHED || Each.Reason == LWS_CALLBACK_CLIENT_RECEIVE;
		});
		// Seconds stays negative when an end was seen but not timed: that measured nothing either
		const bool bAtOnce = Run.bEnded && Run.Seconds < AtOnceSeconds;
		if (!bNullConnect && !bConnected && !bAtOnce)
		{
			Fact(Test, Id, EVerdict::Recorded, Describe(Run, WaitSeconds), Spec, Evidence);
			return;
		}
		const FString Happened = bNullConnect ? FString::Printf(TEXT("lws_client_connect_via_info returned null: %s"), *Run.Sequence)
			: bConnected ? FString::Printf(TEXT("the TCP connect completed and the upgrade went out: %s"), *Describe(Run, WaitSeconds))
			: FString::Printf(TEXT("the connect failed at once: %s"), *Describe(Run, WaitSeconds));
		Fact(Test, Id, EVerdict::Unverified,
			FString::Printf(TEXT("unverified here: the connect to %s did not black-hole (%s); release-pass item (a host without a transparent proxy)"), BlackholeAddress, *Happened),
			Spec, Evidence);
	}

	struct FKill
	{
		bool bReady = false;
		bool bEnded = false;
		double Ms = -1.0;
		FString Sequence;
		FString Key;
		FString KillNote = TEXT("not run");
	};

	/**
	 * A connection to Route killed with lws_set_timeout(LWS_TO_KILL_ASYNC) from a command -- open (bWhenOpen, /ws/echo)
	 * or pending with its upgrade unanswered (/ws/hang) -- timed from the command to WSI_DESTROY under one service wait
	 */
	FKill Kill(const FServer& Site, const FString& Route, int32 ServiceTimeoutMs, bool bWhenOpen)
	{
		FKill Out;
		FRigOptions Options;
		Options.ServiceTimeoutMs = ServiceTimeoutMs;
		FRig Rig(Site, MoveTemp(Options));
		if (!Rig.IsReady())
		{
			return Out;
		}
		Out.Key = NewKey();
		FConnectSpec Spec;
		Spec.Path = FString::Printf(TEXT("%s%skey=%s"), *Route, Route.Contains(TEXT("?")) ? TEXT("&") : TEXT("?"), *Out.Key);
		const int32 Conn = Rig.Connect(MoveTemp(Spec));
		Out.bReady = bWhenOpen ? Rig.WaitReason(Conn, LWS_CALLBACK_CLIENT_ESTABLISHED) : Rig.WaitReason(Conn, ConnectReturned);
		if (!Out.bReady)
		{
			Rig.WaitEnded(Conn);
			return Out;
		}
		// Open: the service thread parks in its wait. Pending: the upgrade is out and unanswered by now
		FPlatformProcess::Sleep(bWhenOpen ? 0.2f : 1.0f);
		Rig.ExecOn(Conn, TEXT("kill"), [](FConn& C) { lws_set_timeout(C.Wsi, PENDING_TIMEOUT_USER_REASON_BASE, LWS_TO_KILL_ASYNC); });
		Out.bEnded = Rig.WaitEnded(Conn, 25.0);
		FEvent Killed;
		FEvent Destroyed;
		if (Rig.Find(Conn, CommandRan, Killed, TEXT("kill")))
		{
			Out.KillNote = Killed.Note;
		}
		// Review M-12: the sequence and the latency only from a kill that ran (a "kill: no wsi" never reached LWS)
		if (Out.bEnded && Out.KillNote.Equals(TEXT("kill"), ESearchCase::CaseSensitive) && Rig.Find(Conn, LWS_CALLBACK_WSI_DESTROY, Destroyed))
		{
			Out.Ms = (Destroyed.Time - Killed.Time) * 1000.0;
			Out.Sequence = CallSequence(Rig.Events(Conn).FilterByPredicate([&Killed](const FEvent& Each) { return Each.Time >= Killed.Time && Each.Reason != CommandRan; }));
		}
		return Out;
	}

	FString Describe(const FKill& Run, int32 ServiceTimeoutMs)
	{
		return Run.Ms >= 0.0
			? FString::Printf(TEXT("%.1f ms from lws_set_timeout(LWS_TO_KILL_ASYNC) to WSI_DESTROY, lws_service waiting up to %d ms"), Run.Ms, ServiceTimeoutMs)
			: FString::Printf(TEXT("not measured: ready %s, kill %s, ended %s (wait %d ms)"), YesNo(Run.bReady), *Run.KillNote, YesNo(Run.bEnded), ServiceTimeoutMs);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeEnvironmentTest, "Probe.VaRest.Lws.Service.Environment",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeEnvironmentTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	const char* LwsVersion = lws_get_library_version();
	const FString Version = Utf8Text(LwsVersion, FCStringAnsi::Strlen(LwsVersion));
	Fact(*this, TEXT("env.lws_version"), AgreesIf(Version.StartsWith(TEXT("3.0.0"))), Version,
		TEXT("T1: libwebsockets 3.0.0 on every target (lws_config.h:34)"), TEXT("lws_get_library_version() in VaRestTests' copy"));
	const char* SslVersion = OpenSSL_version(OPENSSL_VERSION);
	// Review M-5: VaRestTests links OpenSSL statically, but on Linux its calls may bind to the global-scope copy (the
	// HTTP and SSL modules export it): which object the function resolves to goes in the evidence
	FString SslOwner = TEXT("which object provides it is not looked up on this platform");
#if PLATFORM_LINUX
	Dl_info SslInfo = {};
	SslOwner = dladdr(reinterpret_cast<void*>(&OpenSSL_version), &SslInfo) != 0 && SslInfo.dli_fname
		? FString::Printf(TEXT("OpenSSL_version resolves to %s"), *FPaths::GetCleanFilename(UTF8_TO_TCHAR(SslInfo.dli_fname)))
		: FString(TEXT("dladdr found no object for OpenSSL_version"));
#endif
	Fact(*this, TEXT("env.openssl_version"), EVerdict::Recorded, Utf8Text(SslVersion, FCStringAnsi::Strlen(SslVersion)),
		TEXT("none (the engine bundles OpenSSL 1.1.1t, ThirdParty/OpenSSL/OpenSSL.Build.cs:12)"),
		FString::Printf(TEXT("OpenSSL_version(OPENSSL_VERSION) as VaRestTests' calls resolve it; %s"), *SslOwner));
	const FString Platform = ANSI_TO_TCHAR(FPlatformProperties::PlatformName());
	Fact(*this, TEXT("env.build"), EVerdict::Recorded,
		FString::Printf(TEXT("%s, %s, %s"), *Platform, IS_MONOLITHIC ? TEXT("monolithic") : TEXT("modular"), LexToString(FApp::GetBuildConfiguration())),
		TEXT("none"), TEXT("FPlatformProperties::PlatformName, IS_MONOLITHIC, FApp::GetBuildConfiguration"));
	// Review I-4: sampled in FVaRestTestsModule::StartupModule, before any test (tests run in name order, and Coexist,
	// which loads WebSockets, runs before this one)
	Fact(*this, TEXT("env.websockets_loaded_at_start"), EVerdict::Recorded,
		FString::Printf(TEXT("%s at VaRestTests' startup (%s when this test ran)"), YesNo(GWebSocketsLoadedAtModuleStartup), YesNo(FModuleManager::Get().IsModuleLoaded(TEXT("WebSockets")))),
		TEXT("WebSocket spec D30 sets the LWS log level only when the engine's WebSockets module is not loaded"),
		TEXT("FModuleManager::IsModuleLoaded(\"WebSockets\") in FVaRestTestsModule::StartupModule (LoadingPhase Default), and again now"));

	const FString ReleasePass = TEXT("WebSocket spec 16.6: Win64, Mac, Android and iOS in the release pass (bead VaRest-b4d)");
	const FString Here = FString::Printf(TEXT("this run: %s"), *Platform);
	Fact(*this, TEXT("release.win64"), PLATFORM_WINDOWS ? EVerdict::Recorded : EVerdict::Unverified,
		PLATFORM_WINDOWS ? FString(TEXT("this is the Win64 run: see the other rows"))
			: FString(TEXT("unverified here; release-pass item (Win64): run Tools/run_tests.sh Probe.VaRest.Lws and each Probe.VaRest.Risky case there")),
		ReleasePass, Here);
	Fact(*this, TEXT("release.mac"), PLATFORM_MAC ? EVerdict::Recorded : EVerdict::Unverified,
		PLATFORM_MAC ? FString(TEXT("this is the Mac run: see the other rows"))
			: FString(TEXT("unverified here; release-pass item (Mac): run Tools/run_tests.sh Probe.VaRest.Lws and each Probe.VaRest.Risky case there")),
		ReleasePass, Here);
	Fact(*this, TEXT("release.android_ios"), EVerdict::Unverified,
		TEXT("unverified here; release-pass item (Android, iOS): the probe cannot run there (VaRestTests: PlatformAllowList Win64, Mac, Linux); W1's transport suite on devices covers them"),
		ReleasePass, TEXT("VaRest.uplugin"));
	Fact(*this, TEXT("coexist.monolithic"), EVerdict::Unverified,
		TEXT("not run in W0: no VaRest runtime module links LWS before W1, and VaRestTests (UncookedOnly) is never in a game target (ModuleDescriptor.cs:792-793, TargetRules.cs:1190-1193); W1 exit item"),
		TEXT("WebSocket spec 1.2 and 16.4: an engine IWebSocket and a VaRest socket at once in a monolithic Development game"), TEXT("UBT module host types"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeCommandsTest, "Probe.VaRest.Lws.Service.Commands",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeCommandsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeServiceTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	const FRoundTrip Inside = RoundTrip(Site, true);
	const FRoundTrip Between = RoundTrip(Site, false);
	NoteOutcome(*this, TEXT("both round trips ended"), Inside.bEnded && Between.bEnded);
	Fact(*this, TEXT("svc.commands_inside_wait_cancelled"), AgreesIf(Inside.bEstablished && Inside.bEchoed && Inside.bEnded), Describe(Inside),
		TEXT("WebSocket spec 2.1: the service thread drains commands (connect, send, close) in LWS_CALLBACK_EVENT_WAIT_CANCELLED"),
		TEXT("lws_client_connect_via_info, lws_callback_on_writable and the close all called inside EVENT_WAIT_CANCELLED"));
	Fact(*this, TEXT("svc.commands_between_service_calls"), EVerdict::Recorded, Describe(Between),
		TEXT("none: the engine starts sockets between lws_service calls (LwsWebSocketsManager.cpp:485-509)"),
		TEXT("the same round trip with the commands drained before each lws_service"));
	Fact(*this, TEXT("svc.cancel_wake_ms"), AgreesIf(Inside.WakeMs >= 0.0 && Inside.WakeMs < 100.0), FString::Printf(TEXT("%.1f ms"), Inside.WakeMs),
		TEXT("T13: lws_cancel_service wakes the service loop (libwebsockets.h:3704-3716)"),
		TEXT("a command queued while lws_service waited (10 s), timed until it ran in EVENT_WAIT_CANCELLED"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeTimerAndKillTest, "Probe.VaRest.Lws.Service.TimerAndKill",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeTimerAndKillTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeServiceTestPrivate;
	// The engine's ::FEvent (HAL/Event.h:21) is visible here too, so the directive alone makes FEvent ambiguous
	using VaRest::LwsProbe::FEvent;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	{
		// A per-wsi timer, armed from a command while lws_service would wait 10 s (libwebsockets.h:4705-4732): the
		// hrtimer list caps poll()'s wait, so this one is about the shape and keeps the long wait
		FRig Rig(Site);
		if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
		{
			return false;
		}
		FConnectSpec Spec;
		Spec.Path = TEXT("/ws/echo?key=") + NewKey();
		const int32 Conn = Rig.Connect(MoveTemp(Spec));
		if (!TestTrue(TEXT("the echo connection opens"), Rig.WaitReason(Conn, LWS_CALLBACK_CLIENT_ESTABLISHED)))
		{
			return false;
		}
		FPlatformProcess::Sleep(0.2f);
		Rig.ExecOn(Conn, TEXT("timer"), [](FConn& C) { lws_set_timer_usecs(C.Wsi, 200 * 1000); });
		const bool bTimer = Rig.WaitReason(Conn, LWS_CALLBACK_TIMER, 15.0);
		FEvent Armed;
		FEvent Fired;
		const bool bTimed = bTimer && Rig.Find(Conn, CommandRan, Armed, TEXT("timer")) && Rig.Find(Conn, LWS_CALLBACK_TIMER, Fired);
		const double TimerMs = bTimed ? (Fired.Time - Armed.Time) * 1000.0 : -1.0;
		Fact(*this, TEXT("svc.timer_latency_ms"), AgreesIf(bTimed && TimerMs < 1000.0),
			bTimed ? FString::Printf(TEXT("%.1f ms for a 200 ms timer, lws_service waiting up to 10 s"), TimerMs) : FString(TEXT("no LWS_CALLBACK_TIMER within 15 s")),
			TEXT("T14: lws_set_timer_usecs -> LWS_CALLBACK_TIMER, supported by the default poll loop (libwebsockets.h:4705-4732)"),
			TEXT("lws_set_timer_usecs(wsi, 200000) from a command on an open connection"));
		Rig.Close(Conn, 1000, TEXT("done"));
		NoteOutcome(*this, TEXT("the timer connection ended"), Rig.WaitEnded(Conn));
	}

	// A forced close from a command, "killed at the next timeout check" (libwebsockets.h:4673-4679): with the long wait
	// (the shape) and the short one (the control that shows how much of the delay is the wait, review C-1)
	const FKill Long = Kill(Site, TEXT("/ws/echo"), LongWaitMs, true);
	const FKill Short = Kill(Site, TEXT("/ws/echo"), ShortWaitMs, true);
	const FString KillSpec = TEXT("T14: forced close with lws_set_timeout(..., LWS_TO_KILL_ASYNC); WebSocket spec 2.1 loops lws_service(ctx, NextDeadline)");
	Fact(*this, TEXT("svc.kill_async_ms"), EVerdict::Recorded, Describe(Long, LongWaitMs), KillSpec, TEXT("a kill from a command on an open echo connection"));
	Fact(*this, TEXT("svc.kill_async_ms_short_wait"), EVerdict::Recorded, Describe(Short, ShortWaitMs), KillSpec,
		TEXT("the control for svc.kill_async_ms: the same kill with a 100 ms wait"));
	Fact(*this, TEXT("svc.kill_async_sequence"), EVerdict::Recorded, Long.Sequence.IsEmpty() ? FString(TEXT("not measured")) : Long.Sequence,
		TEXT("none"), TEXT("the callbacks after the kill command (the long-wait connection)"));
	FString CloseFrame = TEXT("not measured: the killed connection did not run");
	if (!Long.Key.IsEmpty())
	{
		const FVaRestJson Journal = FetchJournal(*this, Site, Long.Key);
		if (!TestTrue(TEXT("the server's journal arrived"), Journal.IsValid()))
		{
			return false;
		}
		CloseFrame = !Journal[TEXT("ended")].AsBool() ? FString(TEXT("the server still held the connection 5 s after the kill"))
			: JournalEvents(Journal, TEXT("close")).Num() > 0 ? FString(TEXT("a close frame reached the server"))
			: FString(TEXT("no close frame: the TCP connection just ended"));
	}
	Fact(*this, TEXT("svc.kill_async_close_frame"), EVerdict::Recorded, CloseFrame, TEXT("none"), TEXT("the server's journal of the killed connection"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeDefaultHandshakeTimeoutTest, "Probe.VaRest.Lws.Timeout.DefaultHandshake",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeDefaultHandshakeTimeoutTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeServiceTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	const FHang Run = Hang(Site, 0, FString(), 0, 45.0);
	NoteOutcome(*this, TEXT("the unanswered upgrade ended within 45 s"), Run.bEnded);
	Fact(*this, TEXT("timeout.default_secs"), EVerdict::Recorded, Describe(Run, 45.0),
		TEXT("WebSocket spec 0.3 (inferred from 4.3): LWS's default timeout_secs"),
		TEXT("timeout_secs = 0; /ws/hang accepts TCP, reads the upgrade, never answers; lws_service waiting up to 100 ms"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeConfiguredHandshakeTimeoutTest, "Probe.VaRest.Lws.Timeout.ConfiguredHandshake",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeConfiguredHandshakeTimeoutTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeServiceTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	const FHang Run = Hang(Site, 3, FString(), 0, 20.0);
	NoteOutcome(*this, TEXT("the unanswered upgrade ended within 20 s"), Run.bEnded);
	Fact(*this, TEXT("timeout.configured_secs"), AgreesIf(Run.bEnded && Run.Seconds >= 2.5 && Run.Seconds <= 5.0), Describe(Run, 20.0),
		TEXT("lws_context_creation_info::timeout_secs bounds the handshake (libwebsockets.h:2791-2795; it is context-wide)"),
		TEXT("timeout_secs = 3; /ws/hang; lws_service waiting up to 100 ms"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeKillPendingTest, "Probe.VaRest.Lws.Timeout.KillPending",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeKillPendingTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeServiceTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	// How WebSocket spec 2.2 enforces ConnectTimeout: a kill 1 s after the upgrade went out to a server that never
	// answers, timeout_secs left at LWS's default, with the long wait (the shape) and the short one (the control)
	const FKill Long = Kill(Site, TEXT("/ws/hang?ms=60000"), LongWaitMs, false);
	const FKill Short = Kill(Site, TEXT("/ws/hang?ms=60000"), ShortWaitMs, false);
	const FString KillSpec = TEXT("WebSocket spec 2.2: ConnectTimeout enforced with lws_set_timeout(..., LWS_TO_KILL_ASYNC) (T14)");
	Fact(*this, TEXT("timeout.kill_pending_ms"), EVerdict::Recorded, Describe(Long, LongWaitMs), KillSpec, TEXT("a pending attempt to /ws/hang, killed from a command"));
	Fact(*this, TEXT("timeout.kill_pending_ms_short_wait"), EVerdict::Recorded, Describe(Short, ShortWaitMs), KillSpec,
		TEXT("the control for timeout.kill_pending_ms: the same kill with a 100 ms wait"));
	Fact(*this, TEXT("timeout.kill_pending_sequence"), EVerdict::Recorded, Long.Sequence.IsEmpty() ? FString(TEXT("not measured")) : Long.Sequence,
		TEXT("none: the engine reads WSI_DESTROY before an open as a timeout (LwsWebSocket.cpp:392-405)"), TEXT("the callbacks after the kill (the long-wait attempt)"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeBlackholeTest, "Probe.VaRest.Lws.Timeout.Blackhole",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeBlackholeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeServiceTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	// A SYN to BlackholeAddress usually goes unanswered. A host without a route fails at once instead, and one behind a
	// transparent proxy completes the connect: both facts then read unverified (BlackholeFact)
	const FHang Configured = Hang(Site, 5, BlackholeAddress, 80, 40.0);
	NoteOutcome(*this, TEXT("the connect to TEST-NET-1 with timeout_secs 5 ended within 40 s"), Configured.bEnded);
	BlackholeFact(*this, TEXT("timeout.tcp_connect_blackhole"), Configured, 40.0,
		TEXT("WebSocket spec 2.2: whether a set timeout_secs bounds a TCP connect that is never answered"),
		TEXT("timeout_secs = 5; TCP to 192.0.2.1:80; lws_service waiting up to 100 ms"));
	// WebSocket spec 16.4: "default timeout_secs (blackhole address)" -- LWS's default against the kernel's SYN retries
	const FHang Default = Hang(Site, 0, BlackholeAddress, 80, 60.0);
	NoteOutcome(*this, TEXT("the connect to TEST-NET-1 with LWS's default timeout ended within 60 s"), Default.bEnded);
	BlackholeFact(*this, TEXT("timeout.default_secs_blackhole"), Default, 60.0,
		TEXT("WebSocket spec 0.3 (inferred from 4.3) and 16.4: LWS's default timeout_secs, on a blackhole address"),
		TEXT("timeout_secs = 0; TCP to 192.0.2.1:80; lws_service waiting up to 100 ms"));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
