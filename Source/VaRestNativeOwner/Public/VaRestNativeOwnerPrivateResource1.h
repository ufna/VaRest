// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestNativeOwnerAbi.h"

// Calling types only. No directly linkable producer declaration, state, owner
// election, permission flag or exported setter. Actual source/lifetime premises
// and receipt-qualified addresses precede every use. ZERO consumer callers.
// Profile2 changes label/result semantics only; the five signatures and all wire
// records remain exact. Prefix cells0..11 retain their original C1 domains.
// Pool uncertainty from RESERVED is distinct from a known acquired disposition;
// reference-release Extent0/1 is an explicit direct/parent selector, not bytes.
enum { VR_PRIVATE_RESOURCE_VERSION = 1, VR_PRIVATE_RESOURCE_PROFILE = 2, VR_PRIVATE_RESOURCE_CELLS = 140 };

typedef struct VrResourceTicket1
{
	uint32_t Size, Version;
	uint64_t OwnerEpoch, ServiceEpoch, Serial;
	uint32_t Cell;
	uint16_t Kind, Subkind;
} VrResourceTicket1;

typedef struct VrResourceSpec1
{
	uint32_t Kind, Subkind, Cell, Location;
	uint64_t ParentSerial;
} VrResourceSpec1;

typedef struct VrResourceActual1
{
	uint64_t NativeValue, Extent;
	int32_t Result, SavedError;
	uint32_t Location, Reserved;
} VrResourceActual1;

typedef struct VrResourceReply1
{
	uint32_t Size, Version, NativeCalls, Reserved;
	VrResourceTicket1 Ticket;
	uint64_t ReservedTail;
} VrResourceReply1;

typedef VrStatus (VR_CALL *VrPrivateResourceReserve1Fn)(VrOwner*, VrSession,
	const VrResourceSpec1*, uint32_t, VrResourceReply1*);
typedef VrStatus (VR_CALL *VrPrivateResourceObtained1Fn)(VrOwner*, VrSession,
	const VrResourceTicket1*, const VrResourceActual1*, uint32_t, VrResourceReply1*);
typedef VrStatus (VR_CALL *VrPrivateResourceNotObtained1Fn)(VrOwner*, VrSession,
	const VrResourceTicket1*, const VrResourceActual1*, uint32_t, VrResourceReply1*);
typedef VrStatus (VR_CALL *VrPrivateResourceClosed1Fn)(VrOwner*, VrSession,
	const VrResourceTicket1*, const VrResourceActual1*, uint32_t, VrResourceReply1*);
typedef VrStatus (VR_CALL *VrPrivateResourceUncertain1Fn)(VrOwner*, VrSession,
	const VrResourceTicket1*, const VrResourceActual1*, uint32_t, VrResourceReply1*);
