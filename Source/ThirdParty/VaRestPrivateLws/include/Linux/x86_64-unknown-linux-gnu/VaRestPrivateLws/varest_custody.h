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

/* Root clarification dca4ab6d: actual returned source observations only. */
#ifndef VAREST_LWS_CUSTODY_SOURCE_FAULTS1
#define VAREST_LWS_CUSTODY_SOURCE_FAULTS1
enum lws_vr_context_stage1 {
 LWS_VR_CONTEXT_NEVER1=0, LWS_VR_CONTEXT_CREATING1=1, LWS_VR_CONTEXT_CORE_PREFIX1=2,
 LWS_VR_CONTEXT_DESTROYING1=3, LWS_VR_CONTEXT_RETAINED1=4,
 LWS_VR_CONTEXT_NO_OBJECT1=5
};
enum VrLwsSourceFault1 {
 VR_LWS_SOURCE_FAULT_PROTOCOL1=1,
 VR_LWS_SOURCE_FAULT_NATIVE19_OBSERVED1=2
};
#endif

/* Internal SAME-C-source bookkeeping only. None of these fields appoints a
 * caller or substitutes for genuine owner/code/creating-GT/provider lifetimes.
 * Consumers including only the shared declaration guard see no internal state.
 */
#if defined(VAREST_LWS_CUSTODY_INTERNALS1) && !defined(VAREST_LWS_CUSTODY_INTERNAL_TYPES1)
#define VAREST_LWS_CUSTODY_INTERNAL_TYPES1
#define LWS_VR_LOCAL1 __attribute__((visibility("hidden")))
enum lws_vr_physical_stage1 {
 LWS_VR_EMPTY1=0, LWS_VR_RESERVED1=1, LWS_VR_ACQUIRED1=2,
 LWS_VR_NO_RESOURCE1=3, LWS_VR_FREE_RETURNED1=4,
 LWS_VR_UNKNOWN1=5, LWS_VR_RETIRED1=6, LWS_VR_FREE_ENTERED1=7, LWS_VR_UNKNOWN_ACK1=8, LWS_VR_DISPOSE_UNKNOWN1=9
};
enum lws_vr_source_location1 {
 LWS_VR_AT_CONTEXT1=1, LWS_VR_AT_SCRATCH1=2, LWS_VR_AT_POLL1=3,
 LWS_VR_AT_EVENT1=4, LWS_VR_AT_RANDOM1=5, LWS_VR_AT_PIPE1=6,
 LWS_VR_AT_VHOST1=7, LWS_VR_AT_PROTOCOL_COPY1=8,
 LWS_VR_AT_VHOST_LIST1=9, LWS_VR_NEEDS_L2_L3_1=10,
 LWS_VR_WRONG_LEGACY_ROUTE1=11, LWS_VR_AT_AH1=12, LWS_VR_AT_DATA_WSI1=13,
 LWS_VR_AT_ROLE1=14, LWS_VR_AT_RX1=15, LWS_VR_AT_CACHE1=16, LWS_VR_AT_TX1=17,
 LWS_VR_AT_STASH1=18, LWS_VR_AT_STRING1=19, LWS_VR_AT_CLIENT_FD1=20,
 LWS_VR_AT_IO1=21
};
struct lws_vr_resource1 {
 VrLwsRowRef1 Row;
 void *Pointer;
 uint64_t Bytes;
 uint32_t Kind, Subkind;
 int32_t Fd;
 uint32_t Stage;
 VrLwsHookOutcome1 LastHook;
 uint32_t ObservedNative19, AcquisitionAck, TerminalAck, Reserved;
};
struct lws_vr_sparse_fd1 { int32_t Fd; uint32_t Reserved; struct lws *Wsi; };
struct lws_vr_rng_result1 { int32_t RawResult, SavedError; uint32_t Attempted, Disposition; };
struct lws_vr_vhost_slot1 { struct lws_vhost *Object; struct lws_vr_resource1 Allocation; };
struct lws_buflist;
struct allocated_headers;
/* One external self-allocation record per actual WSI. The WSI borrows it. */
struct lws_vr_wsi_slot1 { struct lws *Object; struct lws_vr_resource1 Allocation; };
struct lws_vr_ah_slot1 {
 struct allocated_headers *Object;
 struct lws_vr_resource1 Allocation, Data;
};
struct lws_vr_io_result1 { int64_t Result; int32_t SavedError; uint32_t Operation; int32_t Detail; uint32_t DetailBytes; };
/* SAME-source owning continuations, inside already charged objects. No field
 * appoints source/code/provider authority. Raw provider diagnostics stay raw. */
struct lws_vr_tls_wsi1 {
 struct lws_vr_resource1 Ssl, Bio;
 uint32_t Step, Transferred, CleanupStep, Connected;
 int64_t Result;
 int32_t Error, Decoded;
 uint32_t ReadWant, WriteWant, NameIsIp;
};
struct lws_vr_tls_vhost1 {
 struct lws_vr_resource1 Ctx, MemBio, Local, Store[16];
 const uint8_t *Pem;
 uint32_t PemBytes, RootStage, Step, Parsed, StoreIndex, Failure;
 int64_t Result;
 uint64_t ControlBits;
 int32_t Error;
 uint32_t OptionStep;
};
struct lws_vr_data1 {
 struct lws_vr_resource1 Role, Rx, Cache[2], Tx, Stash, Strings[8], Hostname;
 uint32_t PrepareStep, ConnectStep, OptionStep, CloseStep, ReadSlot, ReadBytes;
 uint32_t Used, ForceClose, InputMask, Created, Destroyed, FdDetached;
 uint32_t ResetPending, ResetAddressBytes, ResetPathBytes, ResetHostBytes, ResetPort;
 uint32_t DeleteNotification;
 int32_t CloseReason;
 /* Sole pending frame owns ALL plaintext in Tx+LWS_PRE. No caller pointer.
  * Once encoded, ordinary trunc_* owns the SAME storage; never both. */
 uint32_t FramePending, FrameBytes, FrameWp;
 uint32_t HandshakeStep, HandshakeBytes, HandshakeYield, CloseAfterTx;
 unsigned char HandshakeNonce[16];
 struct lws_vr_tls_wsi1 Tls;
 uint32_t RouteStep, RouteBytes, RouteSent, RouteReceived;
 unsigned char RouteReply[64];
};
struct lws_vr_context1 {
 const VrLwsCustody1 *View; /* ORIGINAL external holder View; never a copy. */
 /* Borrowed only while a genuine tracked topcall is executing. Cleared before
  * EVERY complete topcall return; never a stored/replenished phase budget. */
 const VrLwsBudgetRef1 *Budget;
 struct lws_vr_resource1 Self, Scratch, Poll, Event, Random, Wake[2];
 struct lws_vr_vhost_slot1 Vhosts[2];
 struct lws_vr_ah_slot1 Headers[2];
 struct lws_vr_wsi_slot1 Data[2];
 struct lws_vr_sparse_fd1 Sparse[3];
 struct lws_vr_rng_result1 RandomResult;
 struct lws_vr_io_result1 Io;
 uint32_t Stage, VhostAttempts, CoreComplete, ServiceCursor, Dispatching, PendingCancel;
 uint32_t CancelVhost;
 int32_t LastDisposition;
 uint32_t IoDeferred; /* actual current dispatch denial, never a quota or permission */
 /* Zero by actual context calloc before publication. All concurrent accesses
  * use atomics; independent of service-only PendingCancel callback cursor. */
 uint32_t ForeignWakePending;
};
struct lws_vr_vhost1 {
 struct lws_vr_resource1 Protocols, SameProtocolList;
 uint32_t PrefixOnly, PrepareStep, ProtocolInit, ProtocolDestroy;
 struct lws_vr_tls_vhost1 Tls;
};
LWS_VR_LOCAL1 void lws_vr_resource_init1(struct lws_vr_resource1 *);
LWS_VR_LOCAL1 int lws_vr_reserve1(const VrLwsCustody1 *,const VrLwsRowRef1 *,
 uint32_t,uint32_t,const VrLwsBudgetRef1 *,struct lws_vr_resource1 *);
LWS_VR_LOCAL1 int lws_vr_ack_retire1(const VrLwsCustody1 *,const VrLwsBudgetRef1 *,struct lws_vr_resource1 *);
LWS_VR_LOCAL1 int lws_vr_never1(const VrLwsCustody1 *,const VrLwsBudgetRef1 *,struct lws_vr_resource1 *,uint32_t);
LWS_VR_LOCAL1 int lws_vr_debit1(const VrLwsCustody1 *,const VrLwsBudgetRef1 *,struct lws_vr_resource1 *,uint32_t);
LWS_VR_LOCAL1 int lws_vr_allocate1(const VrLwsCustody1 *,const VrLwsRowRef1 *,uint32_t,uint32_t,
 uint64_t,const VrLwsBudgetRef1 *,struct lws_vr_resource1 *,uint32_t);
LWS_VR_LOCAL1 int lws_vr_free1(const VrLwsCustody1 *,const VrLwsBudgetRef1 *,struct lws_vr_resource1 *,uint32_t);
LWS_VR_LOCAL1 int lws_vr_close1(const VrLwsCustody1 *,const VrLwsBudgetRef1 *,struct lws_vr_resource1 *,uint32_t);
LWS_VR_LOCAL1 int lws_vr_random_open1(struct lws_context *,const VrLwsBudgetRef1 *);
LWS_VR_LOCAL1 int lws_vr_pipe_create1(struct lws *);
LWS_VR_LOCAL1 int lws_vr_pipe_close1(struct lws *);
LWS_VR_LOCAL1 int lws_vr_event_create1(struct lws_context *);
LWS_VR_LOCAL1 int lws_vr_event_destroy1(struct lws *);
LWS_VR_LOCAL1 struct lws *lws_vr_lookup1(const struct lws_context *,int32_t);
LWS_VR_LOCAL1 int lws_vr_insert_fd1(struct lws_context *,struct lws *);
LWS_VR_LOCAL1 int lws_vr_remove_fd1(struct lws *);
LWS_VR_LOCAL1 int lws_vr_delete_fd1(struct lws_context *,int32_t);
LWS_VR_LOCAL1 int lws_vr_source_result1(const VrLwsCustody1 *,VrLwsResult1 *,int,void *,uint32_t,uint32_t);
#define LWS_VR_CALLBACK_DEFERRED1 (-2147483647)
LWS_VR_LOCAL1 int lws_vr_tracked1(const struct lws_context *);
LWS_VR_LOCAL1 int lws_vr_callback1(struct lws *,enum lws_callback_reasons,void *,void *,size_t);
LWS_VR_LOCAL1 int lws_vr_io1(struct lws *,uint32_t);
LWS_VR_LOCAL1 void lws_vr_io_record1(struct lws_context *,uint32_t,int64_t,int32_t);
LWS_VR_LOCAL1 uint32_t lws_vr_pending_context1(const struct lws_context *);
LWS_VR_LOCAL1 int lws_vr_ah_prepare1(struct lws_context *,const VrLwsBudgetRef1 *);
LWS_VR_LOCAL1 int lws_vr_ah_cleanup1(struct lws_context *,const VrLwsBudgetRef1 *);
LWS_VR_LOCAL1 int lws_vr_data_prepare1(struct lws_context *,unsigned int,struct lws_vhost *,const VrLwsBudgetRef1 *);
LWS_VR_LOCAL1 int lws_vr_data_cleanup1(struct lws_context *,unsigned int,const VrLwsBudgetRef1 *);
LWS_VR_LOCAL1 int lws_vr_data_valid1(const struct lws *);
LWS_VR_LOCAL1 int lws_vr_client_progress1(struct lws *,short);
LWS_VR_LOCAL1 int lws_vr_socket_options1(struct lws *);
LWS_VR_LOCAL1 int lws_vr_client_fd_dispose1(struct lws *,const VrLwsBudgetRef1 *);
LWS_VR_LOCAL1 int lws_vr_request_close1(struct lws *,enum lws_close_status);
LWS_VR_LOCAL1 int lws_vr_close_progress1(struct lws *);
LWS_VR_LOCAL1 int lws_vr_read_claim1(struct lws *);
LWS_VR_LOCAL1 void lws_vr_read_release1(struct lws *);
LWS_VR_LOCAL1 int lws_vr_buf_append1(struct lws *,struct lws_buflist **,const uint8_t *,size_t);
LWS_VR_LOCAL1 size_t lws_vr_buf_next1(struct lws *,struct lws_buflist **,uint8_t **);
LWS_VR_LOCAL1 int lws_vr_buf_use1(struct lws *,struct lws_buflist **,size_t);
LWS_VR_LOCAL1 void lws_vr_buf_destroy1(struct lws *,struct lws_buflist **);
LWS_VR_LOCAL1 int lws_vr_issue_raw1(struct lws *,unsigned char *,size_t);
LWS_VR_LOCAL1 int lws_vr_plain_service1(struct lws_context *,int32_t);
/* SAME-C-source progress only: -1 real failure,0 no-attempt yield,1 queued. */
LWS_VR_LOCAL1 int lws_vr_frame_progress1(struct lws *);
LWS_VR_LOCAL1 int lws_vr_ref_ack1(const VrLwsCustody1*,const VrLwsBudgetRef1*,struct lws_vr_resource1*);
LWS_VR_LOCAL1 int lws_vr_tls_vhost_create1(struct lws_vhost*,const struct lws_context_creation_info*);
LWS_VR_LOCAL1 int lws_vr_tls_vhost_cleanup1(struct lws_vhost*,const VrLwsBudgetRef1*);
LWS_VR_LOCAL1 int lws_vr_tls_wsi_create1(struct lws*);
LWS_VR_LOCAL1 int lws_vr_tls_wsi_cleanup1(struct lws*,const VrLwsBudgetRef1*);
LWS_VR_LOCAL1 int lws_vr_local_route1(struct lws*,short);


#endif
