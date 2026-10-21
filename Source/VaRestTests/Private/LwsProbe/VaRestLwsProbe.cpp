// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestLwsProbe.h"

DEFINE_LOG_CATEGORY(LogVaRestLwsProbe);

#if WITH_DEV_AUTOMATION_TESTS

#include "GenericPlatform/GenericPlatformHttp.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/Guid.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "Ssl.h"
#include "VaRestClient.h"
#include "VaRestHttpTesting.h"

namespace VaRest::LwsProbe::VaRestLwsProbePrivate
{
	/** "http://127.0.0.1:41234" -> host and port */
	bool SplitUrl(const FString& Url, const TCHAR* Scheme, FString& OutHost, int32& OutPort)
	{
		const FString Prefix = FString(Scheme) + TEXT("://");
		if (!Url.StartsWith(Prefix, ESearchCase::IgnoreCase))
		{
			return false;
		}
		FString Authority = Url.Mid(Prefix.Len());
		int32 Slash = INDEX_NONE;
		if (Authority.FindChar(TEXT('/'), Slash))
		{
			Authority.LeftInline(Slash);
		}
		FString PortText;
		if (!Authority.Split(TEXT(":"), &OutHost, &PortText, ESearchCase::CaseSensitive, ESearchDir::FromEnd) || !PortText.IsNumeric())
		{
			return false;
		}
		OutPort = FCString::Atoi(*PortText);
		return !OutHost.IsEmpty() && OutPort > 0 && OutPort < 65536;
	}

	/** In the order of EVerdict */
	const TCHAR* const VerdictNames[] = { TEXT("agrees"), TEXT("differs"), TEXT("recorded"), TEXT("unverified") };

	/** Copies kept of callback bytes: enough for every probe's evidence, bounded against a flood */
	constexpr int64 MaxBytesPerEvent = 64 * 1024;
	constexpr int64 MaxBytesPerRig = 64 * 1024 * 1024;

	/** A UTF-8 copy for lws_client_connect_via_info, kept in Out */
	const char* Utf8Copy(const FString& Text, TArray<ANSICHAR>& Out)
	{
		const FTCHARToUTF8 Converted(*Text);
		Out.SetNumUninitialized(Converted.Length() + 1);
		FMemory::Memcpy(Out.GetData(), Converted.Get(), Converted.Length());
		Out[Converted.Length()] = '\0';
		return Out.GetData();
	}
}

bool VaRest::LwsProbe::FindServer(FAutomationTestBase& Test, FServer& Out, bool bNeedTls)
{
	using namespace VaRest::LwsProbe::VaRestLwsProbePrivate;
	Out.HttpBase = VaRest::Http::Testing::GetTestServerUrl();
	if (Out.HttpBase.IsEmpty() || !SplitUrl(Out.HttpBase, TEXT("http"), Out.Host, Out.Port))
	{
		Test.AddError(TEXT("no test server: run Tools/run_tests.sh Probe.VaRest.Lws, which starts Tools/testserver/server.py and passes -VaRestTestServer=<url> and -VaRestTestServerTls=<url>"));
		return false;
	}
	FString TlsHost;
	if (!SplitUrl(VaRest::Http::Testing::GetTestServerTlsUrl(), TEXT("https"), TlsHost, Out.TlsPort))
	{
		Out.TlsPort = 0;
	}
	if (bNeedTls && Out.TlsPort == 0)
	{
		Test.AddError(TEXT("no TLS listener: -VaRestTestServerTls=<url> is missing (Tools/run_tests.sh passes it, Tools/testserver/server.py --tls-port)"));
		return false;
	}
	return true;
}

void VaRest::LwsProbe::Fact(FAutomationTestBase& Test, const FString& Id, EVerdict Verdict, const FString& Observed, const FString& Spec, const FString& Evidence)
{
	using namespace VaRest::LwsProbe::VaRestLwsProbePrivate;
	const FString Name = VerdictNames[static_cast<int32>(Verdict)];
	const FString Line = FVaRestJson::Object()
		.Set(TEXT("id"), Id)
		.Set(TEXT("verdict"), Name)
		.Set(TEXT("observed"), Observed)
		.Set(TEXT("spec"), Spec)
		.Set(TEXT("evidence"), Evidence)
		.Set(TEXT("test"), Test.GetTestFullName())
		.ToString();
	// Display: in the run's log file at the default verbosity, where Tools/lws_probe_passport.py reads it
	UE_LOG(LogVaRestLwsProbe, Display, TEXT("FACT %s"), *Line);
	Test.AddInfo(FString::Printf(TEXT("%s [%s]: %s"), *Id, *Name, *Observed));
}

void VaRest::LwsProbe::Pending(const FString& Id)
{
	UE_LOG(LogVaRestLwsProbe, Display, TEXT("PENDING %s"), *FVaRestJson::Object().Set(TEXT("id"), Id).ToString());
	// The next call may be the last thing the process does: this line must reach the file first
	GLog->Flush();
}

bool VaRest::LwsProbe::NoteOutcome(FAutomationTestBase& Test, const FString& What, bool bOutcome)
{
	Test.AddInfo(FString::Printf(TEXT("outcome: %s: %s"), *What, YesNo(bOutcome)));
	return bOutcome;
}

// ---- FRig

VaRest::LwsProbe::FRig::FRig(const FServer& InServer, FRigOptions InOptions)
	: Server(InServer)
	, Options(MoveTemp(InOptions))
{
	check(IsInGameThread());
	// One protocol, "varest" (WebSocket spec 2.1). per_session_data_size 0: the connection's userdata is ours
	// (connect_info.userdata), as the engine's (LwsWebSocketsManager.cpp:163-166, LwsWebSocket.cpp:807)
	Protocols[0].name = "varest";
	Protocols[0].callback = &FRig::Callback;
	Protocols[0].per_session_data_size = 0;
	Protocols[0].rx_buffer_size = Options.RxBufferSize;

	lws_context_creation_info Info = {};
	Info.port = CONTEXT_PORT_NO_LISTEN;
	Info.protocols = Protocols;
	Info.uid = -1;
	Info.gid = -1;
	Info.options = Options.ContextOptions;
	// The engine's 32 KiB (LwsWebSocketsManager.cpp:187-190): the handshake header buffer (WebSocket spec 15)
	Info.max_http_header_data2 = 32 * 1024;
	Info.pt_serv_buf_size = 32 * 1024;
	// WebSocket spec 2.1: our ping is per connection, LWS's own is off
	Info.ws_ping_pong_interval = 0;
	Info.timeout_secs = Options.TimeoutSecs;
	// Callback() finds the rig through lws_context_user(lws_get_context(wsi))
	Info.user = this;
	switch (Options.Tls)
	{
	case ETls::None:
		break;
	case ETls::LwsGlobalInit:
		Info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
		break;
	case ETls::SslModule:
	{
		// The engine's order (LwsWebSocketsManager.cpp:205-225). InitializeSsl succeeds only in monolithic builds
		// (SslManager.cpp:26-55); elsewhere CreateSslContext returns null (:76-131) and LWS makes its own context
		ISslManager& SslManager = FModuleManager::LoadModuleChecked<FSslModule>(TEXT("SSL")).GetSslManager();
		if (SslManager.InitializeSsl())
		{
			bSslInitialized = true;
			SslModuleContext = SslManager.CreateSslContext(FSslContextCreateOptions());
		}
		if (SslModuleContext)
		{
			bSslModuleContext = true;
			Info.provided_client_ssl_ctx = SslModuleContext;
		}
		else
		{
			Info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
		}
		break;
	}
	case ETls::ProvidedContext:
		// "Libwebsockets no longer is responsible for freeing the context" (libwebsockets.h:2765-2769)
		Info.provided_client_ssl_ctx = Options.ProvidedSslContext;
		break;
	}
	Context = lws_create_context(&Info);
	if (!Context)
	{
		return;
	}
	// 128 KiB, below normal: the engine's service thread (LwsWebSocketsManager.cpp:239-247)
	Thread = FRunnableThread::Create(this, TEXT("VaRestLwsProbe"), 128 * 1024, TPri_BelowNormal);
}

VaRest::LwsProbe::FRig::~FRig()
{
	Shutdown();
}

void VaRest::LwsProbe::FRig::Shutdown()
{
	check(IsInGameThread());
	if (Thread)
	{
		// Kill(true) calls Stop() and waits for Run() to return: the engine's order (LwsWebSocketsManager.cpp:298-303)
		Thread->Kill(true);
		delete Thread;
		Thread = nullptr;
	}
	if (Context)
	{
		// After the thread, on the game thread (LwsWebSocketsManager.cpp:305-309); the callbacks of connections still open
		// (CLOSED, WSI_DESTROY) arrive inside this call and are recorded like any other
		lws_context_destroy(Context);
		Context = nullptr;
	}
	if (SslModuleContext)
	{
		FModuleManager::GetModuleChecked<FSslModule>(TEXT("SSL")).GetSslManager().DestroySslContext(SslModuleContext);
		SslModuleContext = nullptr;
	}
	if (bSslInitialized)
	{
		// One ShutdownSsl per successful InitializeSsl (ISslManager.h:58-69)
		FModuleManager::GetModuleChecked<FSslModule>(TEXT("SSL")).GetSslManager().ShutdownSsl();
		bSslInitialized = false;
	}
}

uint32 VaRest::LwsProbe::FRig::Run()
{
	while (!bStopping.load())
	{
		if (!Options.bCommandsInsideCallback)
		{
			// The engine's place: before lws_service, on the same thread (LwsWebSocketsManager.cpp:485-509)
			DrainCommands();
		}
		lws_service(Context, Options.ServiceTimeoutMs);
	}
	return 0;
}

void VaRest::LwsProbe::FRig::Stop()
{
	bStopping = true;
	// The one LWS call another thread may make (libwebsockets.h:3704-3716): it ends the service thread's wait
	if (Context)
	{
		lws_cancel_service(Context);
	}
}

void VaRest::LwsProbe::FRig::Enqueue(TFunction<void()> Command)
{
	Commands.Enqueue(MoveTemp(Command));
	if (Context)
	{
		lws_cancel_service(Context);
	}
}

void VaRest::LwsProbe::FRig::DrainCommands()
{
	TFunction<void()> Command;
	while (Commands.Dequeue(Command))
	{
		Command();
	}
}

int32 VaRest::LwsProbe::FRig::Connect(FConnectSpec InSpec)
{
	check(IsInGameThread());
	TUniquePtr<FConn> Made = MakeUnique<FConn>();
	Made->Rig = this;
	Made->Id = NextId++;
	Made->Spec = MoveTemp(InSpec);
	FConn* Raw = Made.Get();
	{
		FScopeLock Lock(&ConnsLock);
		// Never removed before the rig dies: commands and callbacks may keep Raw
		Conns.Add(MoveTemp(Made));
	}
	Enqueue([this, Raw] { ConnectNow(*Raw); });
	return Raw->Id;
}

void VaRest::LwsProbe::FRig::ConnectNow(FConn& Conn)
{
	using namespace VaRest::LwsProbe::VaRestLwsProbePrivate;
	const FConnectSpec& Spec = Conn.Spec;
	const FString Address = Spec.Address.IsEmpty() ? Server.Host : Spec.Address;
	const int32 Port = Spec.Port != 0 ? Spec.Port : (Spec.bTls ? Server.TlsPort : Server.Port);
	const FString Host = Spec.Host.IsEmpty() ? FString::Printf(TEXT("%s:%d"), *Address, Port) : Spec.Host;

	lws_client_connect_info Info = {};
	Info.context = Context;
	Info.address = Utf8Copy(Address, Conn.AddressUtf8);
	Info.port = Port;
	Info.ssl_connection = Spec.bTls ? (LCCSCF_USE_SSL | Spec.SslFlags) : Spec.SslFlags;
	Info.path = Utf8Copy(Spec.Path, Conn.PathUtf8);
	Info.host = Utf8Copy(Host, Conn.HostUtf8);
	Info.origin = Spec.Origin.IsSet() ? Utf8Copy(Spec.Origin.GetValue(), Conn.OriginUtf8) : nullptr;
	Info.protocol = Spec.Protocols.IsEmpty() ? nullptr : Utf8Copy(Spec.Protocols, Conn.ProtocolsUtf8);
	Info.local_protocol_name = Spec.LocalProtocolName.IsEmpty() ? nullptr : Utf8Copy(Spec.LocalProtocolName, Conn.LocalNameUtf8);
	Info.ietf_version_or_minus_one = -1;
	Info.userdata = &Conn;
	// The wsi early, so a command (a kill) can reach a connection that has not opened yet (libwebsockets.h:3441-3451)
	Info.pwsi = &Conn.Wsi;
	Record(Conn.Id, ConnectCalling, 0, Host);
	lws* Made = lws_client_connect_via_info(&Info);
	if (!Made)
	{
		// NULL: the connection was "scrubbed" (libwebsockets.h:3442-3446), and nothing says LWS cleared *pwsi, so the early
		// wsi may be freed: no command may reach it, and FindConn must never match it
		Conn.Wsi = nullptr;
	}
	Record(Conn.Id, ConnectReturned, 0, Made ? TEXT("wsi") : TEXT("null"));
}

void VaRest::LwsProbe::FRig::Exec(const TCHAR* Label, TFunction<void(FRig&)> Command)
{
	const FString Name(Label);
	Enqueue([this, Name, Moved = MoveTemp(Command)]() mutable
	{
		Record(0, CommandRan, 0, Name);
		Moved(*this);
	});
}

void VaRest::LwsProbe::FRig::ExecOn(int32 ConnId, const TCHAR* Label, TFunction<void(FConn&)> Command)
{
	FConn* Target = nullptr;
	{
		FScopeLock Lock(&ConnsLock);
		for (const TUniquePtr<FConn>& Each : Conns)
		{
			if (Each->Id == ConnId)
			{
				Target = Each.Get();
				break;
			}
		}
	}
	const FString Name(Label);
	Enqueue([this, Target, ConnId, Name, Moved = MoveTemp(Command)]() mutable
	{
		if (!Target || !Target->Wsi)
		{
			Record(ConnId, CommandRan, 0, Name + TEXT(": no wsi"));
			return;
		}
		Record(ConnId, CommandRan, 0, Name);
		Moved(*Target);
	});
}

void VaRest::LwsProbe::FRig::Write(int32 ConnId, int32 Protocol, TArray<uint8> Payload)
{
	FWrite Item;
	Item.Protocol = Protocol;
	Item.Buffer.SetNumZeroed(LWS_PRE);
	Item.Buffer.Append(Payload);
	ExecOn(ConnId, TEXT("write"), [Moved = MoveTemp(Item)](FConn& Conn) mutable
	{
		Conn.Outbox.Add(MoveTemp(Moved));
		lws_callback_on_writable(Conn.Wsi);
	});
}

void VaRest::LwsProbe::FRig::WriteText(int32 ConnId, const FString& Text, int32 Protocol)
{
	Write(ConnId, Protocol, Utf8Bytes(Text));
}

void VaRest::LwsProbe::FRig::Close(int32 ConnId, int32 Code, const FString& Reason)
{
	ExecOn(ConnId, TEXT("close"), [Code, Moved = Utf8Bytes(Reason)](FConn& Conn) mutable
	{
		Conn.CloseCode = Code;
		Conn.CloseReason = MoveTemp(Moved);
		lws_callback_on_writable(Conn.Wsi);
	});
}

int32 VaRest::LwsProbe::FRig::WriteNow(FConn& Conn, lws* Wsi, int32 Protocol, TConstArrayView<uint8> Payload, const TCHAR* Label)
{
	TArray<uint8> Buffer;
	Buffer.SetNumZeroed(LWS_PRE + Payload.Num());
	if (Payload.Num() > 0)
	{
		FMemory::Memcpy(Buffer.GetData() + LWS_PRE, Payload.GetData(), Payload.Num());
	}
	// A truncated write is buffered by LWS itself (libwebsockets.h:4914-4930), so Buffer may go when this returns
	const int Wrote = lws_write(Wsi, Buffer.GetData() + LWS_PRE, static_cast<size_t>(Payload.Num()), static_cast<lws_write_protocol>(Protocol));
	Record(Conn.Id, WriteDone, Payload.Num(), FString::Printf(TEXT("%s ret=%d partial=%d"), Label, Wrote, lws_partial_buffered(Wsi)));
	return Wrote;
}

int32 VaRest::LwsProbe::FRig::WriteNext(FConn& Conn, lws* Wsi)
{
	if (Conn.Outbox.Num() == 0)
	{
		if (Conn.CloseCode != 0)
		{
			// The engine's close (LwsWebSocket.cpp:515-530): the close frame's code and reason, then a non-zero return
			lws_close_reason(Wsi, static_cast<lws_close_status>(Conn.CloseCode), Conn.CloseReason.GetData(), static_cast<size_t>(Conn.CloseReason.Num()));
			Record(Conn.Id, CloseRequested, Conn.CloseReason.Num(), FString::Printf(TEXT("code=%d"), Conn.CloseCode));
			return -1;
		}
		return 0;
	}
	FWrite Next = MoveTemp(Conn.Outbox[0]);
	Conn.Outbox.RemoveAt(0);
	const int32 Size = Next.Buffer.Num() - LWS_PRE;
	const int Wrote = lws_write(Wsi, Next.Buffer.GetData() + LWS_PRE, static_cast<size_t>(Size), static_cast<lws_write_protocol>(Next.Protocol));
	Record(Conn.Id, WriteDone, Size, FString::Printf(TEXT("outbox ret=%d partial=%d"), Wrote, lws_partial_buffered(Wsi)));
	if (Wrote < 0)
	{
		return -1;
	}
	if (Conn.Outbox.Num() > 0 || Conn.CloseCode != 0)
	{
		lws_callback_on_writable(Wsi);
	}
	return 0;
}

int32 VaRest::LwsProbe::FRig::AppendHeaders(FConn& Conn, void* In, size_t Len)
{
	// in: a char** to the next free byte; len: the space left (libwebsockets.h:1316-1340). Each line is checked against
	// what is left, the check the engine skips (LwsWebSocket.cpp:537-547, WebSocket spec E19)
	char** Cursor = static_cast<char**>(In);
	int64 Written = 0;
	bool bAll = true;
	for (const FString& Line : Conn.Spec.AppendHeaders)
	{
		const FTCHARToUTF8 Converted(*Line);
		const int64 Need = Converted.Length() + 2;
		if (Written + Need > static_cast<int64>(Len))
		{
			bAll = false;
			break;
		}
		FMemory::Memcpy(*Cursor, Converted.Get(), Converted.Length());
		(*Cursor)[Converted.Length()] = '\r';
		(*Cursor)[Converted.Length() + 1] = '\n';
		*Cursor += Need;
		Written += Need;
	}
	Record(Conn.Id, HeadersAppended, Written, bAll ? TEXT("all") : TEXT("stopped: no space"));
	return 0;
}

VaRest::LwsProbe::FConn* VaRest::LwsProbe::FRig::FindConn(lws* Wsi, void* User)
{
	// The userdata first, over every connection, and a live wsi only when no connection owns the userdata: a freed wsi's
	// address may be reused by the next connection (LWS 3.0 does: Probe.VaRest.Lws.Rig.SequentialConnections fails when
	// the wsi is matched first), a FConn's never is. One pass with both tests would let an older connection whose stored
	// wsi went stale win over the connection the userdata names
	void* WsiUser = Wsi ? lws_wsi_user(Wsi) : nullptr;
	FScopeLock Lock(&ConnsLock);
	for (const TUniquePtr<FConn>& Each : Conns)
	{
		FConn* Candidate = Each.Get();
		if (Candidate == User || Candidate == WsiUser)
		{
			return Candidate;
		}
	}
	if (Wsi)
	{
		for (const TUniquePtr<FConn>& Each : Conns)
		{
			FConn* Candidate = Each.Get();
			if (!Candidate->bDestroyed && Candidate->Wsi == Wsi)
			{
				return Candidate;
			}
		}
	}
	return nullptr;
}

int VaRest::LwsProbe::FRig::Callback(lws* Wsi, lws_callback_reasons Reason, void* User, void* In, size_t Len)
{
	// Also called inside lws_create_context (protocol init, client TLS setup), before FRig::Context is set: the rig comes
	// from the context's user pointer (lws_context_creation_info::user), not from the member
	lws_context* Owner = Wsi ? lws_get_context(Wsi) : nullptr;
	FRig* Rig = Owner ? static_cast<FRig*>(lws_context_user(Owner)) : nullptr;
	return Rig ? Rig->Handle(Wsi, static_cast<int32>(Reason), User, In, Len) : 0;
}

int32 VaRest::LwsProbe::FRig::Handle(lws* Wsi, int32 Reason, void* User, void* In, size_t Len)
{
	switch (Reason)
	{
	// Poll bookkeeping LWS sends with its internal loop too (libwebsockets.h:1396-1449): not evidence
	case LWS_CALLBACK_GET_THREAD_ID:
	case LWS_CALLBACK_ADD_POLL_FD:
	case LWS_CALLBACK_DEL_POLL_FD:
	case LWS_CALLBACK_CHANGE_MODE_POLL_FD:
	case LWS_CALLBACK_LOCK_POLL:
	case LWS_CALLBACK_UNLOCK_POLL:
		return 0;
	case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
		// lws_cancel_service lands here, on the service thread (libwebsockets.h:1551-1556): WebSocket spec 2.1's command point
		if (Options.bCommandsInsideCallback)
		{
			DrainCommands();
		}
		return 0;
	default:
		break;
	}

	FConn* Conn = FindConn(Wsi, User);
	FEvent Event;
	Event.Conn = Conn ? Conn->Id : 0;
	Event.Reason = Reason;
	Event.Time = FPlatformTime::Seconds();
	Event.Len = static_cast<int64>(Len);
	if (Conn)
	{
		Event.Flags |= User == Conn ? UserIsConnection : 0u;
		Event.Flags |= Wsi && lws_wsi_user(Wsi) == Conn ? WsiUserIsConnection : 0u;
	}
	switch (Reason)
	{
	case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
		// in: an error string of len bytes, when not null (libwebsockets.h:1255-1299)
		Event.Note = Utf8Text(In, Len);
		break;
	case LWS_CALLBACK_CLIENT_FILTER_PRE_ESTABLISH:
	{
		// The last point the response headers exist (libwebsockets.h:1301-1307): the selected subprotocol and the status.
		// lws_http_client_http_response is safe here because the header table exists by definition in this callback
		// (T6); without one it dereferences NULL (client.c.o 0x1294-0x12b2), which is why the CONNECTION_ERROR cases
		// are probed one per run (Probe.VaRest.Risky.Lws.StatusInConnectionError.*)
		char Selected[256] = {};
		const int Total = lws_hdr_total_length(Wsi, WSI_TOKEN_PROTOCOL);
		if (Total > 0 && Total < static_cast<int>(sizeof(Selected)))
		{
			lws_hdr_copy(Wsi, Selected, static_cast<int>(sizeof(Selected)), WSI_TOKEN_PROTOCOL);
		}
		Event.Note = FString::Printf(TEXT("protocol=%s status=%u"), *Utf8Text(Selected, FCStringAnsi::Strlen(Selected)), lws_http_client_http_response(Wsi));
		break;
	}
	case LWS_CALLBACK_CLIENT_ESTABLISHED:
	{
		const lws_protocols* Bound = lws_get_protocol(Wsi);
		const FString BoundName = Bound && Bound->name ? Utf8Text(Bound->name, FCStringAnsi::Strlen(Bound->name)) : FString(TEXT("(none)"));
		Event.Note = TEXT("bound=") + BoundName;
		break;
	}
	case LWS_CALLBACK_CLIENT_RECEIVE:
		Event.Flags |= lws_is_first_fragment(Wsi) ? FirstFragment : 0u;
		Event.Flags |= lws_is_final_fragment(Wsi) ? FinalFragment : 0u;
		Event.Flags |= lws_frame_is_binary(Wsi) ? BinaryFrame : 0u;
		Event.Remaining = static_cast<int64>(lws_remaining_packet_payload(Wsi));
		CopyBytes(Event, In, Len);
		break;
	case LWS_CALLBACK_CLIENT_RECEIVE_PONG:
	case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE:
		CopyBytes(Event, In, Len);
		break;
	case LWS_CALLBACK_OPENSSL_PERFORM_SERVER_CERT_VERIFICATION:
	{
		// user: the X509_STORE_CTX*, in: the SSL*, len: preverify_ok (libwebsockets.h:1015-1044)
		X509_STORE_CTX* Store = static_cast<X509_STORE_CTX*>(User);
		Event.Note = FString::Printf(TEXT("preverify=%d error=%d depth=%d"), static_cast<int32>(Len), X509_STORE_CTX_get_error(Store), X509_STORE_CTX_get_error_depth(Store));
		break;
	}
	case LWS_CALLBACK_WSI_DESTROY:
		if (Conn)
		{
			Conn->Wsi = nullptr;
			Conn->bDestroyed = true;
		}
		break;
	default:
		break;
	}
	Append(MoveTemp(Event));

	if (Conn && Conn->Spec.Hook)
	{
		const int32 Hooked = Conn->Spec.Hook(*Conn, Wsi, Reason, User, In, Len);
		if (Hooked != KeepDefault)
		{
			return Hooked;
		}
	}

	switch (Reason)
	{
	case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
		return Conn ? AppendHeaders(*Conn, In, Len) : 0;
	case LWS_CALLBACK_CLIENT_WRITEABLE:
		return Conn ? WriteNext(*Conn, Wsi) : 0;
	case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE:
		// 0 lets LWS echo the close (libwebsockets.h:1217-1225); the engine returns 1 (LwsWebSocket.cpp:389-390)
		return Conn ? Conn->Spec.PeerCloseReturn : 0;
	case LWS_CALLBACK_OPENSSL_LOAD_EXTRA_CLIENT_VERIFY_CERTS:
		if (Options.OnLoadExtraClientVerifyCerts)
		{
			Options.OnLoadExtraClientVerifyCerts(static_cast<SSL_CTX*>(User));
		}
		return 0;
	case LWS_CALLBACK_OPENSSL_PERFORM_SERVER_CERT_VERIFICATION:
		// 0 keeps OpenSSL's verdict, 1 fails the certificate (libwebsockets.h:1029-1037)
		return Options.OnVerifyServerCert && Conn ? Options.OnVerifyServerCert(*Conn, static_cast<X509_STORE_CTX*>(User), static_cast<int64>(Len)) : 0;
	default:
		return 0;
	}
}

void VaRest::LwsProbe::FRig::CopyBytes(FEvent& Event, const void* In, size_t Len)
{
	using namespace VaRest::LwsProbe::VaRestLwsProbePrivate;
	if (!In || Len == 0)
	{
		return;
	}
	const int64 Take = FMath::Min<int64>(static_cast<int64>(Len), MaxBytesPerEvent);
	FScopeLock Lock(&EventsLock);
	if (CopiedBytes + Take > MaxBytesPerRig)
	{
		Event.Note += TEXT(" [bytes not kept: over the rig's 64 MiB]");
		return;
	}
	CopiedBytes += Take;
	Event.Bytes.Append(static_cast<const uint8*>(In), static_cast<int32>(Take));
	if (Take < static_cast<int64>(Len))
	{
		Event.Note += FString::Printf(TEXT(" [first %lld of %lld bytes kept]"), Take, static_cast<int64>(Len));
	}
}

void VaRest::LwsProbe::FRig::Append(FEvent&& Event)
{
	FScopeLock Lock(&EventsLock);
	Recorded.Add(MoveTemp(Event));
}

void VaRest::LwsProbe::FRig::Record(int32 ConnId, int32 Reason, int64 Len, const FString& Note)
{
	FEvent Event;
	Event.Conn = ConnId;
	Event.Reason = Reason;
	Event.Time = FPlatformTime::Seconds();
	Event.Len = Len;
	Event.Note = Note;
	Append(MoveTemp(Event));
}

bool VaRest::LwsProbe::FRig::WaitFor(TFunctionRef<bool(const TArray<FEvent>&)> Done, double Seconds)
{
	// The core ticker too: the HTTP client and the engine's IWebSocket deliver from it (LwsWebSocketsManager.cpp:257-258,
	// BackgroundableTicker.cpp:18-29)
	return VaRest::Http::Testing::PumpUntil([this, &Done]
	{
		FScopeLock Lock(&EventsLock);
		return Done(Recorded);
	}, Seconds, true);
}

bool VaRest::LwsProbe::FRig::WaitReason(int32 ConnId, int32 Reason, double Seconds)
{
	return WaitFor([ConnId, Reason](const TArray<FEvent>& All)
	{
		return All.ContainsByPredicate([ConnId, Reason](const FEvent& Each) { return Each.Conn == ConnId && Each.Reason == Reason; });
	}, Seconds);
}

bool VaRest::LwsProbe::FRig::WaitEnded(int32 ConnId, double Seconds)
{
	return WaitFor([ConnId](const TArray<FEvent>& All)
	{
		return All.ContainsByPredicate([ConnId](const FEvent& Each)
		{
			return Each.Conn == ConnId && (Each.Reason == LWS_CALLBACK_WSI_DESTROY || (Each.Reason == ConnectReturned && Each.Note == TEXT("null")));
		});
	}, Seconds);
}

TArray<VaRest::LwsProbe::FEvent> VaRest::LwsProbe::FRig::Events(int32 ConnId) const
{
	FScopeLock Lock(&EventsLock);
	if (ConnId == 0)
	{
		return Recorded;
	}
	return Recorded.FilterByPredicate([ConnId](const FEvent& Each) { return Each.Conn == ConnId; });
}

bool VaRest::LwsProbe::FRig::Find(int32 ConnId, int32 Reason, FEvent& Out, const TCHAR* NotePrefix) const
{
	FScopeLock Lock(&EventsLock);
	for (const FEvent& Each : Recorded)
	{
		if (Each.Conn == ConnId && Each.Reason == Reason && (!NotePrefix || Each.Note.StartsWith(NotePrefix, ESearchCase::CaseSensitive)))
		{
			Out = Each;
			return true;
		}
	}
	return false;
}

// ---- helpers

FString VaRest::LwsProbe::NewKey()
{
	return FGuid::NewGuid().ToString(EGuidFormats::Digits);
}

FString VaRest::LwsProbe::Utf8Text(const void* Data, size_t Num)
{
	if (!Data || Num == 0)
	{
		return FString();
	}
	const FUTF8ToTCHAR Converted(static_cast<const ANSICHAR*>(Data), static_cast<int32>(Num));
	return FString::ConstructFromPtrSize(Converted.Get(), Converted.Length());
}

FString VaRest::LwsProbe::EventText(const FEvent& Event)
{
	return Utf8Text(Event.Bytes.GetData(), static_cast<size_t>(Event.Bytes.Num()));
}

TArray<uint8> VaRest::LwsProbe::Utf8Bytes(const FString& Source)
{
	const FTCHARToUTF8 Converted(*Source);
	return TArray<uint8>(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
}

FString VaRest::LwsProbe::HexOf(TConstArrayView<uint8> Bytes)
{
	static const TCHAR Digits[] = TEXT("0123456789abcdef");
	FString Out;
	Out.Reserve(Bytes.Num() * 2);
	for (const uint8 Byte : Bytes)
	{
		Out.AppendChar(Digits[Byte >> 4]);
		Out.AppendChar(Digits[Byte & 15]);
	}
	return Out;
}

FString VaRest::LwsProbe::ReasonName(int32 Reason)
{
	switch (Reason)
	{
	case ConnectCalling: return TEXT("connect>");
	case ConnectReturned: return TEXT("connect<");
	case CommandRan: return TEXT("command");
	case WriteDone: return TEXT("write");
	case CloseRequested: return TEXT("close>");
	case HeadersAppended: return TEXT("headers+");
	case Marker: return TEXT("marker");
	case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: return TEXT("CONNECTION_ERROR");
	case LWS_CALLBACK_CLIENT_FILTER_PRE_ESTABLISH: return TEXT("FILTER_PRE_ESTABLISH");
	case LWS_CALLBACK_CLIENT_ESTABLISHED: return TEXT("ESTABLISHED");
	case LWS_CALLBACK_CLOSED: return TEXT("CLOSED");
	case LWS_CALLBACK_CLIENT_RECEIVE: return TEXT("RECEIVE");
	case LWS_CALLBACK_CLIENT_RECEIVE_PONG: return TEXT("RECEIVE_PONG");
	case LWS_CALLBACK_CLIENT_WRITEABLE: return TEXT("WRITEABLE");
	case LWS_CALLBACK_OPENSSL_LOAD_EXTRA_CLIENT_VERIFY_CERTS: return TEXT("LOAD_EXTRA_CLIENT_VERIFY_CERTS");
	case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: return TEXT("APPEND_HANDSHAKE_HEADER");
	case LWS_CALLBACK_PROTOCOL_INIT: return TEXT("PROTOCOL_INIT");
	case LWS_CALLBACK_PROTOCOL_DESTROY: return TEXT("PROTOCOL_DESTROY");
	case LWS_CALLBACK_WSI_CREATE: return TEXT("WSI_CREATE");
	case LWS_CALLBACK_WSI_DESTROY: return TEXT("WSI_DESTROY");
	case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE: return TEXT("PEER_INITIATED_CLOSE");
	case LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP: return TEXT("ESTABLISHED_CLIENT_HTTP");
	case LWS_CALLBACK_CLOSED_CLIENT_HTTP: return TEXT("CLOSED_CLIENT_HTTP");
	case LWS_CALLBACK_HTTP_BIND_PROTOCOL: return TEXT("HTTP_BIND_PROTOCOL");
	case LWS_CALLBACK_HTTP_DROP_PROTOCOL: return TEXT("HTTP_DROP_PROTOCOL");
	case LWS_CALLBACK_OPENSSL_PERFORM_SERVER_CERT_VERIFICATION: return TEXT("PERFORM_SERVER_CERT_VERIFICATION");
	case LWS_CALLBACK_TIMER: return TEXT("TIMER");
	case LWS_CALLBACK_CLIENT_CLOSED: return TEXT("CLIENT_CLOSED");
	default: return FString::Printf(TEXT("reason %d"), Reason);
	}
}

FString VaRest::LwsProbe::CallSequence(const TArray<FEvent>& Events)
{
	TArray<FString> Parts;
	int32 Last = MIN_int32;
	int32 Repeats = 0;
	const auto Flush = [&Parts, &Last, &Repeats]
	{
		if (Repeats > 0)
		{
			Parts.Add(Repeats > 1 ? FString::Printf(TEXT("%s x%d"), *ReasonName(Last), Repeats) : ReasonName(Last));
		}
	};
	for (const FEvent& Each : Events)
	{
		// The rig's own bookkeeping is not a callback
		if (Each.Reason == CommandRan || Each.Reason == ConnectCalling || Each.Reason == HeadersAppended || Each.Reason == Marker)
		{
			continue;
		}
		if (Each.Reason == Last)
		{
			++Repeats;
			continue;
		}
		Flush();
		Last = Each.Reason;
		Repeats = 1;
	}
	Flush();
	return FString::Join(Parts, TEXT(" > "));
}

TArray<VaRest::LwsProbe::FEvent> VaRest::LwsProbe::OfReason(const TArray<FEvent>& Events, int32 Reason)
{
	return Events.FilterByPredicate([Reason](const FEvent& Each) { return Each.Reason == Reason; });
}

bool VaRest::LwsProbe::Opened(const TArray<FEvent>& Events)
{
	return Events.ContainsByPredicate([](const FEvent& Each) { return Each.Reason == LWS_CALLBACK_CLIENT_ESTABLISHED; });
}

FString VaRest::LwsProbe::ErrorOf(const TArray<FEvent>& Events)
{
	const FEvent* Error = Events.FindByPredicate([](const FEvent& Each) { return Each.Reason == LWS_CALLBACK_CLIENT_CONNECTION_ERROR; });
	return Error ? Error->Note : FString::Printf(TEXT("(no CONNECTION_ERROR: %s)"), *CallSequence(Events));
}

FString VaRest::LwsProbe::ConnectResult(const TArray<FEvent>& Events)
{
	const FEvent* Returned = Events.FindByPredicate([](const FEvent& Each) { return Each.Reason == ConnectReturned; });
	return Returned ? Returned->Note : FString(TEXT("(not called)"));
}

bool VaRest::LwsProbe::Visit(FAutomationTestBase& Test, FRig& Rig, FConnectSpec Spec, TArray<FEvent>& Out, double Seconds)
{
	const int32 ConnId = Rig.Connect(MoveTemp(Spec));
	Rig.WaitFor([ConnId](const TArray<FEvent>& All)
	{
		return All.ContainsByPredicate([ConnId](const FEvent& Each)
		{
			return Each.Conn == ConnId && (Each.Reason == LWS_CALLBACK_CLIENT_ESTABLISHED || Each.Reason == LWS_CALLBACK_WSI_DESTROY
				|| (Each.Reason == ConnectReturned && Each.Note == TEXT("null")));
		});
	}, Seconds);
	if (Opened(Rig.Events(ConnId)))
	{
		Rig.Close(ConnId, 1000, TEXT("probe done"));
	}
	const bool bEnded = Rig.WaitEnded(ConnId, Seconds);
	Out = Rig.Events(ConnId);
	NoteOutcome(Test, FString::Printf(TEXT("connection %d ended within %.0f s (%s)"), ConnId, Seconds, *CallSequence(Out)), bEnded);
	return bEnded;
}

bool VaRest::LwsProbe::WaitBytes(FRig& Rig, int32 ConnId, int64 Bytes, double Seconds)
{
	return Rig.WaitFor([ConnId, Bytes](const TArray<FEvent>& All)
	{
		int64 Got = 0;
		for (const FEvent& Each : All)
		{
			Got += Each.Conn == ConnId && Each.Reason == LWS_CALLBACK_CLIENT_RECEIVE ? Each.Len : 0;
		}
		return Got >= Bytes;
	}, Seconds);
}

FVaRestJson VaRest::LwsProbe::FetchJournal(FAutomationTestBase& Test, const FServer& Site, const FString& Key, int32 Connections, int32 WaitMs)
{
	using namespace VaRest::Http::Testing;
	FVaRestClient Client = FVaRestClient::Make(Site.HttpBase);
	Client.WithLogLevel(EVaRestLogLevel::None);
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Client.Get(FString::Printf(TEXT("/ws/journal?key=%s&connections=%d&wait_ms=%d"), *Key, Connections, WaitMs)), Outcome).Send();
	const bool bDone = PumpUntil([Outcome] { return Outcome->IsDone(); }, WaitMs / 1000.0 + 15.0, true);
	if (!bDone || Outcome->SuccessCalls.load() != 1 || !Outcome->Value.IsSet())
	{
		Test.AddError(FString::Printf(TEXT("the journal of %s did not arrive: %s"), *Key, bDone ? *Outcome->Error.Describe() : TEXT("no answer")));
		return FVaRestJson();
	}
	return Outcome->Value.GetValue();
}

TArray<FVaRestJson> VaRest::LwsProbe::JournalEvents(const FVaRestJson& Journal, const TCHAR* Kind, int32 ConnIndex)
{
	TArray<FVaRestJson> Out;
	for (const FVaRestJson& Event : Journal[TEXT("events")].Elements())
	{
		if (Event[TEXT("ev")].AsString().Equals(Kind, ESearchCase::CaseSensitive) && (ConnIndex == 0 || Event[TEXT("conn")].AsInt() == ConnIndex))
		{
			Out.Add(Event);
		}
	}
	return Out;
}

FString VaRest::LwsProbe::JournalHeader(const FVaRestJson& Journal, const TCHAR* Name, int32 ConnIndex)
{
	for (const FVaRestJson& Request : JournalEvents(Journal, TEXT("request"), ConnIndex))
	{
		for (const FVaRestJson& Pair : Request[TEXT("headers")].Elements())
		{
			// HTTP header names are case-insensitive (RFC 9110 5.1)
			if (Pair[0].AsString().Equals(Name, ESearchCase::IgnoreCase))
			{
				return Pair[1].AsString();
			}
		}
	}
	return TEXT("(absent)");
}

TArray<uint8> VaRest::LwsProbe::JournalBytes(const FVaRestJson& Event)
{
	TArray<uint8> Out;
	FBase64::Decode(Event[TEXT("b64")].AsString(), Out);
	return Out;
}

bool VaRest::LwsProbe::FetchBytes(FAutomationTestBase& Test, const FServer& Site, const FString& Path, TArray<uint8>& Out)
{
	using namespace VaRest::Http::Testing;
	FVaRestClient Client = FVaRestClient::Make(Site.HttpBase);
	Client.WithLogLevel(EVaRestLogLevel::None);
	auto Outcome = MakeOutcome<TArray<uint8>>();
	Observe(Client.Get<TArray<uint8>>(Path), Outcome).Send();
	const bool bDone = PumpUntil([Outcome] { return Outcome->IsDone(); }, 20.0, true);
	if (!bDone || Outcome->SuccessCalls.load() != 1 || !Outcome->Value.IsSet())
	{
		Test.AddError(FString::Printf(TEXT("GET %s failed: %s"), *Path, bDone ? *Outcome->Error.Describe() : TEXT("no answer within 20 s")));
		return false;
	}
	Out = Outcome->Value.GetValue();
	return true;
}

int32 VaRest::LwsProbe::AddPemToStore(SSL_CTX* Ctx, TConstArrayView<uint8> Pem)
{
	BIO* Bio = BIO_new_mem_buf(Pem.GetData(), Pem.Num());
	if (!Bio)
	{
		return 0;
	}
	X509_STORE* Store = SSL_CTX_get_cert_store(Ctx);
	int32 Added = 0;
	while (X509* Certificate = PEM_read_bio_X509(Bio, nullptr, nullptr, nullptr))
	{
		Added += X509_STORE_add_cert(Store, Certificate) == 1 ? 1 : 0;
		X509_free(Certificate);
	}
	// The read that ends the loop leaves "no start line" on OpenSSL's error queue
	ERR_clear_error();
	BIO_free(Bio);
	return Added;
}

FString VaRest::LwsProbe::UrlValue(const FString& Value)
{
	return FGenericPlatformHttp::UrlEncode(Value);
}

bool VaRest::LwsProbe::IsMessageStart(int32 Protocol)
{
	// Without LWS_WRITE_NO_FIN (0x40) and LWS_WRITE_H2_STREAM_END (0x80), libwebsockets.h:4819-4827
	const int32 Kind = Protocol & 0x3F;
	return Kind == LWS_WRITE_TEXT || Kind == LWS_WRITE_BINARY;
}

#endif // WITH_DEV_AUTOMATION_TESTS
