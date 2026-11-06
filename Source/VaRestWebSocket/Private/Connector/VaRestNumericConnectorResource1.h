// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once
#include <stdint.h>
#include <stddef.h>

namespace VaRest::WebSocket::NumericConnectorC1
{
// Native-only data carriers. A carrier is NEVER invoked: Resources.cpp restores
// exactly the original producer Fn type. Accessible storage/provenance/role/code
// lifetime are source premises, never conferred by this record or a shape check.
using FResourceEntryAddress1 = void (*)();
enum : uint32_t { ResourceVersion1 = 1, ResourceProfile2 = 2, ResourcePrefixCells1 = 12,
    ResourceCells2 = 140, ResourcePoolCells2 = 128, ResourceJournalBytes2 = 9024, ResourceHolderBytes2 = 9248 };
enum : uint32_t
{
    ResourceEmpty1 = 0, ResourceReserved1 = 1, ResourceObtainedPending1 = 2,
    ResourceOwnedAck1 = 3, ResourceNotObtainedPending1 = 4, ResourceClosedPending1 = 5,
    ResourceUncertainPending1 = 6, ResourceObtainedThenClosed1 = 7,
    ResourceObtainedThenUncertain1 = 8, ResourceUncertaintyAck1 = 9,
    ResourceClosedAck1 = 10, ResourceNotObtainedAck1 = 11,
    ResourceAcquireFaultPending2 = 12, ResourceAcquireFaultAck2 = 13
};
enum : uint32_t { ResourceProtocolFault1 = 1, ResourceRefusalFault1 = 2, ResourceUncertainFault1 = 4 };
enum : uint32_t
{
    ResourceCompleted1 = 0, ResourceNativeBusy1 = 1, ResourceNativeRefused1 = 2,
    ResourceLocalInvalid1 = 3, ResourceProtocolFailure1 = 4,
    ResourceParentDeferred1 = 5, ResourceQuotaDeferred1 = 6
};
struct FResourceTicket1
{
    uint32_t Size, Version;
    uint64_t OwnerEpoch, ServiceEpoch, Serial;
    uint32_t Cell;
    uint16_t Kind, Subkind;
};
struct FResourceSpec1 { uint32_t Kind, Subkind, Cell, Location; uint64_t ParentSerial; };
struct FResourceActual1
{
    uint64_t NativeValue, Extent;
    int32_t Result, SavedError;
    uint32_t Location, Reserved;
};
struct FResourceCell1
{
    uint64_t NativeSerial, ParentSerial, NativeValue, Extent, SourceSerial;
    uint32_t Kind, Subkind, LocalState, SavedError, SourceLocation;
    int32_t Result;
};
struct FResourceJournalHeader1
{
    uint32_t Size, Version, Profile, CellCount;
    uint64_t HookDispatches, NativeIdentityCalls;
    uint32_t ReconcileCursor, Fault, CreateEntered;
    int32_t FirstNativeRefusal;
    uint64_t ReservedTail[2];
};
struct FResourceJournal1 { FResourceJournalHeader1 Header; FResourceCell1 Cells[140]; };
struct FResourceBinding1
{
    uint32_t Size, Version;
    void* Owner;
    uint64_t OwnerEpoch, ServiceEpoch;
    FResourceEntryAddress1 Reserve, Obtained, NotObtained, Closed, Uncertain;
    uint32_t Profile, CellCount;
    FResourceJournal1* Journal;
    uint64_t Reserved;
};
struct FHookOutcome1 { uint32_t Disposition; int32_t NativeStatus; uint32_t NativeCalls, Reserved; };
struct FResourceProgress1
{
    uint32_t Size, Version, VisitedRows, Dispatches, NativeCalls, PendingRows, OutstandingRows, Fault;
};

#if defined(__GNUC__)
#define VAREST_C2_RESOURCE_DECL [[gnu::visibility("hidden")]]
#else
#define VAREST_C2_RESOURCE_DECL
#endif
VAREST_C2_RESOURCE_DECL bool ResourceBindingValid1(const FResourceBinding1*);
// Only actual still-owned LWS client-fd episodes add active physical-close credits.
VAREST_C2_RESOURCE_DECL uint32_t ResourceClientCloseCredits1(const FResourceBinding1*);
// Reserve/AcknowledgeNext alone dispatch. The caller pre-reserves worst2 from
// its SAME turn (including close credits); local deferral is never native BUSY.
VAREST_C2_RESOURCE_DECL FHookOutcome1 ResourceReserve1(const FResourceBinding1*, const FResourceSpec1*);
VAREST_C2_RESOURCE_DECL FHookOutcome1 ResourceAcknowledgeNext1(const FResourceBinding1*, uint32_t cell);
// Infallible recording at prevalidated actual return sites; no OS/native entry.
VAREST_C2_RESOURCE_DECL void ResourceRecordFdResult1(const FResourceBinding1*, uint32_t cell,
    uint64_t sourceSerial, uint32_t sourceLocation, int32_t actualFd, int32_t savedError);
VAREST_C2_RESOURCE_DECL void ResourceRecordMapResult1(const FResourceBinding1*, void* actualRaw,
    uint64_t actualRoundedBytes, int32_t savedError);
VAREST_C2_RESOURCE_DECL void ResourceRecordNotAttempted1(const FResourceBinding1*, uint32_t cell);
VAREST_C2_RESOURCE_DECL void ResourceRecordFdDisposition1(const FResourceBinding1*, uint32_t cell,
    uint64_t sourceSerial, int32_t originalFd, int32_t actualResult, int32_t savedError);
VAREST_C2_RESOURCE_DECL void ResourceRecordPairFinalized1(const FResourceBinding1*, uint32_t slot, uint64_t generation);
#undef VAREST_C2_RESOURCE_DECL
}

// Fixed native-only C custody surface shared with the later private LWS header.
// Source/role/return/O premises remain independently required; no active callers.
#ifndef VAREST_LWS_CUSTODY_DECLARATIONS1
#define VAREST_LWS_CUSTODY_DECLARATIONS1
#include <stdint.h>
#include <stddef.h>
#if defined(__cplusplus)
#define VR_LWS_ASSERT1(X) static_assert(X)
extern "C" {
#else
#define VR_LWS_ASSERT1(X) _Static_assert(X, #X)
#endif
#if defined(__GNUC__)
#define VR_LWS_LOCAL1 __attribute__((visibility("hidden")))
#else
#define VR_LWS_LOCAL1
#endif
struct lws;
struct lws_context;
struct lws_vhost;
struct lws_context_creation_info;
struct lws_client_connect_info;
struct ssl_ctx_st;

enum {
 VR_LWS_CUSTODY_VERSION1=1, VR_LWS_RESOURCE_PROFILE2=2,
 VR_LWS_PREFIX_CELLS1=12, VR_LWS_POOL_CELLS2=128, VR_LWS_TOTAL_CELLS2=140,
 VR_LWS_OWNER_BYTES2=10632, VR_LWS_OWNER_MAPPING_BYTES1=16384,
 VR_LWS_JOURNAL_BYTES2=9024, VR_LWS_STORAGE_OFFSET2=9024,
 VR_LWS_BINDING_OFFSET2=9072, VR_LWS_CONTROL_OFFSET2=9168,
 VR_LWS_VIEW_OFFSET2=9232, VR_LWS_HOLDER_BYTES2=9248,
 VR_LWS_VISIBLE_ALLOCATOR_BYTES2=1048576,
 VR_LWS_ACTIVE_NAMED_CAP1=32, VR_LWS_STARTUP_NAMED_CAP2=1536,
 VR_LWS_FINAL_NAMED_CAP2=2048
};
enum VrLwsSourceDisposition1 {
 VR_LWS_KNOWN_SUCCESS1=0, VR_LWS_DEFERRED_BEFORE_ACQUIRE1=1,
 VR_LWS_NO_RESOURCE_FAILURE1=2, VR_LWS_FAILED_WITH_OUTSTANDING1=3,
 VR_LWS_RELEASE_UNCERTAIN1=4, VR_LWS_PROTOCOL_FAULT1=5
};
enum VrLwsHookDisposition1 {
 VR_LWS_HOOK_COMPLETED1=0, VR_LWS_HOOK_NATIVE_BUSY1=1,
 VR_LWS_HOOK_NATIVE_REFUSED1=2, VR_LWS_HOOK_LOCAL_INVALID1=3,
 VR_LWS_HOOK_PROTOCOL_FAULT1=4, VR_LWS_HOOK_PARENT_DEFERRED1=5,
 VR_LWS_HOOK_QUOTA_DEFERRED1=6
};
/* Event is selected by the EXISTING Fn entry, not an extra wire field. */
enum VrLwsNamedOperation1 {
 VR_LWS_NAMED_ACQUIRE1=1, VR_LWS_NAMED_DISPOSE1=2, VR_LWS_NAMED_RNG1=3,
 // Active source only, exactly one real attempt; no new quota or native entry.
 VR_LWS_NAMED_CONNECT1=4, VR_LWS_NAMED_SOCKET_CONTROL1=5,
 VR_LWS_NAMED_READ1=6, VR_LWS_NAMED_WRITE1=7,
 VR_LWS_NAMED_POLL1=8, VR_LWS_NAMED_SHUTDOWN1=9,
 // Creating startup only: one OUTER Engine roots call, no inner-work claim.
 VR_LWS_NAMED_ENGINE_ROOTS1=10,
 // One selected outer provider setup, not provider internal-work accounting.
 VR_LWS_NAMED_TLS_SETUP1=11
};
enum VrLwsEngineRootsStage1 {
 VR_LWS_ENGINE_ROOTS_NEVER1=0, VR_LWS_ENGINE_ROOTS_ENTERED1=1,
 VR_LWS_ENGINE_ROOTS_ENGINE_RETURNED1=2, VR_LWS_ENGINE_ROOTS_BRIDGE_RETURNED1=3
};
enum VrLwsCreatingPhase1 { VR_LWS_PHASE_STARTUP1=1, VR_LWS_PHASE_FINAL1=2 };
enum VrLwsResourceEvent1 {
 VR_LWS_EVENT_RESERVE1=0, VR_LWS_EVENT_OBTAINED1=1,
 VR_LWS_EVENT_NOT_OBTAINED1=2, VR_LWS_EVENT_CLOSED1=3,
 VR_LWS_EVENT_UNCERTAIN1=4
};
enum VrLwsNativeState2 {
 VR_LWS_EMPTY1=0, VR_LWS_RESERVED1=1, VR_LWS_OWNED1=2,
 VR_LWS_OWNED_UNCERTAIN1=3, VR_LWS_CLOSED1=4,
 VR_LWS_NOT_OBTAINED1=5, VR_LWS_ACQUIRE_FAULT2=6
};
enum VrLwsLocalState2 {
 VR_LWS_LOCAL_EMPTY1=0, VR_LWS_LOCAL_RESERVED1=1,
 VR_LWS_OBTAINED_PENDING1=2, VR_LWS_OWNED_ACK1=3,
 VR_LWS_NOT_OBTAINED_PENDING1=4, VR_LWS_CLOSED_PENDING1=5,
 VR_LWS_UNCERTAIN_PENDING1=6, VR_LWS_OBTAINED_THEN_CLOSED1=7,
 VR_LWS_OBTAINED_THEN_UNCERTAIN1=8, VR_LWS_UNCERTAINTY_ACK1=9,
 VR_LWS_CLOSED_ACK1=10, VR_LWS_NOT_OBTAINED_ACK1=11,
 VR_LWS_ACQUIRE_FAULT_PENDING2=12, VR_LWS_ACQUIRE_FAULT_ACK2=13
};
enum VrLwsResourceKind1 {
 VR_LWS_KIND_LISTENER1=1, VR_LWS_KIND_INGRESS1=2,
 VR_LWS_KIND_PAIR1=3, VR_LWS_KIND_BUFFER1=4,
 VR_LWS_KIND_WSI1=5, VR_LWS_KIND_VHOST1=6,
 VR_LWS_KIND_CONTEXT1=7, VR_LWS_KIND_DEPENDENCY1=8,
 VR_LWS_KIND_WAKE1=9
};
enum VrLwsBufferSubkind2 {
 VR_LWS_BUFFER_C1_MAPPING1=1, VR_LWS_BUFFER_SCRATCH2=2,
 VR_LWS_BUFFER_POLL2=3, VR_LWS_BUFFER_PROTOCOL_COPY2=4,
 VR_LWS_BUFFER_SAME_VHOST_LIST2=5, VR_LWS_BUFFER_WS_ROLE2=6,
 VR_LWS_BUFFER_RX2=7, VR_LWS_BUFFER_STASH2=8,
 VR_LWS_BUFFER_STASH_STRING2=9, VR_LWS_BUFFER_HOSTNAME2=10,
 VR_LWS_BUFFER_TX2=11, VR_LWS_BUFFER_CACHE2=12,
 VR_LWS_BUFFER_AH_OBJECT2=13, VR_LWS_BUFFER_AH_DATA2=14,
 VR_LWS_BUFFER_PUBLIC_KEY_TEMP2=15
};
enum VrLwsWsiSubkind2 {
 VR_LWS_WSI_DATA_OBJECT2=1, VR_LWS_WSI_EVENT_OBJECT2=2,
 VR_LWS_WSI_CLIENT_FD2=3
};
enum VrLwsDependencySubkind2 {
 VR_LWS_DEP_RANDOM_FD2=1, VR_LWS_DEP_SSL_CTX_REF2=2,
 VR_LWS_DEP_SSL_REF2=3, VR_LWS_DEP_SOCKET_BIO_REF2=4,
 VR_LWS_DEP_MEMORY_BIO_REF2=5, VR_LWS_DEP_LOCAL_X509_REF2=6,
 VR_LWS_DEP_STORE_X509_REF2=7, VR_LWS_DEP_INIT_SSL_LIBRARY2=8,
 VR_LWS_DEP_SSL_EX_INDEX2=9, VR_LWS_DEP_SSL_CTX_EX_INDEX2=10,
 VR_LWS_DEP_INIT_ALGORITHMS2=11, VR_LWS_DEP_INIT_ERROR_STRINGS2=12
};
enum VrLwsWakeSubkind2 { VR_LWS_WAKE_READ2=1, VR_LWS_WAKE_WRITE2=2 };
enum VrLwsDerivedPointerResult2 {
 VR_LWS_POINTER_ABSENT2=-1, VR_LWS_POINTER_PRESENT2=1
};
enum VrLwsDerivedVoidResult2 {
 VR_LWS_VOID_FULL_RETURN2=1, VR_LWS_PARENT_VOID_FULL_RETURN2=2
};
/* Explicit meaning of existing Extent only for NEW pool reference releases.
 * Acquired/failure/fault reference Extent remains0. Allocator Extent is bytes.
 * No reserved word/bit carries this selector. OLD12 are byte-exact unchanged.
 */
enum VrLwsReferenceDisposition2 {
 VR_LWS_REFERENCE_DIRECT2=0, VR_LWS_REFERENCE_PARENT2=1
};
enum VrLwsInitOperationKey2 {
 VR_LWS_INIT_SSL_LIBRARY_KEY2=1, VR_LWS_INIT_ALGORITHMS_KEY2=2,
 VR_LWS_INIT_ERROR_STRINGS_KEY2=3
};

typedef struct VrLwsSourceControl1 {
 uint64_t NextSourceSerial, RawContext, ContextSourceSerial;
 uint32_t ContextPhase, CallbackDepth, CancelUsers, CtorEntered;
 uint32_t FullCreateReturned, FullDestroyReturned, Fault, Reserved0;
 uint64_t Reserved1;
} VrLwsSourceControl1;
typedef struct VrLwsCustody1 {
 const void* ResourceBinding; /* actual original C++ Binding96 object roundtrip */
 VrLwsSourceControl1* Control;
} VrLwsCustody1;
typedef struct VrLwsRowRef1 {
 uint32_t Cell, Reserved;
 uint64_t SourceSerial;
} VrLwsRowRef1;
typedef struct VrLwsTurnBorrow1 {
 void* Native; /* actual C1 FNative1, not a layout guess or permission token */
 uint64_t Generation;
} VrLwsTurnBorrow1;
typedef struct VrLwsPhaseBudget1 {
 uint32_t Remaining, Used, Phase, Reserved;
} VrLwsPhaseBudget1;
typedef struct VrLwsBudgetRef1 {
 const VrLwsTurnBorrow1* Active;
 VrLwsPhaseBudget1* Creating;
} VrLwsBudgetRef1; /* exactly ONE NONNULL pointer, selected by the actual source entry */
typedef struct VrLwsSpec1 {
 uint32_t Kind, Subkind, Cell, Location;
 uint64_t ParentSerial;
} VrLwsSpec1;
typedef struct VrLwsActual1 {
 uint64_t NativeValue, Extent;
 int32_t Result, SavedError;
 uint32_t Location, Reserved;
} VrLwsActual1; /* mirrored C carrier; never pointer-punned to VrResourceActual1 */
typedef struct VrLwsHookOutcome1 {
 uint32_t Disposition;
 int32_t NativeStatus;
 uint32_t NativeCalls, Reserved;
} VrLwsHookOutcome1;
typedef struct VrLwsResult1 {
 uint32_t Size, Version, Disposition, SourceLocation;
 void* RawObject;
 uint32_t PendingRows, Fault;
} VrLwsResult1;
typedef struct VrLwsCancelResult1 {
 int32_t RawResult;
 int32_t SavedError;
 uint32_t Attempted;
 uint32_t Disposition;
} VrLwsCancelResult1;
typedef struct VrLwsPipePairRaw1 {
 int32_t Result, Fds[2], SavedError;
} VrLwsPipePairRaw1; /* exact sole pipe2 raw capture, not another retained array */

VR_LWS_ASSERT1(sizeof(void*)==8);
VR_LWS_ASSERT1(sizeof(VrLwsSourceControl1)==64);
VR_LWS_ASSERT1(offsetof(VrLwsSourceControl1,ContextPhase)==24);
VR_LWS_ASSERT1(offsetof(VrLwsSourceControl1,FullCreateReturned)==40);
VR_LWS_ASSERT1(offsetof(VrLwsSourceControl1,Reserved1)==56);
VR_LWS_ASSERT1(sizeof(VrLwsCustody1)==16);
VR_LWS_ASSERT1(sizeof(VrLwsRowRef1)==16);
VR_LWS_ASSERT1(sizeof(VrLwsTurnBorrow1)==16);
VR_LWS_ASSERT1(sizeof(VrLwsPhaseBudget1)==16);
VR_LWS_ASSERT1(sizeof(VrLwsBudgetRef1)==16);
VR_LWS_ASSERT1(sizeof(VrLwsSpec1)==24);
VR_LWS_ASSERT1(offsetof(VrLwsSpec1,ParentSerial)==16);
VR_LWS_ASSERT1(sizeof(VrLwsActual1)==32);
VR_LWS_ASSERT1(offsetof(VrLwsActual1,Extent)==8);
VR_LWS_ASSERT1(offsetof(VrLwsActual1,Result)==16);
VR_LWS_ASSERT1(offsetof(VrLwsActual1,SavedError)==20);
VR_LWS_ASSERT1(offsetof(VrLwsActual1,Location)==24);
VR_LWS_ASSERT1(offsetof(VrLwsActual1,Reserved)==28);
VR_LWS_ASSERT1(sizeof(VrLwsHookOutcome1)==16);
VR_LWS_ASSERT1(sizeof(VrLwsResult1)==32);
VR_LWS_ASSERT1(offsetof(VrLwsResult1,RawObject)==16);
VR_LWS_ASSERT1(sizeof(VrLwsPipePairRaw1)==16);
VR_LWS_ASSERT1(sizeof(VrLwsCancelResult1)==16);

/* Source-return status is SourceDisposition1, NOT a rewritten native status.
 * All outputs/access spans/code/role/lifetime are genuine caller premises.
 * Every result/never record below is pure prepared stores, no native/OS call.
 * source_location is a LOCAL diagnostic only; Actual.Location is ALWAYS
 * the issued RowRef.Cell, including failures/never/fault/replay. Reserved=0.
 */
VR_LWS_LOCAL1 int32_t varest_lws_create_context_tracked1(
 const struct lws_context_creation_info*,const VrLwsCustody1*,
 VrLwsPhaseBudget1*,VrLwsResult1*);
VR_LWS_LOCAL1 int32_t varest_lws_create_vhost_tracked1(
 struct lws_context*,const struct lws_context_creation_info*,const VrLwsCustody1*,
 VrLwsPhaseBudget1*,VrLwsResult1*);
VR_LWS_LOCAL1 int32_t varest_lws_connect_client_tracked1(
 const struct lws_client_connect_info*,const VrLwsCustody1*,
 const VrLwsTurnBorrow1*,VrLwsResult1*);
VR_LWS_LOCAL1 int32_t varest_lws_service_tracked1(
 struct lws_context*,int32_t timeout_ms,const VrLwsCustody1*,
 const VrLwsTurnBorrow1*,VrLwsResult1*);
VR_LWS_LOCAL1 int32_t varest_lws_destroy_context_tracked1(
 struct lws_context*,const VrLwsCustody1*,VrLwsPhaseBudget1*,VrLwsResult1*);

/* Foreign source: same enqueue mutex protects WHOLE return and immutable fd.
 * No GT journal/phase/active-turn borrow. Sole producer pending1 store is CAS.
 */
VR_LWS_LOCAL1 int32_t varest_lws_cancel_tracked1(
 struct lws_context*,const VrLwsCustody1*,VrLwsCancelResult1*);

/* Actual synchronous creating source only; no retained callback/actor packet.
 * C source advances ENGINE_RETURNED to BRIDGE_RETURNED only AFTER full return.
 */
VR_LWS_LOCAL1 int32_t varest_ws_c2l_install_engine_roots1(
 struct lws_vhost*,struct ssl_ctx_st*,const VrLwsCustody1*,
 const VrLwsRowRef1* actual_ctx_row,VrLwsPhaseBudget1*,
 uint32_t* actual_vhost_root_stage,VrLwsResult1*);

/* Synchronous pure typed creating borrow. Real immutable Service backing/code
 * remains protected until every borrowing BIO fully releases, including BUSY.
 */
VR_LWS_LOCAL1 int32_t varest_ws_c2l_borrow_extra_trusted_pem1(
 struct lws_vhost*,struct ssl_ctx_st*,const VrLwsCustody1*,
 const VrLwsRowRef1*,const uint8_t** data,uint32_t* bytes,VrLwsHookOutcome1*);

/* These source-private WS bridges never import/copy producer bodies or types.
 * C source reserve/record calls target pool12..139 only. C1 uses its old typed
 * C++ record functions for prefix0..11 without semantic normalization changes.
 */
VR_LWS_LOCAL1 void varest_ws_c2l_reserve1(const VrLwsCustody1*,const VrLwsSpec1*,
 const VrLwsBudgetRef1*,VrLwsRowRef1*,VrLwsHookOutcome1*);
VR_LWS_LOCAL1 void varest_ws_c2l_ack_next1(const VrLwsCustody1*,uint32_t cell,
 const VrLwsBudgetRef1*,VrLwsHookOutcome1*);
VR_LWS_LOCAL1 void varest_ws_c2l_record_never_attempted1(
 const VrLwsCustody1*,const VrLwsRowRef1*,uint32_t source_location);
VR_LWS_LOCAL1 void varest_ws_c2l_record_fd_acquire1(
 const VrLwsCustody1*,const VrLwsRowRef1*,int32_t raw_fd,int32_t saved_errno,
 uint32_t source_location);
VR_LWS_LOCAL1 void varest_ws_c2l_record_pipe_acquire1(
 const VrLwsCustody1*,const VrLwsRowRef1* read_side,const VrLwsRowRef1* write_side,
 const VrLwsPipePairRaw1*,uint32_t source_location);
VR_LWS_LOCAL1 void varest_ws_c2l_record_pointer_acquire1(
 const VrLwsCustody1*,const VrLwsRowRef1*,const void* raw_pointer,
 uint64_t requested_bytes,int32_t saved_allocator_errno,uint32_t source_location);
VR_LWS_LOCAL1 void varest_ws_c2l_record_store_ref_acquire1(
 const VrLwsCustody1*,const VrLwsRowRef1*,const void* borrowed_argument,
 int32_t raw_bool,uint32_t source_location);
VR_LWS_LOCAL1 void varest_ws_c2l_record_index_acquire1(
 const VrLwsCustody1*,const VrLwsRowRef1*,int32_t raw_index,uint32_t source_location);
VR_LWS_LOCAL1 void varest_ws_c2l_record_init_acquire1(
 const VrLwsCustody1*,const VrLwsRowRef1*,int32_t raw_bool,uint32_t source_location);
/* Init operation key is FIXED by subkind8/11/12, never a caller op-id flag. */
VR_LWS_LOCAL1 void varest_ws_c2l_record_fd_dispose1(
 const VrLwsCustody1*,const VrLwsRowRef1*,int32_t original_fd,
 int32_t raw_close_result,int32_t saved_errno,uint32_t source_location);
VR_LWS_LOCAL1 void varest_ws_c2l_record_void_dispose_return1(
 const VrLwsCustody1*,const VrLwsRowRef1*,const void* original_pointer,
 uint64_t original_allocator_bytes,uint32_t source_location);
VR_LWS_LOCAL1 void varest_ws_c2l_record_void_dispose_uncertain_return1(
 const VrLwsCustody1*,const VrLwsRowRef1*,const void* original_pointer,
 uint64_t original_allocator_bytes,uint32_t source_location);
VR_LWS_LOCAL1 void varest_ws_c2l_record_bio_dispose_return1(
 const VrLwsCustody1*,const VrLwsRowRef1*,const void* original_pointer,
 int32_t raw_bio_free_result,uint32_t source_location);
VR_LWS_LOCAL1 void varest_ws_c2l_record_parent_release_return1(
 const VrLwsCustody1*,const VrLwsRowRef1*,const void* child_reference_identity,
 uint32_t source_location);
VR_LWS_LOCAL1 void varest_ws_c2l_record_parent_release_uncertain_return1(
 const VrLwsCustody1*,const VrLwsRowRef1*,const void* child_reference_identity,
 uint32_t source_location);
// Pure preparation: actual typed native parent serial and one EMPTY pool cell; no dispatch/retry.
VR_LWS_LOCAL1 void varest_ws_c2l_prepare_spec1(const VrLwsCustody1*, const VrLwsRowRef1* parent, uint32_t kind, uint32_t subkind, VrLwsSpec1* spec_out, VrLwsHookOutcome1* outcome_out);
// Root clarification2c5b81bc: actual same gas, one fixed named attempt; no OS.
VR_LWS_LOCAL1 void varest_ws_c2l_debit_named1(const VrLwsCustody1*,const VrLwsRowRef1*,
 const VrLwsBudgetRef1*,uint32_t operation,VrLwsHookOutcome1*);
// Pure actual row observation, no ACK/native call; same exclusive source interval.
VR_LWS_LOCAL1 void varest_ws_c2l_observe_row1(const VrLwsCustody1*,const VrLwsRowRef1*,uint32_t* local_state_out,VrLwsHookOutcome1* outcome_out);
// Genuine ALL physical/source references already retired; terminal ACK alone is insufficient.
VR_LWS_LOCAL1 void varest_ws_c2l_retire_source_row1(const VrLwsCustody1*,const VrLwsRowRef1*,VrLwsHookOutcome1*);
/* Parent helpers fix reference Extent1 internally; no arbitrary actor flag.
 * OLD12 exact. No init/index release is declared without a real inverse grant.
 * No missing/nonreturned void call is relabelled as a normal-return observation.
 */
#if defined(__cplusplus)
}
#endif
#undef VR_LWS_ASSERT1
#undef VR_LWS_LOCAL1
#endif // VAREST_LWS_CUSTODY_DECLARATIONS1
