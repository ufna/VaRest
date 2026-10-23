// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/ValueOrError.h"
#include "VaRestSocketTransport.h"

namespace VaRest::WebSocket
{
	/** What a connect request becomes before the service thread sees it (WebSocket spec 2.2) */
	struct FSocketTarget
	{
		bool bTls = false;
		/** wss+insecure://, or wss:// with bAllowSelfSignedTls; never under Shipping rules (spec 2.2, 15) */
		bool bInsecure = false;
		/** The host as TCP and the TLS name check take it: no brackets, no port */
		FString Host;
		bool bHostIsIp = false;
		int32 Port = 0;
		/** The Host header: the host ([...] for IPv6) and a port that is not the scheme's default (LwsWebSocket.cpp:775-785) */
		FString HostHeader;
		/** From '/' on; bytes past ASCII, spaces and controls percent-encoded (RFC 3987 3.1) */
		FString PathAndQuery;
		/** Empty: no Origin header */
		FString Origin;
		/** The offered subprotocols joined with ", "; empty: none */
		FString Protocols;
		/** "Name: value\r\n" for each header, ASCII */
		TArray<uint8> HeaderBlock;
		/** What the engine's URL filter sees: scheme (wss for wss+insecure) and host, never the path or query (plan decision 15) */
		FString FilterUrl;
	};

	struct FTargetError
	{
		EVaRestSocketFailure Failure = EVaRestSocketFailure::BadUrl;
		/** Never quotes a header value, the path or the query: it reaches logs */
		FString Message;
	};

	/** The most header bytes VaRest appends: the 32 KiB handshake buffer (spec 15), which LWS's own lines share -- the exact room is checked again in APPEND_HANDSHAKE_HEADER */
	inline constexpr int32 MaxHeaderBlockBytes = 32 * 1024;

	/**
	 * Any thread: Request's URL, headers, subprotocols and origin, checked and encoded (spec 2.2; plan decisions 13, 14).
	 * bShippingRules: wss+insecure is refused and bAllowSelfSignedTls ignored, as in a Shipping build.
	 */
	TValueOrError<FSocketTarget, FTargetError> PrepareTarget(const FVaRestSocketConnectRequest& Request, bool bShippingRules = UE_BUILD_SHIPPING != 0);

	/** Four decimal octets, 0-255 each */
	bool IsIpv4Literal(FStringView Host);

	/**
	 * Spec 2.2 (LwsWebSocket.cpp:112-122): the engine's URL request filter (URLRequestFilter.h:28-70) for FilterUrl, from
	 * [Online.HttpManager <scheme>] in GEngineIni -- game thread -- or, for tests, from Allowlist (plan decision 19).
	 */
	bool IsAllowedByFilter(const FString& FilterUrl, const TArray<TPair<FString, TArray<FString>>>* Allowlist = nullptr);
}
