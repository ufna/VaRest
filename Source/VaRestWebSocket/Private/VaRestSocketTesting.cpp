// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSocketTesting.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/Paths.h"
#include "VaRestLwsConn.h"
#include "VaRestLwsTransport.h"

#if PLATFORM_LINUX
#include <dlfcn.h>
#endif

namespace VaRest::WebSocket::Testing::VaRestSocketTestingPrivate
{
	class FTestTransport final : public ILwsTestTransport
	{
	public:
		explicit FTestTransport(VaRest::WebSocket::Lws::FServiceOptions InOptions)
			: Inner(MoveTemp(InOptions))
		{
		}

		virtual ~FTestTransport() override { Inner.Shutdown(); }

		virtual TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Connect(FVaRestSocketConnectRequest&& Request, TFunction<void()> OnEvents) override
		{
			return Inner.Connect(MoveTemp(Request), MoveTemp(OnEvents));
		}

		virtual void Shutdown() override { Inner.Shutdown(); }

		virtual FLwsCounters GetCounters() const override
		{
			FLwsCounters Out;
			if (const VaRest::WebSocket::Lws::FServiceCounters* Counters = Inner.GetCounters())
			{
				Out.LiveConnections = Counters->LiveConnections.load();
				Out.KillsIssued = Counters->KillsIssued.load();
				Out.KillsLanded = Counters->KillsLanded.load();
				Out.ContextCreations = Counters->ContextCreations.load();
				Out.UnsentCloses = Counters->UnsentCloses.load();
				Out.LastKillToDestroySeconds = Counters->LastKillToDestroySeconds.load();
				Out.MaxKillToDestroySeconds = Counters->MaxKillToDestroySeconds.load();
			}
			return Out;
		}

		virtual bool IsShuttingDown() const override { return Inner.IsShuttingDown(); }

	private:
		VaRest::WebSocket::Lws::FTransport Inner;
	};
}

TSharedRef<VaRest::WebSocket::Testing::ILwsTestTransport, ESPMode::ThreadSafe> VaRest::WebSocket::Testing::MakeLwsTransport(FLwsTransportOptions Options)
{
	VaRest::WebSocket::Lws::FServiceOptions ServiceOptions;
	ServiceOptions.ExtraTrustedPem = MoveTemp(Options.ExtraTrustedPem);
	ServiceOptions.TcpAddressOverride = MoveTemp(Options.TcpAddressOverride);
	ServiceOptions.bOverrideAllowlist = Options.bOverrideAllowlist;
	ServiceOptions.Allowlist = MoveTemp(Options.Allowlist);
	ServiceOptions.ActiveWaitCapMs = Options.ActiveWaitCapMs;
	ServiceOptions.ShortWaitCapMs = Options.ShortWaitCapMs;
	ServiceOptions.ExitFlushSeconds = Options.ExitFlushSeconds;
	ServiceOptions.bFailThreadForTest = Options.bFailThreadForTest;
	ServiceOptions.bOnlyKillReapsForTest = Options.bOnlyKillReapsForTest;
	ServiceOptions.ThreadName = TEXT("VaRestLwsTest");
	return MakeShared<VaRestSocketTestingPrivate::FTestTransport, ESPMode::ThreadSafe>(MoveTemp(ServiceOptions));
}

FString VaRest::WebSocket::Testing::DescribeLwsCopy()
{
#if PLATFORM_LINUX
	Dl_info Info = {};
	if (dladdr(reinterpret_cast<void*>(&lws_create_context), &Info) != 0 && Info.dli_fname)
	{
		return FPaths::GetCleanFilename(UTF8_TO_TCHAR(Info.dli_fname));
	}
	return TEXT("(dladdr found no file)");
#else
	return TEXT("(not on this platform)");
#endif
}

const void* VaRest::WebSocket::Testing::GetLwsCreateContextAddress()
{
	return reinterpret_cast<const void*>(&lws_create_context);
}

FString VaRest::WebSocket::Testing::DescribeLwsLogLevel()
{
	return FString::Printf(TEXT("err=%d warn=%d notice=%d info=%d client=%d"), lwsl_visible(LLL_ERR), lwsl_visible(LLL_WARN), lwsl_visible(LLL_NOTICE),
		lwsl_visible(LLL_INFO), lwsl_visible(LLL_CLIENT));
}

int32 VaRest::WebSocket::Testing::GetTlsHandshakesInFlight()
{
	return VaRest::WebSocket::Lws::TlsHandshakesInFlight().load();
}

VaRest::WebSocket::Testing::FScopedTlsHandshakeInFlight::FScopedTlsHandshakeInFlight()
{
	VaRest::WebSocket::Lws::TlsHandshakesInFlight().fetch_add(1);
}

VaRest::WebSocket::Testing::FScopedTlsHandshakeInFlight::~FScopedTlsHandshakeInFlight()
{
	VaRest::WebSocket::Lws::TlsHandshakesInFlight().fetch_sub(1);
}

bool VaRest::WebSocket::Testing::CertificateMatchesIp(TConstArrayView<uint8> Pem, FStringView Ip)
{
	if (Pem.Num() == 0)
	{
		return false;
	}
	BIO* Bio = BIO_new_mem_buf(Pem.GetData(), Pem.Num());
	if (!Bio)
	{
		return false;
	}
	X509* Certificate = PEM_read_bio_X509(Bio, nullptr, nullptr, nullptr);
	BIO_free(Bio);
	ERR_clear_error();
	const bool bMatch = VaRest::WebSocket::Lws::CertificateMatchesIp(Certificate, FString(Ip));
	// X509_free takes null
	X509_free(Certificate);
	return bMatch;
}

#endif // WITH_DEV_AUTOMATION_TESTS
