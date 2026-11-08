// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "VaRestLwsConn.h"

#if PLATFORM_LINUX
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/Base64.h"
#include "Misc/SecureHash.h"
#include "VaRestSocketTesting.h"
#include <openssl/pem.h>
#include <arpa/inet.h>
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

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

#if PLATFORM_LINUX

namespace VaRest::WebSocket::Lws::VaRestLwsTlsTestPrivate
{

	inline constexpr ANSICHAR LoopbackText[] = "varest-private-wss";

	/** A real independent RFC6455 peer. No LWS server, key file, trust bypass, global signal handler or detached thread. */
	class FLoopbackTlsPeer final : public FRunnable
	{
	public:
		~FLoopbackTlsPeer() { StopAndJoin(); ReleaseTls(); }

		bool Start(EVP_PKEY* Key, X509* Leaf)
		{
			Context = SSL_CTX_new(TLS_server_method());
			SocketMethod = BIO_meth_new(BIO_TYPE_SOURCE_SINK | BIO_get_new_index(), "VaRest test nonblocking socket");
			if (!Context || !SocketMethod || SSL_CTX_use_certificate(Context, Leaf) != 1
				|| SSL_CTX_use_PrivateKey(Context, Key) != 1 || SSL_CTX_check_private_key(Context) != 1
				|| BIO_meth_set_create(SocketMethod, &CreateBio) != 1 || BIO_meth_set_destroy(SocketMethod, &DestroyBio) != 1
				|| BIO_meth_set_read(SocketMethod, &ReadBio) != 1 || BIO_meth_set_write(SocketMethod, &WriteBio) != 1
				|| BIO_meth_set_ctrl(SocketMethod, &ControlBio) != 1) return Fail(TEXT("TLS setup"));
			Listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
			sockaddr_in Address{};
			Address.sin_family = AF_INET;
			Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			socklen_t AddressSize = sizeof(Address);
			if (Listener < 0 || bind(Listener, reinterpret_cast<sockaddr*>(&Address), sizeof(Address)) != 0
				|| getsockname(Listener, reinterpret_cast<sockaddr*>(&Address), &AddressSize) != 0 || listen(Listener, 1) != 0)
				return Fail(TEXT("loopback listener"));
			Port = ntohs(Address.sin_port);
			Thread = FRunnableThread::Create(this, TEXT("VaRestTlsLoopback"));
			return Thread != nullptr || Fail(TEXT("peer thread"));
		}

		void Stop() override { bStop.store(true, std::memory_order_release); }

		void StopAndJoin()
		{
			Stop();
			if (Thread)
			{
				// Every socket/BIO operation is nonblocking; poll checks Stop at most every 25 ms.
				Thread->WaitForCompletion();
				delete Thread;
				Thread = nullptr;
			}
			if (Listener >= 0) { close(Listener); Listener = -1; }
		}

		/** Only after joining; release before transport Shutdown can retire its SSL-manager reference. */
		void ReleaseTls()
		{
			check(!Thread);
			SSL_CTX_free(Context); Context = nullptr;
			BIO_meth_free(SocketMethod); SocketMethod = nullptr;
		}

		uint16 GetPort() const { return Port; }
		const FString& GetErrorAfterJoin() const { check(!Thread); return Error; }
		bool AcceptedTcp() const { return bAccepted.load(std::memory_order_acquire); }
		bool Upgraded() const { return bUpgraded.load(std::memory_order_acquire); }
		bool EchoedText() const { return bEchoed.load(std::memory_order_acquire); }
		bool EchoedClose() const { return bClosed.load(std::memory_order_acquire); }

		uint32 Run() override
		{
			Until = FPlatformTime::Seconds() + 15.0;
			SSL* Tls = nullptr;
			ON_SCOPE_EXIT
			{
				SSL_free(Tls);
				if (Connection >= 0) { close(Connection); Connection = -1; }
				ERR_clear_error();
			};
			while (Ready())
			{
				Connection = accept4(Listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
				if (Connection >= 0) break;
				if (errno == EINTR) continue;
				if ((errno == EAGAIN || errno == EWOULDBLOCK) && Wait(Listener, POLLIN)) continue;
				if (!bStop.load()) Fail(TEXT("accept"));
				return 0;
			}
			if (Connection < 0) return 0;
			bAccepted.store(true, std::memory_order_release);
			Tls = SSL_new(Context);
			BIO* Bio = Tls ? BIO_new(SocketMethod) : nullptr;
			if (!Bio) { Fail(TEXT("server SSL/BIO")); return 0; }
			BIO_set_data(Bio, &Connection);
			SSL_set_bio(Tls, Bio, Bio); // SSL owns the BIO; Connection and SocketMethod outlive SSL_free.
			bool bHandshake = false;
			while (Ready())
			{
				ERR_clear_error();
				const int Result = SSL_accept(Tls);
				if (Result == 1) { bHandshake = true; break; }
				const int SslError = SSL_get_error(Tls, Result);
				if (!Retry(SslError)) break;
			}
			if (!bHandshake) { Fail(TEXT("TLS handshake")); return 0; }
			if (!Upgrade(Tls)) return 0;
			uint8 Payload[125];
			int32 Num = 0;
			if (!ReadFrame(Tls, 1, Payload, Num) || Num != sizeof(LoopbackText) - 1
				|| FMemory::Memcmp(Payload, LoopbackText, Num) != 0) { Fail(TEXT("masked client text")); return 0; }
			if (!WriteFrame(Tls, 1, Payload, Num)) return 0;
			bEchoed.store(true, std::memory_order_release);
			if (!ReadFrame(Tls, 8, Payload, Num) || Num < 2 || Payload[0] != 3 || Payload[1] != 232)
				{ Fail(TEXT("masked client close 1000")); return 0; }
			if (WriteFrame(Tls, 8, Payload, Num)) bClosed.store(true, std::memory_order_release);
			// The WebSocket close was echoed. No claim of a two-sided TLS close-notify exchange.
			return 0;
		}

	private:
		// A socket BIO with MSG_NOSIGNAL avoids changing the process's SIGPIPE policy on negative TLS paths.
		static int CreateBio(BIO* Bio) { BIO_set_init(Bio, 1); BIO_set_data(Bio, nullptr); return 1; }
		static int DestroyBio(BIO*) { return 1; } // The worker closes its FD after SSL_free.
		static long ControlBio(BIO*, int Command, long, void*) { return Command == BIO_CTRL_FLUSH ? 1 : 0; }
		static int ReadBio(BIO* Bio, char* Bytes, int Num)
		{
			BIO_clear_retry_flags(Bio);
			const int Result = static_cast<int>(recv(*static_cast<int*>(BIO_get_data(Bio)), Bytes, Num, 0));
			if (Result < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) BIO_set_retry_read(Bio);
			return Result;
		}
		static int WriteBio(BIO* Bio, const char* Bytes, int Num)
		{
			BIO_clear_retry_flags(Bio);
			const int Result = static_cast<int>(send(*static_cast<int*>(BIO_get_data(Bio)), Bytes, Num, MSG_NOSIGNAL));
			if (Result < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) BIO_set_retry_write(Bio);
			return Result;
		}
		bool Fail(const TCHAR* Stage) { if (Error.IsEmpty()) Error = Stage; return false; }
		bool Ready()
		{
			if (bStop.load(std::memory_order_acquire)) return false;
			return FPlatformTime::Seconds() < Until || Fail(TEXT("peer deadline"));
		}
		bool Wait(int Fd, short Events)
		{
			while (Ready())
			{
				pollfd Poll { Fd, Events, 0 };
				const int Result = poll(&Poll, 1, 25);
				if (Result < 0 && errno == EINTR) continue;
				if (Result < 0) return Fail(TEXT("poll"));
				if (Result > 0) return true; // SSL/read/accept diagnoses HUP/ERR on the next nonblocking operation.
			}
			return false;
		}
		bool Retry(int SslError)
		{
			if (SslError == SSL_ERROR_WANT_READ) return Wait(Connection, POLLIN);
			if (SslError == SSL_ERROR_WANT_WRITE) return Wait(Connection, POLLOUT);
			return false;
		}
		bool Transfer(SSL* Tls, bool bWrite, uint8* Bytes, int32 Num)
		{
			int32 Offset = 0;
			while (Offset < Num && Ready())
			{
				ERR_clear_error();
				const int Result = bWrite ? SSL_write(Tls, Bytes + Offset, Num - Offset) : SSL_read(Tls, Bytes + Offset, Num - Offset);
				if (Result > 0) { Offset += Result; continue; }
				const int SslError = SSL_get_error(Tls, Result);
				if (!Retry(SslError)) return Fail(bWrite ? TEXT("TLS write") : TEXT("TLS read"));
			}
			return Offset == Num;
		}
		bool Write(SSL* Tls, const void* Bytes, int32 Num)
		{
			return Transfer(Tls, true, const_cast<uint8*>(static_cast<const uint8*>(Bytes)), Num);
		}
		bool Upgrade(SSL* Tls)
		{
			ANSICHAR Header[8192]{};
			int32 Used = 0;
			while (Used < sizeof(Header) - 1)
			{
				if (!Transfer(Tls, false, reinterpret_cast<uint8*>(Header + Used), 1)) return false;
				const uint8 Byte = static_cast<uint8>(Header[Used++]);
				if (Byte == 0 || Byte >= 128) return Fail(TEXT("non-ASCII upgrade"));
				if (Used >= 4 && FMemory::Memcmp(Header + Used - 4, "\r\n\r\n", 4) == 0) break;
			}
			if (Used < 4 || FMemory::Memcmp(Header + Used - 4, "\r\n\r\n", 4) != 0) return Fail(TEXT("upgrade limit"));
			TArray<FString> Lines;
			FString(UTF8_TO_TCHAR(Header)).ParseIntoArrayLines(Lines);
			if (Lines.IsEmpty() || Lines[0] != TEXT("GET /loopback HTTP/1.1")) return Fail(TEXT("upgrade request line"));
			FString Key;
			bool Host = false, UpgradeHeader = false, ConnectionHeader = false, Version = false;
			for (int32 Index = 1; Index < Lines.Num(); ++Index)
			{
				FString Name, Value;
				if (!Lines[Index].Split(TEXT(":"), &Name, &Value)) return Fail(TEXT("upgrade field"));
				Value.TrimStartAndEndInline();
				if (Name.Equals(TEXT("Host"), ESearchCase::IgnoreCase))
					{ if (Host || Value != FString::Printf(TEXT("127.0.0.1:%u"), Port)) return Fail(TEXT("upgrade host")); Host = true; }
				else if (Name.Equals(TEXT("Upgrade"), ESearchCase::IgnoreCase))
					{ if (UpgradeHeader || !Value.Equals(TEXT("websocket"), ESearchCase::IgnoreCase)) return Fail(TEXT("upgrade token")); UpgradeHeader = true; }
				else if (Name.Equals(TEXT("Connection"), ESearchCase::IgnoreCase))
					{ if (ConnectionHeader || !Value.Equals(TEXT("Upgrade"), ESearchCase::IgnoreCase)) return Fail(TEXT("connection token")); ConnectionHeader = true; }
				else if (Name.Equals(TEXT("Sec-WebSocket-Version"), ESearchCase::IgnoreCase))
					{ if (Version || Value != TEXT("13")) return Fail(TEXT("upgrade version")); Version = true; }
				else if (Name.Equals(TEXT("Sec-WebSocket-Key"), ESearchCase::IgnoreCase))
					{ if (!Key.IsEmpty()) return Fail(TEXT("duplicate upgrade key")); Key = MoveTemp(Value); }
			}
			TArray<uint8> DecodedKey;
			if (!Host || !UpgradeHeader || !ConnectionHeader || !Version || !FBase64::Decode(Key, DecodedKey) || DecodedKey.Num() != 16)
				return Fail(TEXT("upgrade mandatory fields"));
			const FString HashInput = Key + TEXT("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
			const FTCHARToUTF8 ToHash(*HashInput);
			uint8 Digest[20];
			FSHA1::HashBuffer(ToHash.Get(), ToHash.Length(), Digest);
			const FString Response = TEXT("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ")
				+ FBase64::Encode(Digest, sizeof(Digest)) + TEXT("\r\n\r\n");
			const FTCHARToUTF8 Utf8(*Response);
			if (!Write(Tls, Utf8.Get(), Utf8.Length())) return false;
			bUpgraded.store(true, std::memory_order_release);
			return true;
		}
		bool ReadFrame(SSL* Tls, uint8 Opcode, uint8* Payload, int32& Num)
		{
			uint8 Header[6];
			if (!Transfer(Tls, false, Header, 6)) return false;
			Num = Header[1] & 127;
			// This finite test sends only short final text and close frames; reject compression, fragments and extended lengths.
			if (Header[0] != (0x80 | Opcode) || !(Header[1] & 0x80) || Num > 125) return Fail(TEXT("frame header"));
			if (!Transfer(Tls, false, Payload, Num)) return false;
			for (int32 Index = 0; Index < Num; ++Index) Payload[Index] ^= Header[2 + Index % 4];
			return true;
		}
		bool WriteFrame(SSL* Tls, uint8 Opcode, const uint8* Payload, int32 Num)
		{
			const uint8 Header[2] { static_cast<uint8>(0x80 | Opcode), static_cast<uint8>(Num) };
			return Write(Tls, Header, 2) && Write(Tls, Payload, Num);
		}

		SSL_CTX* Context = nullptr;
		BIO_METHOD* SocketMethod = nullptr;
		FRunnableThread* Thread = nullptr;
		int Listener = -1, Connection = -1;
		uint16 Port = 0;
		double Until = 0.0;
		FString Error; // GT before Start, then worker-owned; GT reads only after join.
		std::atomic<bool> bStop { false }, bAccepted { false }, bUpgraded { false }, bEchoed { false }, bClosed { false };
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsTlsLoopbackTest, "VaRest.WebSocket.Lws.TlsLoopback",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsTlsLoopbackTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::Testing;
	using namespace VaRest::WebSocket::Lws::VaRestLwsTlsTestPrivate;
	EVP_PKEY* Key = MakeKey();
	X509* Leaf = Key ? MakeLeaf(Key, "IP:127.0.0.1") : nullptr;
	BIO* PublicPem = BIO_new(BIO_s_mem());
	ON_SCOPE_EXIT { BIO_free(PublicPem); X509_free(Leaf); EVP_PKEY_free(Key); };
	if (!TestTrue(TEXT("an in-memory key, IP certificate and public PEM are made"), Leaf && PublicPem && PEM_write_bio_X509(PublicPem, Leaf) == 1)) return false;
	char* PemBytes = nullptr;
	const long PemSize = BIO_get_mem_data(PublicPem, &PemBytes);
	if (!TestTrue(TEXT("only the public certificate is exported to memory"), PemSize > 0 && PemSize <= MAX_int32)) return false;
	TArray<uint8> TrustedPem;
	TrustedPem.Append(reinterpret_cast<const uint8*>(PemBytes), static_cast<int32>(PemSize));

	for (bool bWrongName : { false, true })
	{
		if (!TestEqual(TEXT("the previous private transport has retired all TLS attempts"), GetTlsHandshakesInFlight(), 0)) return false;
		FLoopbackTlsPeer Peer;
		if (!TestTrue(TEXT("a real ephemeral IPv4 TLS listener and joined peer thread start"), Peer.Start(Key, Leaf))) return false;
		FLwsTransportOptions Options;
		Options.ExtraTrustedPem = TrustedPem;
		Options.TcpAddressOverride = TEXT("127.0.0.1"); // The wrong-name control must reach TLS without DNS.
		Options.bOverrideAllowlist = true;
		Options.Allowlist.Emplace(TEXT("wss"), TArray<FString>{ TEXT("127.0.0.1"), TEXT("wrong.invalid") });
		Options.ActiveWaitCapMs = Options.ShortWaitCapMs = 10;
		const auto Transport = MakeLwsTransport(MoveTemp(Options));
		ON_SCOPE_EXIT { Peer.StopAndJoin(); Peer.ReleaseTls(); Transport->Shutdown(); };
		FVaRestSocketConnectRequest Request;
		Request.Url = FString::Printf(TEXT("wss://%s:%u/loopback"), bWrongName ? TEXT("wrong.invalid") : TEXT("127.0.0.1"), Peer.GetPort());
		Request.ConnectTimeout = 5.0;
		Request.CloseTimeout = 2.0;
		Request.PingInterval = 0.0;
		Request.LogName = bWrongName ? TEXT("loopback-name-refusal") : TEXT("loopback-strict-tls");
		const auto Connection = Transport->Connect(MoveTemp(Request));
		int32 Opens = 0, Messages = 0, Terminals = 0;
		bool bOrdered = true, bSent = false, bPayload = false;
		FVaRestSocketEvent Terminal;
		const double Until = FPlatformTime::Seconds() + 10.0;
		while (FPlatformTime::Seconds() < Until && Terminals == 0)
		{
			TArray<FVaRestSocketEvent> Events;
			Connection->PopEvents(Events);
			for (const FVaRestSocketEvent& Event : Events)
			{
				bOrdered &= Terminals == 0;
				if (Event.Type == EVaRestSocketEventType::Open)
				{
					bOrdered &= Opens == 0 && Messages == 0;
					++Opens;
					bSent = Connection->Send(FVaRestSocketFrame::Text(FStringView(TEXT("varest-private-wss"))));
				}
				else if (Event.Type == EVaRestSocketEventType::Message)
				{
					bOrdered &= Opens == 1 && Messages == 0;
					++Messages;
					bPayload = Event.Kind == EVaRestSocketFrameKind::Text && Event.Bytes.Num() == sizeof(LoopbackText) - 1
						&& FMemory::Memcmp(Event.Bytes.GetData(), LoopbackText, Event.Bytes.Num()) == 0;
					Connection->Consumed(Event.Bytes.Num());
					Connection->Close(1000, TEXT("loopback done"));
				}
				else if (Event.IsTerminal()) { ++Terminals; Terminal = Event; }
			}
			if (!Terminals) FPlatformProcess::SleepNoStats(0.002f);
		}
		const double RetireUntil = FPlatformTime::Seconds() + 3.0;
		while (Transport->GetCounters().LiveConnections != 0 && FPlatformTime::Seconds() < RetireUntil) FPlatformProcess::SleepNoStats(0.002f);
		Peer.StopAndJoin();
		TestTrue(TEXT("the actual loopback TCP connection reached the independent peer"), Peer.AcceptedTcp());
		TestEqual(TEXT("exactly one actual LWS context was created"), Transport->GetCounters().ContextCreations, 1);
		TestEqual(TEXT("the actual LWS connection was destroyed before the next context"), Transport->GetCounters().LiveConnections, 0);
		TestTrue(TEXT("one ordered terminal event arrived within the real deadline"), bOrdered && Terminals == 1);
		if (bWrongName)
		{
			TestTrue(TEXT("a trusted IP-only certificate cannot authenticate a different TLS name"), Opens == 0 && Messages == 0
				&& Terminal.Type == EVaRestSocketEventType::Error && Terminal.End.Failure == EVaRestSocketFailure::TlsVerification);
			TestFalse(TEXT("the name-refused connection never reached WebSocket upgrade"), Peer.Upgraded());
		}
		else
		{
			TestTrue(FString::Printf(TEXT("independent peer completed its exchange (%s)"), *Peer.GetErrorAfterJoin()), Peer.GetErrorAfterJoin().IsEmpty());
			TestTrue(TEXT("strict production WSS opened, sent masked text and received the exact echo"), Opens == 1 && bSent && Messages == 1 && bPayload
				&& Peer.Upgraded() && Peer.EchoedText());
			TestTrue(TEXT("the real masked client close was echoed and ended cleanly with 1000"), Peer.EchoedClose()
				&& Terminal.Type == EVaRestSocketEventType::Closed && Terminal.End.Code == 1000 && Terminal.End.bWasClean
				&& Terminal.End.Failure == EVaRestSocketFailure::None);
		}
	}
	TestEqual(TEXT("both real TLS attempts have retired"), GetTlsHandshakesInFlight(), 0);
	return true;
}

#endif // PLATFORM_LINUX

#endif // WITH_DEV_AUTOMATION_TESTS
