// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "VaRestLwsConn.h"

namespace VaRest::WebSocket::Lws::VaRestLwsTlsTestPrivate
{
	struct FOutcome
	{
		bool bVerified = false;
		int32 Error = -1;
		int32 Depth = -1;
		/** VaRest's verdict/detail, before LWS applies its final store-error check. */
		int32 Verdict = -1;
		FString Detail;
	};

	/** Borrowed only during synchronous X509_verify_cert; detached before the context is destroyed. */
	struct FVerification
	{
		const FSocketTarget& Target;
		FOutcome& Outcome;
	};

	/**
	 * Game thread: OpenSSL's verify callback, forwarding to VaRest's decision with LWS 3.0's convention (libwebsockets.h:1028-1035): a
	 * verdict of 1 refuses in any case; 0 accepts only when no error is left (VaRest reset it, or there was none)
	 */
	int VerifyAsLws(int PreverifyOk, X509_STORE_CTX* Store)
	{
		FVerification* Verification = static_cast<FVerification*>(X509_STORE_CTX_get_app_data(Store));
		if (!Verification)
		{
			return 0;
		}
		FString Detail;
		Verification->Outcome.Verdict = EvaluateServerCertificate(Verification->Target, nullptr, Store, PreverifyOk, Detail);
		Verification->Outcome.Detail = MoveTemp(Detail);
		if (Verification->Outcome.Verdict != 0)
		{
			return 0;
		}
		return PreverifyOk || X509_STORE_CTX_get_error(Store) == X509_V_OK ? 1 : 0;
	}

	/** Game thread: an EC P-256 key; null on failure */
	EVP_PKEY* MakeKey()
	{
		EVP_PKEY* Key = nullptr;
		EVP_PKEY_CTX* Context = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
		if (Context && EVP_PKEY_keygen_init(Context) == 1 && EVP_PKEY_CTX_set_ec_paramgen_curve_nid(Context, NID_X9_62_prime256v1) == 1)
		{
			EVP_PKEY_keygen(Context, &Key);
		}
		EVP_PKEY_CTX_free(Context);
		return Key;
	}

	/** Game thread: a self-signed leaf for Key; NotAfter is seconds from now (negative for expiry), SAN as given. Null on failure. */
	X509* MakeLeaf(EVP_PKEY* Key, const char* SubjectAltName, long NotAfter = 3600)
	{
		X509* Leaf = X509_new();
		if (!Leaf)
		{
			return nullptr;
		}
		X509_set_version(Leaf, 2);
		ASN1_INTEGER_set(X509_get_serialNumber(Leaf), 1);
		X509_gmtime_adj(X509_getm_notBefore(Leaf), -3600);
		X509_gmtime_adj(X509_getm_notAfter(Leaf), NotAfter);
		X509_set_pubkey(Leaf, Key);
		X509_NAME* Name = X509_get_subject_name(Leaf);
		X509_NAME_add_entry_by_txt(Name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("varest-tls-test"), -1, -1, 0);
		X509_set_issuer_name(Leaf, Name);
		// A null config and context are enough for DNS and IP entries (x509v3.h X509V3_EXT_conf_nid)
		X509_EXTENSION* San = X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name, SubjectAltName);
		const bool bMade = San && X509_add_ext(Leaf, San, -1) == 1 && X509_sign(Leaf, Key, EVP_sha256()) > 0;
		X509_EXTENSION_free(San);
		if (!bMade)
		{
			X509_free(Leaf);
			return nullptr;
		}
		return Leaf;
	}

	/** Game thread: OpenSSL's verification of Leaf, Trusted in the store (or nothing), the host name set as LWS 3.0 sets it: an IP host as a DNS name [tls.hostname_ip_literal] */
	FOutcome Verify(X509* Leaf, X509* Trusted, const FSocketTarget& Target, bool bCheckSignature = false)
	{
		FOutcome Out;
		X509_STORE* Store = X509_STORE_new();
		X509_STORE_CTX* Context = X509_STORE_CTX_new();
		if (Store && Context && (!Trusted || X509_STORE_add_cert(Store, Trusted) == 1) && X509_STORE_CTX_init(Context, Store, Leaf, nullptr) == 1)
		{
			X509_VERIFY_PARAM_set1_host(X509_STORE_CTX_get0_param(Context), TCHAR_TO_UTF8(*Target.Host), 0);
			X509_STORE_CTX_set_verify_cb(Context, &VerifyAsLws);
			FVerification Verification { Target, Out };
			X509_STORE_CTX_set_app_data(Context, &Verification);
			if (bCheckSignature)
			{
				X509_VERIFY_PARAM_set_flags(X509_STORE_CTX_get0_param(Context), X509_V_FLAG_CHECK_SS_SIGNATURE);
			}
			Out.bVerified = X509_verify_cert(Context) == 1;
			X509_STORE_CTX_set_app_data(Context, nullptr);
			Out.Error = X509_STORE_CTX_get_error(Context);
			Out.Depth = X509_STORE_CTX_get_error_depth(Context);
		}
		X509_STORE_CTX_free(Context);
		X509_STORE_free(Store);
		ERR_clear_error();
		return Out;
	}

	/** Any thread: a secure target for the named host. */
	FSocketTarget TargetFor(const TCHAR* Host, bool bIp)
	{
		FSocketTarget Target;
		Target.bTls = true;
		Target.Host = Host;
		Target.bHostIsIp = bIp;
		return Target;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsTlsIpCheckTest, "VaRest.WebSocket.Lws.TlsIpCheck",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsTlsIpCheckTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket;
	using namespace VaRest::WebSocket::Lws;
	using namespace VaRest::WebSocket::Lws::VaRestLwsTlsTestPrivate;
	EVP_PKEY* Key = MakeKey();
	X509* IpLeaf = Key ? MakeLeaf(Key, "IP:127.0.0.1") : nullptr;
	X509* WrongIpLeaf = Key ? MakeLeaf(Key, "IP:10.0.0.1") : nullptr;
	X509* DnsOnlyLeaf = Key ? MakeLeaf(Key, "DNS:localhost") : nullptr;
	X509* ExpiredLeaf = Key ? MakeLeaf(Key, "IP:127.0.0.1", -60) : nullptr;
	X509* BadSignatureLeaf = Key ? MakeLeaf(Key, "IP:127.0.0.1") : nullptr;
	ON_SCOPE_EXIT
	{
		// Each takes null
		X509_free(IpLeaf);
		X509_free(WrongIpLeaf);
		X509_free(DnsOnlyLeaf);
		X509_free(ExpiredLeaf);
		X509_free(BadSignatureLeaf);
		EVP_PKEY_free(Key);
	};
	if (!TestTrue(TEXT("the key and the leaves are made"), IpLeaf && WrongIpLeaf && DnsOnlyLeaf && ExpiredLeaf && BadSignatureLeaf))
	{
		return false;
	}
	const FSocketTarget Ip = TargetFor(TEXT("127.0.0.1"), true);
	// Ruling Q3: accepted only for an IP host, at depth 0, when the leaf has that IP
	const FOutcome Right = Verify(IpLeaf, IpLeaf, Ip);
	TestTrue(FString::Printf(TEXT("a trusted leaf with the host's IP SAN is accepted (error %d)"), Right.Error), Right.bVerified && Right.Error == X509_V_OK);
	const FOutcome WrongIp = Verify(WrongIpLeaf, WrongIpLeaf, Ip);
	TestTrue(FString::Printf(TEXT("a trusted leaf for another IP is refused, error 62 at depth 0 (error %d, depth %d)"), WrongIp.Error, WrongIp.Depth),
		!WrongIp.bVerified && WrongIp.Error == X509_V_ERR_HOSTNAME_MISMATCH && WrongIp.Depth == 0);
	const FOutcome DnsOnly = Verify(DnsOnlyLeaf, DnsOnlyLeaf, Ip);
	TestTrue(FString::Printf(TEXT("a trusted leaf with a DNS-only SAN is refused for an IP host, error 62 (error %d)"), DnsOnly.Error),
		!DnsOnly.bVerified && DnsOnly.Error == X509_V_ERR_HOSTNAME_MISMATCH);
	// The chain error comes first and stands: the IP check clears error 62 and nothing else
	const FOutcome Untrusted = Verify(IpLeaf, nullptr, Ip);
	TestTrue(FString::Printf(TEXT("an untrusted leaf with the host's IP SAN is still refused, error 18 (error %d)"), Untrusted.Error),
		!Untrusted.bVerified && Untrusted.Error == X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT);
	// The rule is for IP hosts only: a name host keeps OpenSSL's name check
	const FOutcome NameOnIpLeaf = Verify(IpLeaf, IpLeaf, TargetFor(TEXT("localhost"), false));
	TestTrue(FString::Printf(TEXT("a name host against an IP-only leaf is refused, error 62 (error %d)"), NameOnIpLeaf.Error),
		!NameOnIpLeaf.bVerified && NameOnIpLeaf.Error == X509_V_ERR_HOSTNAME_MISMATCH);
	const FOutcome Name = Verify(DnsOnlyLeaf, DnsOnlyLeaf, TargetFor(TEXT("localhost"), false));
	TestTrue(FString::Printf(TEXT("a name host against its DNS SAN is accepted (error %d)"), Name.Error), Name.bVerified);
	// OpenSSL continues checking after the IP name correction: time and signature failures still stand.
	const FOutcome Expired = Verify(ExpiredLeaf, ExpiredLeaf, Ip);
	TestTrue(FString::Printf(TEXT("an expired leaf with the right IP is refused, error 10 (error %d)"), Expired.Error),
		!Expired.bVerified && Expired.Error == X509_V_ERR_CERT_HAS_EXPIRED);
	const ASN1_BIT_STRING* Signature = nullptr;
	X509_get0_signature(&Signature, nullptr, BadSignatureLeaf);
	if (!TestTrue(TEXT("the leaf has a signature to corrupt"), Signature && ASN1_STRING_length(Signature) > 0))
	{
		return false;
	}
	// OpenSSL exposes a const view; this test owns the mutable certificate. Flip a signature byte without changing its SAN.
	const_cast<unsigned char*>(ASN1_STRING_get0_data(Signature))[ASN1_STRING_length(Signature) - 1] ^= 1;
	const FOutcome BadSignature = Verify(BadSignatureLeaf, BadSignatureLeaf, Ip, true);
	TestTrue(FString::Printf(TEXT("a bad signature with the right IP is refused, error 7 (error %d)"), BadSignature.Error),
		!BadSignature.bVerified && BadSignature.Error == X509_V_ERR_CERT_SIGNATURE_FAILURE);
	// Hostname mismatch naturally occurs at the leaf in OpenSSL. Exercise the depth guard directly as well.
	X509_STORE_CTX* NonLeaf = X509_STORE_CTX_new();
	if (!TestNotNull(TEXT("a context for the depth guard"), NonLeaf))
	{
		return false;
	}
	X509_STORE_CTX_set_current_cert(NonLeaf, IpLeaf);
	X509_STORE_CTX_set_error(NonLeaf, X509_V_ERR_HOSTNAME_MISMATCH);
	X509_STORE_CTX_set_error_depth(NonLeaf, 1);
	FString Detail;
	TestEqual(TEXT("error 62 above the leaf is refused"), EvaluateServerCertificate(Ip, nullptr, NonLeaf, 0, Detail), 1);
	TestEqual(TEXT("the non-leaf store error stays 62"), X509_STORE_CTX_get_error(NonLeaf), X509_V_ERR_HOSTNAME_MISMATCH);
	TestTrue(TEXT("the refusal names the error and depth"), Detail.Contains(TEXT("X509 error 62 at depth 1"), ESearchCase::CaseSensitive));
	X509_STORE_CTX_free(NonLeaf);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsTlsInsecureRefusalTest, "VaRest.WebSocket.Lws.TlsInsecureRefusal",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsTlsInsecureRefusalTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket;
	using namespace VaRest::WebSocket::Lws::VaRestLwsTlsTestPrivate;
	EVP_PKEY* Key = MakeKey();
	X509* ExpiredLeaf = Key ? MakeLeaf(Key, "DNS:localhost", -60) : nullptr;
	ON_SCOPE_EXIT
	{
		X509_free(ExpiredLeaf);
		EVP_PKEY_free(Key);
	};
	if (!TestNotNull(TEXT("the expired leaf is made"), ExpiredLeaf))
	{
		return false;
	}
	FSocketTarget Insecure = TargetFor(TEXT("localhost"), false);
	Insecure.bInsecure = true;
	// LWS consumes allowed self-signed errors before our callback; trust the leaf and match its DNS SAN to isolate expiry.
	// Our flags omit ALLOW_EXPIRED: OpenSSL rejects even the old early-return implementation. The regression is VaRest's
	// own refusal and detail, which FConn::OnVerifyServerCertificate maps to PendingFailure=TlsVerification.
	const FOutcome Expired = Verify(ExpiredLeaf, ExpiredLeaf, Insecure);
	TestTrue(TEXT("OpenSSL still refuses an expired insecure leaf"), !Expired.bVerified && Expired.Error == X509_V_ERR_CERT_HAS_EXPIRED && Expired.Depth == 0);
	TestEqual(TEXT("VaRest refuses the insecure expiry, supplying the TlsVerification classification input"), Expired.Verdict, 1);
	TestTrue(TEXT("VaRest's insecure refusal names X509 error 10 at depth 0"), Expired.Detail.Contains(TEXT("X509 error 10 at depth 0"), ESearchCase::CaseSensitive));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
