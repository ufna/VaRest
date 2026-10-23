// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestLwsConn.h"

#include "Interfaces/ISslCertificateManager.h"

bool VaRest::WebSocket::Lws::CertificateMatchesIp(X509* Certificate, const FString& Ip)
{
	// X509_check_ip_asc compares the iPAddress SAN entries (OpenSSL 1.1.1t include/openssl/x509v3.h:714): 1 a match, 0 none, < 0 not an address
	return Certificate && X509_check_ip_asc(Certificate, TCHAR_TO_UTF8(*Ip), 0) == 1;
}

int32 VaRest::WebSocket::Lws::EvaluateServerCertificate(const FSocketTarget& Target, ISslCertificateManager* Certificates, X509_STORE_CTX* Store, int32 PreverifyOk, FString& OutDetail)
{
	int32 Ok = PreverifyOk;
	const int32 Error = X509_STORE_CTX_get_error(Store);
	const int32 Depth = X509_STORE_CTX_get_error_depth(Store);
	if (!Ok && !Target.bInsecure && Error == X509_V_ERR_HOSTNAME_MISMATCH && Depth == 0 && Target.bHostIsIp
		&& CertificateMatchesIp(X509_STORE_CTX_get_current_cert(Store), Target.Host))
	{
		// VaRest-sjj (plan decision 9, ruling Q3): LWS 3.0 checks an IP host as a DNS name and fails it even with that IP in the
		// SAN [tls.hostname_ip_literal]; the SAN's IP entries are checked here instead. Only this error, at the leaf, for an IP
		// host, when the leaf has that IP: a chain error comes before it and stands, a wrong IP or a DNS-only SAN stands, and
		// OpenSSL's later checks (signatures, dates) still run and fail on their own errors. LWS keeps a verdict the callback
		// reset to X509_V_OK and returned 0 for (libwebsockets.h:1028-1035)
		X509_STORE_CTX_set_error(Store, X509_V_OK);
		Ok = 1;
	}
	if (!Ok)
	{
		OutDetail = FString::Printf(TEXT("the server's certificate was refused: X509 error %d at depth %d (%s)"), Error, Depth, UTF8_TO_TCHAR(X509_verify_cert_error_string(Error)));
		return 1;
	}
	if (Target.bInsecure)
	{
		// LWS consumes the errors allowed by its flags before this callback (openssl-client.c.o,
		// OpenSSL_client_verify_callback 0x23f-0x267, 0x2ca-0x2f0). Remaining errors must reach the refusal above;
		// only successful insecure verification bypasses pins. Our flags do not include ALLOW_EXPIRED (review I-1).
		return 0;
	}
	// The certificate manager's own check (pinned keys) on what OpenSSL accepted, for the host without its port: the engine's
	// step (LwsWebSocket.cpp:549-572; ISslCertificateManager.h:58-66)
	if (Certificates && !Certificates->VerifySslCertificates(Store, Target.Host))
	{
		OutDetail = FString::Printf(TEXT("the certificate manager refused the chain for %s (including pinned keys)"), *Target.Host);
		return 1;
	}
	return 0;
}

int32 VaRest::WebSocket::Lws::FConn::OnVerifyServerCertificate(FService& Service, X509_STORE_CTX* Store, int32 PreverifyOk)
{
	if (State == EConnState::Ended)
	{
		// Review conc I-1: an attempt VaRest already ended finishes no TLS handshake
		return 1;
	}
	if (!Target.bTls || AppendCalls > 0)
	{
		// A redirect's TLS handshake: a ws:// attempt has none of its own, and a wss:// attempt's own comes before its request
		// was built (APPEND_HANDSHAKE_HEADER) -- so LWS is connecting to a redirect's target [hs.redirect_to_tls]. Refused
		// before that handshake ends, so no request reaches the target (ruling Q2, plan decision 10). It also refuses TLS 1.2
		// renegotiation after the request (decision 10, review conc M-14)
		PendingFailure = EVaRestSocketFailure::Redirect;
		PendingDetail = TEXT("the server answered the upgrade with a redirect, which VaRest does not follow; refused at the redirect TLS handshake (ruling Q2)");
		return 1;
	}
	FString Detail;
	const int32 Verdict = EvaluateServerCertificate(Target, Service.GetCertificates(), Store, PreverifyOk, Detail);
	if (Verdict != 0)
	{
		// Terminal (ruling Q3, spec 3.4 as Task 14 amends it): W2 never retries it
		PendingFailure = EVaRestSocketFailure::TlsVerification;
		PendingDetail = MoveTemp(Detail);
	}
	return Verdict;
}
