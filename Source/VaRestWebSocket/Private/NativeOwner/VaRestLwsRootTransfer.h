// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "Connector/VaRestNumericConnectorResource1.h"

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
namespace VaRest::WebSocket::Lws
{
// Actual creating frame owns the service/code/manager through COMPLETE return.
// No instance, retained callback, new actor or source-authority packet.
class [[gnu::visibility("hidden")]] FTrackedEngineRoots1 final
{
public:
    static int32_t Install(lws_vhost*, ssl_ctx_st*, const VrLwsCustody1*,
        const VrLwsRowRef1*, VrLwsPhaseBudget1*, uint32_t*, VrLwsResult1*);
    // Return is SourceDisposition, not NativeStatus. Hook fields are pure:
    // COMPLETED/LOCAL_INVALID/PROTOCOL_FAULT, native status/calls/reserved zero.
    // Unvalidated outputs remain untouched; safe refusal writes null/zero only.
    // Actual immutable Service/backing/code survives every borrowing BIO return.
    static int32_t BorrowExtraTrustedPem(lws_vhost*, ssl_ctx_st*, const VrLwsCustody1*,
        const VrLwsRowRef1*, const uint8_t**, uint32_t*, VrLwsHookOutcome1*);
};
}
#endif
