// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestM5NativeClosureProof.h"
#include "VaRestM5NativeClosureTypes.h"
#include "CoreGlobals.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProperties.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Parse.h"
#include "Tasks/Task.h"
#include "UObject/PropertyOptional.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "VaRestClock.h"
#include "VaRestConnectivity.h"
#include "VaRestDynamicRequest.h"
#include "VaRestDynamicTypes.h"
#include <atomic>

DEFINE_LOG_CATEGORY_STATIC(LogVaRestM5NativeClosure, Log, All);
namespace VaRestM5NativeClosure
{
enum class EOp : uint8 { Construct, Copy, Assign, Destroy, Equal, Hash, Count };
constexpr uint8 Root = 0, Leaf = 1, Types = 2, Ops = uint8(EOp::Count);
std::atomic<bool> Tracking{false};
std::atomic<int64> Counts[Types][Ops]{}, Live[Types]{};
std::atomic<int64> WrongThread{0}, MissingDependency{0}, BadValues{0};
std::atomic<uint32> TypedMask{0};
// NO strong global owner: production's explicit native registry also stores this dependency weakly.
TWeakObjectPtr<UEnum> Dependency;
FString DependencyDefault;
void Record(uint8 Type, EOp Op)
{
    if (!Tracking.load(std::memory_order_acquire)) return;
    Counts[Type][uint8(Op)].fetch_add(1, std::memory_order_relaxed);
    if (!IsInGameThread()) WrongThread.fetch_add(1, std::memory_order_relaxed);
    if (Op == EOp::Construct || Op == EOp::Copy) Live[Type].fetch_add(1, std::memory_order_relaxed);
    if (Op == EOp::Destroy) Live[Type].fetch_sub(1, std::memory_order_relaxed);
}
FString ReadNativeDependency()
{
    if (IsInGameThread())
        if (const UEnum* Enum = Dependency.Get()) return Enum->GetNameStringByIndex(0);
    if (Tracking.load(std::memory_order_acquire)) MissingDependency.fetch_add(1, std::memory_order_relaxed);
    return TEXT("unavailable-native-dependency"); // no dead-pointer dereference, even in a negative candidate
}
bool NativeValuesRetired() { return Live[Root].load() == 0 && Live[Leaf].load() == 0; }
FString Object(const TCHAR* Name, bool Optional)
{
    return FString::Printf(TEXT("{\"name\":\"%s\",%s\"mode\":\"One\",\"times\":[\"2026-11-18T00:00:00.000Z\"],\"labels\":[\"lower\",\"UPPER\"],\"dates\":{\"date\":\"2026-11-18T00:00:00.000Z\"}}"),
        Name, Optional ? TEXT("\"maybe\":{\"text\":\"leaf\",\"number\":23},") : TEXT(""));
}
FString Reply(int32 Case)
{
    if (Case == 0) return Object(TEXT("scalar"), true);
    if (Case == 1) return TEXT("[") + Object(TEXT("array0"), true) + TEXT(",") + Object(TEXT("array1"), false) + TEXT("]");
    return TEXT("{\"one\":") + Object(TEXT("map0"), true) + TEXT(",\"two\":") + Object(TEXT("map1"), false) + TEXT("}");
}
void ObserveRealDestruction(const FVaRestM5ClosureRoot& Value)
{
    if (!Tracking.load(std::memory_order_acquire)) return;
    if (!IsInGameThread()) { WrongThread.fetch_add(1, std::memory_order_relaxed); return; }
    const TCHAR* Names[] = {TEXT("scalar"), TEXT("array0"), TEXT("array1"), TEXT("map0"), TEXT("map1")};
    int32 Index = INDEX_NONE;
    for (int32 I = 0; I != 5; ++I) if (Value.Name == Names[I]) { Index = I; break; }
    if (Index == INDEX_NONE) return; // native default/manual qualification values are not wire evidence
    const bool Optional = Index == 0 || Index == 1 || Index == 3;
    const FDateTime Date(2026, 11, 18);
    const FDateTime* Named = Value.Dates.Find(TEXT("date"));
    const bool Match = IsInGameThread() && Value.NativeDefault == DependencyDefault &&
        Value.Mode == EVaRestM5ClosureMode::One && Value.Maybe.IsSet() == Optional &&
        (!Optional || (Value.Maybe->Text == TEXT("leaf") && Value.Maybe->Number == 23)) &&
        Value.Times.Num() == 1 && Value.Times[0] == Date && Value.Labels.Num() == 2 &&
        Value.Labels.Contains(TEXT("lower")) && Value.Labels.Contains(TEXT("UPPER")) &&
        Value.Dates.Num() == 1 && Named && *Named == Date;
    if (Match) TypedMask.fetch_or(1u << Index, std::memory_order_relaxed);
    else BadValues.fetch_add(1, std::memory_order_relaxed);
}
struct FCell
{
    std::atomic<bool> Done{false}, WrongCallbackThread{false};
    int32 Success = 0, Complete = 0;
    uint64 CallbackFrame = 0;
    uint32 TypedAtSuccess = 0;
    bool ValuesRetiredAtSuccess = false;
    FVaRestDynamicResult Shell;
    FVaRestError Error;
    FVaRestResponse Response;
};
struct FWorkerDrop
{
    std::atomic<bool> Done{false}, OffGT{false};
};
}

using namespace VaRestM5NativeClosure;
FVaRestM5ClosureLeaf::FVaRestM5ClosureLeaf() : Text(TEXT("leaf-default")) { Record(Leaf, EOp::Construct); }
FVaRestM5ClosureLeaf::FVaRestM5ClosureLeaf(const FVaRestM5ClosureLeaf& Other) : Text(Other.Text), Number(Other.Number) { Record(Leaf, EOp::Copy); }
FVaRestM5ClosureLeaf& FVaRestM5ClosureLeaf::operator=(const FVaRestM5ClosureLeaf& Other)
{ Record(Leaf, EOp::Assign); if (this != &Other) { Text = Other.Text; Number = Other.Number; } return *this; }
FVaRestM5ClosureLeaf::~FVaRestM5ClosureLeaf() { Record(Leaf, EOp::Destroy); }
bool FVaRestM5ClosureLeaf::operator==(const FVaRestM5ClosureLeaf& Other) const
{ Record(Leaf, EOp::Equal); return Text == Other.Text && Number == Other.Number; }
uint32 GetTypeHash(const FVaRestM5ClosureLeaf& Value)
{ Record(Leaf, EOp::Hash); return HashCombine(GetTypeHash(Value.Text), GetTypeHash(Value.Number)); }
FVaRestM5ClosureRoot::FVaRestM5ClosureRoot() : Name(TEXT("native-default")), NativeDefault(ReadNativeDependency()) { Record(Root, EOp::Construct); }
FVaRestM5ClosureRoot::FVaRestM5ClosureRoot(const FVaRestM5ClosureRoot& Other)
    : Name(Other.Name), NativeDefault(Other.NativeDefault), Maybe(Other.Maybe), Mode(Other.Mode), Times(Other.Times), Labels(Other.Labels), Dates(Other.Dates) { Record(Root, EOp::Copy); }
FVaRestM5ClosureRoot& FVaRestM5ClosureRoot::operator=(const FVaRestM5ClosureRoot& Other)
{
    Record(Root, EOp::Assign);
    if (this != &Other) { Name = Other.Name; NativeDefault = Other.NativeDefault; Maybe = Other.Maybe; Mode = Other.Mode; Times = Other.Times; Labels = Other.Labels; Dates = Other.Dates; }
    return *this;
}
FVaRestM5ClosureRoot::~FVaRestM5ClosureRoot() { ObserveRealDestruction(*this); Record(Root, EOp::Destroy); }
bool FVaRestM5ClosureRoot::operator==(const FVaRestM5ClosureRoot& Other) const
{
    Record(Root, EOp::Equal);
    if (Name != Other.Name || NativeDefault != Other.NativeDefault || !(Maybe == Other.Maybe) || Mode != Other.Mode || Times != Other.Times || Labels.Num() != Other.Labels.Num() || Dates.Num() != Other.Dates.Num()) return false;
    for (const FString& Item : Labels) if (!Other.Labels.Contains(Item)) return false;
    for (const auto& Item : Dates) { const FDateTime* Value = Other.Dates.Find(Item.Key); if (!Value || *Value != Item.Value) return false; }
    return true;
}
uint32 GetTypeHash(const FVaRestM5ClosureRoot& Value)
{ Record(Root, EOp::Hash); return HashCombine(HashCombine(GetTypeHash(Value.Name), GetTypeHash(Value.NativeDefault)), Value.Maybe.IsSet() ? GetTypeHash(Value.Maybe.GetValue()) : 0u); }

class FVaRestM5NativeClosureProof::FImpl final
{
public:
    ~FImpl() { Stop(); }
    void Start()
    {
        if (IsRunningCommandlet()) return; // never creates/loads fixture content in cook
        if (!IsInGameThread() || GIsEditor || !FPlatformProperties::RequiresCookedData() ||
            FParse::Param(FCommandLine::Get(), TEXT("VaRestM5Endpoints")) || FParse::Param(FCommandLine::Get(), TEXT("VaRestM5SettingsOnly")))
        { Fail(TEXT("entry requires a separate ordinary cooked Game process")); return; }
        OmitLeaf = FParse::Param(FCommandLine::Get(), TEXT("VaRestM5NativeOmitLeafControl"));
        FParse::Value(FCommandLine::Get(), TEXT("VaRestM5NativePort="), Port);
        if (Port < 1024 || Port > 65535) { Fail(TEXT("invalid owned loopback port")); return; }
        PostInit = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FImpl::AfterInit);
    }
private:
    enum class EPhase : uint8 { Waiting, Capture, Reply, Retire, Drop, Done };
    void AfterInit()
    {
        FCoreDelegates::GetOnPostEngineInit().Remove(PostInit); PostInit.Reset();
        FVaRestM5ClosureRoot::StaticStruct()->PrepareCppStructOps();
        FVaRestM5ClosureLeaf::StaticStruct()->PrepareCppStructOps();
        const auto* Optional = FindFProperty<FOptionalProperty>(FVaRestM5ClosureRoot::StaticStruct(), GET_MEMBER_NAME_CHECKED(FVaRestM5ClosureRoot, Maybe));
        const auto* Inner = Optional ? CastField<FStructProperty>(Optional->GetValueProperty()) : nullptr;
        if (!Inner || Inner->Struct != FVaRestM5ClosureLeaf::StaticStruct()) { Fail(TEXT("actual generated Optional does not own the only leaf")); return; }
        FString Error; FVaRestDynamicLimits Limits;
        Budget = FVaRestDynamicBudget::Create(Limits, Error);
        Client = FVaRestClient::Make(FString::Printf(TEXT("http://127.0.0.1:%d"), Port));
        Client.WithConnectivity(FVaRestConnectivity::Make(FVaRestRealClock::Get()));
        if (!Budget || !Client.IsValid()) { Fail(TEXT("ordinary public client/budget unavailable")); return; }
        if (!OmitLeaf)
        {
            RegisteredLeaf = FVaRestDynamicNativeTypes::RegisterGameThreadType(FVaRestM5ClosureLeaf::StaticStruct(), TEXT("VaRest.M5.Cooked.Optional.Leaf.v1"));
            if (!RegisteredLeaf) { Fail(TEXT("actual leaf registration refused")); return; }
        }
        Tracking.store(true, std::memory_order_release);
        Started = FPlatformTime::Seconds();
        Frame = FCoreDelegates::OnEndFrame.AddRaw(this, &FImpl::Tick);
        BeginCase();
    }
    void BeginCase()
    {
        Enum.Reset(NewObject<UEnum>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UEnum::StaticClass(), TEXT("VaRestM5NativeClosureDependency_1")), RF_Transient));
        Dropped.Reset(NewObject<UEnum>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UEnum::StaticClass(), TEXT("VaRestM5NativeClosureDropped_1")), RF_Transient));
        if (!Enum || !Dropped) { Fail(TEXT("real transient enum/control unavailable")); return; }
        TArray<TPair<FName, int64>> Entries;
        Entries.Emplace(FName(*(Enum->GetName() + TEXT("::Entry"))), int64(0));
        if (!Enum->SetEnums(Entries, UEnum::ECppForm::EnumClass, UEnum::EUnderlyingType::uint8, EEnumFlags::None, UEnum::EAddMaxKeyIfMissing::Yes)) { Fail(TEXT("actual enum definition refused")); return; }
        Entries[0].Key = FName(*(Dropped->GetName() + TEXT("::Entry")));
        if (!Dropped->SetEnums(Entries, UEnum::ECppForm::EnumClass, UEnum::EUnderlyingType::uint8, EEnumFlags::None, UEnum::EAddMaxKeyIfMissing::Yes)) { Fail(TEXT("dropped control enum definition refused")); return; }
        WeakEnum = Enum.Get(); WeakDropped = Dropped.Get(); Dependency = Enum.Get();
        DependencyDefault = Enum->GetNameStringByIndex(0);
        const UObject* Dependencies[] = {Enum.Get()};
        RegisteredRoot = FVaRestDynamicNativeTypes::RegisterGameThreadType(FVaRestM5ClosureRoot::StaticStruct(), TEXT("VaRest.M5.Cooked.Optional.Root.v1"), MakeArrayView(Dependencies));
        if (!RegisteredRoot) { Fail(TEXT("actual root/nonreflected dependency registration refused")); return; }
        PublicationFrame = GFrameCounter; Phase = EPhase::Capture;
    }
    bool QualifyNativeOperations()
    {
        {
            FVaRestM5ClosureLeaf A; A.Text = TEXT("manual");
            FVaRestM5ClosureLeaf B(A);
            FVaRestM5ClosureLeaf::StaticStruct()->CopyScriptStruct(&B, &A);
            if (!FVaRestM5ClosureLeaf::StaticStruct()->CompareScriptStruct(&B, &A, 0) ||
                FVaRestM5ClosureLeaf::StaticStruct()->GetStructTypeHash(&B) != FVaRestM5ClosureLeaf::StaticStruct()->GetStructTypeHash(&A)) return false;
            B.Number = 99;
            if (FVaRestM5ClosureLeaf::StaticStruct()->CompareScriptStruct(&B, &A, 0)) return false;
            FVaRestM5ClosureRoot R; R.Name = TEXT("manual"); R.Maybe.Emplace(A);
            FVaRestM5ClosureRoot S(R);
            FVaRestM5ClosureRoot::StaticStruct()->CopyScriptStruct(&S, &R);
            if (!FVaRestM5ClosureRoot::StaticStruct()->CompareScriptStruct(&S, &R, 0) ||
                FVaRestM5ClosureRoot::StaticStruct()->GetStructTypeHash(&S) != FVaRestM5ClosureRoot::StaticStruct()->GetStructTypeHash(&R)) return false;
            S.Name = TEXT("different");
            if (FVaRestM5ClosureRoot::StaticStruct()->CompareScriptStruct(&S, &R, 0)) return false;
        }
        for (uint8 Type = 0; Type != Types; ++Type)
            for (uint8 Op = 0; Op != Ops; ++Op) if (Counts[Type][Op].load() == 0) return false;
        for (uint8 Type = 0; Type != Types; ++Type)
            UE_LOG(LogVaRestM5NativeClosure, Display, TEXT("VAREST_M5_NATIVE_MANUAL_OPS type=%d init=%lld copy=%lld assign=%lld destroy=%lld equal=%lld hash=%lld mode=GameThread source=actualScriptStructAndNativeOps"), int32(Type),
                static_cast<long long>(Counts[Type][uint8(EOp::Construct)].load()), static_cast<long long>(Counts[Type][uint8(EOp::Copy)].load()),
                static_cast<long long>(Counts[Type][uint8(EOp::Assign)].load()), static_cast<long long>(Counts[Type][uint8(EOp::Destroy)].load()),
                static_cast<long long>(Counts[Type][uint8(EOp::Equal)].load()), static_cast<long long>(Counts[Type][uint8(EOp::Hash)].load()));
        return NativeValuesRetired() && WrongThread.load() == 0 && MissingDependency.load() == 0;
    }
    void Capture()
    {
        FVaRestDynamicResultConfig Config; Config.ResponseType.Kind = EVaRestTypeKind::Struct;
        Config.ResponseType.Type = FVaRestM5ClosureRoot::StaticStruct();
        Config.ResponseType.Container = Case == 0 ? EVaRestTypeContainer::None : Case == 1 ? EVaRestTypeContainer::Array : EVaRestTypeContainer::Map;
        Config.Budget = Budget;
        auto Request = Client.RequestDynamic(TEXTVIEW("GET"), FString::Printf(TEXT("/m5/native-closure/%d"), Case), MoveTemp(Config));
        // The SAME actual public builder owns its production closure during collection.
        // The missing-entry control never admitted that closure, so it must report its real Config result instead.
        if (!OmitLeaf)
        {
            Enum.Reset(); Dropped.Reset();
            CollectGarbage(RF_NoFlags, true);
            if (WeakDropped.IsValid()) { Fail(TEXT("CONTROL_INVALID matching dropped enum was not collected")); return; }
            if (!WeakEnum.IsValid()) { Fail(TEXT("RequiredNativeDependencyPinMissing: same request lost its only nonreflected dependency before wire")); return; }
            if (Case == 0 && !QualifyNativeOperations()) { Fail(TEXT("per-type actual native lifecycle qualification failed")); return; }
            UE_LOG(LogVaRestM5NativeClosure, Display, TEXT("VAREST_M5_NATIVE_PIN case=%d sameBuilder=1 externalStrongOwners=0 liveEnum=1 droppedEnum=0 beforeWire=1"), Case);
        }
        Cell = MakeShared<FCell, ESPMode::ThreadSafe>(); const auto CaptureCell = Cell.ToSharedRef();
        Handle = MoveTemp(Request).NoAuth().NoCache().NoRetry().NoEnvelope().KeepBody().Timeout(10.f, 5.f)
            .OnSuccess([CaptureCell](const FVaRestDynamicResult& Result)
            {
                if (!IsInGameThread()) { CaptureCell->WrongCallbackThread.store(true); CaptureCell->Done.store(true, std::memory_order_release); return; }
                ++CaptureCell->Success; CaptureCell->Shell = Result;
                CaptureCell->TypedAtSuccess = TypedMask.load(); CaptureCell->ValuesRetiredAtSuccess = NativeValuesRetired();
            })
            .OnComplete([CaptureCell](const TVaRestResult<FVaRestDynamicResult>& Result)
            {
                if (!IsInGameThread()) { CaptureCell->WrongCallbackThread.store(true); CaptureCell->Done.store(true, std::memory_order_release); return; }
                ++CaptureCell->Complete; CaptureCell->Error = Result.Error; CaptureCell->Response = Result.Response;
                CaptureCell->CallbackFrame = GFrameCounter; CaptureCell->Done.store(true, std::memory_order_release);
            }).Send();
        Phase = EPhase::Reply;
    }
    void AcceptReply()
    {
        if (Cell->WrongCallbackThread.load()) { Fail(TEXT("public dynamic callback left GT")); return; }
        if (Cell->Error.IsError())
        { Fail(*FString::Printf(TEXT("actual public dynamic error kind=%d code=%s message=%s"), int32(Cell->Error.Kind), *Cell->Error.Code, *Cell->Error.Message)); return; }
        const uint32 Required[] = {1u, 6u, 24u};
        const auto Type = Cell->Shell.GetType(); const auto ExpectedJson = FVaRestJson::Parse(Reply(Case));
        const FTCHARToUTF8 Bytes(*Reply(Case));
        if (Cell->Success != 1 || Cell->Complete != 1 || Cell->Response.Status != 200 || Cell->Response.Attempts != 1 || Cell->Response.bFromCache || Cell->Response.ContentType != TEXT("application/json") || !Cell->Response.DecodeWarnings.IsEmpty() ||
            Type.Kind != EVaRestTypeKind::Struct || Type.Container != (Case == 0 ? EVaRestTypeContainer::None : Case == 1 ? EVaRestTypeContainer::Array : EVaRestTypeContainer::Map) ||
            Type.Availability != EVaRestDynamicAvailability::Available || !Cell->Shell.HasBody() || !ExpectedJson.HasValue() || !Cell->Shell.GetJson().Equals(ExpectedJson.GetValue()) ||
            Cell->Response.Body.Num() != Bytes.Length() || FMemory::Memcmp(Cell->Response.Body.GetData(), Bytes.Get(), Bytes.Length()) != 0 ||
            (Cell->TypedAtSuccess & Required[Case]) != Required[Case] || !Cell->ValuesRetiredAtSuccess || !NativeValuesRetired() || BadValues.load() || WrongThread.load() || MissingDependency.load())
        { Fail(TEXT("actual native values/raw bytes/metadata or pre-callback destruction differs")); return; }
        if (GFrameCounter <= Cell->CallbackFrame || Client.GetInFlightCount() != 0) return;
        if (Budget->NumPending() || Budget->InputBytes() || Budget->OutputBytes() || Budget->ParseEstimateBytes()) { Fail(TEXT("actual owned dynamic admission did not release")); return; }
        Shells.Add(MoveTemp(Cell->Shell)); Cell.Reset();
        FVaRestDynamicNativeTypes::UnregisterGameThreadType(FVaRestM5ClosureRoot::StaticStruct()); RegisteredRoot = false;
        Enum.Reset(); Dropped.Reset(); // relevant only for missing-entry control; positive already dropped these before Send
        CollectGarbage(RF_NoFlags, true);
        if (WeakEnum.IsValid() || WeakDropped.IsValid() || !Shells.Last().HasBody() || !NativeValuesRetired()) { Fail(TEXT("owned qualification retirement retained enum/value or damaged plain shell")); return; }
        Dependency.Reset();
        UE_LOG(LogVaRestM5NativeClosure, Display, TEXT("VAREST_M5_NATIVE_CASE case=%d actualWire=1 mode=GameThread typedDtorMask=%u valuesRetiredBeforeCallback=1 dependencyCollectedAfterOwnedRetirement=1 pendingBudget=0"), Case, TypedMask.load());
        ++Case;
        if (Case != 3) { BeginCase(); return; }
        FVaRestDynamicNativeTypes::UnregisterGameThreadType(FVaRestM5ClosureLeaf::StaticStruct()); RegisteredLeaf = false;
        PublicationFrame = GFrameCounter; Phase = EPhase::Drop;
    }
    void DropShells()
    {
        if (GFrameCounter <= PublicationFrame || Client.GetInFlightCount() != 0) return;
        if (!Drop)
        {
#if WITH_DEV_AUTOMATION_TESTS
            for (const auto& Shell : Shells) if (Shell.GetNativeOwnerCountForTesting() != 1) return; // actual last owner, no inferred count
            for (const auto& Shell : Shells)
            {
                const auto Probe = MakeShared<FVaRestDynamicReleaseProbe, ESPMode::ThreadSafe>();
                if (!Shell.AttachReleaseProbeForTesting(Probe)) { Fail(TEXT("actual native payload probe could not attach")); return; }
                ReleaseProbes.Add(Probe);
            }
#endif
            Drop = MakeShared<FWorkerDrop, ESPMode::ThreadSafe>(); const auto NativeDrop = Drop.ToSharedRef();
            DropTask = UE::Tasks::Launch(TEXT("VaRest.M5.Cooked.NativeShellDrop"), [OwnedShells = MoveTemp(Shells), NativeDrop]() mutable
            {
                NativeDrop->OffGT.store(!IsInGameThread(), std::memory_order_relaxed);
                OwnedShells.Reset(); // plain public data only; no client/clock/type/property/enum captured
                NativeDrop->Done.store(true, std::memory_order_release);
            });
            return;
        }
        if (!Drop->Done.load(std::memory_order_acquire) || !DropTask.IsCompleted()) return;
        if (!Drop->OffGT.load() || !NativeValuesRetired() || WrongThread.load() || MissingDependency.load() || BadValues.load() || TypedMask.load() != 31u)
        { Fail(TEXT("native shell release or actual GT lifecycle failed")); return; }
        bool LastReleaseProven = false;
#if WITH_DEV_AUTOMATION_TESTS
        for (const auto& Probe : ReleaseProbes)
            if (!Probe->bDestroyed.load(std::memory_order_acquire) || Probe->bDestroyedOnGameThread.load()) { Fail(TEXT("exact final native payload did not die on worker")); return; }
        LastReleaseProven = ReleaseProbes.Num() == 3;
#endif
        Tracking.store(false, std::memory_order_release);
        UE_LOG(LogVaRestM5NativeClosure, Display, TEXT("VAREST_M5_NATIVE_PASS cases=3 OptionalOnlyLeaf=qualified transientPinNecessary=1 nativeValuesGT=1 actualTypedDtorMask=31 valuesRetiredBeforeCallbacks=1 ownedBudgetsZero=1 shellDropOffGT=1 lastNativePayloadReleaseProven=%d reflectedWorkers=disabled moduleStop=OPEN wholeTask14=OPEN"), LastReleaseProven ? 1 : 0);
        Phase = EPhase::Done; Stop();
        FPlatformMisc::RequestExitWithStatus(false, 0, TEXT("VaRestM5NativeClosure"));
    }
    void Tick()
    {
        if (FPlatformTime::Seconds() - Started > 45.0) { Fail(TEXT("owned finite native fixture deadline")); return; }
        if (Phase == EPhase::Capture && GFrameCounter > PublicationFrame + 1) Capture();
        else if (Phase == EPhase::Reply && Cell && Cell->Done.load(std::memory_order_acquire)) AcceptReply();
        else if (Phase == EPhase::Drop) DropShells();
    }
    void Fail(const TCHAR* Reason)
    {
        UE_LOG(LogVaRestM5NativeClosure, Error, TEXT("VAREST_M5_NATIVE_FAIL case=%d reason=%s"), Case, Reason);
        Phase = EPhase::Done; Stop();
        FPlatformMisc::RequestExitWithStatus(false, 73, TEXT("VaRestM5NativeClosure"));
    }
    void Stop()
    {
        FCoreDelegates::GetOnPostEngineInit().Remove(PostInit); PostInit.Reset();
        FCoreDelegates::OnEndFrame.Remove(Frame); Frame.Reset();
        if (RegisteredRoot) { FVaRestDynamicNativeTypes::UnregisterGameThreadType(FVaRestM5ClosureRoot::StaticStruct()); RegisteredRoot = false; }
        if (RegisteredLeaf) { FVaRestDynamicNativeTypes::UnregisterGameThreadType(FVaRestM5ClosureLeaf::StaticStruct()); RegisteredLeaf = false; }
        Client.CancelAll(); Client = FVaRestClient();
        Enum.Reset(); Dropped.Reset(); Dependency.Reset();
        // Any remaining public completion captures FCell only, never this runner or any reflected owner.
        Cell.Reset();
    }
    FDelegateHandle PostInit, Frame;
    FVaRestClient Client;
    TSharedPtr<FVaRestDynamicBudget, ESPMode::ThreadSafe> Budget;
    TStrongObjectPtr<UEnum> Enum, Dropped;
    TWeakObjectPtr<UEnum> WeakEnum, WeakDropped;
    TSharedPtr<FCell, ESPMode::ThreadSafe> Cell;
    TArray<FVaRestDynamicResult> Shells;
#if WITH_DEV_AUTOMATION_TESTS
    TArray<TSharedPtr<FVaRestDynamicReleaseProbe, ESPMode::ThreadSafe>> ReleaseProbes;
#endif
    TSharedPtr<FWorkerDrop, ESPMode::ThreadSafe> Drop;
    UE::Tasks::TTask<void> DropTask;
    FVaRestRequestHandle Handle;
    EPhase Phase = EPhase::Waiting;
    uint64 PublicationFrame = 0;
    double Started = 0;
    int32 Port = 18766, Case = 0;
    bool RegisteredRoot = false, RegisteredLeaf = false, OmitLeaf = false;
};
FVaRestM5NativeClosureProof::FVaRestM5NativeClosureProof() : Impl(MakeUnique<FImpl>()) {}
FVaRestM5NativeClosureProof::~FVaRestM5NativeClosureProof() = default;
void FVaRestM5NativeClosureProof::Start() { Impl->Start(); }
