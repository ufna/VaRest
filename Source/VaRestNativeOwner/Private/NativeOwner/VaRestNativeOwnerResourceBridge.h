// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once
#include "VaRestNativeOwnerPrivateResource1.h"

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
// Producer-only names. Consumers may use qualified calling values, never link
// these symbols. Admission remains absent; the owner public table stays eight.
extern "C"
{
	__attribute__((visibility("hidden"))) VrStatus VR_CALL VrPrivateResourceReserve1(VrOwner*, VrSession, const VrResourceSpec1*, uint32_t, VrResourceReply1*);
	__attribute__((visibility("hidden"))) VrStatus VR_CALL VrPrivateResourceObtained1(VrOwner*, VrSession, const VrResourceTicket1*, const VrResourceActual1*, uint32_t, VrResourceReply1*);
	__attribute__((visibility("hidden"))) VrStatus VR_CALL VrPrivateResourceNotObtained1(VrOwner*, VrSession, const VrResourceTicket1*, const VrResourceActual1*, uint32_t, VrResourceReply1*);
	__attribute__((visibility("hidden"))) VrStatus VR_CALL VrPrivateResourceClosed1(VrOwner*, VrSession, const VrResourceTicket1*, const VrResourceActual1*, uint32_t, VrResourceReply1*);
	__attribute__((visibility("hidden"))) VrStatus VR_CALL VrPrivateResourceUncertain1(VrOwner*, VrSession, const VrResourceTicket1*, const VrResourceActual1*, uint32_t, VrResourceReply1*);
}
#endif
