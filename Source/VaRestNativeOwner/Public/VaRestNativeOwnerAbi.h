// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define VR_CALL __cdecl
#else
#define VR_CALL
#endif

// Permanent bootstrap1 / owner2. These records contain process-local POD only.
// Source: ws-b9-native-owner-design-escrow2-frozen.md sections3 and5.
typedef int32_t VrStatus;
typedef struct VrOwner VrOwner;

enum
{
	VR_STATUS_OK = 0,
	VR_STATUS_PENDING = 1,
	VR_STATUS_BUSY = 2,
	VR_STATUS_CAPACITY = 3,
	VR_STATUS_OLD_OWNER_BUSY = 4,
	VR_STATUS_WRONG_THREAD = 5,
	VR_STATUS_UNBOUND_CONTROL = 6,
	VR_STATUS_DISCOVERY_UNESTABLISHED = 7,
	VR_STATUS_SCAN_LIMIT = 8,
	VR_STATUS_PROVENANCE = 9,
	VR_STATUS_ABI_INCOMPATIBLE = 10,
	VR_STATUS_STALE = 11,
	VR_STATUS_EXPIRED = 12,
	VR_STATUS_NATIVE_ERROR = 13,
	VR_STATUS_POISONED = 14,
	VR_STATUS_FORK_UNESTABLISHED = 15,
	VR_STATUS_INVALID_INPUT = 16,
	VR_STATUS_NO_RESULT = 17,
	VR_STATUS_NAMESPACE_INELIGIBLE = 18,
	VR_STATUS_RELEASE_UNCERTAIN = 19
};

enum
{
	VR_BOOTSTRAP_BYTES = 112,
	VR_BOOTSTRAP_VERSION = 1,
	VR_OWNER_VERSION = 2,
	VR_POINTER_BITS = 64,
	VR_ENDIAN_LITTLE = 1,
	VR_MAX_SLOTS = 2,
	VR_MAX_ADDRESSES = 8,
	VR_MAX_HOSTNAME_BYTES = 255,
	VR_BOUND_CONTROL = 0x01,
	VR_CONTROL_BUSY = 0x02,
	VR_RETIRED = 0x04,
	VR_POISONED = 0x08,
	VR_BOOTSTRAP_FLAGS_MASK = 0x0f
};

#define VR_BOOTSTRAP_MAGIC UINT64_C(0x56524e4f574e5231)

enum
{
	VR_STATE_INVALID = 0,
	VR_STATE_UNOWNED = 1,
	VR_STATE_ACTIVE = 2,
	VR_STATE_DORMANT = 3,
	VR_STATE_RETIRING = 4,
	VR_STATE_RETIRED = 5,
	VR_STATE_POISONED = 6
};

enum
{
	VR_POISON_NONE = 0,
	VR_POISON_THREAD_RETIRE = 1,
	VR_POISON_HANDLE_CLOSE = 2,
	VR_POISON_WSA_CLEANUP = 3,
	VR_POISON_CONNECTOR_ACCOUNTING = 4,
	VR_POISON_CLOCK = 5,
	VR_POISON_GENERATION = 6,
	VR_POISON_ARENA_RELEASE = 7,
	VR_POISON_LOADER_RELEASE = 8,
	VR_POISON_NATIVE_INVARIANT = 9
};

enum
{
	VR_PLATFORM_NONE = 0,
	VR_PLATFORM_WINDOWS_X64 = 1,
	VR_PLATFORM_GNU_LINUX_X64 = 2,
	VR_PLATFORM_MACOS64 = 3,
	VR_PLATFORM_IOS64 = 4,
	VR_PLATFORM_ANDROID64 = 5,
	VR_PIN_NONE = 0,
	VR_PIN_OWNED_WIN_MODULE = 1,
	VR_PIN_OWNED_GNU_DLOPEN = 2,
	VR_PIN_PROCESS_LIFETIME = 3,
	VR_ADDRESS_IPV4 = 4,
	VR_ADDRESS_IPV6 = 6
};

typedef struct { uint64_t hi, lo; } VrImageId;
typedef struct { uint64_t owner_epoch, service_epoch; } VrSession;
typedef struct
{
	uint64_t owner_epoch, service_epoch, slot_generation;
	uint32_t slot, reserved;
} VrTicket;
typedef struct
{
	uint32_t size, version, family, scope;
	uint16_t port_be, reserved16;
	uint8_t address[16];
	uint8_t reserved8[4];
} VrAddress;
typedef struct
{
	uint32_t size, version, count, resolver_error;
	VrTicket ticket;
	VrAddress address[VR_MAX_ADDRESSES];
} VrResult;
typedef struct
{
	uint32_t size, version;
	uint64_t deadline_ns;
	VrSession session;
	uint32_t hostname_bytes, flags;
	uint16_t port_be, reserved16;
	uint32_t reserved;
	char hostname[VR_MAX_HOSTNAME_BYTES + 1];
} VrResolve;
typedef struct
{
	uint32_t size, version, state, occupied_slots;
	uint64_t owner_epoch, service_epoch;
	VrImageId image;
	uint32_t connector_leased, poison_reason, service_executing, reserved;
} VrSnapshot;
typedef struct
{
	uint32_t size, version, platform, kind;
	uint64_t value;
	VrImageId image;
} VrPin;
typedef struct
{
	uint32_t size, version;
	VrImageId successor;
	uint64_t expected_epoch;
} VrRetire;
typedef struct
{
	uint32_t size, version;
	VrPin released_owner_pin;
	uint64_t retired_epoch;
} VrRetired;

typedef struct VrOwnerApi2
{
	uint32_t size, version;
	VrStatus (VR_CALL *query)(VrOwner*, VrSnapshot*);
	VrStatus (VR_CALL *poll_retirement)(VrOwner*, VrSnapshot*);
	VrStatus (VR_CALL *attach_service)(VrOwner*, VrSession*);
	VrStatus (VR_CALL *detach_service)(VrOwner*, VrSession);
	VrStatus (VR_CALL *submit)(VrOwner*, const VrResolve*, VrTicket*);
	VrStatus (VR_CALL *copy_result)(VrOwner*, VrTicket, VrResult*);
	VrStatus (VR_CALL *discard)(VrOwner*, VrTicket);
	VrStatus (VR_CALL *retire)(VrOwner*, const VrRetire*, VrRetired*);
} VrOwnerApi2;

typedef struct
{
	uint32_t size, version;
	uint64_t magic;
	uint32_t pointer_bits, endian, owner_abi, flags;
	VrImageId image;
	uint8_t build_id[32];
	VrOwner* owner;
	const VrOwnerApi2* api;
	uint64_t reserved[2];
} VrBootstrap1;

// Private descriptor storage, never published through bootstrap reserved bytes.
enum
{
	VR_PHASE_INVALID = 0,
	VR_PHASE_UNOWNED = 1,
	VR_PHASE_ACTIVE = 2,
	VR_PHASE_DORMANT = 3,
	VR_PHASE_RETIRING = 4,
	VR_PHASE_RETIRED = 5,
	VR_PHASE_POISONED = 6,
	VR_PHASE_PREPARED = 7,
	VR_PHASE_ESCROW = 8,
	VR_PHASE_CLOSE_IN_FLIGHT = 9,
	VR_PHASE_RELEASE_UNCERTAIN = 10,
	VR_DISPOSITION_NONE = 0,
	VR_DISPOSITION_PREPARED = 1,
	VR_DISPOSITION_TRANSFERRED = 2,
	VR_DISPOSITION_ATTEMPTING = 3,
	VR_DISPOSITION_CONSUMED = 4,
	VR_DISPOSITION_UNCERTAIN = 5,
	VR_OLD_NONE = 0,
	VR_OLD_OWNED = 1,
	VR_OLD_UNCERTAIN = 2,
	VR_OLD_IN_FLIGHT = 3
};

typedef struct
{
	uint32_t size, version, phase, disposition;
	uint32_t self_owned, old_state, close_attempted, reserved32;
	VrPin self_pin, old_pin;
	VrImageId expected_old;
	uint64_t expected_old_epoch, transfer_generation, reserved64[2];
} VrInlineEscrow2;

#ifdef __cplusplus
extern "C" {
#endif
VrStatus VR_CALL VaRestNativeOwnerBootstrap1(uint32_t output_bytes, VrBootstrap1* output);
#ifdef __cplusplus
}

#include <type_traits>

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "native owner ABI2 requires little endian");
#elif !defined(_WIN32)
#error Native owner ABI2 requires a known target byte order
#endif

static_assert(sizeof(void*) == 8 && sizeof(uintptr_t) == 8 && sizeof(VrStatus) == 4);
static_assert(sizeof(uint32_t) == 4 && sizeof(uint64_t) == 8);
static_assert(sizeof(&VaRestNativeOwnerBootstrap1) == 8);

#define VR_ABI_RECORD(Type, Bytes, Alignment) \
	static_assert(sizeof(Type) == Bytes && alignof(Type) == Alignment); \
	static_assert(std::is_standard_layout_v<Type> && std::is_trivially_copyable_v<Type>)
#define VR_ABI_OFFSET(Type, Field, Offset) static_assert(offsetof(Type, Field) == Offset)

VR_ABI_RECORD(VrImageId, 16, 8);
VR_ABI_RECORD(VrSession, 16, 8);
VR_ABI_RECORD(VrTicket, 32, 8);
VR_ABI_RECORD(VrAddress, 40, 4);
VR_ABI_RECORD(VrResult, 368, 8);
VR_ABI_RECORD(VrResolve, 304, 8);
VR_ABI_RECORD(VrSnapshot, 64, 8);
VR_ABI_RECORD(VrPin, 40, 8);
VR_ABI_RECORD(VrRetire, 32, 8);
VR_ABI_RECORD(VrRetired, 56, 8);
VR_ABI_RECORD(VrOwnerApi2, 72, 8);
VR_ABI_RECORD(VrBootstrap1, 112, 8);
VR_ABI_RECORD(VrInlineEscrow2, 160, 8);
VR_ABI_OFFSET(VrTicket, slot, 24);
VR_ABI_OFFSET(VrTicket, reserved, 28);
VR_ABI_OFFSET(VrAddress, port_be, 16);
VR_ABI_OFFSET(VrAddress, address, 20);
VR_ABI_OFFSET(VrAddress, reserved8, 36);
VR_ABI_OFFSET(VrResult, ticket, 16);
VR_ABI_OFFSET(VrResult, address, 48);
VR_ABI_OFFSET(VrResolve, deadline_ns, 8);
VR_ABI_OFFSET(VrResolve, session, 16);
VR_ABI_OFFSET(VrResolve, hostname_bytes, 32);
VR_ABI_OFFSET(VrResolve, flags, 36);
VR_ABI_OFFSET(VrResolve, port_be, 40);
VR_ABI_OFFSET(VrResolve, reserved, 44);
VR_ABI_OFFSET(VrResolve, hostname, 48);
VR_ABI_OFFSET(VrSnapshot, owner_epoch, 16);
VR_ABI_OFFSET(VrSnapshot, image, 32);
VR_ABI_OFFSET(VrSnapshot, connector_leased, 48);
VR_ABI_OFFSET(VrSnapshot, reserved, 60);
VR_ABI_OFFSET(VrPin, size, 0);
VR_ABI_OFFSET(VrPin, version, 4);
VR_ABI_OFFSET(VrPin, platform, 8);
VR_ABI_OFFSET(VrPin, kind, 12);
VR_ABI_OFFSET(VrPin, value, 16);
VR_ABI_OFFSET(VrPin, image, 24);
VR_ABI_OFFSET(VrRetire, successor, 8);
VR_ABI_OFFSET(VrRetire, expected_epoch, 24);
VR_ABI_OFFSET(VrRetired, released_owner_pin, 8);
VR_ABI_OFFSET(VrRetired, retired_epoch, 48);
VR_ABI_OFFSET(VrOwnerApi2, size, 0);
VR_ABI_OFFSET(VrOwnerApi2, version, 4);
VR_ABI_OFFSET(VrOwnerApi2, query, 8);
VR_ABI_OFFSET(VrOwnerApi2, poll_retirement, 16);
VR_ABI_OFFSET(VrOwnerApi2, attach_service, 24);
VR_ABI_OFFSET(VrOwnerApi2, detach_service, 32);
VR_ABI_OFFSET(VrOwnerApi2, submit, 40);
VR_ABI_OFFSET(VrOwnerApi2, copy_result, 48);
VR_ABI_OFFSET(VrOwnerApi2, discard, 56);
VR_ABI_OFFSET(VrOwnerApi2, retire, 64);
VR_ABI_OFFSET(VrBootstrap1, size, 0);
VR_ABI_OFFSET(VrBootstrap1, version, 4);
VR_ABI_OFFSET(VrBootstrap1, magic, 8);
VR_ABI_OFFSET(VrBootstrap1, pointer_bits, 16);
VR_ABI_OFFSET(VrBootstrap1, endian, 20);
VR_ABI_OFFSET(VrBootstrap1, owner_abi, 24);
VR_ABI_OFFSET(VrBootstrap1, flags, 28);
VR_ABI_OFFSET(VrBootstrap1, image, 32);
VR_ABI_OFFSET(VrBootstrap1, build_id, 48);
VR_ABI_OFFSET(VrBootstrap1, owner, 80);
VR_ABI_OFFSET(VrBootstrap1, api, 88);
VR_ABI_OFFSET(VrBootstrap1, reserved, 96);
VR_ABI_OFFSET(VrInlineEscrow2, size, 0);
VR_ABI_OFFSET(VrInlineEscrow2, version, 4);
VR_ABI_OFFSET(VrInlineEscrow2, phase, 8);
VR_ABI_OFFSET(VrInlineEscrow2, disposition, 12);
VR_ABI_OFFSET(VrInlineEscrow2, self_owned, 16);
VR_ABI_OFFSET(VrInlineEscrow2, old_state, 20);
VR_ABI_OFFSET(VrInlineEscrow2, close_attempted, 24);
VR_ABI_OFFSET(VrInlineEscrow2, reserved32, 28);
VR_ABI_OFFSET(VrInlineEscrow2, self_pin, 32);
VR_ABI_OFFSET(VrInlineEscrow2, old_pin, 72);
VR_ABI_OFFSET(VrInlineEscrow2, expected_old, 112);
VR_ABI_OFFSET(VrInlineEscrow2, expected_old_epoch, 128);
VR_ABI_OFFSET(VrInlineEscrow2, transfer_generation, 136);
VR_ABI_OFFSET(VrInlineEscrow2, reserved64, 144);

#undef VR_ABI_RECORD
#undef VR_ABI_OFFSET
#endif
