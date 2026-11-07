// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

// The one place VaRestWebSocket includes the engine-bundled libwebsockets 3.0 and its OpenSSL, as the engine's WebSockets
// module does (Runtime/Online/WebSockets/Private/Lws/LwsWebSocketsManager.h:14-24) and the W0 probe proved
// (Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeLws.h): Windows types allowed around the headers (libwebsockets.h:52-61
// pulls in winsock2.h there), inside the third-party warning scope. Never included from a public header.
//
// UI is renamed for the headers: OpenSSL's typedef struct ui_st UI (OpenSSL/1.1.1t/include/Unix/openssl/ossl_typ.h:144)
// collides with CoreUObject's namespace UI (UObject/ObjectMacros.h:965), which this module's UENUM headers bring in. The
// engine does the same where it includes OpenSSL next to UObject code (Runtime/Online/BuildPatchServices/Private/Common/Crypto.cpp:6-12).

#include "CoreMinimal.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#endif

#define UI UI_ST
THIRD_PARTY_INCLUDES_START
#if defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
#if defined(LIBWEBSOCKET_H_3060898B846849FF9F88F5DB59B5950C)
#error "An LWS header preceded the private VaRest LWS include; inspect the actual PCH/unity include closure."
#endif
#include "VaRestPrivateLws/varest_external_names.h"
#include "VaRestPrivateLws/libwebsockets.h"
#else
#include "libwebsockets.h"
#endif
#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>
THIRD_PARTY_INCLUDES_END
#undef UI

#if PLATFORM_WINDOWS
#include "Windows/HideWindowsPlatformTypes.h"
#endif
