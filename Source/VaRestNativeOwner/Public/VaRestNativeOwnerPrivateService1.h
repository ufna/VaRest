// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestNativeOwnerAbi.h"

// Declaration-only private calling types. Qualified addresses and real caller/
// mapping/lifetime premises come from a separately accepted Supplier, not here.
// No directly linkable private function, state, implementation or API macro.
#ifdef __cplusplus
extern "C"
{
#endif

typedef VrStatus (VR_CALL *VrPrivateAdmitControl1Fn)(uint32_t output_bytes, VrBootstrap1* output);
typedef VrStatus (VR_CALL *VrPrivateSessionStatus1Fn)(VrOwner*, VrSession);
typedef void (VR_CALL *VrPrivateSessionVoid1Fn)(VrOwner*, VrSession);
typedef VrStatus (VR_CALL *VrPrivatePumpRegistered1Fn)(VrOwner*, VrSession, uint32_t registered);

#ifdef __cplusplus
}
#endif
