// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

// The one place the probe includes the engine-bundled libwebsockets 3.0 and its OpenSSL, the way the engine's WebSockets
// module does (Runtime/Online/WebSockets/Private/Lws/LwsWebSocketsManager.h:14-24): Windows types allowed around the
// headers (libwebsockets.h:52-61 pulls in winsock2.h there), inside the third-party warning scope. libwebsockets.h
// includes openssl/ssl.h itself (libwebsockets.h:221); the others are for the probe's own TLS checks.
//
// UI is renamed for the headers: OpenSSL's typedef struct ui_st UI (OpenSSL/1.1.1t/include/Unix/openssl/ossl_typ.h:144)
// collides with CoreUObject's namespace UI (UObject/ObjectMacros.h:965), which is already in scope wherever the probe's
// header includes this one, unity or not: VaRestLwsProbe.h includes VaRestJson.h first, whose line 7 includes
// UObject/Class.h, which includes ObjectMacros.h (Class.h:57). The engine does the same where it includes OpenSSL next
// to UObject code (Runtime/Online/BuildPatchServices/Private/Common/Crypto.cpp:6-12); the engine's WebSockets module does
// not need it because it depends on Core and HTTP only (WebSockets.Build.cs:92-97).

#include "CoreMinimal.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#endif

#define UI UI_ST
THIRD_PARTY_INCLUDES_START
#include "libwebsockets.h"
#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
THIRD_PARTY_INCLUDES_END
#undef UI

#if PLATFORM_WINDOWS
#include "Windows/HideWindowsPlatformTypes.h"
#endif
