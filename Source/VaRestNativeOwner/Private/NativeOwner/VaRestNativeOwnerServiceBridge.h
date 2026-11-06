// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestNativeOwnerPrivateService1.h"

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)

// Producer-only declarations: real admission plus eight service entries.
// Consumers use source/receipt-qualified typed values, never link these names.
#ifdef __cplusplus
extern "C"
{
#endif

__attribute__((visibility("hidden"))) VrStatus VR_CALL VrPrivateAdmitCurrentControl1(uint32_t output_bytes, VrBootstrap1* output);
__attribute__((visibility("hidden"))) VrStatus VR_CALL VrPrivateServiceNeverCreated1(VrOwner*, VrSession);
__attribute__((visibility("hidden"))) VrStatus VR_CALL VrPrivateRegisterCurrentServiceThread1(VrOwner*, VrSession);
__attribute__((visibility("hidden"))) VrStatus VR_CALL VrPrivateServiceJoinObserved1(VrOwner*, VrSession);
__attribute__((visibility("hidden"))) void VR_CALL VrPrivateRequestStop1(VrOwner*, VrSession);
__attribute__((visibility("hidden"))) VrStatus VR_CALL VrPrivatePumpRegistered1(VrOwner*, VrSession, uint32_t);
__attribute__((visibility("hidden"))) VrStatus VR_CALL VrPrivatePumpEnter1(VrOwner*, VrSession);
__attribute__((visibility("hidden"))) void VR_CALL VrPrivatePumpLeave1(VrOwner*, VrSession);
__attribute__((visibility("hidden"))) void VR_CALL VrPrivateRequestDetachAfterJoin1(VrOwner*, VrSession);

#ifdef __cplusplus
}
#endif

#endif
