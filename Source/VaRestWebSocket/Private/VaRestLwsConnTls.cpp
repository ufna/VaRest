// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestLwsConn.h"

int32 VaRest::WebSocket::Lws::FConn::OnVerifyServerCertificate(FService& Service, X509_STORE_CTX* Store, int32 PreverifyOk)
{
	if (State == EConnState::Ended)
	{
		// Review conc I-1: an attempt VaRest already ended finishes no TLS handshake (Task 7's veto in APPEND is the other half)
		return 1;
	}
	if (!Target.bTls || AppendCalls > 0)
	{
		// A redirect's TLS handshake: a ws:// attempt has none of its own, and a wss:// attempt's own comes before its request
		// was built (APPEND_HANDSHAKE_HEADER) -- so LWS is connecting to a redirect's target [hs.redirect_to_tls]. Refused
		// before that handshake ends, so no request reaches the target (ruling Q2, plan decision 10). It also refuses a TLS 1.2
		// renegotiation after the request, which OpenSSL 1.1.1 clients accept by default (review conc M-14): rare, noted in
		// decision 10
		PendingFailure = EVaRestSocketFailure::Redirect;
		PendingDetail = TEXT("the server answered the upgrade with a redirect, which VaRest does not follow (ruling Q2)");
		return 1;
	}
	// OpenSSL's verdict stands (libwebsockets.h:1015-1044); Task 13 adds VaRest's checks
	return 0;
}
