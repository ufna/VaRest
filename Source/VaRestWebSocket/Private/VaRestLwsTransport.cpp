// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestLwsTransport.h"

#include "Async/Async.h"
#include "Modules/ModuleManager.h"
#include "VaRestLwsConn.h"
#include "VaRestSocketLink.h"
#include "VaRestWebSocketLog.h"

namespace VaRest::WebSocket::Lws::VaRestLwsTransportPrivate
{
	/** Set once GetDefaultTransport made the default transport */
	std::atomic<bool> bDefaultMade{ false };

	/**
	 * The consumer's handle on one FConn (WebSocket spec 1.3): commands go to the service thread, and the link is shared with
	 * it. Send and Close are game thread; PopEvents and Consumed the consumer's thread; the getters and the destructor any thread
	 */
	class FHandle final : public IVaRestSocketConnection
	{
	public:
		FHandle(TWeakPtr<FService, ESPMode::ThreadSafe> InService, uint64 InId, TSharedRef<FSocketLink, ESPMode::ThreadSafe> InLink)
			: Service(MoveTemp(InService))
			, Id(InId)
			, Link(MoveTemp(InLink))
		{
		}

		virtual ~FHandle() override
		{
			// Any thread (spec 1.4): the events stop at once; the service thread closes or cancels the connection
			Link->bReleased = true;
			Post([ConnId = Id](FService& Self)
			{
				if (FConn* Conn = FConn::Find(Self, ConnId))
				{
					Conn->Release(Self);
				}
			});
		}

		virtual int32 PopEvents(TArray<FVaRestSocketEvent>& Out) override
		{
			return Link->PopAll(Out);
		}

		virtual bool Send(FVaRestSocketFrame&& Frame) override
		{
			check(IsInGameThread());
			// Review conc M-6: after Close the service thread would drop it (FConn::Send, bCloseQueued); say so here instead
			if (!Frame.IsValid() || Link->bEnded.load() || Link->bCloseRequested.load())
			{
				return false;
			}
			// Counted on the caller's side, so the buffered amount includes what the service thread has not seen yet (spec 2.4).
			// What this add produced goes with the frame: the high-water mark is decided on it (Task 3 review I-1)
			const int64 Length = Frame.Length;
			const int64 After = Link->BufferedBytes.fetch_add(Length) + Length;
			const bool bQueued = Post([ConnId = Id, Moved = MoveTemp(Frame), Shared = Link, After](FService& Self) mutable
			{
				if (FConn* Conn = FConn::Find(Self, ConnId))
				{
					Conn->Send(Self, MoveTemp(Moved), After);
				}
				else
				{
					Shared->BufferedBytes.fetch_sub(Moved.Length);
				}
			});
			if (!bQueued)
			{
				// No service took it (Task 6: use Enqueue's result): the bytes were never the socket's
				Link->BufferedBytes.fetch_sub(Length);
			}
			return bQueued;
		}

		virtual void Close(int32 Code, FStringView Reason) override
		{
			check(IsInGameThread());
			// Spec 3.5 through the helper the fake shares, Warnings and all, at every call (Task 3 review I-2)
			FOutboundClose Outbound = MakeOutboundClose(Link->GetLabel(), Code, Reason);
			Link->bCloseRequested = true;
			Post([ConnId = Id, Sent = Outbound.Code, Moved = MoveTemp(Outbound.Reason)](FService& Self) mutable
			{
				if (FConn* Conn = FConn::Find(Self, ConnId))
				{
					Conn->Close(Self, Sent, MoveTemp(Moved), EVaRestSocketCloseBy::Client, EVaRestSocketFailure::None);
				}
			});
		}

		virtual void Consumed(int64 Bytes) override
		{
			Link->PendingInboundBytes.fetch_sub(Bytes);
		}

		virtual int64 GetBufferedAmount() const override
		{
			return FMath::Max<int64>(Link->BufferedBytes.load(), 0);
		}

		virtual int64 GetPendingInboundBytes() const override
		{
			return Link->PendingInboundBytes.load();
		}

		virtual bool HasEnded() const override
		{
			return Link->bEnded.load();
		}

	private:
		/** Any thread: true when the service queued it; false when there is none or it no longer accepts (the command is released) */
		bool Post(FCommand&& Command) const
		{
			if (const TSharedPtr<FService, ESPMode::ThreadSafe> Pinned = Service.Pin())
			{
				return Pinned->Enqueue(MoveTemp(Command));
			}
			return false;
		}

		const TWeakPtr<FService, ESPMode::ThreadSafe> Service;
		const uint64 Id;
		const TSharedRef<FSocketLink, ESPMode::ThreadSafe> Link;
	};

	/**
	 * Game thread, inside Connect: no service to report from, so the Error is queued here and its notification posted to a
	 * later game-thread task -- never a call back inside Connect (E7)
	 */
	TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> RefuseConnect(const TSharedRef<FSocketLink, ESPMode::ThreadSafe>& Refused, uint64 ConnId)
	{
		FVaRestSocketEvent Event;
		Event.Type = EVaRestSocketEventType::Error;
		Event.End.Failure = EVaRestSocketFailure::Shutdown;
		Event.End.Detail = TEXT("the WebSocket transport is shut down or could not start");
		UE_LOG(LogVaRestSocket, Verbose, TEXT("%s: never opened: %s, %s"), *Refused->GetLabel(), LexToString(Event.End.Failure), *Event.End.Detail);
		Refused->Push(MoveTemp(Event), false);
		AsyncTask(ENamedThreads::GameThread, [Refused] { Refused->Notify(); });
		return MakeShared<FHandle, ESPMode::ThreadSafe>(TWeakPtr<FService, ESPMode::ThreadSafe>(), ConnId, Refused);
	}
}

VaRest::WebSocket::Lws::FTransport::FTransport(FServiceOptions InOptions)
	: Options(MoveTemp(InOptions))
{
}

VaRest::WebSocket::Lws::FTransport::~FTransport()
{
	if (Service.IsValid())
	{
		check(IsInGameThread());
		Service->Shutdown();
	}
}

bool VaRest::WebSocket::Lws::FTransport::EnsureStarted()
{
	check(IsInGameThread());
	if (bShutDown)
	{
		return false;
	}
	if (!Service.IsValid())
	{
		Service = MakeShared<FService, ESPMode::ThreadSafe>(Options);
	}
	// FService::Start makes no second context after a failure (review conc I-2) and none while a wss attempt is in flight (I-3)
	return Service->IsRunning() || Service->Start();
}

void VaRest::WebSocket::Lws::FTransport::Shutdown()
{
	check(IsInGameThread());
	bShutDown = true;
	if (Service.IsValid())
	{
		Service->Shutdown();
	}
}

void VaRest::WebSocket::Lws::FTransport::ResetAfterFork()
{
	check(IsInGameThread());
	if (Service.IsValid())
	{
		Service->Shutdown();
		Service.Reset();
	}
	bShutDown = false;
}

const VaRest::WebSocket::Lws::FServiceCounters* VaRest::WebSocket::Lws::FTransport::GetCounters() const
{
	return Service.IsValid() ? &Service->GetCounters() : nullptr;
}

TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> VaRest::WebSocket::Lws::FTransport::Connect(FVaRestSocketConnectRequest&& Request, TFunction<void()> OnEvents)
{
	using namespace VaRest::WebSocket::Lws::VaRestLwsTransportPrivate;
	check(IsInGameThread());
	LLM_SCOPE_BYTAG(VaRest_WebSocket);
	const uint64 ConnId = NextId++;
	// Task 3 review M-1: the link names the connection in its own lines too -- the one label, FConn and the handle read it there
	const TSharedRef<FSocketLink, ESPMode::ThreadSafe> Link = MakeShared<FSocketLink, ESPMode::ThreadSafe>(MoveTemp(OnEvents), FConn::MakeLabel(ConnId, Request));
	TUniquePtr<FConn> Conn = MakeUnique<FConn>(ConnId, Link, MoveTemp(Request));
	Conn->Prepare(Options);
	if (!EnsureStarted())
	{
		return RefuseConnect(Link, ConnId);
	}
	if (Conn->NeedsTlsCount())
	{
		// Re-review N-2: counted here, on the game thread, where every FService::Start reads the count, so a transport started
		// right after this Connect -- before the service thread ran the Adopt -- still sees the attempt (VaRest-rti, I-3)
		Conn->MarkTlsCounted();
	}
	if (!Service->Enqueue([Moved = MoveTemp(Conn)](FService& Self) mutable { Self.Adopt(MoveTemp(Moved)); }))
	{
		// Task 6: Enqueue's result is used. Not reached while EnsureStarted holds -- Shutdown runs on this thread too -- but if
		// it were, the command and the FConn in it are released already (uncounting the attempt), and the consumer still gets
		// its one terminal event
		return RefuseConnect(Link, ConnId);
	}
	return MakeShared<FHandle, ESPMode::ThreadSafe>(Service, ConnId, Link);
}

TSharedRef<IVaRestSocketTransport, ESPMode::ThreadSafe> VaRest::WebSocket::GetDefaultTransport()
{
	// Leaked on purpose, as VaRest::Http::GetDefaultTransport: nothing of it runs at static destruction; the engine-exit hook
	// shuts it down. A function-local static: its first use from any thread makes it once (C++11 [stmt.dcl]/4)
	static TSharedRef<IVaRestSocketTransport, ESPMode::ThreadSafe>* const Instance = []
	{
		VaRest::WebSocket::Lws::VaRestLwsTransportPrivate::bDefaultMade = true;
		return new TSharedRef<IVaRestSocketTransport, ESPMode::ThreadSafe>(
			MakeShared<VaRest::WebSocket::Lws::FTransport, ESPMode::ThreadSafe>(VaRest::WebSocket::Lws::FServiceOptions()));
	}();
	return *Instance;
}

VaRest::WebSocket::Lws::FTransport& VaRest::WebSocket::Lws::GetDefault()
{
	return static_cast<FTransport&>(VaRest::WebSocket::GetDefaultTransport().Get());
}

void VaRest::WebSocket::Lws::OnModuleStartup()
{
	constexpr bool bSharedCopy = IS_MONOLITHIC || UE_MERGED_MODULES;
	// In a monolithic or merged build, ModuleExists lists the statically linked modules (ModuleManager.cpp:325-345, through
	// FindModules' statically linked branch, :283-322)
	const auto IsLinked = [](const TCHAR* Name) { return FModuleManager::Get().ModuleExists(Name); };
	if (!ShouldCreateAtStartup(bSharedCopy, IsLinked))
	{
		return;
	}
	// Plan decision 8 (VaRest-rti): the engine's WebSockets module first -- it makes its one context at startup
	// (WebSocketsModule.cpp:29-44) -- so that context exists before any VaRest handshake; then VaRest's own, before game code
	// can start an engine handshake. Creations by WebSocketServer, WebSocketNetworking and HttpNetworkReplayStreaming later
	// remain a documented residual risk (spec 1.2)
	if (IsLinked(TEXT("WebSockets")))
	{
		FModuleManager::Get().LoadModule(TEXT("WebSockets"));
	}
	GetDefault().EnsureStarted();
}

void VaRest::WebSocket::Lws::ShutdownDefault()
{
	if (VaRestLwsTransportPrivate::bDefaultMade.load())
	{
		GetDefault().Shutdown();
	}
}

void VaRest::WebSocket::Lws::ResetDefaultAfterFork()
{
	if (VaRestLwsTransportPrivate::bDefaultMade.load())
	{
		GetDefault().ResetAfterFork();
	}
	OnModuleStartup();
}
