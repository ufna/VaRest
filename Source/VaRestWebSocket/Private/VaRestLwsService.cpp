// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestLwsService.h"

#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"
#include "Misc/Fork.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "HAL/PlatformProcess.h"
#include "PlatformHttp.h"
#include "Ssl.h"
#include "VaRestSocketTransport.h"
#include "VaRestSocketTypes.h"
#include "VaRestWebSocketLog.h"

// VaRestSocketTransport.h for MaxConnectTimeoutSeconds: this file must compile alone, outside a unity blob (review spec I-2b)
static_assert(VaRest::WebSocket::FrameHeadroom >= LWS_PRE, "FVaRestSocketFrame's headroom must hold libwebsockets' frame header (libwebsockets.h:4773)");

namespace VaRest::WebSocket::Lws::VaRestLwsServicePrivate
{
	/** One libwebsockets line for UE's log: trailing white space dropped, controls shown as '?' (a server's bytes may be in it) */
	FString CleanLine(const char* Line)
	{
		FString Text = Line ? FString(UTF8_TO_TCHAR(Line)) : FString();
		Text.TrimEndInline();
		for (TCHAR& Char : Text)
		{
			if (Char < 0x20 || Char == 0x7F)
			{
				Char = TEXT('?');
			}
		}
		return Text;
	}

	/** VaRest's emitter for its LWS copy: errors and warnings, both Warnings, as the engine reports its own (LwsWebSocketsManager.cpp:403-424) */
	void EmitLwsLine(int Level, const char* Line)
	{
		UE_LOG(LogVaRestSocket, Warning, TEXT("libwebsockets(%s): %s"), Level == LLL_ERR ? TEXT("err") : TEXT("warn"), *CleanLine(Line));
	}

	/**
	 * Whether the service's thread is a real one, so the game thread may wait for it: the engine's own test before it lets
	 * lws_service block (LwsWebSocketsManager.cpp:110-118). Elsewhere the main thread ticks the forkable thread (Fork.h:86-93)
	 */
	bool HasRealThread()
	{
		return FPlatformProcess::SupportsMultithreading() || FForkProcessHelper::IsForkedMultithreadInstance();
	}
}

void VaRest::WebSocket::Lws::Utf8Copy(const FString& Text, TArray<ANSICHAR>& Out)
{
	const FTCHARToUTF8 Converted(*Text);
	Out.SetNumUninitialized(Converted.Length() + 1);
	if (Converted.Length() > 0)
	{
		FMemory::Memcpy(Out.GetData(), Converted.Get(), Converted.Length());
	}
	Out[Converted.Length()] = '\0';
}

void VaRest::WebSocket::Lws::ApplyLogLevel()
{
	// D30 (spec 1.2): VaRest's own LWS copy in modular builds, always; the one copy a monolithic or merged build shares, only
	// while the engine's WebSockets module is not loaded. A WebSockets module that loads later sets its own level and emitter
	// (LwsWebSocketsManager.cpp:174-176), and VaRest's lines then go to LogWebSockets: never both in one copy
	constexpr bool bSharedCopy = IS_MONOLITHIC || UE_MERGED_MODULES;
	if (!bSharedCopy || !FModuleManager::Get().IsModuleLoaded(TEXT("WebSockets")))
	{
		lws_set_log_level(LLL_ERR | LLL_WARN, &VaRestLwsServicePrivate::EmitLwsLine);
	}
}

bool VaRest::WebSocket::Lws::ShouldCreateAtStartup(bool bSharedCopy, TFunctionRef<bool(const TCHAR*)> IsLinked)
{
	if (!bSharedCopy)
	{
		return false;
	}
	// The engine modules that link libwebsockets and make their own contexts (spec 1.2)
	for (const TCHAR* Name : { TEXT("WebSockets"), TEXT("WebSocketServer"), TEXT("WebSocketNetworking"), TEXT("HttpNetworkReplayStreaming") })
	{
		if (IsLinked(Name))
		{
			return true;
		}
	}
	return false;
}

std::atomic<int32>& VaRest::WebSocket::Lws::TlsHandshakesInFlight()
{
	static std::atomic<int32> Count{ 0 };
	return Count;
}

VaRest::WebSocket::Lws::FService::FService(FServiceOptions InOptions)
	: Options(MoveTemp(InOptions))
{
}

VaRest::WebSocket::Lws::FService::~FService()
{
	// The owner shuts a running service down on the game thread first (FTransport::Shutdown); this is the backstop
	if (Thread || Context)
	{
		check(IsInGameThread());
		Shutdown();
	}
}

bool VaRest::WebSocket::Lws::FService::Start()
{
	using namespace VaRest::WebSocket::Lws::VaRestLwsServicePrivate;
	check(IsInGameThread());
	if (Context)
	{
		return true;
	}
	if (bShuttingDown.load() || bStartFailed)
	{
		// Review conc I-2: never a second lws_create_context after a failed start -- in a shared LWS copy a retry could run
		// while an engine wss handshake is in flight (VaRest-rti, ruling Q2)
		return false;
	}
	if (const int32 InFlight = TlsHandshakesInFlight().load(); InFlight != 0)
	{
		// Review conc I-3: the creation would re-allocate the ex-data index those handshakes' verify callbacks read
		// [coexist.context_create_reallocates_ssl_index]. Not latched: nothing was created, and the next Start may run
		UE_LOG(LogVaRestSocket, Error, TEXT("libwebsockets: no context is made while %d VaRest wss handshake(s) are in flight (VaRest-rti); WebSocket connections end with Shutdown"), InFlight);
		return false;
	}
	LLM_SCOPE_BYTAG(VaRest_WebSocket);
	ApplyLogLevel();
	// One protocol, "varest" (spec 2.1). per_session_data_size 0: the connection's userdata is ours (connect_info.userdata),
	// as the engine's (LwsWebSocketsManager.cpp:163-166). Protocols[1] stays zero: LWS's terminator
	Protocols[0].name = "varest";
	Protocols[0].callback = &FService::Callback;
	Protocols[0].per_session_data_size = 0;
	Protocols[0].rx_buffer_size = RxBufferBytes;

	lws_context_creation_info Info = {};
	Info.port = CONTEXT_PORT_NO_LISTEN;
	Info.protocols = Protocols;
	Info.uid = -1;
	Info.gid = -1;
	// The engine's options (LwsWebSocketsManager.cpp:184) and JUST_USE_RAW_ORIGIN (T4, libwebsockets.h:2618): .origin as given
	Info.options = LWS_SERVER_OPTION_PEER_CERT_NOT_REQUIRED | LWS_SERVER_OPTION_DISABLE_OS_CA_CERTS | LWS_SERVER_OPTION_JUST_USE_RAW_ORIGIN;
	Info.max_http_header_data2 = HeaderBufferBytes;
	Info.pt_serv_buf_size = HeaderBufferBytes;
	// Spec 2.1, 5: VaRest's ping is per connection (W1b); LWS's own is off
	Info.ws_ping_pong_interval = 0;
	// Spec 2.2: context-wide, the backstop of every handshake stage [timeout.configured_secs]; each socket's shorter deadline is VaRest's kill
	Info.timeout_secs = static_cast<unsigned int>(VaRest::WebSocket::MaxConnectTimeoutSeconds);
	// Callback() finds this service through lws_context_user(lws_get_context(wsi)), also inside lws_create_context
	Info.user = this;
	// The engine's proxy setting, command line or ini (LwsWebSocketsManager.cpp:196-203; GenericPlatformHttp.cpp:640-651)
	const FString Proxy = FPlatformHttp::GetConfiguredProxyAddress();
	if (!Proxy.IsEmpty())
	{
		Utf8Copy(Proxy, ProxyUtf8);
		Info.http_proxy_address = ProxyUtf8.GetData();
	}
	FSslModule& Ssl = FModuleManager::LoadModuleChecked<FSslModule>(TEXT("SSL"));
	Certificates = &Ssl.GetCertificateManager();
	SslManager = &Ssl.GetSslManager();
	// The engine's order (LwsWebSocketsManager.cpp:205-225): the SSL module's context where InitializeSsl succeeds -- monolithic
	// and merged builds only (SslManager.cpp:26-55) -- else LWS's own, which raises LOAD_EXTRA_CLIENT_VERIFY_CERTS (T23)
	if (SslManager->InitializeSsl())
	{
		bSslInitialized = true;
		ProvidedSslCtx = SslManager->CreateSslContext(FSslContextCreateOptions());
	}
	if (ProvidedSslCtx)
	{
		// Its roots come from CreateSslContext itself (bAddCertificates, ISslManager.h:46-47) [tls.provided_ctx_load_extra_fired]
		AddExtraTrusted(ProvidedSslCtx);
		Info.provided_client_ssl_ctx = ProvidedSslCtx;
	}
	else
	{
		Info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
	}
	Context = lws_create_context(&Info);
	Counters.ContextCreations.fetch_add(1);
	if (!Context)
	{
		UE_LOG(LogVaRestSocket, Error, TEXT("libwebsockets: lws_create_context failed; WebSocket connections end with Shutdown"));
		ReleaseSsl();
		bStartFailed = true;
		return false;
	}
	bStopping = false;
	{
		// Context is set before bAccepting under the lock: an Enqueue that sees bAccepting sees the context
		FScopeLock Guard(&EnqueueLock);
		bAccepting = true;
	}
	// 128 KiB, below normal, forkable: the engine's service thread (LwsWebSocketsManager.cpp:239-247). The thread's start
	// follows every write above, Context among them (the context is the game thread's until here)
	Thread = Options.bFailThreadForTest ? nullptr
		: FForkProcessHelper::CreateForkableThread(this, *Options.ThreadName, 128 * 1024, TPri_BelowNormal, FPlatformAffinity::GetNoAffinityMask());
	if (!Thread)
	{
		bStartFailed = true;
		UE_LOG(LogVaRestSocket, Error, TEXT("libwebsockets: the service thread could not start; WebSocket connections end with Shutdown"));
		{
			// No Enqueue is inside lws_cancel_service once this is released, and none calls it again
			FScopeLock Guard(&EnqueueLock);
			bAccepting = false;
		}
		lws_context_destroy(Context);
		Context = nullptr;
		ReleaseSsl();
		return false;
	}
	UE_LOG(LogVaRestSocket, Verbose, TEXT("libwebsockets %s: context and thread %s started"), UTF8_TO_TCHAR(lws_get_library_version()), *Options.ThreadName);
	return true;
}

void VaRest::WebSocket::Lws::FService::Shutdown()
{
	check(IsInGameThread());
	// Review conc M-3: the close frames already asked for (the hub's 1001 at engine exit, spec 1.4) go out first, while the
	// thread still runs. Only where it is a real thread: the main thread ticks it otherwise (Fork.h:86-93), and does not here
	if (Thread && bAccepting.load() && Options.ExitFlushSeconds > 0.0 && VaRestLwsServicePrivate::HasRealThread())
	{
		FlushCloses(Options.ExitFlushSeconds);
	}
	// Before bAccepting drops: whoever sees the service no longer running sees that Shutdown has begun (FConn::Start)
	bShuttingDown = true;
	{
		// Every Enqueue from here on drops its command, and none is still inside lws_cancel_service: the context may go
		FScopeLock Guard(&EnqueueLock);
		bAccepting = false;
	}
	if (Thread)
	{
		// Kill(true) calls Stop() and joins Run() (PThreadRunnableThread.h:276-290): the engine's order
		// (LwsWebSocketsManager.cpp:298-303). From the join on, the thread's state is the game thread's
		Thread->Kill(true);
		delete Thread;
		Thread = nullptr;
	}
	// The thread is gone: its state is the game thread's now. Queued commands run (a connect among them reports Shutdown)
	DrainCommands();
	for (TPair<uint64, TUniquePtr<IServiceConnection>>& Pair : Connections)
	{
		Pair.Value->EndForShutdown();
	}
	if (Context)
	{
		// After the thread, on the game thread (LwsWebSocketsManager.cpp:305-309); the connections' last callbacks
		// (CLIENT_CLOSED, WSI_DESTROY) arrive inside it, here, and each has reported already
		lws_context_destroy(Context);
		Context = nullptr;
	}
	Connections.Empty();
	Live.Empty();
	Counters.LiveConnections = 0;
	ReleaseSsl();
}

void VaRest::WebSocket::Lws::FService::FlushCloses(double Seconds)
{
	// A marker queued after every command so far: once it has run, UnsentCloses counts each close asked for before this call
	const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> Reached = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	Enqueue([Reached](FService& Self) { *Reached = true; });
	const double Until = FPlatformTime::Seconds() + Seconds;
	while (FPlatformTime::Seconds() < Until && (!Reached->load() || Counters.UnsentCloses.load() > 0))
	{
		FPlatformProcess::SleepNoStats(0.001f);
	}
}

bool VaRest::WebSocket::Lws::FService::Enqueue(FCommand&& Command)
{
	{
		FScopeLock Guard(&EnqueueLock);
		if (bAccepting.load())
		{
			Commands.Enqueue(MoveTemp(Command));
			// The one LWS call another thread may make (T13, libwebsockets.h:3704-3716): the command runs in EVENT_WAIT_CANCELLED
			// (T24). Under the lock: the context exists until Shutdown has taken it (libwebsockets.h:3712-3713)
			lws_cancel_service(Context);
			return true;
		}
	}
	// Refused (review conc M-7): released here, outside the lock -- its captures' destructors are the caller's code -- and
	// at once, even when the caller moved it from a named function that outlives this call
	{
		const FCommand Dropped = MoveTemp(Command);
	}
	UE_LOG(LogVaRestSocket, Verbose, TEXT("libwebsockets: a command was dropped: the service %s"), bShuttingDown.load() ? TEXT("is shut down") : TEXT("is not running"));
	return false;
}

void VaRest::WebSocket::Lws::FService::Adopt(TUniquePtr<IServiceConnection>&& Connection)
{
	IServiceConnection* Raw = Connection.Get();
	// A second connection under one id would replace the first while Live still held its address
	checkf(!Connections.Contains(Raw->GetId()), TEXT("ws#%llu adopted twice"), Raw->GetId());
	Live.Add(Raw);
	Connections.Add(Raw->GetId(), MoveTemp(Connection));
	Counters.LiveConnections = Connections.Num();
	Raw->Start(*this);
}

VaRest::WebSocket::Lws::IServiceConnection* VaRest::WebSocket::Lws::FService::Find(uint64 Id) const
{
	const TUniquePtr<IServiceConnection>* Found = Connections.Find(Id);
	return Found ? Found->Get() : nullptr;
}

int32 VaRest::WebSocket::Lws::FService::ComputeWaitMs(double Now) const
{
	if (Connections.Num() == 0)
	{
		return Options.IdleWaitMs;
	}
	double Next = TNumericLimits<double>::Max();
	bool bShort = false;
	for (const TPair<uint64, TUniquePtr<IServiceConnection>>& Pair : Connections)
	{
		Next = FMath::Min(Next, Pair.Value->NextDeadline());
		bShort = bShort || Pair.Value->WantsShortWait();
	}
	const int32 Cap = bShort ? Options.ShortWaitCapMs : Options.ActiveWaitCapMs;
	if (Next == TNumericLimits<double>::Max())
	{
		return Cap;
	}
	// Clamped as a double before the cast; rounded up, so a wait never ends just before its deadline and spins once more
	return FMath::CeilToInt32(FMath::Clamp((Next - Now) * 1000.0, 0.0, static_cast<double>(Cap)));
}

uint32 VaRest::WebSocket::Lws::FService::Run()
{
	LLM_SCOPE_BYTAG(VaRest_WebSocket);
	while (!bStopping.load())
	{
		ServiceOnce(ComputeWaitMs(FPlatformTime::Seconds()));
	}
	return 0;
}

void VaRest::WebSocket::Lws::FService::Stop()
{
	// From Kill on the game thread, while Run may wait in lws_service: the cancel ends that wait. A cancel made while Run is
	// between its bStopping check and lws_service is not lost: it leaves a byte in LWS's cancel pipe, which the next poll
	// sees (the Release archive: rops_handle_POLLIN_pipe reads the pipe before it broadcasts EVENT_WAIT_CANCELLED; review
	// conc, "Checked and sound" 1)
	bStopping = true;
	if (Context)
	{
		lws_cancel_service(Context);
	}
}

void VaRest::WebSocket::Lws::FService::Tick()
{
	// No wait where the main thread ticks the service: the engine polls with 0 there too (LwsWebSocketsManager.cpp:110-118, :509)
	if (Context && !bStopping.load())
	{
		ServiceOnce(0);
	}
}

void VaRest::WebSocket::Lws::FService::ServiceOnce(int32 WaitMs)
{
	lws_service(Context, WaitMs);
	// Spec 2.1: VaRest's deadlines after each return, outside any callback, where a kill and a queued event are safe
	CheckDeadlines(FPlatformTime::Seconds());
	EraseDestroyed();
}

void VaRest::WebSocket::Lws::FService::DrainCommands()
{
	// The queue's one consumer: the service thread inside EVENT_WAIT_CANCELLED, or the game thread once it is joined. No
	// lock is held while a command runs
	FCommand Command;
	while (Commands.Dequeue(Command))
	{
		Command(*this);
		Command.Reset();
		Counters.CommandsRun.fetch_add(1);
	}
}

void VaRest::WebSocket::Lws::FService::CheckDeadlines(double Now)
{
	for (TPair<uint64, TUniquePtr<IServiceConnection>>& Pair : Connections)
	{
		Pair.Value->CheckDeadlines(*this, Now);
	}
}

void VaRest::WebSocket::Lws::FService::EraseDestroyed()
{
	for (auto It = Connections.CreateIterator(); It; ++It)
	{
		if (It->Value->IsDestroyed())
		{
			// LWS calls back no more for it: WSI_DESTROY came, or there never was a wsi
			Live.Remove(It->Value.Get());
			It.RemoveCurrent();
		}
	}
	Counters.LiveConnections = Connections.Num();
}

int VaRest::WebSocket::Lws::FService::Callback(lws* Wsi, lws_callback_reasons Reason, void* User, void* In, size_t Len)
{
	// Also called inside lws_create_context (protocol init, client TLS setup), before Context is set: the service comes from
	// the context's user pointer (lws_context_creation_info::user), as the probe's rig does (VaRestLwsProbe.cpp:492-499)
	lws_context* Owner = Wsi ? lws_get_context(Wsi) : nullptr;
	FService* Self = Owner ? static_cast<FService*>(lws_context_user(Owner)) : nullptr;
	return Self ? Self->Handle(Wsi, static_cast<int32>(Reason), User, In, Len) : 0;
}

int32 VaRest::WebSocket::Lws::FService::Handle(lws* Wsi, int32 Reason, void* User, void* In, size_t Len)
{
	switch (Reason)
	{
	case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
		// lws_cancel_service lands here, on the service thread (libwebsockets.h:1551-1556): spec 2.1's command point
		DrainCommands();
		return 0;
	case LWS_CALLBACK_OPENSSL_LOAD_EXTRA_CLIENT_VERIFY_CERTS:
		// user is the SSL_CTX (libwebsockets.h:961-967); raised inside lws_create_context, on the game thread
		LoadClientCertificates(static_cast<SSL_CTX*>(User));
		return 0;
	default:
		break;
	}
	IServiceConnection* Connection = FindForCallback(Wsi, User, Reason);
	return Connection ? Connection->OnCallback(*this, Wsi, Reason, User, In, Len) : 0;
}

VaRest::WebSocket::Lws::IServiceConnection* VaRest::WebSocket::Lws::FService::FindForCallback(lws* Wsi, void* User, int32 Reason) const
{
	// The userdata, never the wsi: LWS 3.0 reuses a freed wsi's address for the next connection (bead VaRest-81s;
	// Probe.VaRest.Lws.Rig.SequentialConnections). PERFORM_SERVER_CERT_VERIFICATION's user is the X509_STORE_CTX, so the
	// userdata comes from lws_wsi_user there, as the engine does (LwsWebSocketsManager.cpp:447-456). T29: the others carry it in user
	if (Reason != LWS_CALLBACK_OPENSSL_PERFORM_SERVER_CERT_VERIFICATION && User && Live.Contains(static_cast<const IServiceConnection*>(User)))
	{
		return static_cast<IServiceConnection*>(User);
	}
	void* WsiUser = Wsi ? lws_wsi_user(Wsi) : nullptr;
	return WsiUser && Live.Contains(static_cast<const IServiceConnection*>(WsiUser)) ? static_cast<IServiceConnection*>(WsiUser) : nullptr;
}

void VaRest::WebSocket::Lws::FService::LoadClientCertificates(SSL_CTX* Ctx)
{
	// The engine's roots (LwsWebSocketsManager.cpp:441-446): with LWS's own context they come only from here (T23, tls.load_extra_fired)
	if (Certificates && Ctx)
	{
		Certificates->AddCertificatesToSslContext(Ctx);
	}
	AddExtraTrusted(Ctx);
}

void VaRest::WebSocket::Lws::FService::AddExtraTrusted(SSL_CTX* Ctx) const
{
	if (!Ctx || Options.ExtraTrustedPem.Num() == 0)
	{
		return;
	}
	BIO* Bio = BIO_new_mem_buf(Options.ExtraTrustedPem.GetData(), Options.ExtraTrustedPem.Num());
	if (!Bio)
	{
		return;
	}
	X509_STORE* Store = SSL_CTX_get_cert_store(Ctx);
	int32 Added = 0;
	while (X509* Certificate = PEM_read_bio_X509(Bio, nullptr, nullptr, nullptr))
	{
		Added += X509_STORE_add_cert(Store, Certificate) == 1 ? 1 : 0;
		X509_free(Certificate);
	}
	// The read that ends the loop leaves "no start line" on OpenSSL's error queue (VaRestLwsProbe.cpp:985-986)
	ERR_clear_error();
	BIO_free(Bio);
	UE_LOG(LogVaRestSocket, Verbose, TEXT("libwebsockets: %d extra trusted certificate(s)"), Added);
}

void VaRest::WebSocket::Lws::FService::ReleaseSsl()
{
	// Modules unload last-loaded first (ModuleManager.cpp:1458): at module shutdown the SSL module, loaded after this one, may be gone
	if (!SslManager || !FModuleManager::Get().IsModuleLoaded(TEXT("SSL")))
	{
		ProvidedSslCtx = nullptr;
		bSslInitialized = false;
		return;
	}
	if (ProvidedSslCtx)
	{
		SslManager->DestroySslContext(ProvidedSslCtx);
		ProvidedSslCtx = nullptr;
	}
	if (bSslInitialized)
	{
		// One ShutdownSsl per InitializeSsl that counted (ISslManager.h:58-69; outside monolithic and merged builds neither
		// counts, SslManager.cpp:26-74)
		SslManager->ShutdownSsl();
		bSslInitialized = false;
	}
}
