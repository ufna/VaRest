// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestLwsService.h"
#include "VaRestLwsCopyPolicy.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"
#include "Misc/Fork.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "PlatformHttp.h"
#include "Ssl.h"
#include "VaRestSocketTransport.h"
#include "VaRestSocketTypes.h"
#include "VaRestWebSocketLog.h"
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
#include "NativeOwner/VaRestNativeOwnerCallerEntry.h"
#include "NativeOwner/VaRestNativeOwnerServiceAdapter.h"
#include "NativeOwner/VaRestNativeOwnerResources.h"
#include <type_traits>
#endif
#if WITH_DEV_AUTOMATION_TESTS
#include "VaRestLwsConn.h"
#endif

// VaRestSocketTransport.h for MaxConnectTimeoutSeconds: this file must compile alone, outside a unity blob (review spec I-2b)
static_assert(VaRest::WebSocket::FrameHeadroom >= LWS_PRE, "FVaRestSocketFrame's headroom must hold libwebsockets' frame header (libwebsockets.h:4773)");

namespace VaRest::WebSocket::Lws::VaRestLwsServicePrivate
{
	/** One libwebsockets line for UE's log: trailing white space dropped, controls shown as '?' (a server's bytes may be in it) */
	FString CleanLine(const char* Line)
	{
		FString Text = Line ? FString(UTF8_TO_TCHAR(Line)) : FString();
		Text.TrimEndInline();
		for (TCHAR& Char : Text)
		{
			if (Char < 0x20 || Char == 0x7F)
			{
				Char = TEXT('?');
			}
		}
		return Text;
	}

	/** VaRest's emitter for its LWS copy: errors and warnings, both Warnings, as the engine reports its own (LwsWebSocketsManager.cpp:403-424) */
	void EmitLwsLine(int Level, const char* Line)
	{
		UE_LOG(LogVaRestSocket, Warning, TEXT("libwebsockets(%s): %s"), Level == LLL_ERR ? TEXT("err") : TEXT("warn"), *CleanLine(Line));
	}

	/**
	 * Whether the service's thread is a real one, so the game thread may wait for it: the engine's own test before it lets
	 * lws_service block (LwsWebSocketsManager.cpp:110-118). Elsewhere the main thread ticks the forkable thread (Fork.h:86-93)
	 */
	bool HasRealThread()
	{
		return FPlatformProcess::SupportsMultithreading() || FForkProcessHelper::IsForkedMultithreadInstance();
	}
}

void VaRest::WebSocket::Lws::Utf8Copy(const FString& Text, TArray<ANSICHAR>& Out)
{
	const FTCHARToUTF8 Converted(*Text);
	Out.SetNumUninitialized(Converted.Length() + 1);
	if (Converted.Length() > 0)
	{
		FMemory::Memcpy(Out.GetData(), Converted.Get(), Converted.Length());
	}
	Out[Converted.Length()] = '\0';
}

void VaRest::WebSocket::Lws::ApplyLogLevel()
{
	// At most once per module copy (review M-1): lws_set_log_level is two plain stores to the copy's globals, which an
	// earlier service's thread may be reading in _lws_log (the default transport's outlives its test). The first Start comes
	// before every VaRest service thread, so that one write happens-before every reader (thread creation). Set only when
	// applied, never std::call_once: a shared copy may rightly skip while WebSockets is loaded and apply at a later start
	static std::atomic<bool> bApplied{ false };
	if (bApplied.load())
	{
		return;
	}
	// D30: private external labels select our own copy, including qualified monolithic Game. Otherwise the legacy
	// monolithic/merged Engine copy is shared and its loaded WebSockets module owns the level/emitter.
	// A later Engine module load must not change a namespaced private copy's level or expose its query text.
	constexpr bool bSharedCopy = SharesEngineLws;
	if (!bSharedCopy || !FModuleManager::Get().IsModuleLoaded(TEXT("WebSockets")))
	{
		lws_set_log_level(LLL_ERR | LLL_WARN, &VaRestLwsServicePrivate::EmitLwsLine);
		bApplied = true;
	}
}

bool VaRest::WebSocket::Lws::ShouldCreateAtStartup(bool bSharedCopy, TFunctionRef<bool(const TCHAR*)> IsLinked)
{
	if (!bSharedCopy)
	{
		return false;
	}
	// The engine modules that link libwebsockets and make their own contexts (spec 1.2)
	for (const TCHAR* Name : { TEXT("WebSockets"), TEXT("WebSocketServer"), TEXT("WebSocketNetworking"), TEXT("HttpNetworkReplayStreaming") })
	{
		if (IsLinked(Name))
		{
			return true;
		}
	}
	return false;
}

std::atomic<int32>& VaRest::WebSocket::Lws::TlsHandshakesInFlight()
{
	static std::atomic<int32> Count{ 0 };
	return Count;
}

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
namespace VaRest::WebSocket::Lws
{
FVaRestNativeOwnerHostStep1 FService::NativeFrameDisposition1(FVaRestNativeOwnerHostStep1 Step) const
{
    // Caller is the actual physical GT. These are actual cause-specific journals,
    // never a last-frame cache or an inference of native19 from local retention.
    if (NativeCreating.AdmissionStatus == VR_STATUS_RELEASE_UNCERTAIN
        || NativeCreating.AttachStatus == VR_STATUS_RELEASE_UNCERTAIN
        || NativeCreating.CallerDispositionStatus == VR_STATUS_RELEASE_UNCERTAIN)
        return {16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Retained),VR_STATUS_RELEASE_UNCERTAIN};
    const TSharedPtr<FNativeOwnerServiceAdapter, ESPMode::ThreadSafe> Held = NativeCreating.Adapter;
    if (Held.IsValid()) Step = Held->PreserveFrameDisposition1(Step);
    if (Step.ActualStatus == VR_STATUS_RELEASE_UNCERTAIN) return Step;
    if (NativeCreating.InEntry || NativeCreating.InCreate || NativeCreating.InCleanup)
        return {16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Pending),VR_STATUS_BUSY};
    return Step;
}

[[gnu::used]] FVaRestNativeOwnerHostStep1 FService::PumpNativeOnCurrentControl()
{
    const VrStatus Role = RequireNativeOwnerControlThread(TEXT("existing native Service frame"));
    if (Role != VR_STATUS_OK) return {16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Refused),Role};
    if (!NativeOwnerRealThreadModeAllowed())
        return {16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Refused),VR_STATUS_FORK_UNESTABLISHED};
#if defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
    if (NativeCreating.InEntry || NativeCreating.InCreate || NativeCreating.InCleanup)
        return NativeFrameDisposition1({16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Pending),VR_STATUS_BUSY});
    if (NativeCreating.Phase == ENativeEntryPhase1::Fresh)
        return NativeFrameDisposition1({16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Refused),VR_STATUS_NO_RESULT});
    if (bShuttingDown.load() || bStopping.load() || !bAccepting.load())
        return NativeFrameDisposition1({16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Refused),VR_STATUS_STALE});
    if (NativeCreating.Phase != ENativeEntryPhase1::ChildCreated || !NativeCreating.Adapter.IsValid()
        || !NativeCreating.Resources || !NativeChildSnapshot || !Thread || !Context
        || NativeChildSnapshot->GetRegistrationDiagnostic() != VR_STATUS_OK)
        return NativeFrameDisposition1({16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Refused),VR_STATUS_OLD_OWNER_BUSY});
    const TSharedRef<FNativeOwnerServiceAdapter, ESPMode::ThreadSafe> Held = NativeCreating.Adapter.ToSharedRef();
    const FVaRestNativeOwnerHostStep1 Actual = Held->PumpOnControlThread();
    return NativeFrameDisposition1(Actual); // FULL pinned adapter/Pump return, not a status0 assumption.
#else
    return {16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Refused),VR_STATUS_DISCOVERY_UNESTABLISHED};
#endif
}

[[gnu::used]] VrStatus FService::PrepareNativeOwnerOnCurrentControl()
{
	const VrStatus Role = RequireNativeOwnerControlThread(TEXT("creating native owner entry"));
	if (Role != VR_STATUS_OK) return Role;
	if (!NativeOwnerRealThreadModeAllowed()) return VR_STATUS_FORK_UNESTABLISHED;
	if (NativeCreating.Phase != ENativeEntryPhase1::Fresh || NativeCreating.InEntry || NativeCreating.InCleanup || NativeCreating.InCreate || NativeChildSnapshot)
		return VR_STATUS_STALE;
	if (Thread || Context || bAccepting.load() || bShuttingDown.load() || bStartFailed)
		return VR_STATUS_OLD_OWNER_BUSY;
	// The future sole FTransport lane MUST hold a local strong FService and real
	// WS/Core/allocator/provider/code/H lifetime through this FULL call and every
	// retained outcome. Core1 excludes conforming entries, not arbitrary retires.
	NativeCreating.Phase = ENativeEntryPhase1::Entering;
	NativeCreating.InEntry = true;
	FCallerEntryTransaction Transaction;
	const VrStatus Status = FNativeSourceSupplier::TryCreateForCurrentControl(Transaction, *this);
	NativeCreating.AdmissionStatus = Transaction.NativeStatus;
	NativeCreating.CallerDispositionStatus = Transaction.DispositionStatus;
	NativeCreating.InEntry = false;
	if (!NativeCreating.Adapter.IsValid())
		NativeCreating.Phase = Status == VR_STATUS_RELEASE_UNCERTAIN ? ENativeEntryPhase1::Retained : ENativeEntryPhase1::Finished;
	// No context/connector/thread is started here. A stop arriving during final
	// gate return stays latched for the next bounded no-create cleanup; that later
	// step cannot borrow or rewrite the already-finished initiating Core gate.
	return Status;
}

[[gnu::used]] VrStatus FService::AttachActualOwnerDuringEntry(FCallerEntryTransaction& Transaction)
{
	const VrStatus Role = RequireNativeOwnerControlThread(TEXT("actual native owner handoff"));
	if (Role != VR_STATUS_OK) return Role;
	// Consistency only: this private edge is invoked directly AFTER the actual
	// qualified FULL Q4 return and selected15 clear. No saved tuple/bootstrap or
	// constructor/state bit grants original216 range, O, arena or caller authority.
	if (!NativeCreating.InEntry || NativeCreating.Phase != ENativeEntryPhase1::Entering
		|| Transaction.Phase != (4u | 0x100u) || Transaction.NativeStatus != VR_STATUS_OK
		|| Transaction.SelectedEntry.Admit || Transaction.SelectedEntry.Getter
		|| !Transaction.NativeOutcome.owner || !Transaction.NativeOutcome.api
		|| NativeCreating.Adapter.IsValid() || NativeCreating.Resources || Thread || Context)
		return VR_STATUS_INVALID_INPUT;
	NativeCreating.AdmissionStatus = Transaction.NativeStatus;
	NativeCreating.Phase = ENativeEntryPhase1::Attaching;
	if (bShuttingDown.load())
	{
		NativeCreating.Phase = ENativeEntryPhase1::Finished;
		return VR_STATUS_STALE;
	}
	static_assert(std::is_standard_layout_v<VrPrivateOwnerApiObject1>
		&& offsetof(VrPrivateOwnerApiObject1, Public) == 0 && sizeof(VrPrivateOwnerApiObject1) == 216);
	static_assert(sizeof(FNativeOwnerServiceValues) == 160);
	// Recover the actual standard-layout first member, BEFORE any tail read.
	// Whole216 original/RO/source qualification and uninterrupted genuine A-O
	// plus retire exclusion are independently CLOSED source premises of this edge.
	const auto* Object = reinterpret_cast<const VrPrivateOwnerApiObject1*>(Transaction.NativeOutcome.api);
	const VrPrivateOwnerTail1& Actual = Object->Private;
	// Header/type consistency is not a range probe or a qualification certificate.
	if (Object->Public.size != 72 || Object->Public.version != 2
		|| Actual.Magic[0] != 'V' || Actual.Magic[1] != 'R' || Actual.Magic[2] != 'P' || Actual.Magic[3] != 'R'
		|| Actual.Magic[4] != 'I' || Actual.Magic[5] != 'V' || Actual.Magic[6] != '1' || Actual.Magic[7] != 0
		|| Actual.Size != 144 || Actual.Version != 1 || Actual.SignatureAbi != 1
		|| Actual.Count != 14 || Actual.Mask != 0x3fff || Actual.ResourceProfile != 2 || Actual.ResourceCells != 140
		|| !Actual.VrPrivateAdmitCurrentControl1 || !Actual.VrPrivateResourceReserve1
		|| !Actual.VrPrivateResourceObtained1 || !Actual.VrPrivateResourceNotObtained1
		|| !Actual.VrPrivateResourceClosed1 || !Actual.VrPrivateResourceUncertain1)
	{
		NativeCreating.Phase = ENativeEntryPhase1::Finished;
		return VR_STATUS_ABI_INCOMPATIBLE;
	}
	const FNativeOwnerServiceValues Values {Transaction.NativeOutcome.image, Transaction.NativeOutcome.owner,
		Object->Public, {Actual.VrPrivateServiceNeverCreated1, Actual.VrPrivateRegisterCurrentServiceThread1,
		Actual.VrPrivateServiceJoinObserved1, Actual.VrPrivateRequestStop1, Actual.VrPrivatePumpRegistered1,
		Actual.VrPrivatePumpEnter1, Actual.VrPrivatePumpLeave1, Actual.VrPrivateRequestDetachAfterJoin1}};
	// Publish the strong real adapter BEFORE its query/attach/Pump/Child calls.
	NativeCreating.Adapter = FNativeOwnerServiceAdapter::CreateForService(Values);
	if (!NativeCreating.Adapter.IsValid())
	{
		NativeCreating.Phase = ENativeEntryPhase1::Finished;
		return VR_STATUS_NATIVE_ERROR;
	}
	if (bShuttingDown.load())
	{
		(void)FinishNativeOwnerNoCreateOnCurrentControl();
		return VR_STATUS_STALE;
	}
	const VrStatus Attached = NativeCreating.Adapter->AttachBeforeCreatingResources();
	NativeCreating.AttachStatus = Attached; // Separate actual adapter return, never native admission normalization.
	if (Attached != VR_STATUS_OK || bShuttingDown.load())
	{
		const VrStatus Finished = FinishNativeOwnerNoCreateOnCurrentControl();
		if (Attached == VR_STATUS_RELEASE_UNCERTAIN || Finished == VR_STATUS_RELEASE_UNCERTAIN)
			return VR_STATUS_RELEASE_UNCERTAIN;
		return Attached != VR_STATUS_OK ? Attached : (Finished != VR_STATUS_OK ? Finished : VR_STATUS_STALE);
	}
	// Existing I2 stored its actual Issued Session/StartReserved BEFORE fallible
	// Pump/Child allocation. Never derive either epoch from bootstrap image.lo.
	const VrSession& Session = NativeCreating.Adapter->Session;
	if (NativeCreating.Adapter->Outcome != FNativeOwnerServiceAdapter::ECreatingOutcome::StartReserved
		|| Session.owner_epoch == 0 || Session.service_epoch == 0)
	{
		NativeCreating.Phase = ENativeEntryPhase1::Retained;
		return VR_STATUS_RELEASE_UNCERTAIN; // Local unknown handoff duty; NativeStatus remains its actual0.
	}
	using FCarrier = C2ResourcesC1::FResourceEntryAddress1;
	static_assert(sizeof(FCarrier) == 8 && sizeof(FCarrier) == sizeof(VrPrivateResourceReserve1Fn));
	// Exactly five typed function-pointer carriers, restored to those SAME types
	// by Resources.cpp. No type-punned object read, extra address array or call
	// through void(*)(). Holder9248 is UE metadata only: no C1 Create/mmap/socket.
	NativeCreating.Resources = new FNativeOwnerResources1(Values.Owner, Session.owner_epoch, Session.service_epoch,
		reinterpret_cast<FCarrier>(Actual.VrPrivateResourceReserve1),
		reinterpret_cast<FCarrier>(Actual.VrPrivateResourceObtained1),
		reinterpret_cast<FCarrier>(Actual.VrPrivateResourceNotObtained1),
		reinterpret_cast<FCarrier>(Actual.VrPrivateResourceClosed1),
		reinterpret_cast<FCarrier>(Actual.VrPrivateResourceUncertain1));
	if (!NativeCreating.Resources || bShuttingDown.load())
	{
		const VrStatus Finished = FinishNativeOwnerNoCreateOnCurrentControl();
		return Finished != VR_STATUS_OK ? Finished : (bShuttingDown.load() ? VR_STATUS_STALE : VR_STATUS_NATIVE_ERROR);
	}
	NativeCreating.Phase = ENativeEntryPhase1::Attached;
	return VR_STATUS_OK;
}

[[gnu::used]] bool FService::NativeEntryResourcesAreMetadataOnly() const
{
	if (!NativeCreating.Resources) return true;
	const auto& R = *NativeCreating.Resources;
	if (R.Storage.Size != 48 || R.Storage.Version != 1 || R.Storage.Raw || R.Storage.MappingBytes
		|| R.Storage.Object || R.Storage.State || R.Storage.NativeError || R.Storage.Reserved
		|| R.Journal.Header.Size != 64 || R.Journal.Header.Version != 1
		|| R.Journal.Header.Profile != 2 || R.Journal.Header.CellCount != 140) return false;
	// This narrow I3-A edge has NEVER entered C1. Check its actual fixed journal,
	// not an absent pointer or a generic counter setter. Future I3-B/C2-L cleanup
	// must use genuine resource-return/ACK hooks instead of this fresh-only path.
	const uint8* Bytes = reinterpret_cast<const uint8*>(&R.Journal);
	for (uint32 I = 16; I < 9024; ++I) if (Bytes[I] != 0) return false;
	if (!C2ResourcesC1::ResourceBindingValid1(&R.Binding)
		|| R.Binding.Journal != &R.Journal || R.View.ResourceBinding != &R.Binding || R.View.Control != &R.Control) return false;
	// This edge is genuinely never-entered metadata only. Once ANY source event
	// enters Control/Journal, later real custody/full-return cleanup is mandatory.
	const uint8* ControlBytes = reinterpret_cast<const uint8*>(&R.Control);
	for (uint32 I = 0; I < 64; ++I) if (ControlBytes[I] != 0) return false;
	return true;
}

[[gnu::used]] VrStatus FService::FinishNativeOwnerNoCreateOnCurrentControl()
{
	const VrStatus Role = RequireNativeOwnerControlThread(TEXT("native entry no-create outcome"));
	if (Role != VR_STATUS_OK) return Role;
	if (NativeCreating.InCleanup || NativeCreating.InCreate) return VR_STATUS_BUSY;
	// No thread/context/connector was ever started by I3-A. These are guards
	// against using this narrow helper as later real creating/join authority.
	if (Thread || Context || bAccepting.load() || !NativeEntryResourcesAreMetadataOnly()
		|| (NativeCreating.Resources && !NativeCreating.Adapter.IsValid()))
	{
		NativeCreating.Phase = ENativeEntryPhase1::Retained;
		// SERVICE disposition of an unknown owned source journal, never a
		// relabeling of admission/attach or known native poison14 as native19.
		return VR_STATUS_RELEASE_UNCERTAIN;
	}
	NativeCreating.InCleanup = true;
	VrStatus Status = VR_STATUS_OK;
	if (NativeCreating.Adapter.IsValid())
	{
		const TSharedRef<FNativeOwnerServiceAdapter, ESPMode::ThreadSafe> KeepAlive = NativeCreating.Adapter.ToSharedRef();
		using EOutcome = FNativeOwnerServiceAdapter::ECreatingOutcome;
		if (!KeepAlive->CanReleaseCreatingAdapter())
		{
			NativeCreating.Phase = ENativeEntryPhase1::NoCreatePending;
			if (KeepAlive->Outcome == EOutcome::StartReserved)
				Status = KeepAlive->AbortStartupBeforeThreadCreate();
			else if (KeepAlive->Outcome == EOutcome::NeverCreatedPending || KeepAlive->Outcome == EOutcome::NeverCreatedRecorded)
			{
				// Retry only the existing retained BUSY bookkeeping; no create,
				// join, OS close, second registration or new native admission.
				Status = KeepAlive->StopBeforeCreatingJoin();
				if (KeepAlive->Outcome == EOutcome::NeverCreatedPending && KeepAlive->LastOutcomeStatus == VR_STATUS_BUSY)
				{
					const VrStatus Recorded = KeepAlive->RetryPendingOutcomeOnControlThread();
					if (Status == VR_STATUS_RELEASE_UNCERTAIN || Recorded == VR_STATUS_RELEASE_UNCERTAIN)
						Status = VR_STATUS_RELEASE_UNCERTAIN;
					else if (Status == VR_STATUS_OK) Status = Recorded;
				}
			}
			else Status = VR_STATUS_RELEASE_UNCERTAIN; // SERVICE unknown-duty disposition, not a renamed native return14. No fake NeverCreated.
			if (KeepAlive->Outcome == EOutcome::NeverCreatedRecorded)
			{
				// All actual I3-A resource storage is proven never-entered metadata.
				// Keep its scalar pointer through the full non-native delete return.
				FNativeOwnerResources1* Metadata = NativeCreating.Resources;
				NativeCreating.Resources = nullptr;
				delete Metadata;
				const VrStatus Detached = KeepAlive->AfterKnownCreatingResourcesClosed();
				if (Status == VR_STATUS_RELEASE_UNCERTAIN || Detached == VR_STATUS_RELEASE_UNCERTAIN)
					Status = VR_STATUS_RELEASE_UNCERTAIN;
				else if (Status == VR_STATUS_OK) Status = Detached;
			}
		}
		if (KeepAlive->CanReleaseCreatingAdapter())
		{
			FNativeOwnerResources1* Metadata = NativeCreating.Resources;
			NativeCreating.Resources = nullptr;
			delete Metadata;
			NativeCreating.Adapter.Reset();
			NativeCreating.Phase = ENativeEntryPhase1::Finished;
		}
		else if (Status == VR_STATUS_RELEASE_UNCERTAIN)
			NativeCreating.Phase = ENativeEntryPhase1::Retained;
		else
		{
			NativeCreating.Phase = ENativeEntryPhase1::NoCreatePending;
			// A still-charged journal is never reported as completed merely
			// because this step had no further eligible native call.
			if (Status == VR_STATUS_OK)
				Status = KeepAlive->LastOutcomeStatus != VR_STATUS_OK && KeepAlive->LastOutcomeStatus != VR_STATUS_PENDING
					? KeepAlive->LastOutcomeStatus : VR_STATUS_PENDING;
		}
	}
	else NativeCreating.Phase = ENativeEntryPhase1::Finished;
	NativeCreating.InCleanup = false;
	return Status;
}

namespace
{
    // Distinct raw operations remain separately journaled.19 dominates actual
    // uncertainty, while a derived retained-duty19 is never an old native14 rename.
    static VrStatus NativeCleanupDisposition1(VrStatus First, VrStatus Second, VrStatus Progress)
    {
        if (First == VR_STATUS_RELEASE_UNCERTAIN || Second == VR_STATUS_RELEASE_UNCERTAIN || Progress == VR_STATUS_RELEASE_UNCERTAIN)
            return VR_STATUS_RELEASE_UNCERTAIN;
        return First != VR_STATUS_OK ? First : (Second != VR_STATUS_OK ? Second : Progress);
    }
#if defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
    // Exact selected C source constants: varest_custody.h source-location1 and
    // context phases4/5. These are local diagnostics, not a new common ABI.
    constexpr uint32 NativeContextLocation1 = 1, NativeContextRetained1 = 4, NativeContextAbsent1 = 5;
    enum class ENativeContextReturn1 : uint8 { Unknown, Live, NoObject, Freed };
    struct FNativeContextSelf1
    {
        uint64_t SourceSerial = 0, NativeSerial = 0, NativeValue = 0, Extent = 0;
        uint32 Cell = 140;
    }; // Transient pre-call witness only; no retained Cell64/API copy.
    static_assert(sizeof(FNativeContextSelf1) == 40);
    static bool CaptureNativeContextSelf1(const C2ResourcesC1::FResourceBinding1& R,
        const VrLwsSourceControl1& C, const lws_context* Object, FNativeContextSelf1& Out)
    {
        if (!Object || !C.CtorEntered || !C.FullCreateReturned || !C.ContextSourceSerial
            || C.RawContext != reinterpret_cast<uintptr_t>(Object)) return false;
        for (uint32 I = 12; I < 140; ++I) {
            const auto& Row = R.Journal->Cells[I];
            if (Row.Kind != VR_LWS_KIND_CONTEXT1 || Row.Subkind != 1 || Row.SourceSerial != C.ContextSourceSerial) continue;
            if (Out.Cell != 140 || !Row.NativeSerial || Row.ParentSerial || !Row.Extent
                || Row.NativeValue != reinterpret_cast<uintptr_t>(Object) || Row.SourceLocation != NativeContextLocation1
                || Row.Result != VR_LWS_POINTER_PRESENT2 || Row.SavedError
                || (Row.LocalState != C2ResourcesC1::ResourceObtainedPending1 && Row.LocalState != C2ResourcesC1::ResourceOwnedAck1)) return false;
            Out = {Row.SourceSerial, Row.NativeSerial, Row.NativeValue, Row.Extent, I};
        }
        return Out.Cell != 140;
    }
    static ENativeContextReturn1 QualifyNativeContextReturn1(const C2ResourcesC1::FResourceBinding1& R,
        const VrLwsSourceControl1& C, const VrLwsResult1& A, int32 Raw, const FNativeContextSelf1* Before)
    {
        // Genuine creating/known-joined exclusive source interval and exact C
        // implementation precede this PURE bounded external-journal inspection.
        if (A.Size != sizeof(A) || A.Version != 1 || Raw < VR_LWS_KNOWN_SUCCESS1 || Raw > VR_LWS_PROTOCOL_FAULT1
            || A.Disposition != static_cast<uint32>(Raw) || A.Fault != C.Fault || !C.FullCreateReturned
            || C.CallbackDepth || C.CancelUsers || C.Reserved0 || C.Reserved1) return ENativeContextReturn1::Unknown;
        if (Before && (!C.FullDestroyReturned || C.ContextSourceSerial != Before->SourceSerial)) return ENativeContextReturn1::Unknown;
        if (A.RawObject) {
            if (!C.CtorEntered || C.RawContext != reinterpret_cast<uintptr_t>(A.RawObject)
                || (Before && C.RawContext != Before->NativeValue)) return ENativeContextReturn1::Unknown;
            FNativeContextSelf1 Current;
            return CaptureNativeContextSelf1(R, C, static_cast<const lws_context*>(A.RawObject), Current)
                && (!Before || (Current.Cell == Before->Cell && Current.NativeSerial == Before->NativeSerial && Current.Extent == Before->Extent))
                ? ENativeContextReturn1::Live : ENativeContextReturn1::Unknown;
        }
        if (C.RawContext || A.SourceLocation != NativeContextLocation1
            || (C.ContextPhase != NativeContextRetained1 && C.ContextPhase != NativeContextAbsent1 && C.CtorEntered))
            return ENativeContextReturn1::Unknown;
        uint32 Count = 0, Match = 140;
        for (uint32 I = 12; I < 140; ++I) {
            const auto& Row = R.Journal->Cells[I];
            if (Row.LocalState == C2ResourcesC1::ResourceEmpty1) continue;
            ++Count;
            if (Row.Kind != VR_LWS_KIND_CONTEXT1 || Row.Subkind != 1 || !Row.SourceSerial || !Row.NativeSerial
                || Row.ParentSerial || Row.SourceLocation != NativeContextLocation1 || !Row.Extent || Match != 140)
                return ENativeContextReturn1::Unknown;
            Match = I;
        }
        // Source cleanup permits physical self free ONLY after every child and
        // its source references are retired. Thus <=one pool row may remain.
        if (Count > 1 || A.PendingRows != Count) return ENativeContextReturn1::Unknown;
        if (C.ContextSourceSerial) {
            if (!C.CtorEntered) return ENativeContextReturn1::Unknown;
            if (!Count) {
                // C ack_retire consumed the exact self row before its local
                // witness retired; fixed free-tail/serial/phase/result identify
                // this branch, not NULL or an empty journal alone.
                return C.ContextPhase == NativeContextAbsent1
                    ? ENativeContextReturn1::Freed : ENativeContextReturn1::Unknown;
            }
            const auto& Row = R.Journal->Cells[Match];
            if (Row.SourceSerial != C.ContextSourceSerial || !Row.NativeValue || Row.SavedError
                || Row.Result != VR_LWS_VOID_FULL_RETURN2
                || (Row.LocalState != C2ResourcesC1::ResourceObtainedThenClosed1
                    && Row.LocalState != C2ResourcesC1::ResourceClosedPending1 && Row.LocalState != C2ResourcesC1::ResourceClosedAck1)
                || (Before && (Match != Before->Cell || Row.NativeSerial != Before->NativeSerial
                    || Row.NativeValue != Before->NativeValue || Row.Extent != Before->Extent))) return ENativeContextReturn1::Unknown;
            return ENativeContextReturn1::Freed;
        }
        if (Before) return ENativeContextReturn1::Unknown;
        if (!C.CtorEntered) {
            return !Count && !C.ContextPhase && Raw == VR_LWS_DEFERRED_BEFORE_ACQUIRE1
                ? ENativeContextReturn1::NoObject : ENativeContextReturn1::Unknown;
        }
        if (Count) {
            const auto& Row = R.Journal->Cells[Match];
            if (Row.NativeValue || Row.Result != VR_LWS_POINTER_ABSENT2
                || (Row.LocalState != C2ResourcesC1::ResourceNotObtainedPending1 && Row.LocalState != C2ResourcesC1::ResourceNotObtainedAck1))
                return ENativeContextReturn1::Unknown;
        }
        return C.ContextPhase == NativeContextAbsent1 || (Count && C.ContextPhase == NativeContextRetained1)
            ? ENativeContextReturn1::NoObject : ENativeContextReturn1::Unknown;
    }
#endif
}
[[gnu::used]] VrStatus FService::PrepareNativeStartupPhaseBeforeResourcesOnCurrentControl()
{
    const VrStatus Role = RequireNativeOwnerControlThread(TEXT("native startup phase before resources"));
    if (Role != VR_STATUS_OK) return Role;
    if (!NativeOwnerRealThreadModeAllowed()) return VR_STATUS_DISCOVERY_UNESTABLISHED;
    if (NativeCreating.Phase != ENativeEntryPhase1::Attached || NativeCreating.InEntry || NativeCreating.InCleanup || NativeCreating.InCreate
        || !NativeCreating.Adapter.IsValid() || !NativeCreating.Resources || Thread || Context || NativeChildSnapshot
        || bShuttingDown.load() || bAccepting.load() || !NativeEntryResourcesAreMetadataOnly()) return VR_STATUS_INVALID_INPUT;
    // Fixed private program point BEFORE any actual C1/constructor operation.
    // An original phase16 remains Adapter-owned across every pending return.
    return NativeCreating.Adapter->InitializeStartupPhaseBeforeResources();
}

[[gnu::used]] VrStatus FService::StartNativeOnCurrentControl()
{
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
    const VrStatus Role = RequireNativeOwnerControlThread(TEXT("native tracked startup"));
    if (Role != VR_STATUS_OK) return Role;
    // Independent SOURCE prerequisite: actual manager/provider is already
    // initialized and continuously protected BEFORE this entry. Registry/pointer
    // presence does NOT establish initialization, code O or roots lifetime.
    FSslModule* const Ssl = FModuleManager::GetModulePtr<FSslModule>(TEXT("SSL"));
    if (!Ssl) return VR_STATUS_DISCOVERY_UNESTABLISHED;
    if (NativeCreating.Phase == ENativeEntryPhase1::Fresh) {
        const VrStatus Entered = PrepareNativeOwnerOnCurrentControl();
        if (Entered != VR_STATUS_OK) return Entered;
    }
    if (NativeCreating.Phase != ENativeEntryPhase1::Attached || NativeCreating.InEntry || NativeCreating.InCleanup
        || NativeCreating.InCreate || bShuttingDown.load() || !NativeCreating.Adapter.IsValid() || !NativeCreating.Resources)
        return VR_STATUS_STALE;
    const auto KeepAlive = NativeCreating.Adapter.ToSharedRef();
    if (!KeepAlive->GetCreatingPhaseOnControlThread()) {
        const VrStatus Prepared = PrepareNativeStartupPhaseBeforeResourcesOnCurrentControl();
        if (Prepared != VR_STATUS_OK) return Prepared;
    }
    VrLwsPhaseBudget1* const Phase = KeepAlive->GetCreatingPhaseOnControlThread();
    if (!Phase || Phase->Phase != VR_LWS_PHASE_STARTUP1) return VR_STATUS_INVALID_INPUT;
    // Existing public policy rejects upstream CONNECT proxy configuration. This
    // captured absence selects direct C1 mode0; the INTERNAL proxy below is C1.
    if (!FPlatformHttp::GetConfiguredProxyAddress().IsEmpty()) return VR_STATUS_INVALID_INPUT;
    NativeRunning.NoProxyCaptured = true;
    NativeCreating.InCreate = true;
    auto& R = *NativeCreating.Resources;
    if (!R.Journal.Header.CreateEntered) {
        const auto Created = C2ResourcesC1::Create1(&R.Storage, &R.Binding);
        NativeRunning.RawRunStatus = static_cast<int32>(Created); // Raw journal already durable on return.
        if (Created != C2ResourcesC1::EStatus1::OK) {
            NativeCreating.InCreate = false;
            const VrStatus Finished = FinishNativeCleanupChildOnCurrentControl();
            return NativeCleanupDisposition1(Created == C2ResourcesC1::EStatus1::STORAGE_UNCERTAIN ? VR_STATUS_RELEASE_UNCERTAIN : VR_STATUS_NATIVE_ERROR, VR_STATUS_OK, Finished);
        }
    }
    Protocols[0].name = "varest"; Protocols[0].callback = &FService::Callback;
    Protocols[0].per_session_data_size = 0; Protocols[0].rx_buffer_size = RxBufferBytes;
    lws_context_creation_info Info {};
    Info.port = CONTEXT_PORT_NO_LISTEN; Info.protocols = Protocols; Info.uid = -1; Info.gid = -1;
    Info.options = LWS_SERVER_OPTION_EXPLICIT_VHOSTS | LWS_SERVER_OPTION_PEER_CERT_NOT_REQUIRED
        | LWS_SERVER_OPTION_DISABLE_OS_CA_CERTS | LWS_SERVER_OPTION_JUST_USE_RAW_ORIGIN;
    Info.max_http_header_data2 = HeaderBufferBytes; Info.pt_serv_buf_size = HeaderBufferBytes;
    Info.max_http_header_pool = 2; Info.fd_limit_per_thread = 3;
    Info.timeout_secs = static_cast<unsigned int>(MaxConnectTimeoutSeconds); Info.user = this;
    Certificates = &Ssl->GetCertificateManager();
    SslManager = &Ssl->GetSslManager();
    // No InitializeSsl, owned init reference, old LoadClientCertificates or
    // ordinary LWS global-init call. The qualified tracked TLS source owns CTX.
    VrLwsResult1 Actual {};
    if (!R.Control.CtorEntered) {
        NativeRunning.InC = true;
        const int32 Raw = varest_lws_create_context_tracked1(&Info, &R.View, Phase, &Actual);
        R.Control.FullCreateReturned = 1; // Literal WHOLE call return, NOT successful construction/closure.
        const auto Boundary = QualifyNativeContextReturn1(R.Binding, R.Control, Actual, Raw, nullptr);
        Context = static_cast<lws_context*>(Actual.RawObject); // Preserve actual live/unknown raw output; never dereference unknown.
        if (Boundary == ENativeContextReturn1::Freed || Boundary == ENativeContextReturn1::NoObject) {
            Context = nullptr; NativeRunning.Vhosts[0] = NativeRunning.Vhosts[1] = nullptr;
            NativeRunning.FinishStep = 4; // Actual no-C-object boundary; EXTERNAL ACKs remain. No invented destroy call/flag.
        }
        if (Boundary == ENativeContextReturn1::Unknown || Raw == VR_LWS_RELEASE_UNCERTAIN1 || Raw == VR_LWS_PROTOCOL_FAULT1)
            NativeRunning.FinishStep = 5; // Sticky uncertainty; no ctor/destroy re-entry through unknown aliases.
        NativeRunning.CreateDisposition = Raw;
        NativeRunning.InC = false;
        Counters.ContextCreations.fetch_add(1);
        if (Boundary != ENativeContextReturn1::Live || Raw != VR_LWS_KNOWN_SUCCESS1) {
            NativeCreating.InCreate = false;
            const VrStatus Finished = FinishNativeCleanupChildOnCurrentControl();
            return NativeCleanupDisposition1(Raw == VR_LWS_RELEASE_UNCERTAIN1 ? VR_STATUS_RELEASE_UNCERTAIN : VR_STATUS_NATIVE_ERROR, VR_STATUS_OK, Finished);
        }
    }
    if (!Context || !R.Control.FullCreateReturned) { NativeCreating.InCreate = false; return VR_STATUS_RELEASE_UNCERTAIN; }
    for (uint32 Slot = 0; Slot != 2; ++Slot) {
        if (NativeRunning.Vhosts[Slot]) continue;
        C2ResourcesC1::FLocalRouteSecret1 Secret {}; Secret.Size = sizeof(Secret); Secret.Version = 1;
        const auto Copied = C2ResourcesC1::CopyLocalRouteSecret1(R.Storage.Object, Slot, &Secret);
        if (Copied != C2ResourcesC1::EStatus1::OK || Secret.ProxyBytes != 80 || !Secret.PortHost || Secret.ProxyAddress[80]) {
            NativeCreating.InCreate = false;
            const VrStatus Finished = FinishNativeCleanupChildOnCurrentControl();
            return NativeCleanupDisposition1(VR_STATUS_NATIVE_ERROR, VR_STATUS_OK, Finished);
        }
        // Selected C successor must copy+validate all80 credential bytes before
        // this transient source object retires; NEVER use getenv/ordinary LWS.
        Info.http_proxy_address = Secret.ProxyAddress; Info.http_proxy_port = Secret.PortHost;
        Info.vhost_name = Slot ? "varest-native-1" : "varest-native-0";
        // Selected tracked C successor interprets this as owned TLS setup;
        // it MUST NOT execute ordinary LWS global init/fresh private indices.
        Info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
        NativeRunning.InC = true;
        const int32 Raw = varest_lws_create_vhost_tracked1(Context, &Info, &R.View, Phase, &Actual);
        NativeRunning.Vhosts[Slot] = static_cast<lws_vhost*>(Actual.RawObject);
        NativeRunning.CreateDisposition = Raw;
        NativeRunning.InC = false;
        volatile char* const Erase = Secret.ProxyAddress;
        for (uint32 I = 0; I < sizeof(Secret.ProxyAddress); ++I) Erase[I] = 0;
        if (Raw != VR_LWS_KNOWN_SUCCESS1 || Actual.Size != sizeof(Actual) || Actual.Version != 1
            || Actual.Disposition != static_cast<uint32>(Raw) || !NativeRunning.Vhosts[Slot] || bShuttingDown.load()) {
            NativeCreating.InCreate = false;
            const VrStatus Finished = FinishNativeCleanupChildOnCurrentControl();
            return NativeCleanupDisposition1(Raw == VR_LWS_RELEASE_UNCERTAIN1 ? VR_STATUS_RELEASE_UNCERTAIN : VR_STATUS_NATIVE_ERROR, VR_STATUS_OK, Finished);
        }
    }
    const VrStatus Child = CreateNativeChildInCreatingScopeOnCurrentControl(); // SAME InCreate through InitSync/factory return.
    if (Child == VR_STATUS_OK || NativeCreating.InCreate || NativeCreating.InCleanup) return Child;
    const VrStatus Finished = FinishNativeCleanupChildOnCurrentControl();
    return NativeCleanupDisposition1(Child, VR_STATUS_OK, Finished);
#else
    return VR_STATUS_DISCOVERY_UNESTABLISHED; // No private-C source selection/effects.
#endif
}

const C2ResourcesC1::FResourceBinding1* FService::NativeBinding() const
{ return NativeChildSnapshot ? NativeChildSnapshot->Resources : nullptr; }
C2ResourcesC1::FNative1* FService::NativeObject() const
{ return NativeChildSnapshot ? NativeChildSnapshot->Native : nullptr; }
const VrLwsCustody1* FService::NativeView() const
{ return NativeChildSnapshot ? NativeChildSnapshot->View : nullptr; }
const VrLwsTurnBorrow1* FService::NativeBorrow() const
{ return NativeRunning.Borrow; }
lws_vhost* FService::NativeVhost(uint32 Slot) const
{ return NativeChildSnapshot && Slot < 2 ? NativeChildSnapshot->Vhosts[Slot] : nullptr; }
VrStatus FService::SubmitNativeResolve(const FString& Host, uint16 Port, uint64 Deadline, VrTicket& Out)
{
    if (!NativeChildSnapshot || !NativeRunning.Borrow || !NativeRunning.NoProxyCaptured) return VR_STATUS_STALE;
    FTCHARToUTF8 Encoded(*Host);
    if (Encoded.Length() < 1 || Encoded.Length() > VR_MAX_HOSTNAME_BYTES) return VR_STATUS_INVALID_INPUT;
    VrResolve Request {}; Request.size = sizeof(Request); Request.version = VR_OWNER_VERSION;
    Request.deadline_ns = Deadline; Request.session = NativeChildSnapshot->Session;
    Request.hostname_bytes = Encoded.Length(); Request.port_be = static_cast<uint16>((Port << 8) | (Port >> 8));
    FMemory::Memcpy(Request.hostname, Encoded.Get(), Request.hostname_bytes);
    // One actual ABI2 call for this request generation, OUTSIDE named32.
    return NativeChildSnapshot->Values.Api.submit(NativeChildSnapshot->Values.Owner, &Request, &Out);
}
bool FService::NativeTicketMatches(VrTicket T) const
{
    return NativeChildSnapshot && T.owner_epoch == NativeChildSnapshot->Session.owner_epoch
        && T.service_epoch == NativeChildSnapshot->Session.service_epoch && T.slot_generation && T.slot < 2 && !T.reserved;
}
VrStatus FService::CopyNativeResolve(VrTicket Ticket, VrResult& Out)
{
    if (!NativeChildSnapshot || !NativeRunning.Borrow || Ticket.slot >= 2) return VR_STATUS_STALE;
    const uint8 Bit = static_cast<uint8>(1u << Ticket.slot);
    if (NativeRunning.CopiedSlots & Bit) return VR_STATUS_PENDING; // Local finite cursor, not a native BUSY observation.
    NativeRunning.CopiedSlots |= Bit;
    return NativeChildSnapshot->Values.Api.copy_result(NativeChildSnapshot->Values.Owner, Ticket, &Out);
}
VrStatus FService::DiscardNativeResolve(VrTicket Ticket)
{
    if (!NativeChildSnapshot || !NativeRunning.Borrow) return VR_STATUS_STALE;
    return NativeChildSnapshot->Values.Api.discard(NativeChildSnapshot->Values.Owner, Ticket);
}
void FService::DrainNativeCommands(uint32 Limit)
{
    FCommand Command;
    while (Limit && NativeRunning.CommandsLeft && Commands.Dequeue(Command)) {
        --Limit; --NativeRunning.CommandsLeft;
        Command(*this); Command.Reset(); Counters.CommandsRun.fetch_add(1);
    }
}
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
void FService::RunNativeService(const FNativeOwnerServiceChildSnapshot& Child)
{
    // Immutable child source values, never the creating journal/phase/sharedptr.
    uint64 GraceUntil = 0;
    while (!bStopping.load() && (Child.IsLocalNativeWorkPermitted()
        || (bShuttingDown.load() && Child.GetRegistrationDiagnostic() == VR_STATUS_OK))) {
        const auto Began = C2ResourcesC1::BeginTurn1(Child.Native, Child.Resources);
        if (Began != C2ResourcesC1::EStatus1::OK) { NativeRunning.RawRunStatus = static_cast<int32>(Began); break; }
        const auto Before = C2ResourcesC1::AdvanceBefore1(Child.Native, Child.Resources);
        VrLwsTurnBorrow1 Borrow {};
        const auto Borrowed = C2ResourcesC1::BorrowResourceTurn1(Child.Native, Child.Resources, &Borrow);
        NativeRunning.CommandsLeft = 16; NativeRunning.CopiedSlots = 0;
        if (Borrowed == C2ResourcesC1::EStatus1::OK) {
            NativeRunning.Borrow = &Borrow;
            DrainNativeCommands(16);
            // At most16 fair stage visits. Key-only selection does not walk
            // gaps in the historical ID space or retain a map iterator across
            // callbacks. Actual held-map bookkeeping is separate from named32.
            // No candidate/result/API array or second gas is introduced.
            for (uint32 Visit = 0; Visit < 16; ++Visit) {
                uint64 Next = 0, First = 0;
                for (const auto& Pair : Connections) {
                    if (!First || Pair.Key < First) First = Pair.Key;
                    if (Pair.Key > NativeRunning.VisitAfter && (!Next || Pair.Key < Next)) Next = Pair.Key;
                }
                if (!Next) Next = First;
                if (!Next) break;
                NativeRunning.VisitAfter = Next;
                if (FConn* Conn = FConn::Find(*this, Next)) {
                    Conn->AdvanceNative(*this);
                    Conn->CheckDeadlines(*this, FPlatformTime::Seconds());
                }
            }
            uint64_t Started = 0;
            if (C2ResourcesC1::BorrowedTurnTimestamp1(&Borrow, Child.Resources, &Started) == C2ResourcesC1::EStatus1::OK
                && bShuttingDown.load()) {
                if (!GraceUntil) {
                    const double Seconds = FMath::Clamp(Options.ExitFlushSeconds, 0.0, 0.25);
                    const uint64 Window = static_cast<uint64>(Seconds * 1000000000.0);
                    GraceUntil = Started > UINT64_MAX - Window ? Started : Started + Window;
                }
                if (Started >= GraceUntil || Counters.UnsentCloses.load() == 0) bStopping = true;
            }
            // A queued command is runnable. Held RX/payload/BUSY/pending-open
            // alone never selects poll0. Wake byte + bounded20ms cover races.
            const int32 Wait = Commands.IsEmpty() ? 20 : 0;
            VrLwsResult1 Actual {};
            NativeRunning.InC = true;
            const int32 Raw = varest_lws_service_tracked1(Child.Context, Wait, Child.View, &Borrow, &Actual);
            NativeRunning.InC = false;
            Child.ServiceDiagnostic.store(Raw, std::memory_order_release);
            Child.SourceFullReturnDiagnostic.store(1, std::memory_order_release);
            // WHOLE C return, then remaining SAME16 drain/recheck. A fatal
            // source result cuts work BEFORE any new command/ABI2 submission.
            const bool Fatal = Raw == VR_LWS_RELEASE_UNCERTAIN1 || Raw == VR_LWS_PROTOCOL_FAULT1
                || Actual.Size != sizeof(Actual) || Actual.Version != 1 || Actual.Disposition != static_cast<uint32>(Raw);
            if (Fatal) bStopping = true;
            else DrainNativeCommands(16);
            EraseDestroyed();
            NativeRunning.Borrow = nullptr;
        }
        // No stack borrow escapes to End/Stop/GT, including denial/error exits.
        NativeRunning.Borrow = nullptr;
        Borrow.Native = nullptr; Borrow.Generation = 0;
        const auto After = C2ResourcesC1::AdvanceAfter1(Child.Native, Child.Resources);
        const auto Ended = C2ResourcesC1::EndTurn1(Child.Native, Child.Resources);
        NativeRunning.RawRunStatus = static_cast<int32>(Ended);
        if (Before == C2ResourcesC1::EStatus1::RETAINED_POISON || After == C2ResourcesC1::EStatus1::RETAINED_POISON
            || Ended != C2ResourcesC1::EStatus1::OK) break;
    }
    bStopping = true;
    { FScopeLock Guard(&EnqueueLock); bAccepting = false; }
}

#endif

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
[[gnu::used]] VrStatus FService::CreateNativeCleanupChildOnCurrentControl()
{
    const VrStatus Role = RequireNativeOwnerControlThread(TEXT("actual native cleanup-child creation"));
    if (Role != VR_STATUS_OK) return Role;
    if (!NativeOwnerRealThreadModeAllowed()) return VR_STATUS_DISCOVERY_UNESTABLISHED;
    if (NativeCreating.Phase != ENativeEntryPhase1::Attached || NativeCreating.InEntry || NativeCreating.InCleanup || NativeCreating.InCreate
        || !NativeCreating.Adapter.IsValid() || !NativeCreating.Resources || Thread || NativeChildSnapshot || bAccepting.load())
        return VR_STATUS_INVALID_INPUT;
    NativeCreating.InCreate = true;
    return CreateNativeChildInCreatingScopeOnCurrentControl();
}
VrStatus FService::CreateNativeChildInCreatingScopeOnCurrentControl()
{
    if (!NativeCreating.InCreate || !NativeCreating.Adapter.IsValid() || !NativeCreating.Resources) return VR_STATUS_INVALID_INPUT;
    const TSharedRef<FNativeOwnerServiceAdapter, ESPMode::ThreadSafe> KeepAlive = NativeCreating.Adapter.ToSharedRef();
    VrLwsPhaseBudget1* Phase = KeepAlive->GetCreatingPhaseOnControlThread();
    if (!Phase || Phase->Phase != VR_LWS_PHASE_STARTUP1) { NativeCreating.InCreate = false; return VR_STATUS_INVALID_INPUT; } // NEVER late-init after C1.
    // Genuine FULL successful C1/tracked preparation return, same code/mapping
    // and resource lifetime are independent CALLSITE premises of this dormant edge.
    const auto* Binding = NativeCreating.Resources->GetBinding1();
    auto* Storage = NativeCreating.Resources->GetStorage1();
    if (!Storage->Object || Storage->Raw != Storage->Object || Storage->State != 2
        || !C2ResourcesC1::ResourceBindingValid1(Binding) || !Binding->Journal->Header.CreateEntered) {
        NativeCreating.InCreate = false; return VR_STATUS_INVALID_INPUT;
    }
    if (bShuttingDown.load() || Options.bFailThreadForTest)
    {
        // Labeled source skip BEFORE preparation/factory; never an observed OS failure.
        const VrStatus Aborted = KeepAlive->AbortStartupBeforeThreadCreate();
        NativeCreating.Phase = ENativeEntryPhase1::NoCreatePending;
        NativeCreating.InCreate = false;
        return NativeCleanupDisposition1(Aborted, VR_STATUS_OK, VR_STATUS_PENDING);
    }
    NativeCreating.InCreate = true; // Includes fallible single-snapshot allocation and factory's InitSync wait.
    const FNativeOwnerServiceChildSnapshot* Child = KeepAlive->PrepareRealThreadCreate(Storage->Object, Binding, Context,
        &NativeCreating.Resources->View, NativeRunning.Vhosts[0], NativeRunning.Vhosts[1]);
    if (!Child)
    {
        NativeCreating.InCreate = false;
        const VrStatus Aborted = KeepAlive->AbortStartupBeforeThreadCreate();
        NativeCreating.Phase = ENativeEntryPhase1::NoCreatePending;
        return NativeCleanupDisposition1(Aborted, VR_STATUS_OK, VR_STATUS_PENDING);
    }
    NativeChildSnapshot = Child; // Stable immutable publication BEFORE the ONLY native factory call.
    bStopping = false;
    // Exact selected real-factory/no-fork/always-TRUE Init theorem is CLOSED
    // until qualified; GetThreadType later is merely consistency, not permission.
    Thread = FForkProcessHelper::CreateForkableThread(this, *Options.ThreadName, 128 * 1024, TPri_BelowNormal, FPlatformAffinity::GetNoAffinityMask());
    if (!Thread)
    {
        NativeChildSnapshot = nullptr; // Genuine selected NULL means no created child under that theorem.
        const VrStatus NoChild = KeepAlive->ObserveKnownNoChildAfterCreateFailure();
        NativeCreating.Phase = ENativeEntryPhase1::NoCreatePending;
        NativeCreating.InCreate = false;
        return NativeCleanupDisposition1(NoChild, VR_STATUS_OK, VR_STATUS_PENDING);
    }
    const VrStatus Created = KeepAlive->ObserveRealThreadCreated(*Thread); // Journal before diagnostics/fallible work.
    if (Created != VR_STATUS_OK)
    {
        NativeCreating.Phase = ENativeEntryPhase1::Retained;
        NativeCreating.InCreate = false;
        return Created; // Wrapper/source/factory uncertainty: no second Create or guessed first join.
    }
    NativeCreating.Phase = ENativeEntryPhase1::ChildCreated;
    const VrStatus Registered = Child->GetRegistrationDiagnostic(); // Existing InitSync precedes this acquire load.
    NativeCreating.InCreate = false;
    if (Registered == VR_STATUS_OK && Child->Context && Child->View && Child->Vhosts[0] && Child->Vhosts[1]
        && Child->IsLocalNativeWorkPermitted() && !bShuttingDown.load() && !bStopping.load()) {
        FScopeLock Guard(&EnqueueLock);
        if (!bShuttingDown.load() && !bStopping.load()) { bAccepting = true; return VR_STATUS_OK; }
    }
    const VrStatus Finished = FinishNativeCleanupChildOnCurrentControl();
    return NativeCleanupDisposition1(Registered, VR_STATUS_OK, Finished);
}

#else
[[gnu::used]] VrStatus FService::CreateNativeCleanupChildOnCurrentControl()
{
    const VrStatus Role = RequireNativeOwnerControlThread(TEXT("actual native cleanup-child creation"));
    if (Role != VR_STATUS_OK) return Role;
    if (!NativeOwnerRealThreadModeAllowed()) return VR_STATUS_DISCOVERY_UNESTABLISHED;
    if (NativeCreating.Phase != ENativeEntryPhase1::Attached || NativeCreating.InEntry || NativeCreating.InCleanup || NativeCreating.InCreate
        || !NativeCreating.Adapter.IsValid() || !NativeCreating.Resources || Thread || NativeChildSnapshot || bAccepting.load())
        return VR_STATUS_INVALID_INPUT;
    const TSharedRef<FNativeOwnerServiceAdapter, ESPMode::ThreadSafe> KeepAlive = NativeCreating.Adapter.ToSharedRef();
    VrLwsPhaseBudget1* Phase = KeepAlive->GetCreatingPhaseOnControlThread();
    if (!Phase || Phase->Phase != VR_LWS_PHASE_STARTUP1) return VR_STATUS_INVALID_INPUT; // NEVER late-init after C1.
    // Genuine FULL successful C1/tracked preparation return, same code/mapping
    // and resource lifetime are independent CALLSITE premises of this dormant edge.
    const auto* Binding = NativeCreating.Resources->GetBinding1();
    auto* Storage = NativeCreating.Resources->GetStorage1();
    if (!Storage->Object || Storage->Raw != Storage->Object || Storage->State != 2
        || !C2ResourcesC1::ResourceBindingValid1(Binding) || !Binding->Journal->Header.CreateEntered) return VR_STATUS_INVALID_INPUT;
    if (bShuttingDown.load() || Options.bFailThreadForTest)
    {
        // Labeled source skip BEFORE preparation/factory; never an observed OS failure.
        const VrStatus Aborted = KeepAlive->AbortStartupBeforeThreadCreate();
        NativeCreating.Phase = ENativeEntryPhase1::NoCreatePending;
        return NativeCleanupDisposition1(Aborted, VR_STATUS_OK, VR_STATUS_PENDING);
    }
    NativeCreating.InCreate = true; // Includes fallible single-snapshot allocation and factory's InitSync wait.
    const FNativeOwnerServiceChildSnapshot* Child = KeepAlive->PrepareRealThreadCreate(Storage->Object, Binding);
    if (!Child)
    {
        NativeCreating.InCreate = false;
        const VrStatus Aborted = KeepAlive->AbortStartupBeforeThreadCreate();
        NativeCreating.Phase = ENativeEntryPhase1::NoCreatePending;
        return NativeCleanupDisposition1(Aborted, VR_STATUS_OK, VR_STATUS_PENDING);
    }
    NativeChildSnapshot = Child; // Stable immutable publication BEFORE the ONLY native factory call.
    bStopping = false;
    // Exact selected real-factory/no-fork/always-TRUE Init theorem is CLOSED
    // until qualified; GetThreadType later is merely consistency, not permission.
    Thread = FForkProcessHelper::CreateForkableThread(this, *Options.ThreadName, 128 * 1024, TPri_BelowNormal, FPlatformAffinity::GetNoAffinityMask());
    if (!Thread)
    {
        NativeChildSnapshot = nullptr; // Genuine selected NULL means no created child under that theorem.
        const VrStatus NoChild = KeepAlive->ObserveKnownNoChildAfterCreateFailure();
        NativeCreating.Phase = ENativeEntryPhase1::NoCreatePending;
        NativeCreating.InCreate = false;
        return NativeCleanupDisposition1(NoChild, VR_STATUS_OK, VR_STATUS_PENDING);
    }
    const VrStatus Created = KeepAlive->ObserveRealThreadCreated(*Thread); // Journal before diagnostics/fallible work.
    if (Created != VR_STATUS_OK)
    {
        NativeCreating.Phase = ENativeEntryPhase1::Retained;
        NativeCreating.InCreate = false;
        return Created; // Wrapper/source/factory uncertainty: no second Create or guessed first join.
    }
    NativeCreating.Phase = ENativeEntryPhase1::ChildCreated;
    const VrStatus Registered = Child->GetRegistrationDiagnostic(); // Existing InitSync precedes this acquire load.
    NativeCreating.InCreate = false;
    // This component creates a CLEANUP child, never a usable native service.
    // EVERY real nonnull outcome (including refusal/already-exited Run) takes
    // actual first-join custody. No bAccepting publication or legacy LWS fallback.
    const VrStatus Finished = FinishNativeCleanupChildOnCurrentControl();
    return NativeCleanupDisposition1(Registered, VR_STATUS_OK, Finished);
}

#endif

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
[[gnu::used]] VrStatus FService::FinishNativeCleanupChildOnCurrentControl()
{
    const VrStatus Role = RequireNativeOwnerControlThread(TEXT("native actual first join/final closure"));
    if (Role != VR_STATUS_OK) return Role;
    bShuttingDown = true;
    if (NativeCreating.InEntry || NativeCreating.InCreate || NativeCreating.InCleanup) return VR_STATUS_BUSY;
    if (!NativeCreating.Adapter.IsValid()) return FinishNativeOwnerNoCreateOnCurrentControl();
    if (NativeRunning.FinishStep == 5) return VR_STATUS_RELEASE_UNCERTAIN; // Sticky actual malformed/uncertain C return; NEVER re-enter it.
    if (NativeRunning.FinishStep == 3) {
        // Previous actual no-child/join + WHOLE C/C1/metadata deletion completed.
        // Only existing callback/pump/value retirement may remain; no rejoin or
        // stale resource/snapshot read is allowed on this continuation.
        NativeCreating.InCleanup = true;
        const auto Held = NativeCreating.Adapter.ToSharedRef();
        const VrStatus Stopped = Held->StopBeforeCreatingJoin();
        const VrStatus Detached = Held->AfterKnownCreatingResourcesClosed();
        if (Held->CanReleaseCreatingAdapter()) { NativeCreating.Adapter.Reset(); NativeCreating.Phase = ENativeEntryPhase1::Finished; }
        NativeCreating.InCleanup = false;
        return NativeCleanupDisposition1(Stopped, Held->LastOutcomeStatus, Detached);
    }
    if (!Thread && !NativeChildSnapshot && NativeEntryResourcesAreMetadataOnly()) return FinishNativeOwnerNoCreateOnCurrentControl();
    NativeCreating.InCleanup = true;
    const auto KeepAlive = NativeCreating.Adapter.ToSharedRef();
    // This genuine queue marker is admitted BEFORE cutoff; no marker is
    // invented after admission closes. Timed grace is never a join/close proof.
    const auto Reached = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
    const bool Grace = Thread && bAccepting.load() && !bStopping.load() && Options.ExitFlushSeconds > 0
        && Enqueue([Reached](FService&) { Reached->store(true); });
    {
        FScopeLock Guard(&EnqueueLock);
        bAccepting = false;
        // Taking this SAME lock fences WHOLE preceding synchronous C cancels.
        // RELEASE BEFORE any tracked destroy/protocol/Conn/native callback.
    }
    const VrStatus Stopped = KeepAlive->StopBeforeCreatingJoin();
    if (Grace) {
        const double Until = FPlatformTime::Seconds() + FMath::Clamp(Options.ExitFlushSeconds, 0.0, 0.25);
        while (FPlatformTime::Seconds() < Until && (!Reached->load() || Counters.UnsentCloses.load() > 0))
            FPlatformProcess::SleepNoStats(0.001f); // Bounded grace only; NEVER retirement evidence.
    }
    using EOutcome = FNativeOwnerServiceAdapter::ECreatingOutcome;
    VrStatus Recorded = VR_STATUS_OK;
    if (KeepAlive->Outcome == EOutcome::CreatedJoinUnobserved) {
        FRunnableThread* const Joining = KeepAlive->PrepareFirstCreatingJoin();
        if (!Joining || Joining != Thread || !NativeChildSnapshot || KeepAlive->Child.Get() != NativeChildSnapshot) {
            NativeCreating.Phase = ENativeEntryPhase1::Retained; NativeCreating.InCleanup = false;
            return NativeCleanupDisposition1(Stopped, VR_STATUS_OK, VR_STATUS_RELEASE_UNCERTAIN);
        }
        const bool Joined = Joining->Kill(true); // FIRST and ONLY actual observation, captured immediately.
        if (!Joined) {
            KeepAlive->ObserveUncertainFirstJoin();
            NativeCreating.Phase = ENativeEntryPhase1::Retained; NativeCreating.InCleanup = false;
            return NativeCleanupDisposition1(Stopped, VR_STATUS_OK, VR_STATUS_RELEASE_UNCERTAIN);
        }
        Recorded = KeepAlive->ObserveKnownFirstSuccessfulJoin(); // Genuine fact BEFORE native CAS.
        Thread = nullptr; delete Joining;
        NativeCreating.Phase = ENativeEntryPhase1::JoinedPending;
    } else if (KeepAlive->Outcome == EOutcome::JoinedPending || KeepAlive->Outcome == EOutcome::JoinedRecorded
        || KeepAlive->Outcome == EOutcome::NeverCreatedPending || KeepAlive->Outcome == EOutcome::NeverCreatedRecorded) {
        Recorded = KeepAlive->LastOutcomeStatus;
        if (Recorded == VR_STATUS_BUSY) Recorded = KeepAlive->RetryPendingOutcomeOnControlThread(); // Bookkeeping ONLY.
    } else if (KeepAlive->Outcome == EOutcome::StartReserved && !KeepAlive->CreatePrepared && !Thread && !NativeChildSnapshot) {
        Recorded = KeepAlive->AbortStartupBeforeThreadCreate();
        NativeCreating.Phase = ENativeEntryPhase1::NoCreatePending;
    } else {
        NativeCreating.Phase = ENativeEntryPhase1::Retained; NativeCreating.InCleanup = false;
        return NativeCleanupDisposition1(Stopped, VR_STATUS_OK, VR_STATUS_RELEASE_UNCERTAIN);
    }
    const bool NoChild = KeepAlive->Outcome == EOutcome::NeverCreatedPending || KeepAlive->Outcome == EOutcome::NeverCreatedRecorded;
    if ((!NoChild && !KeepAlive->JoinFactDurable) || !NativeCreating.Resources
        || (!NoChild && (!NativeChildSnapshot || NativeRunning.Borrow || NativeRunning.InC))) {
        NativeCreating.InCleanup = false; return NativeCleanupDisposition1(Stopped, Recorded, VR_STATUS_RELEASE_UNCERTAIN);
    }
    if (!NoChild) {
        const auto Entry = static_cast<C2ResourcesC1::EStatus1>(NativeChildSnapshot->GetC1EntryDiagnostic());
        const auto Cleaned = static_cast<C2ResourcesC1::EStatus1>(NativeChildSnapshot->GetC1CleanupDiagnostic());
        if ((Entry != C2ResourcesC1::EStatus1::OK && Entry != C2ResourcesC1::EStatus1::STOPPING)
            || Cleaned != C2ResourcesC1::EStatus1::OK) {
            NativeCreating.Phase = ENativeEntryPhase1::Retained; NativeCreating.InCleanup = false;
            return NativeCleanupDisposition1(Stopped, Recorded, VR_STATUS_RELEASE_UNCERTAIN);
        }
    }
    // Only now may GT inspect the journal/Conn/turn diagnostics. An uncertain
    // ABI2 output remains retained; never guess discard or manufacture a ticket.
    for (const auto& Pair : Connections) {
        if (static_cast<const FConn*>(Pair.Value.Get())->NativeInterestUncertain()) {
            NativeCreating.InCleanup = false; return NativeCleanupDisposition1(Stopped, Recorded, VR_STATUS_RELEASE_UNCERTAIN);
        }
    }
    // Drop, NEVER execute, commands after join: they have no actual Between.
    // Captures/destructors retire outside EnqueueLock; pending stays retained.
    FCommand Dropped;
    for (uint32 K = 0; K != 16 && Commands.Dequeue(Dropped); ++K) Dropped.Reset();
    if (!Commands.IsEmpty()) { NativeCreating.InCleanup = false; return NativeCleanupDisposition1(Stopped, Recorded, VR_STATUS_PENDING); }
    auto& R = *NativeCreating.Resources;
    VrLwsPhaseBudget1* Phase = KeepAlive->GetCreatingPhaseOnControlThread();
    if (!Phase || Phase->Phase != VR_LWS_PHASE_FINAL1 || __atomic_load_n(&R.Control.CancelUsers, __ATOMIC_ACQUIRE)) {
        NativeCreating.InCleanup = false; return NativeCleanupDisposition1(Stopped, Recorded, VR_STATUS_RELEASE_UNCERTAIN);
    }
    if (NativeRunning.FinishStep == 0) {
        if (Context) {
            FNativeContextSelf1 Original;
            if (!CaptureNativeContextSelf1(R.Binding, R.Control, Context, Original)) {
                NativeRunning.FinishStep = 5; NativeCreating.InCleanup = false;
                return NativeCleanupDisposition1(Stopped, Recorded, VR_STATUS_RELEASE_UNCERTAIN);
            }
            VrLwsResult1 Actual {};
            NativeRunning.InC = true;
            const int32 Raw = varest_lws_destroy_context_tracked1(Context, &R.View, Phase, &Actual);
            R.Control.FullDestroyReturned = 1; // Literal WHOLE explicit call return; ctor unwind NEVER writes this.
            const auto Boundary = QualifyNativeContextReturn1(R.Binding, R.Control, Actual, Raw, &Original);
            if (Boundary == ENativeContextReturn1::Freed) {
                // Clear DEAD aliases before any diagnostic, callback, ACK or
                // other fallible/reentrant work. Never re-enter C on this pointer.
                Context = nullptr; NativeRunning.Vhosts[0] = NativeRunning.Vhosts[1] = nullptr;
                NativeRunning.FinishStep = 4;
            }
            NativeRunning.DestroyDisposition = Raw;
            NativeRunning.InC = false;
            const bool ContinueLive = Boundary == ENativeContextReturn1::Live
                && (Raw == VR_LWS_DEFERRED_BEFORE_ACQUIRE1 || Raw == VR_LWS_FAILED_WITH_OUTSTANDING1);
            if (Boundary != ENativeContextReturn1::Freed) {
                if (!ContinueLive) NativeRunning.FinishStep = 5;
                NativeCreating.InCleanup = false;
                return NativeCleanupDisposition1(Stopped, Recorded, ContinueLive ? VR_STATUS_PENDING : VR_STATUS_RELEASE_UNCERTAIN);
            }
            if (Raw == VR_LWS_RELEASE_UNCERTAIN1 || Raw == VR_LWS_PROTOCOL_FAULT1 || R.Control.Fault) {
                NativeRunning.FinishStep = 5; NativeCreating.InCleanup = false;
                return NativeCleanupDisposition1(Stopped, Recorded, VR_STATUS_RELEASE_UNCERTAIN);
            }
        } else if (!R.Control.CtorEntered && !R.Control.FullCreateReturned && !R.Control.RawContext
            && !R.Control.ContextSourceSerial && !R.Control.ContextPhase && !R.Control.FullDestroyReturned
            && !R.Control.CallbackDepth && !R.Control.CancelUsers && !NativeRunning.Vhosts[0] && !NativeRunning.Vhosts[1]) {
            // Actual C1-only/no-C-call program path. No returned constructor or
            // destroy is invented; later external rows still have to reconcile.
            NativeRunning.FinishStep = 4;
        } else {
            NativeRunning.FinishStep = 5; NativeCreating.InCleanup = false;
            return NativeCleanupDisposition1(Stopped, Recorded, VR_STATUS_RELEASE_UNCERTAIN);
        }
    }
    if (NativeRunning.FinishStep == 4) {
        // Actual physical C boundary already returned (explicit or ctor unwind),
        // or the source never called C. No C object is touched on continuation.
        const auto External = R.ReconcileAfterC1ReturnOnCreatingThread(Phase);
        if (R.Control.Fault || External.Fault || External.PendingRows) {
            NativeCreating.InCleanup = false;
            return NativeCleanupDisposition1(Stopped, Recorded, R.Control.Fault || External.Fault ? VR_STATUS_RELEASE_UNCERTAIN : VR_STATUS_PENDING);
        }
        for (uint32 I = 140; I-- > 12;) {
            const auto& Cell = R.Journal.Cells[I];
            if (Cell.LocalState != C2ResourcesC1::ResourceClosedAck1 && Cell.LocalState != C2ResourcesC1::ResourceNotObtainedAck1) continue;
            const VrLwsRowRef1 Ref {I,0,Cell.SourceSerial}; VrLwsHookOutcome1 Actual {};
            varest_ws_c2l_retire_source_row1(&R.View, &Ref, &Actual); // All real C stack/physical/callback/child refs already retired.
        }
        for (uint32 I = 12; I < 140; ++I) {
            if (R.Journal.Cells[I].LocalState != C2ResourcesC1::ResourceEmpty1) {
                NativeCreating.InCleanup = false;
                return NativeCleanupDisposition1(Stopped, Recorded, VR_STATUS_PENDING);
            }
        }
        NativeRunning.FinishStep = 1; // ONLY now may C1 destruction begin; gas/serials never reset.
    }
    if (NativeRunning.FinishStep == 1) {
        if (R.Storage.Object) {
            if (NoChild) {
                const auto Closed = C2ResourcesC1::StopNeverStartedPairs1(R.Storage.Object, &R.Binding);
                if (Closed != C2ResourcesC1::EStatus1::OK) {
                    NativeCreating.InCleanup = false; return NativeCleanupDisposition1(Stopped, Recorded, VR_STATUS_RELEASE_UNCERTAIN);
                }
            }
            const auto Destroyed = C2ResourcesC1::DestroyAfterContext1(&R.Storage, &R.Binding);
            NativeRunning.RawRunStatus = static_cast<int32>(Destroyed); // ONLY after WHOLE Create/Destroy return.
            if (Destroyed != C2ResourcesC1::EStatus1::OK) {
                NativeCreating.InCleanup = false;
                return NativeCleanupDisposition1(Stopped, Recorded, Destroyed == C2ResourcesC1::EStatus1::BUSY ? VR_STATUS_PENDING : VR_STATUS_RELEASE_UNCERTAIN);
            }
        }
        if (R.Storage.Raw || R.Storage.Object || R.Storage.MappingBytes || R.Storage.State) {
            NativeCreating.InCleanup = false; return NativeCleanupDisposition1(Stopped, Recorded, VR_STATUS_RELEASE_UNCERTAIN);
        }
        NativeRunning.FinishStep = 2;
    }
    const auto Progress = R.ReconcileAfterC1ReturnOnCreatingThread(Phase);
    if (R.Control.Fault || Progress.Fault || Progress.PendingRows) {
        NativeCreating.InCleanup = false; return NativeCleanupDisposition1(Stopped, Recorded, R.Control.Fault || Progress.Fault ? VR_STATUS_RELEASE_UNCERTAIN : VR_STATUS_PENDING);
    }
    // Whole context/no-object return + actual no-child/join retired all physical
    // consumers. A terminal ACK alone never grants this source-row retirement.
    for (uint32 I = 140; I-- > 12;) {
        const auto& Cell = R.Journal.Cells[I];
        if (Cell.LocalState != C2ResourcesC1::ResourceClosedAck1 && Cell.LocalState != C2ResourcesC1::ResourceNotObtainedAck1) continue;
        const VrLwsRowRef1 Ref {I,0,Cell.SourceSerial}; VrLwsHookOutcome1 Actual {};
        varest_ws_c2l_retire_source_row1(&R.View, &Ref, &Actual);
    }
    for (uint32 I = 0; I < 140; ++I) {
        const uint32 State = R.Journal.Cells[I].LocalState;
        if (I < 12 ? (State != C2ResourcesC1::ResourceEmpty1 && State != C2ResourcesC1::ResourceClosedAck1 && State != C2ResourcesC1::ResourceNotObtainedAck1)
            : State != C2ResourcesC1::ResourceEmpty1) {
            NativeCreating.InCleanup = false; return NativeCleanupDisposition1(Stopped, Recorded, VR_STATUS_PENDING);
        }
    }
    for (auto& Pair : Connections) Pair.Value->EndForShutdown();
    Connections.Empty(); Live.Empty(); Counters.LiveConnections = 0;
    // Delete WHOLE9248 metadata only AFTER full C/C1 returns and every receipt.
    FNativeOwnerResources1* Metadata = NativeCreating.Resources;
    NativeCreating.Resources = nullptr; delete Metadata;
    NativeChildSnapshot = nullptr; // Joined snapshot's final users have retired.
    Certificates = nullptr; SslManager = nullptr; // Borrowed aliases only, no unowned provider release.
    NativeRunning.FinishStep = 3; // Actual complete resource/metadata return, not a caller supplied permission.
    const VrStatus Detached = KeepAlive->AfterKnownCreatingResourcesClosed();
    if (KeepAlive->CanReleaseCreatingAdapter()) {
        NativeCreating.Adapter.Reset(); NativeCreating.Phase = ENativeEntryPhase1::Finished;
    }
    NativeCreating.InCleanup = false;
    return NativeCleanupDisposition1(Stopped, Recorded, Detached);
}

#else
[[gnu::used]] VrStatus FService::FinishNativeCleanupChildOnCurrentControl()
{
    const VrStatus Role = RequireNativeOwnerControlThread(TEXT("native cleanup-child stop/first join"));
    if (Role != VR_STATUS_OK) return Role;
    bShuttingDown = true;
    if (NativeCreating.InEntry || NativeCreating.InCreate || NativeCreating.InCleanup) return VR_STATUS_BUSY;
    if (!NativeCreating.Adapter.IsValid()) return FinishNativeOwnerNoCreateOnCurrentControl();
    // The original I3-A metadata-only lane still has genuine pre-create cleanup.
    if (!Thread && !NativeChildSnapshot && NativeEntryResourcesAreMetadataOnly()) return FinishNativeOwnerNoCreateOnCurrentControl();
    NativeCreating.InCleanup = true;
    const TSharedRef<FNativeOwnerServiceAdapter, ESPMode::ThreadSafe> KeepAlive = NativeCreating.Adapter.ToSharedRef();
    const VrStatus Stopped = KeepAlive->StopBeforeCreatingJoin(); // Actual StopEpoch/pump disable-removal before join.
    {
        FScopeLock Guard(&EnqueueLock);
        bAccepting = false;
    }
    using EOutcome = FNativeOwnerServiceAdapter::ECreatingOutcome;
    VrStatus Recorded = VR_STATUS_OK;
    if (KeepAlive->Outcome == EOutcome::CreatedJoinUnobserved)
    {
        FRunnableThread* const Joining = KeepAlive->PrepareFirstCreatingJoin();
        if (!Joining || Joining != Thread || !NativeChildSnapshot || KeepAlive->Child.Get() != NativeChildSnapshot)
        {
            NativeCreating.Phase = ENativeEntryPhase1::Retained; NativeCreating.InCleanup = false;
            return NativeCleanupDisposition1(Stopped, VR_STATUS_OK, VR_STATUS_RELEASE_UNCERTAIN);
        }
        const bool Joined = Joining->Kill(true); // FIRST and ONLY actual join observation; capture immediately.
        if (!Joined)
        {
            KeepAlive->ObserveUncertainFirstJoin();
            NativeCreating.Phase = ENativeEntryPhase1::Retained; NativeCreating.InCleanup = false;
            return NativeCleanupDisposition1(Stopped, VR_STATUS_OK, VR_STATUS_RELEASE_UNCERTAIN);
        }
        // TRUE is genuine whole-thread retirement only under the exact primary
        // no-competing-join theorem. Publish S3's fact BEFORE any GT resource hook.
        Recorded = KeepAlive->ObserveKnownFirstSuccessfulJoin();
        Thread = nullptr;
        delete Joining; // Safe ONLY after actual first true; never a second join instruction.
        NativeCreating.Phase = ENativeEntryPhase1::JoinedPending;
        // Keep stable snapshot through later native Stop/diagnostic users until
        // actual final resource retirement; absence will never select legacy cleanup.
    }
    else if (KeepAlive->Outcome == EOutcome::JoinedPending || KeepAlive->Outcome == EOutcome::JoinedRecorded)
    {
        Recorded = KeepAlive->LastOutcomeStatus;
        if (Recorded == VR_STATUS_BUSY) Recorded = KeepAlive->RetryPendingOutcomeOnControlThread();
    }
    else if (KeepAlive->Outcome == EOutcome::StartReserved && !KeepAlive->CreatePrepared && !Thread && !NativeChildSnapshot)
    {
        Recorded = KeepAlive->AbortStartupBeforeThreadCreate();
        NativeCreating.Phase = ENativeEntryPhase1::NoCreatePending;
    }
    else if (KeepAlive->Outcome == EOutcome::NeverCreatedPending || KeepAlive->Outcome == EOutcome::NeverCreatedRecorded)
    {
        Recorded = KeepAlive->LastOutcomeStatus;
        if (Recorded == VR_STATUS_BUSY) Recorded = KeepAlive->RetryPendingOutcomeOnControlThread();
    }
    else
    {
        // In particular, uncertain factory/first join can never reach delete,
        // NoChild, another join, resource readback or legacy context teardown.
        NativeCreating.Phase = ENativeEntryPhase1::Retained; NativeCreating.InCleanup = false;
        return NativeCleanupDisposition1(Stopped, VR_STATUS_OK, VR_STATUS_RELEASE_UNCERTAIN);
    }
    VrStatus Progress = VR_STATUS_PENDING;
    if (KeepAlive->JoinFactDurable && !NativeChildSnapshot)
    {
        NativeCreating.Phase = ENativeEntryPhase1::Retained;
        Progress = VR_STATUS_RELEASE_UNCERTAIN;
    }
    else if (KeepAlive->JoinFactDurable && NativeChildSnapshot)
    {
        // ONLY after genuine joined Run has retired exclusive native journal access.
        const auto Entry = static_cast<C2ResourcesC1::EStatus1>(NativeChildSnapshot->GetC1EntryDiagnostic());
        const auto Cleanup = static_cast<C2ResourcesC1::EStatus1>(NativeChildSnapshot->GetC1CleanupDiagnostic());
        if ((Entry != C2ResourcesC1::EStatus1::OK && Entry != C2ResourcesC1::EStatus1::STOPPING)
            || Cleanup != C2ResourcesC1::EStatus1::OK)
        {
            NativeCreating.Phase = ENativeEntryPhase1::Retained;
            Progress = VR_STATUS_RELEASE_UNCERTAIN; // Derived retained SOURCE duty; raw entry/cleanup/native diagnostics unchanged.
        }
        else if (NativeCreating.Resources)
        {
            VrLwsPhaseBudget1* Phase = KeepAlive->GetCreatingPhaseOnControlThread();
            if (!Phase || Phase->Phase != VR_LWS_PHASE_FINAL1) Progress = VR_STATUS_RELEASE_UNCERTAIN;
            else
            {
                const auto Actual = NativeCreating.Resources->ReconcileAfterC1ReturnOnCreatingThread(Phase);
                if (Actual.Fault) Progress = VR_STATUS_RELEASE_UNCERTAIN;
            }
        }
    }
    // Real L4 tracked context/SSL/callback/physical-return closure is absent.
    // NEVER call AfterKnownCreatingResourcesClosed or any legacy destructor
    // merely because pointers/counters/this progress happen to be zero.
    NativeCreating.InCleanup = false;
    return NativeCleanupDisposition1(Stopped, Recorded, Progress);
}

#endif

[[gnu::used]] VrStatus FService::FinishNativeOwnerEntryDisposition(FCallerEntryTransaction& Transaction, VrStatus Status)
{
	if (!NativeCreating.InEntry) return VR_STATUS_INVALID_INPUT;
	NativeCreating.AdmissionStatus = Transaction.NativeStatus;
	// No new Gate/lookup/readiness argument. The actual initiating frame alone
	// may make its already-owned allowance permanent after an observed19.
	if (NativeCreating.Phase == ENativeEntryPhase1::Attached && Status == VR_STATUS_OK && !bShuttingDown.load())
		return VR_STATUS_OK;
	const VrStatus Finished = FinishNativeOwnerNoCreateOnCurrentControl();
	if (Finished != VR_STATUS_OK) return Finished;
	return Status == VR_STATUS_OK && bShuttingDown.load() ? VR_STATUS_STALE : VR_STATUS_OK;
}
}
#endif

VaRest::WebSocket::Lws::FService::FService(FServiceOptions InOptions)
	: Options(MoveTemp(InOptions))
{
}

VaRest::WebSocket::Lws::FService::~FService()
{
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
	// Actual owner MUST retain service and WS while this duty is live. Destruction
	// is not a native finish/retry/ACK route or code-lifetime repair.
	if (NativeCreating.Adapter.IsValid() || NativeCreating.Resources || NativeCreating.InEntry || NativeCreating.InCleanup)
		UE_LOG(LogVaRestSocket, Error, TEXT("Native entry service destroyed before its creating journal retired; no completion is claimed"));
#endif
	// The owner shuts a running service down on the game thread first (FTransport::Shutdown); this is the backstop
	if (Thread || Context)
	{
		check(IsInGameThread());
		Shutdown();
	}
}

bool VaRest::WebSocket::Lws::FService::Start()
{
	check(IsInGameThread());
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
	// Ordinary/default Start never selects the dormant tracked private lane.
	if (NativeCreating.Phase != ENativeEntryPhase1::Fresh) return false;
#endif
	if (Context)
	{
		return true;
	}
	if (bShuttingDown.load() || bStartFailed)
	{
		// Review conc I-2: never a second lws_create_context after a failed start -- in a shared LWS copy a retry could run
		// while an engine wss handshake is in flight (VaRest-rti, ruling Q2)
		return false;
	}
	if (const int32 InFlight = TlsHandshakesInFlight().load(); InFlight != 0)
	{
		// Review conc I-3: the creation would re-allocate the ex-data index those handshakes' verify callbacks read
		// [coexist.context_create_reallocates_ssl_index]. Not latched: nothing was created, and the next Start may run
		UE_LOG(LogVaRestSocket, Error, TEXT("libwebsockets: no context is made while %d VaRest wss handshake(s) are in flight (VaRest-rti); WebSocket connections end with Shutdown"), InFlight);
		return false;
	}
	LLM_SCOPE_BYTAG(VaRest_WebSocket);
	// Plan decision 16 as amended (review M-1): the D30 rule runs at the first start that may apply it
	ApplyLogLevel();
	// One protocol, "varest" (spec 2.1). per_session_data_size 0: the connection's userdata is ours (connect_info.userdata),
	// as the engine's (LwsWebSocketsManager.cpp:163-166). Protocols[1] stays zero: LWS's terminator
	Protocols[0].name = "varest";
	Protocols[0].callback = &FService::Callback;
	Protocols[0].per_session_data_size = 0;
	Protocols[0].rx_buffer_size = RxBufferBytes;

	lws_context_creation_info Info = {};
	Info.port = CONTEXT_PORT_NO_LISTEN;
	Info.protocols = Protocols;
	Info.uid = -1;
	Info.gid = -1;
	// The engine's options (LwsWebSocketsManager.cpp:184) and JUST_USE_RAW_ORIGIN (T4, libwebsockets.h:2618): .origin as given
	Info.options = LWS_SERVER_OPTION_PEER_CERT_NOT_REQUIRED | LWS_SERVER_OPTION_DISABLE_OS_CA_CERTS | LWS_SERVER_OPTION_JUST_USE_RAW_ORIGIN;
	Info.max_http_header_data2 = HeaderBufferBytes;
	Info.pt_serv_buf_size = HeaderBufferBytes;
	// Spec 2.1, 5: VaRest's ping is per connection (W1b); LWS's own is off
	Info.ws_ping_pong_interval = 0;
	// Spec 2.2: context-wide, the backstop of every handshake stage [timeout.configured_secs]; each socket's shorter deadline is VaRest's kill
	Info.timeout_secs = static_cast<unsigned int>(VaRest::WebSocket::MaxConnectTimeoutSeconds);
	// Callback() finds this service through lws_context_user(lws_get_context(wsi)), also inside lws_create_context
	Info.user = this;
	// The engine's proxy setting, command line or ini (LwsWebSocketsManager.cpp:196-203; GenericPlatformHttp.cpp:640-651)
	const FString Proxy = FPlatformHttp::GetConfiguredProxyAddress();
	if (!Proxy.IsEmpty())
	{
		Utf8Copy(Proxy, ProxyUtf8);
		Info.http_proxy_address = ProxyUtf8.GetData();
	}
	FSslModule& Ssl = FModuleManager::LoadModuleChecked<FSslModule>(TEXT("SSL"));
	Certificates = &Ssl.GetCertificateManager();
	SslManager = &Ssl.GetSslManager();
	// The engine's order (LwsWebSocketsManager.cpp:205-225): the SSL module's context where InitializeSsl succeeds -- monolithic
	// and merged builds only (SslManager.cpp:26-55) -- else LWS's own, which raises LOAD_EXTRA_CLIENT_VERIFY_CERTS (T23)
	if (SslManager->InitializeSsl())
	{
		bSslInitialized = true;
		ProvidedSslCtx = SslManager->CreateSslContext(FSslContextCreateOptions());
	}
	if (ProvidedSslCtx)
	{
		// Its roots come from CreateSslContext itself (bAddCertificates, ISslManager.h:46-47) [tls.provided_ctx_load_extra_fired]
		AddExtraTrusted(ProvidedSslCtx);
		Info.provided_client_ssl_ctx = ProvidedSslCtx;
	}
	else
	{
		Info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
	}
#if defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
	// Private LWS has private ex-data indices. Its TLS client setup also checks this option BEFORE adopting
	// provided_client_ssl_ctx (tls-client.c). A genuine FSslManager context in monolithic Game does not initialize
	// those private globals. OpenSSL1.1.1 init is idempotent; LWS respects the supplied context's caller ownership.
	// Keep the existing handshake-in-flight gate above before every context creation.
	Info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
#endif
	Context = lws_create_context(&Info);
	Counters.ContextCreations.fetch_add(1);
	if (!Context)
	{
		UE_LOG(LogVaRestSocket, Error, TEXT("libwebsockets: lws_create_context failed; WebSocket connections end with Shutdown"));
		ReleaseSsl();
		bStartFailed = true;
		return false;
	}
	bStopping = false;
	{
		// Context is set before bAccepting under the lock: an Enqueue that sees bAccepting sees the context
		FScopeLock Guard(&EnqueueLock);
		bAccepting = true;
	}
	// Read before the thread exists, so no lws_* call but the cancel runs off the service thread while it runs (review M-8)
	const char* const LwsVersion = lws_get_library_version();
	// 128 KiB, below normal, forkable: the engine's service thread (LwsWebSocketsManager.cpp:239-247). The thread's start
	// follows every write above, Context among them (the context is the game thread's until here)
	Thread = Options.bFailThreadForTest ? nullptr
		: FForkProcessHelper::CreateForkableThread(this, *Options.ThreadName, 128 * 1024, TPri_BelowNormal, FPlatformAffinity::GetNoAffinityMask());
	if (!Thread)
	{
		bStartFailed = true;
		UE_LOG(LogVaRestSocket, Error, TEXT("libwebsockets: the service thread could not start; WebSocket connections end with Shutdown"));
		{
			// No Enqueue is inside lws_cancel_service once this is released, and none calls it again
			FScopeLock Guard(&EnqueueLock);
			bAccepting = false;
		}
		lws_context_destroy(Context);
		Context = nullptr;
		ReleaseSsl();
		return false;
	}
	UE_LOG(LogVaRestSocket, Verbose, TEXT("libwebsockets %s: context and thread %s started"), UTF8_TO_TCHAR(LwsVersion), *Options.ThreadName);
	return true;
}

void VaRest::WebSocket::Lws::FService::Shutdown()
{
	check(IsInGameThread());
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
	if (NativeCreating.Phase != ENativeEntryPhase1::Fresh)
	{
		bShuttingDown = true;
		// Reentrant construction/factory Stop cuts only the actual advisory
		// permit/native epoch. No journal/resource destruction or no-child claim.
		if (NativeCreating.InCreate)
		{
			if (NativeCreating.Adapter.IsValid()) (void)NativeCreating.Adapter->StopBeforeCreatingJoin();
			return;
		}
		// Entry/cleanup FULLreturn owns the next step; never destroy a live attach.
		if (!NativeCreating.InEntry && !NativeCreating.InCleanup)
			(void)FinishNativeCleanupChildOnCurrentControl();
		return;
	}
#endif
	// First (review M-7): a connect queued just before Shutdown and adopted during the flush ends with Shutdown
	// (FConn::Start) instead of starting -- LWS 3.0 resolves DNS on the service thread, which would hold the flush and the
	// join. Closes are never gated on it, so the ones asked for still go out
	bShuttingDown = true;
	// Review conc M-3: the close frames already asked for (the hub's 1001 at engine exit, spec 1.4) go out first, while the
	// thread still runs. Only where it is a real thread: the main thread ticks it otherwise (Fork.h:86-93), and does not here
	if (Thread && bAccepting.load() && Options.ExitFlushSeconds > 0.0 && VaRestLwsServicePrivate::HasRealThread())
	{
		FlushCloses(Options.ExitFlushSeconds);
	}
	{
		// Every Enqueue from here on drops its command, and none is still inside lws_cancel_service: the context may go
		FScopeLock Guard(&EnqueueLock);
		bAccepting = false;
	}
	if (Thread)
	{
		// Kill(true) calls Stop() and joins Run() (PThreadRunnableThread.h:276-290): the engine's order
		// (LwsWebSocketsManager.cpp:298-303). From the join on, the thread's state is the game thread's
		Thread->Kill(true);
		delete Thread;
		Thread = nullptr;
	}
	// The thread is gone: its state is the game thread's now. Queued commands run (a connect among them reports Shutdown)
	DrainCommands();
	for (TPair<uint64, TUniquePtr<IServiceConnection>>& Pair : Connections)
	{
		Pair.Value->EndForShutdown();
	}
	if (Context)
	{
		// After the thread, on the game thread (LwsWebSocketsManager.cpp:305-309); the connections' last callbacks
		// (CLIENT_CLOSED, WSI_DESTROY) arrive inside it, here, and each has reported already
		lws_context_destroy(Context);
		Context = nullptr;
	}
	Connections.Empty();
	Live.Empty();
	Counters.LiveConnections = 0;
#if WITH_DEV_AUTOMATION_TESTS
	CompletePressureFencesForTests(true);
#endif
	ReleaseSsl();
}

void VaRest::WebSocket::Lws::FService::FlushCloses(double Seconds)
{
	// A marker queued after every command so far: once it has run, UnsentCloses counts each close asked for before this call
	const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> Reached = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	Enqueue([Reached](FService& Self) { *Reached = true; });
	const double Until = FPlatformTime::Seconds() + Seconds;
	while (FPlatformTime::Seconds() < Until && (!Reached->load() || Counters.UnsentCloses.load() > 0))
	{
		FPlatformProcess::SleepNoStats(0.001f);
	}
}

bool VaRest::WebSocket::Lws::FService::Enqueue(FCommand&& Command)
{
	{
		FScopeLock Guard(&EnqueueLock);
		if (bAccepting.load())
		{
			Commands.Enqueue(MoveTemp(Command));
			// The one LWS call another thread may make (T13, libwebsockets.h:3704-3716): the command runs in EVENT_WAIT_CANCELLED
			// (T24). Under the lock: the context exists until Shutdown has taken it (libwebsockets.h:3712-3713)
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
            if (NativeChildSnapshot) {
                VrLwsCancelResult1 Actual {};
                (void)varest_lws_cancel_tracked1(Context, NativeChildSnapshot->View, &Actual);
                // Failed wake keeps the command owned; no immediate retry or
                // foreign journal/phase/turn read. SAME lock spans WHOLE call.
            } else
#endif
                lws_cancel_service(Context);
            return true;
		}
	}
	// Refused (review conc M-7): released here, outside the lock -- its captures' destructors are the caller's code -- and
	// at once, even when the caller moved it from a named function that outlives this call
	{
		const FCommand Dropped = MoveTemp(Command);
	}
	UE_LOG(LogVaRestSocket, Verbose, TEXT("libwebsockets: a command was dropped: the service %s"), bShuttingDown.load() ? TEXT("is shut down") : TEXT("is not running"));
	return false;
}

void VaRest::WebSocket::Lws::FService::Adopt(TUniquePtr<IServiceConnection>&& Connection)
{
	IServiceConnection* Raw = Connection.Get();
	// A second connection under one id would replace the first while Live still held its address
	checkf(!Connections.Contains(Raw->GetId()), TEXT("ws#%llu adopted twice"), Raw->GetId());
	Live.Add(Raw);
	Connections.Add(Raw->GetId(), MoveTemp(Connection));
	Counters.LiveConnections = Connections.Num();
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
    if (NativeChildSnapshot) NativeRunning.MaxConnId = FMath::Max(NativeRunning.MaxConnId, Raw->GetId());
#endif
    Raw->Start(*this);
}

VaRest::WebSocket::Lws::IServiceConnection* VaRest::WebSocket::Lws::FService::Find(uint64 Id) const
{
	const TUniquePtr<IServiceConnection>* Found = Connections.Find(Id);
	return Found ? Found->Get() : nullptr;
}

int32 VaRest::WebSocket::Lws::FService::ComputeWaitMs(double Now) const
{
	if (Connections.Num() == 0)
	{
		return Options.IdleWaitMs;
	}
	double Next = TNumericLimits<double>::Max();
	bool bShort = false;
	for (const TPair<uint64, TUniquePtr<IServiceConnection>>& Pair : Connections)
	{
		Next = FMath::Min(Next, Pair.Value->NextDeadline());
		bShort = bShort || Pair.Value->WantsShortWait();
	}
	const int32 Cap = bShort ? Options.ShortWaitCapMs : Options.ActiveWaitCapMs;
	if (Next == TNumericLimits<double>::Max())
	{
		return Cap;
	}
	// Clamped as a double before the cast; rounded up, so a wait never ends just before its deadline and spins once more
	return FMath::CeilToInt32(FMath::Clamp((Next - Now) * 1000.0, 0.0, static_cast<double>(Cap)));
}

bool VaRest::WebSocket::Lws::FService::Init()
{
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
    const FNativeOwnerServiceChildSnapshot* const Child = NativeChildSnapshot;
    if (Child) (void)Child->RegisterFromActualInit();
#endif
    return true; // ALL native Init diagnostics precede Core's existing true-Init sync.
}

uint32 VaRest::WebSocket::Lws::FService::Run()
{
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
    const FNativeOwnerServiceChildSnapshot* const Child = NativeChildSnapshot;
    if (Child)
    {
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
        if (Child->GetRegistrationDiagnostic() == VR_STATUS_OK && Child->Context && Child->View)
            RunNativeService(*Child);
#endif
        Child->RunCleanupFromActualRun();
        return 0;
    }
#endif
	LLM_SCOPE_BYTAG(VaRest_WebSocket);
	while (!bStopping.load())
	{
		ServiceOnce(ComputeWaitMs(FPlatformTime::Seconds()));
	}
	return 0;
}

void VaRest::WebSocket::Lws::FService::Stop()
{
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
    const FNativeOwnerServiceChildSnapshot* const Child = NativeChildSnapshot;
    if (Child)
    {
        bStopping = true;
        Child->RequestCleanupStopFromActualStop();
        return; // No untracked lws_cancel_service from native Stop.
    }
    // No native child can read this GT journal while the stable word is null:
    // publication precedes its factory, and removal requires known no-child/join.
    if (NativeCreating.Phase != ENativeEntryPhase1::Fresh)
    {
        bStopping = true;
        if (NativeCreating.Resources && NativeCreating.Resources->Storage.Object)
            C2ResourcesC1::RequestStop1(NativeCreating.Resources->Storage.Object);
        return;
    }
#endif
	// From Kill on the game thread, while Run may wait in lws_service: the cancel ends that wait. A cancel made while Run is
	// between its bStopping check and lws_service is not lost: it leaves a byte in LWS's cancel pipe, which the next poll
	// sees (the Release archive: rops_handle_POLLIN_pipe reads the pipe before it broadcasts EVENT_WAIT_CANCELLED; review
	// conc, "Checked and sound" 1)
	bStopping = true;
	if (Context)
	{
		lws_cancel_service(Context);
	}
}

void VaRest::WebSocket::Lws::FService::Tick()
{
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
    if (NativeCreating.Phase != ENativeEntryPhase1::Fresh) return; // Native mode is never a fake/GT service loop.
#endif
	// No wait where the main thread ticks the service: the engine polls with 0 there too (LwsWebSocketsManager.cpp:110-118, :509)
	if (Context && !bStopping.load())
	{
		ServiceOnce(0);
	}
}

void VaRest::WebSocket::Lws::FService::ServiceOnce(int32 WaitMs)
{
	lws_service(Context, WaitMs);
	// Spec 2.1: VaRest's deadlines after each return, outside any callback, where a kill and a queued event are safe
	CheckDeadlines(FPlatformTime::Seconds());
	EraseDestroyed();
#if WITH_DEV_AUTOMATION_TESTS
	++PressurePassForTests;
	CompletePressureFencesForTests();
#endif
}

#if WITH_DEV_AUTOMATION_TESTS
void VaRest::WebSocket::Lws::FService::QueuePressureFenceForTests(PressureTesting::FPassFence Fence)
{
	using namespace PressureTesting;
	FConn* Conn = FConn::Find(*this, Fence.Connection);
	if (Fence.Probe->bCanceled.load(std::memory_order_acquire))
		Fence.Probe->Acknowledge(Fence.Request, EFenceStatus::Canceled);
	else if (Fence.Probe->Owner != Fence.Owner || !Conn || !Conn->MatchesPressureProbeForTests(*Fence.Probe))
		Fence.Probe->Acknowledge(Fence.Request, Fence.Probe->bRetired.load(std::memory_order_acquire) ? EFenceStatus::Destroyed : EFenceStatus::Stale);
	else if (!PressureFencesForTests.IsEmpty())
		Fence.Probe->Acknowledge(Fence.Request, EFenceStatus::Refused);
	else
	{
		Fence.AfterPass = PressurePassForTests + 1;
		PressureFencesForTests.Add(MoveTemp(Fence));
	}
}

void VaRest::WebSocket::Lws::FService::CompletePressureFencesForTests(bool bShutdown)
{
	using namespace PressureTesting;
	for (int32 Index = PressureFencesForTests.Num() - 1; Index >= 0; --Index)
	{
		auto& Fence = PressureFencesForTests[Index];
		if (!bShutdown && PressurePassForTests < Fence.AfterPass) continue;
		FConn* Conn = FConn::Find(*this, Fence.Connection);
		if (bShutdown || Fence.Probe->bCanceled.load(std::memory_order_acquire))
			Fence.Probe->Acknowledge(Fence.Request, EFenceStatus::Canceled);
		else if (Fence.Probe->Owner != Fence.Owner || !Conn || !Conn->MatchesPressureProbeForTests(*Fence.Probe))
			Fence.Probe->Acknowledge(Fence.Request, Fence.Probe->bRetired.load(std::memory_order_acquire) ? EFenceStatus::Destroyed : EFenceStatus::Stale);
		else
			Fence.Probe->Acknowledge(Fence.Request, EFenceStatus::Complete, Conn->PressureSnapshotForTests(Fence.Request, PressurePassForTests));
		PressureFencesForTests.RemoveAt(Index);
	}
}
#endif

void VaRest::WebSocket::Lws::FService::DrainCommands()
{
	// The queue's one consumer: the service thread inside EVENT_WAIT_CANCELLED, or the game thread once it is joined. No
	// lock is held while a command runs
	FCommand Command;
	while (Commands.Dequeue(Command))
	{
		Command(*this);
		Command.Reset();
		Counters.CommandsRun.fetch_add(1);
	}
}

void VaRest::WebSocket::Lws::FService::CheckDeadlines(double Now)
{
	for (TPair<uint64, TUniquePtr<IServiceConnection>>& Pair : Connections)
	{
		Pair.Value->CheckDeadlines(*this, Now);
	}
}

void VaRest::WebSocket::Lws::FService::EraseDestroyed()
{
	for (auto It = Connections.CreateIterator(); It; ++It)
	{
		if (It->Value->IsDestroyed())
		{
			// LWS calls back no more for it: WSI_DESTROY came, or there never was a wsi
			Live.Remove(It->Value.Get());
			It.RemoveCurrent();
		}
	}
	Counters.LiveConnections = Connections.Num();
}

int VaRest::WebSocket::Lws::FService::Callback(lws* Wsi, lws_callback_reasons Reason, void* User, void* In, size_t Len)
{
	// Also called inside lws_create_context (protocol init, client TLS setup), before Context is set: the service comes from
	// the context's user pointer (lws_context_creation_info::user), as the probe's rig does (VaRestLwsProbe.cpp:492-499)
	lws_context* Owner = Wsi ? lws_get_context(Wsi) : nullptr;
	FService* Self = Owner ? static_cast<FService*>(lws_context_user(Owner)) : nullptr;
	return Self ? Self->Handle(Wsi, static_cast<int32>(Reason), User, In, Len) : 0;
}

int32 VaRest::WebSocket::Lws::FService::Handle(lws* Wsi, int32 Reason, void* User, void* In, size_t Len)
{
	switch (Reason)
	{
	case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
		// lws_cancel_service lands here, on the service thread (libwebsockets.h:1551-1556): spec 2.1's command point
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
        if (NativeChildSnapshot) return 0; // Bounded single Between drain, never callback recursion.
#endif
        DrainCommands();
        return 0;
	case LWS_CALLBACK_OPENSSL_LOAD_EXTRA_CLIENT_VERIFY_CERTS:
		// user is the SSL_CTX (libwebsockets.h:961-967); raised inside lws_create_context, on the game thread
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
        if (NativeChildSnapshot || (IsInGameThread() && NativeCreating.Phase != ENativeEntryPhase1::Fresh)) return 0;
#endif
        LoadClientCertificates(static_cast<SSL_CTX*>(User));
		return 0;
	default:
		break;
	}
	IServiceConnection* Connection = FindForCallback(Wsi, User, Reason);
	return Connection ? Connection->OnCallback(*this, Wsi, Reason, User, In, Len) : 0;
}

VaRest::WebSocket::Lws::IServiceConnection* VaRest::WebSocket::Lws::FService::FindForCallback(lws* Wsi, void* User, int32 Reason) const
{
	// The userdata, never the wsi: LWS 3.0 reuses a freed wsi's address for the next connection (bead VaRest-81s;
	// Probe.VaRest.Lws.Rig.SequentialConnections). PERFORM_SERVER_CERT_VERIFICATION's user is the X509_STORE_CTX, so the
	// userdata comes from lws_wsi_user there, as the engine does (LwsWebSocketsManager.cpp:447-456). T29: the others carry it in user
	if (Reason != LWS_CALLBACK_OPENSSL_PERFORM_SERVER_CERT_VERIFICATION && User && Live.Contains(static_cast<const IServiceConnection*>(User)))
	{
		return static_cast<IServiceConnection*>(User);
	}
	void* WsiUser = Wsi ? lws_wsi_user(Wsi) : nullptr;
	return WsiUser && Live.Contains(static_cast<const IServiceConnection*>(WsiUser)) ? static_cast<IServiceConnection*>(WsiUser) : nullptr;
}

void VaRest::WebSocket::Lws::FService::LoadClientCertificates(SSL_CTX* Ctx)
{
	// The engine's roots (LwsWebSocketsManager.cpp:441-446): with LWS's own context they come only from here (T23, tls.load_extra_fired)
	if (Certificates && Ctx)
	{
		Certificates->AddCertificatesToSslContext(Ctx);
	}
	AddExtraTrusted(Ctx);
}

void VaRest::WebSocket::Lws::FService::AddExtraTrusted(SSL_CTX* Ctx) const
{
	if (!Ctx || Options.ExtraTrustedPem.Num() == 0)
	{
		return;
	}
	BIO* Bio = BIO_new_mem_buf(Options.ExtraTrustedPem.GetData(), Options.ExtraTrustedPem.Num());
	if (!Bio)
	{
		return;
	}
	X509_STORE* Store = SSL_CTX_get_cert_store(Ctx);
	int32 Added = 0;
	while (X509* Certificate = PEM_read_bio_X509(Bio, nullptr, nullptr, nullptr))
	{
		Added += X509_STORE_add_cert(Store, Certificate) == 1 ? 1 : 0;
		X509_free(Certificate);
	}
	// The read that ends the loop leaves "no start line" on OpenSSL's error queue (VaRestLwsProbe.cpp:985-986)
	ERR_clear_error();
	BIO_free(Bio);
	UE_LOG(LogVaRestSocket, Verbose, TEXT("libwebsockets: %d extra trusted certificate(s)"), Added);
}

void VaRest::WebSocket::Lws::FService::ReleaseSsl()
{
	// Modules unload last-loaded first (ModuleManager.cpp:1458): at module shutdown the SSL module, loaded after this one, may be gone
	if (!SslManager || !FModuleManager::Get().IsModuleLoaded(TEXT("SSL")))
	{
		ProvidedSslCtx = nullptr;
		bSslInitialized = false;
		return;
	}
	if (ProvidedSslCtx)
	{
		SslManager->DestroySslContext(ProvidedSslCtx);
		ProvidedSslCtx = nullptr;
	}
	if (bSslInitialized)
	{
		// One ShutdownSsl per InitializeSsl that counted (ISslManager.h:58-69; outside monolithic and merged builds neither
		// counts, SslManager.cpp:26-74)
		SslManager->ShutdownSsl();
		bSslInitialized = false;
	}
}
