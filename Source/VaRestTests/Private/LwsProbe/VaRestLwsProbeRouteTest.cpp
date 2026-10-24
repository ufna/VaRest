// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestLwsProbe.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "HAL/CriticalSection.h"
#include "../WebSocket/VaRestWsTestUtil.h"

namespace VaRest::LwsProbe::VaRestLwsProbeRouteTestPrivate
{
    struct FOwnership
    {
        std::atomic<int32> Frees{0};
        std::atomic<int32> Stage{0};
        FCriticalSection Lock;
        TArray<FVaRestJson> Destructions;
        void Snapshot(FRig& Rig, int32 InStage, const TCHAR* Label)
        {
            Stage.store(InStage);
            const FString Row = FVaRestJson::Object().Set(TEXT("phase"), TEXT("ownership"))
                .Set(TEXT("stage"), InStage).Set(TEXT("label"), Label).Set(TEXT("frees"), Frees.load()).ToString();
            Rig.Record(0, Marker, InStage, Row);
            UE_LOG(LogVaRestLwsProbe, Display, TEXT("ROUTE-OWNERSHIP %s"), *Row);
            GLog->Flush();
        }
        FVaRestJson Export()
        {
            FScopeLock Guard(&Lock);
            FVaRestJson Out = FVaRestJson::Array();
            for (const auto& Row : Destructions) { Out.Add(Row); }
            return Out;
        }
    };
    void ContextFreed(void* Parent, void* Pointer, CRYPTO_EX_DATA* Data, int Index, long ArgL, void* ArgP)
    {
        if (!Pointer) { return; }
        auto* State = static_cast<FOwnership*>(Pointer);
        const FVaRestJson Row = FVaRestJson::Object().Set(TEXT("stage"), State->Stage.load())
            .Set(TEXT("count"), State->Frees.fetch_add(1) + 1);
        {
            FScopeLock Guard(&State->Lock);
            if (State->Destructions.Num() < 8) { State->Destructions.Add(Row); }
        }
        UE_LOG(LogVaRestLwsProbe, Display, TEXT("ROUTE-FINAL-CTX-DESTRUCTION %s"), *Row.ToString());
        GLog->Flush();
    }
    FVaRestJson Encode(const FEvent& Item)
    {
        return FVaRestJson::Object().Set(TEXT("seq"), Item.Sequence).Set(TEXT("conn"), Item.Conn)
            .Set(TEXT("wsi"), Item.WsiToken).Set(TEXT("reason"), Item.Reason).Set(TEXT("name"), ReasonName(Item.Reason)).Set(TEXT("time"), Item.Time)
            .Set(TEXT("len"), Item.Len).Set(TEXT("input_present"), Item.bInputPresent)
            .Set(TEXT("hex"), HexOf(Item.Bytes)).Set(TEXT("note"), Item.Note)
            .Set(TEXT("depth"), Item.CertDepth).Set(TEXT("x509_error"), Item.CertError)
            .Set(TEXT("preverify"), Item.Preverify).Set(TEXT("flags"), static_cast<int64>(Item.Flags));
    }
    bool CommandSeen(FRig& Rig, const FString& Label)
    {
        return Rig.WaitFor([Label](const TArray<FEvent>& All)
        {
            return All.ContainsByPredicate([Label](const FEvent& Item)
            {
                return Item.Reason == CommandRan && Item.Note.Equals(Label, ESearchCase::CaseSensitive);
            });
        }, 3.0);
    }
    void Fence(FRig& Rig, const TCHAR* Label)
    {
        Rig.Exec(Label, [](FRig& Self) {});
    }
    bool Retired(FRig& Rig, int32 ConnId)
    {
        return Rig.WaitFor([ConnId](const TArray<FEvent>& All)
        {
            TSet<int32> Associated;
            for (const FEvent& Item : All) { if (Item.Conn == ConnId && Item.WsiToken) { Associated.Add(Item.WsiToken); } }
            TSet<int32> Destroyed;
            bool bReturnedNull = false;
            for (const FEvent& Item : All)
            {
                if (Associated.Contains(Item.WsiToken) && Item.Reason == LWS_CALLBACK_WSI_DESTROY) { Destroyed.Add(Item.WsiToken); }
                if (Item.Conn == ConnId && Item.Reason == ConnectReturned && Item.Note.StartsWith(TEXT("null"), ESearchCase::CaseSensitive)) { bReturnedNull = true; }
            }
            return Associated.Num() > 0 ? Destroyed.Num() == Associated.Num() : bReturnedNull;
        }, 3.0);
    }
    bool Primary(FRig& Rig, int32 ConnId)
    {
        return Rig.WaitFor([ConnId](const TArray<FEvent>& All)
        {
            return All.ContainsByPredicate([ConnId](const FEvent& Item)
            {
                return Item.Conn == ConnId && (Item.Reason == LWS_CALLBACK_CLIENT_ESTABLISHED
                    || Item.Reason == LWS_CALLBACK_CLIENT_CONNECTION_ERROR || Item.Reason == LWS_CALLBACK_WSI_DESTROY
                    || (Item.Reason == ConnectReturned && Item.Note.StartsWith(TEXT("null"), ESearchCase::CaseSensitive)));
            });
        }, 5.0);
    }
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsRouteAttemptTest, "Probe.VaRest.Lws.Route.Attempt",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestLwsRouteAttemptTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::LwsProbe;
    using namespace VaRest::LwsProbe::VaRestLwsProbeRouteTestPrivate;
    using VaRest::LwsProbe::FEvent;
    VaRest::WsTest::ExpectLwsLines(*this);
    FString CaseId, Mode, FirstProxy, NextProxy, TracePath, CaPath, Path, GateReady, GateRelease;
    const TCHAR* Cmd = FCommandLine::Get();
    const bool bInputs = FParse::Value(Cmd, TEXT("VaRestRouteCase="), CaseId)
        && FParse::Value(Cmd, TEXT("VaRestRouteMode="), Mode)
        && FParse::Value(Cmd, TEXT("VaRestRouteFirst="), FirstProxy)
        && FParse::Value(Cmd, TEXT("VaRestRouteNext="), NextProxy)
        && FParse::Value(Cmd, TEXT("VaRestRouteTrace="), TracePath)
        && FParse::Value(Cmd, TEXT("VaRestRouteCa="), CaPath)
        && FParse::Value(Cmd, TEXT("VaRestRoutePath="), Path)
        && FParse::Value(Cmd, TEXT("VaRestRouteGateReady="), GateReady)
        && FParse::Value(Cmd, TEXT("VaRestRouteGateRelease="), GateRelease);
    if (!TestTrue(TEXT("route inputs"), bInputs)) { return false; }
    FServer Site;
    if (!FindServer(*this, Site, true)) { return false; }
    TArray<uint8> Pem;
    if (!TestTrue(TEXT("CA bytes"), FFileHelper::LoadFileToArray(Pem, *CaPath))) { return false; }
    const bool bTls = FParse::Param(Cmd, TEXT("VaRestRouteTls"));
    const bool bProvided = FParse::Param(Cmd, TEXT("VaRestRouteProvided"));
    const bool bHeld = Mode.Equals(TEXT("held"), ESearchCase::CaseSensitive)
        || Mode.Equals(TEXT("held-reuse"), ESearchCase::CaseSensitive);
    const bool bLate = Mode.Equals(TEXT("held"), ESearchCase::CaseSensitive);
    const bool bFallback = Mode.Equals(TEXT("fallback"), ESearchCase::CaseSensitive);
    const bool bUntrusted = Mode.Equals(TEXT("untrusted"), ESearchCase::CaseSensitive);
    int32 SeparatePort = 0;
    FParse::Value(Cmd, TEXT("VaRestRouteProxyPort="), SeparatePort);
    auto Ownership = MakeShared<FOwnership, ESPMode::ThreadSafe>();
    SSL_CTX* Provided = nullptr;
    if (bProvided)
    {
        Provided = SSL_CTX_new(TLS_client_method());
        if (!TestNotNull(TEXT("provided context"), Provided)) { return false; }
        const int Slot = SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr, &ContextFreed);
        if (!TestTrue(TEXT("free observer slot"), Slot >= 0)) { SSL_CTX_free(Provided); return false; }
        if (!TestEqual(TEXT("install free observer"), SSL_CTX_set_ex_data(Provided, Slot, &Ownership.Get()), 1))
        { SSL_CTX_free(Provided); return false; }
        if (!bUntrusted) { AddPemToStore(Provided, Pem); }
    }
    ON_SCOPE_EXIT { if (Provided && Ownership->Frees.load() == 0) { SSL_CTX_free(Provided); } };
    FRigOptions Options;
    Options.bExplicitRoutes = true;
    Options.bCommandsInsideCallback = false;
    Options.ServiceTimeoutMs = 50;
    Options.TimeoutSecs = 5;
    Options.MaxRecordedEvents = 8192;
    Options.Tls = bProvided ? ETls::ProvidedContext : ETls::LwsGlobalInit;
    Options.ProvidedSslContext = Provided;
    Options.OnRouteDestroyStage = [Ownership](FRig& Self, bool bBefore)
    {
        Ownership->Snapshot(Self, bBefore ? 1 : 2, bBefore ? TEXT("vhost-before") : TEXT("vhost-after"));
    };
    if (!bUntrusted)
    {
        Options.OnLoadExtraClientVerifyCerts = [SavedPem = Pem](SSL_CTX* Ctx) { AddPemToStore(Ctx, SavedPem); };
    }
    FRig Rig(Site, MoveTemp(Options));
    if (!TestTrue(TEXT("one raw context ready"), Rig.IsReady())) { return false; }
    auto MakeRoute = [](const TCHAR* Name, const FString& Proxy, uint32 Port)
    {
        FRouteSpec Out;
        Out.Name = Name;
        Out.ProxyAddress = Proxy;
        Out.ProxyPort = Port;
        return Out;
    };
    const int32 FirstRoute = Rig.AddRoute(MakeRoute(TEXT("route-first"), FirstProxy, 0));
    int32 NextRoute = bLate ? 0 : Rig.AddRoute(MakeRoute(TEXT("route-next"), NextProxy, static_cast<uint32>(SeparatePort)));
    Fence(Rig, TEXT("routes-precreated"));
    bool bHarness = CommandSeen(Rig, TEXT("routes-precreated"));
    FConnectSpec Spec;
    Spec.RouteId = Mode.Equals(TEXT("precreated"), ESearchCase::CaseSensitive) ? NextRoute : FirstRoute;
    Spec.Address = TEXT("localhost"); // CONNECT identity, never the proxy's numeric peer
    Spec.Host = FString::Printf(TEXT("localhost:%d"), bTls ? Site.TlsPort : Site.Port);
    Spec.Path = Path;
    Spec.bTls = bTls;
    Spec.AppendHeaders.Add(TEXT("Authorization: Bearer route-probe-marker"));
    Spec.Hook = [](FConn& Connection, lws* InWsi, int32 Reason, void* User, void* In, size_t Len)
    {
        if (Reason != LWS_CALLBACK_OPENSSL_PERFORM_SERVER_CERT_VERIFICATION) { return KeepDefault; }
        // First compare the callback WSI against the owned connection. Never inspect an unexpected pointer.
        const bool bKnownWsi = InWsi && InWsi == Connection.Wsi;
        SSL* KnownSsl = bKnownWsi ? lws_get_ssl(InWsi) : nullptr;
        const bool bKnownInput = KnownSsl && In == KnownSsl;
        X509_STORE_CTX* Store = static_cast<X509_STORE_CTX*>(User);
        // Store belongs to this documented callback; only inspect it after the owned WSI/SSL input match.
        const bool bStoreSsl = bKnownInput && Store
            && X509_STORE_CTX_get_ex_data(Store, SSL_get_ex_data_X509_STORE_CTX_idx()) == KnownSsl;
        const bool bUser = bKnownWsi && lws_wsi_user(InWsi) == &Connection;
        const FString Row = Connection.Rig->SslIdentity(Connection).Set(TEXT("phase"), TEXT("verify-identity"))
            .Set(TEXT("known_wsi"), bKnownWsi).Set(TEXT("ssl_input"), bKnownInput)
            .Set(TEXT("store_ssl"), bStoreSsl).Set(TEXT("wsi_user"), bUser)
            .Set(TEXT("preverify"), static_cast<int64>(Len)).ToString();
        Connection.Rig->Record(Connection.Id, Marker, 0, Row);
        UE_LOG(LogVaRestLwsProbe, Display, TEXT("ROUTE-IDENTITY %s"), *Row);
        GLog->Flush();
        return KeepDefault;
    };
    Pending(TEXT("route-first-connect"));
    int32 Active = Rig.Connect(Spec);
    const int32 Initial = Active;
    int32 Neighbor = 0;
    if (bHeld)
    {
        Rig.WatchConnection(Active);
        bHarness = Rig.WaitFor([GateReady](const TArray<FEvent>& All)
        { return IFileManager::Get().FileExists(*GateReady); }, 4.0) && bHarness;
        Rig.RecordSslIdentity(Active, TEXT("ssl-before-route"));
        Fence(Rig, TEXT("identity-before-fence"));
        bHarness = CommandSeen(Rig, TEXT("identity-before-fence")) && bHarness;
        if (bHarness && bLate)
        {
            Pending(TEXT("route-late-create-live-ssl"));
            NextRoute = Rig.AddRoute(MakeRoute(TEXT("route-late"), NextProxy, 0));
        }
        if (bHarness && !bLate)
        {
            FConnectSpec NeighborSpec = Spec;
            NeighborSpec.RouteId = NextRoute;
            NeighborSpec.Path = TEXT("/ws/echo?key=") + CaseId + TEXT("-neighbor");
            Neighbor = Rig.Connect(MoveTemp(NeighborSpec));
            const bool bNeighborPrimary = Primary(Rig, Neighbor);
            Rig.Record(Neighbor, Marker, bNeighborPrimary && Opened(Rig.Events(Neighbor)) ? 1 : 0, TEXT("neighbor-open-held"));
            if (Opened(Rig.Events(Neighbor)))
            {
                Rig.RecordSslIdentity(Neighbor, TEXT("neighbor-ssl-open"));
                Fence(Rig, TEXT("neighbor-ssl-fence"));
                bHarness = CommandSeen(Rig, TEXT("neighbor-ssl-fence")) && bHarness;
                Rig.WriteText(Neighbor, TEXT("neighbor-route-echo"));
                const bool bNeighborEchoed = Rig.WaitFor([Neighbor](const TArray<FEvent>& All)
                {
                    return All.ContainsByPredicate([Neighbor](const FEvent& Item)
                    {
                        return Item.Conn == Neighbor && Item.Reason == LWS_CALLBACK_CLIENT_RECEIVE
                            && EventText(Item).Equals(TEXT("neighbor-route-echo"), ESearchCase::CaseSensitive);
                    });
                }, 2.0);
                Rig.Record(Neighbor, Marker, bNeighborEchoed ? 1 : 0, TEXT("neighbor-echo-held"));
                Rig.Close(Neighbor, 1000, TEXT("neighbor-done"));
            }
            const bool bNeighborRetired = Retired(Rig, Neighbor);
            Rig.Record(Neighbor, Marker, bNeighborRetired ? 1 : 0, TEXT("neighbor-retired-held"));
            bHarness = bNeighborRetired && bHarness;
        }
        Fence(Rig, TEXT("late-route-fence"));
        bHarness = CommandSeen(Rig, TEXT("late-route-fence")) && bHarness;
        Rig.RecordSslIdentity(Active, TEXT("ssl-after-route"));
        Fence(Rig, TEXT("identity-after-fence"));
        bHarness = CommandSeen(Rig, TEXT("identity-after-fence")) && bHarness;
        if (bHarness)
        {
            Rig.Record(Active, Marker, 0, TEXT("hold-release-request"));
            GLog->Flush();
            bHarness = FFileHelper::SaveStringToFile(TEXT("release\n"), *GateRelease,
                FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        }
    }
    const bool bFirstPrimary = Primary(Rig, Active);
    bool bRetiredBeforeFallback = false;
    if (bFallback && !Opened(Rig.Events(Active)))
    {
        bRetiredBeforeFallback = Retired(Rig, Active);
        Rig.Record(Active, Marker, bRetiredBeforeFallback ? 1 : 0, TEXT("fallback-retired"));
        if (bRetiredBeforeFallback)
        {
            Spec.RouteId = NextRoute;
            Active = Rig.Connect(Spec);
        }
    }
    const bool bFinalPrimary = Active == Initial ? bFirstPrimary : Primary(Rig, Active);
    const bool bOpened = Opened(Rig.Events(Active));
    bool bEchoed = false;
    if (bOpened)
    {
        Rig.RecordSslIdentity(Active, TEXT("ssl-open"));
        Fence(Rig, TEXT("ssl-open-fence"));
        bHarness = CommandSeen(Rig, TEXT("ssl-open-fence")) && bHarness;
        Rig.WriteText(Active, TEXT("route-echo"));
        bEchoed = Rig.WaitFor([Active](const TArray<FEvent>& All)
        {
            return All.ContainsByPredicate([Active](const FEvent& Item)
            {
                return Item.Conn == Active && Item.Reason == LWS_CALLBACK_CLIENT_RECEIVE
                    && EventText(Item).Equals(TEXT("route-echo"), ESearchCase::CaseSensitive);
            });
        }, 2.0);
    }
    Ownership->Snapshot(Rig, 0, TEXT("cleanup-start"));
    if (Mode.Equals(TEXT("destroy-active"), ESearchCase::CaseSensitive))
    {
        Pending(TEXT("route-destroy-active"));
        Rig.DestroyRoute(Spec.RouteId);
        Fence(Rig, TEXT("active-route-destroyed"));
        bHarness = CommandSeen(Rig, TEXT("active-route-destroyed")) && bHarness;
    }
    else
    {
        Ownership->Snapshot(Rig, 1, TEXT("vhost-skipped-before"));
        Ownership->Snapshot(Rig, 2, TEXT("vhost-skipped-after"));
        if (bOpened) { Rig.Close(Active, 1000, TEXT("route-done")); }
    }
    Ownership->Snapshot(Rig, 3, TEXT("retirement-before"));
    const bool bRetired = Retired(Rig, Active);
    Ownership->Snapshot(Rig, 4, TEXT("retirement-after"));
    Ownership->Snapshot(Rig, 5, TEXT("shutdown-before"));
    Rig.Record(0, Marker, 0, TEXT("shutdown-begin"));
    Rig.Shutdown();
    Rig.Record(0, Marker, 0, TEXT("shutdown-end"));
    Ownership->Snapshot(Rig, 6, TEXT("shutdown-after"));
    const int32 FreesAfterContext = Ownership->Frees.load();
    Ownership->Snapshot(Rig, 7, TEXT("caller-before"));
    if (Provided && FreesAfterContext == 0) { SSL_CTX_free(Provided); }
    Provided = nullptr;
    Ownership->Snapshot(Rig, 8, TEXT("caller-after"));
    const int32 FreesAfterCaller = Ownership->Frees.load();
    FVaRestJson Events = FVaRestJson::Array();
    for (const FEvent& Item : Rig.Events()) { Events.Add(Encode(Item)); }
    const FVaRestJson Output = FVaRestJson::Object().Set(TEXT("schema"), 1).Set(TEXT("case"), CaseId)
        .Set(TEXT("mode"), Mode).Set(TEXT("tls"), bTls).Set(TEXT("provided"), bProvided)
        .Set(TEXT("first_route"), FirstRoute).Set(TEXT("next_route"), NextRoute)
        .Set(TEXT("first_connection"), Initial).Set(TEXT("final_connection"), Active)
        .Set(TEXT("neighbor_connection"), Neighbor).Set(TEXT("selected_route"), Spec.RouteId)
        .Set(TEXT("harness_ok"), bHarness).Set(TEXT("first_primary"), bFirstPrimary)
        .Set(TEXT("final_primary"), bFinalPrimary).Set(TEXT("opened"), bOpened).Set(TEXT("echoed"), bEchoed)
        .Set(TEXT("retired_before_fallback"), bRetiredBeforeFallback).Set(TEXT("retired_before_shutdown"), bRetired)
        .Set(TEXT("final_ctx_destructions"), Ownership->Export())
        .Set(TEXT("frees_after_context"), FreesAfterContext).Set(TEXT("frees_after_caller"), FreesAfterCaller)
        .Set(TEXT("shutdown_completed"), true).Set(TEXT("dropped"), Rig.DroppedEventCount()).Set(TEXT("events"), Events);
    TestTrue(TEXT("trace persisted after joined teardown"), FFileHelper::SaveStringToFile(Output.ToString(), *TracePath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    TestEqual(TEXT("no recorder loss"), Rig.DroppedEventCount(), 0);
    TestTrue(TEXT("gate/fence acquisition"), bHarness);
    NoteOutcome(*this, TEXT("route open"), bOpened);
    NoteOutcome(*this, TEXT("route echo"), bEchoed);
    // Route/SSL/init/refusal outcomes are measurements. No guessed safety assertion.
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestRouteBudgetTest, "Probe.VaRest.Lws.Route.Budget",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestRouteBudgetTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::LwsProbe;
    for (int32 Index = 0; Index < 8; ++Index) { TestTrue(TEXT("route slot admitted"), CanAddRoute(Index)); }
    TestFalse(TEXT("ninth refused"), CanAddRoute(8));
    TestFalse(TEXT("negative count refused"), CanAddRoute(-1));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestRouteUnknownTokenTest, "Probe.VaRest.Lws.Route.UnknownToken",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestRouteUnknownTokenTest::RunTest(const FString& Parameters)
{
    int32 OwnedStorage = 0, UnexpectedStorage = 0, Accesses = 0;
    const void* Known[] = {&OwnedStorage};
    TestFalse(TEXT("unowned token refused before accessor"), VaRest::LwsProbe::WithOwnedToken(
        &UnexpectedStorage, MakeArrayView(Known), [&Accesses] { ++Accesses; }));
    TestEqual(TEXT("no accessor on unowned token"), Accesses, 0);
    TestTrue(TEXT("owned token inspected"), VaRest::LwsProbe::WithOwnedToken(
        &OwnedStorage, MakeArrayView(Known), [&Accesses] { ++Accesses; }));
    TestEqual(TEXT("exactly one owned access"), Accesses, 1);
    return true;
}

#endif
