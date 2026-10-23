// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestLwsProbe.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "PlatformHttp.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "../WebSocket/VaRestWsTestUtil.h"

namespace VaRest::LwsProbe::VaRestLwsProbeProxyTestPrivate
{
	FVaRestJson Encode(const FEvent& Item)
	{
		return FVaRestJson::Object()
			.Set(TEXT("seq"), Item.Sequence).Set(TEXT("conn"), Item.Conn)
			.Set(TEXT("wsi"), Item.WsiToken).Set(TEXT("reason"), Item.Reason)
			.Set(TEXT("name"), ReasonName(Item.Reason)).Set(TEXT("time"), Item.Time)
			.Set(TEXT("len"), Item.Len).Set(TEXT("input_present"), Item.bInputPresent)
			.Set(TEXT("hex"), HexOf(Item.Bytes)).Set(TEXT("note"), Item.Note)
			.Set(TEXT("depth"), Item.CertDepth).Set(TEXT("x509_error"), Item.CertError)
			.Set(TEXT("preverify"), Item.Preverify);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProxyAttemptTest, "Probe.VaRest.Lws.Proxy.Attempt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestLwsProxyAttemptTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using VaRest::LwsProbe::FEvent;
	VaRest::WsTest::ExpectLwsLines(*this);
	FString CaseId, Mode, TraceFile, CaFile, RequestPath, ExplicitProxy;
	const TCHAR* Command = FCommandLine::Get();
	const bool bInputs = FParse::Value(Command, TEXT("VaRestProxyCase="), CaseId)
		&& FParse::Value(Command, TEXT("VaRestProxyMode="), Mode)
		&& FParse::Value(Command, TEXT("VaRestProxyTrace="), TraceFile)
		&& FParse::Value(Command, TEXT("VaRestProxyCa="), CaFile)
		&& FParse::Value(Command, TEXT("VaRestProxyPath="), RequestPath);
	if (!TestTrue(TEXT("matrix arguments supplied"), bInputs)) { return false; }
	FServer Site;
	if (!FindServer(*this, Site, true)) { return false; }
	TArray<uint8> Ca;
	if (!TestTrue(TEXT("local test CA readable"), FFileHelper::LoadFileToArray(Ca, *CaFile))) { return false; }
	const bool bTls = FParse::Param(Command, TEXT("VaRestProxyTls"));
	const bool bUntrusted = FParse::Param(Command, TEXT("VaRestProxyUntrusted"));
	const bool bExplicit = FParse::Value(Command, TEXT("VaRestTestProxy="), ExplicitProxy);
	FRigOptions Options;
	Options.ServiceTimeoutMs = 100;
	Options.TimeoutSecs = 2;
	Options.MaxRecordedEvents = 4096;
	// TLS is enabled even on ws because the redirect control may switch schemes.
	Options.Tls = ETls::LwsGlobalInit;
	if (bExplicit) { Options.ProxyAddress = ExplicitProxy; }
	if (!bUntrusted)
	{
		Options.OnLoadExtraClientVerifyCerts = [Pem = MoveTemp(Ca)](SSL_CTX* Context)
		{
			AddPemToStore(Context, Pem);
		};
	}
	const FString ConfiguredProxy = FPlatformHttp::GetConfiguredProxyAddress();
	FRig Rig(Site, MoveTemp(Options));
	if (!TestTrue(TEXT("raw context ready"), Rig.IsReady())) { return false; }
	FConnectSpec Spec;
	Spec.bTls = bTls;
	Spec.Address = TEXT("127.0.0.1");
	// DNS SAN verification succeeds without production IP-literal repair; TCP remains numeric.
	Spec.Host = FString::Printf(TEXT("localhost:%d"), bTls ? Site.TlsPort : Site.Port);
	Spec.Path = RequestPath;
	Spec.AppendHeaders.Add(TEXT("Authorization: Bearer b1-probe-marker"));
	const double PrimaryBudget = Mode.Equals(TEXT("silent"), ESearchCase::CaseSensitive) ? 30.0 : 8.0;
	const double Begin = FPlatformTime::Seconds();
	const int32 ConnectionId = Rig.Connect(MoveTemp(Spec));
	const bool bPrimaryCompleted = Rig.WaitFor([ConnectionId](const TArray<FEvent>& All)
	{
		return All.ContainsByPredicate([ConnectionId](const FEvent& Item)
		{
			return Item.Conn == ConnectionId && (Item.Reason == LWS_CALLBACK_CLIENT_ESTABLISHED
				|| Item.Reason == LWS_CALLBACK_CLIENT_CONNECTION_ERROR || Item.Reason == LWS_CALLBACK_WSI_DESTROY);
		});
	}, PrimaryBudget);
	Rig.Record(ConnectionId, Marker, bPrimaryCompleted ? 1 : 0, TEXT("proxy-primary-finished"));
	const double PrimarySeconds = FPlatformTime::Seconds() - Begin;
	const bool bOpened = Opened(Rig.Events(ConnectionId));
	bool bEchoed = false;
	if (bOpened)
	{
		Rig.WriteText(ConnectionId, TEXT("b1-echo"));
		bEchoed = Rig.WaitFor([ConnectionId](const TArray<FEvent>& All)
		{
			return All.ContainsByPredicate([ConnectionId](const FEvent& Item)
			{
				return Item.Conn == ConnectionId && Item.Reason == LWS_CALLBACK_CLIENT_RECEIVE
					&& EventText(Item).Equals(TEXT("b1-echo"), ESearchCase::CaseSensitive);
			});
		}, 2.0);
		Rig.Close(ConnectionId, 1000, TEXT("b1-done"));
	}
	const bool bDestroyWaitCompleted = Rig.WaitReason(ConnectionId, LWS_CALLBACK_WSI_DESTROY, 3.0);
	Rig.Record(ConnectionId, Marker, 0, TEXT("proxy-shutdown-begin"));
	Rig.Shutdown(); // thread joined, context destroyed; only now collect final callbacks
	Rig.Record(ConnectionId, Marker, 0, TEXT("proxy-shutdown-end"));
	const auto Seen = Rig.Events(); // include context callbacks and unresolved conn=0 evidence
	const FEvent* ShutdownMarker = Seen.FindByPredicate([ConnectionId](const FEvent& Item)
	{
		return Item.Conn == ConnectionId && Item.Reason == Marker
			&& Item.Note.Equals(TEXT("proxy-shutdown-begin"), ESearchCase::CaseSensitive);
	});
	const int64 ShutdownSequence = ShutdownMarker ? ShutdownMarker->Sequence : 0;
	const bool bDestroyedBeforeShutdown = Seen.ContainsByPredicate([ConnectionId, ShutdownSequence](const FEvent& Item)
	{
		return Item.Conn == ConnectionId && Item.Reason == LWS_CALLBACK_WSI_DESTROY && Item.Sequence < ShutdownSequence;
	});
	FVaRestJson Rows = FVaRestJson::Array();
	for (const FEvent& Item : Seen) { Rows.Add(VaRestLwsProbeProxyTestPrivate::Encode(Item)); }
	const bool bDestroyed = Seen.ContainsByPredicate([ConnectionId](const FEvent& Item)
	{
		return Item.Conn == ConnectionId && Item.Reason == LWS_CALLBACK_WSI_DESTROY;
	});
	const FVaRestJson Trace = FVaRestJson::Object()
		.Set(TEXT("schema"), 1).Set(TEXT("case"), CaseId).Set(TEXT("mode"), Mode).Set(TEXT("connection"), ConnectionId)
		.Set(TEXT("configured_proxy_absent"), ConfiguredProxy.IsEmpty())
		.Set(TEXT("explicit_proxy"), bExplicit ? ExplicitProxy : FString())
		.Set(TEXT("tls"), bTls).Set(TEXT("untrusted"), bUntrusted)
		.Set(TEXT("primary_budget_seconds"), PrimaryBudget).Set(TEXT("primary_completed"), bPrimaryCompleted)
		.Set(TEXT("primary_seconds"), PrimarySeconds).Set(TEXT("opened"), bOpened)
		.Set(TEXT("echoed"), bEchoed).Set(TEXT("destroy_wait_completed"), bDestroyWaitCompleted)
		.Set(TEXT("destroyed_before_shutdown"), bDestroyedBeforeShutdown)
		.Set(TEXT("destroyed"), bDestroyed).Set(TEXT("shutdown_completed"), true)
		.Set(TEXT("dropped_events"), Rig.DroppedEventCount())
		.Set(TEXT("lws_version"), UTF8_TO_TCHAR(lws_get_library_version()))
		.Set(TEXT("openssl_version"), UTF8_TO_TCHAR(OpenSSL_version(OPENSSL_VERSION)))
		.Set(TEXT("events"), Rows);
	TestTrue(TEXT("trace saved"), FFileHelper::SaveStringToFile(Trace.ToString(), *TraceFile,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	TestEqual(TEXT("trace loss"), Rig.DroppedEventCount(), 0);
	TestTrue(TEXT("bounded primary attempt"), bPrimaryCompleted);
	// No assertion about counts, error text, routing or redirect-follow behavior here.
	NoteOutcome(*this, TEXT("opened"), bOpened);
	NoteOutcome(*this, TEXT("echoed"), bEchoed);
	return true;
}
#endif
