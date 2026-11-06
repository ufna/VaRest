// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestFakeSocketTransport.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/Async.h"
#include "VaRestSocketLink.h"
#include "VaRestWebSocketLog.h"

namespace VaRest::WebSocket::VaRestFakeSocketTransportPrivate
{
	/** The attempt's name in log lines: the request's LogName, as the real transport uses it, else "fake#<index>" */
	FString MakeFakeLabel(int32 InIndex, const FVaRestSocketConnectRequest& InRequest)
	{
		return InRequest.LogName.IsEmpty() ? FString::Printf(TEXT("fake#%d"), InIndex) : InRequest.LogName;
	}
}

/** The consumer's handle on a fake attempt; its destruction is the release */
class FVaRestFakeSocketConnection final : public IVaRestSocketConnection
{
public:
	explicit FVaRestFakeSocketConnection(TSharedRef<FVaRestFakeSocketAttempt, ESPMode::ThreadSafe> InAttempt)
		: Attempt(MoveTemp(InAttempt))
	{
	}

	virtual ~FVaRestFakeSocketConnection() override
	{
		// Any thread (IVaRestSocketConnection): the events stop at once; the close or cancel uses the attempt's game-thread
		// state, so it runs there -- now, or from a task that keeps the attempt alive
		Attempt->OnReleased();
		if (IsInGameThread())
		{
			Attempt->CloseReleased();
		}
		else
		{
			AsyncTask(ENamedThreads::GameThread, [Released = Attempt] { Released->CloseReleased(); });
		}
	}

	virtual int32 PopEvents(TArray<FVaRestSocketEvent>& Out) override { return Attempt->Link->PopAll(Out); }
	virtual bool Send(FVaRestSocketFrame&& Frame) override { return Attempt->OnSend(MoveTemp(Frame)); }
	virtual void Close(int32 Code, FStringView Reason) override { Attempt->OnClose(Code, Reason); }
	virtual void Consumed(int64 Bytes) override { Attempt->OnConsumed(Bytes); }
	virtual void ConsumedMessages(int64 Bytes, int32 Messages) override { Attempt->OnConsumed(Bytes); Attempt->Link->PendingInboundMessages.fetch_sub(Messages, std::memory_order_relaxed); }
	// Relaxed: counts (VaRestSocketLink.h)
	virtual int64 GetBufferedAmount() const override { return FMath::Max<int64>(Attempt->Link->BufferedBytes.load(std::memory_order_relaxed), 0); }
	virtual int64 GetPendingInboundBytes() const override { return Attempt->Link->PendingInboundBytes.load(std::memory_order_relaxed); }
	virtual bool HasEnded() const override { return Attempt->HasEnded(); }
	virtual bool SetIdleDeadline(double Seconds) override
	{
		check(IsInGameThread());
		if (!FMath::IsFinite(Seconds) || Seconds < 0 || Attempt->HasEnded() || Attempt->WasReleased() || Attempt->bCloseRequested) return false;
		Attempt->IdleDeadlineSeconds = Seconds;
		return true;
	}

private:
	TSharedRef<FVaRestFakeSocketAttempt, ESPMode::ThreadSafe> Attempt;
};

FVaRestFakeSocketAttempt::FVaRestFakeSocketAttempt(int32 InIndex, FVaRestSocketConnectRequest&& InRequest, TFunction<void()> InOnEvents)
	: Index(InIndex)
	, Request(MoveTemp(InRequest))
	// Request is initialised first (declaration order), so the label reads the moved-in request
	, Link(MakeShared<VaRest::WebSocket::FSocketLink, ESPMode::ThreadSafe>(MoveTemp(InOnEvents),
		VaRest::WebSocket::VaRestFakeSocketTransportPrivate::MakeFakeLabel(InIndex, Request)))
{
}

bool FVaRestFakeSocketAttempt::HasEnded() const
{
	// Acquire: pairs with the link's release after the terminal event's enqueue (VaRestSocketLink.h)
	return Link->bEnded.load(std::memory_order_acquire);
}

void FVaRestFakeSocketAttempt::Push(FVaRestSocketEvent&& Event)
{
	check(IsInGameThread());
	const bool bInsideConsumer = ConsumerDepth > 0;
	Link->Push(MoveTemp(Event), !bInsideConsumer);
	if (bInsideConsumer)
	{
		// E7: never inside the consumer's call; a game-thread task notifies (PumpUntil and the engine's tick run it)
		AsyncTask(ENamedThreads::GameThread, [Weak = TWeakPtr<VaRest::WebSocket::FSocketLink, ESPMode::ThreadSafe>(Link)]
		{
			if (const TSharedPtr<VaRest::WebSocket::FSocketLink, ESPMode::ThreadSafe> Pinned = Weak.Pin())
			{
				Pinned->Notify();
			}
		});
	}
}

FVaRestFakeSocketAttempt& FVaRestFakeSocketAttempt::Open(const FString& InProtocol, double ConnectSeconds)
{
	bOpen = bOpen || !HasEnded();
	FVaRestSocketEvent Event;
	Event.Type = EVaRestSocketEventType::Open;
	Event.Protocol = InProtocol;
	Event.Seconds = ConnectSeconds;
	Push(MoveTemp(Event));
	return *this;
}

FVaRestFakeSocketAttempt& FVaRestFakeSocketAttempt::Message(EVaRestSocketFrameKind InKind, TConstArrayView<uint8> InBytes)
{
	FVaRestSocketEvent Event;
	Event.Type = EVaRestSocketEventType::Message;
	Event.Kind = InKind;
	Event.Bytes.Reserve(InBytes.Num() + 1);
	Event.Bytes.Append(InBytes.GetData(), InBytes.Num());
	Push(MoveTemp(Event));
	return *this;
}

FVaRestFakeSocketAttempt& FVaRestFakeSocketAttempt::Text(FStringView InText)
{
	const FTCHARToUTF8 Converted(InText.GetData(), InText.Len());
	return Message(EVaRestSocketFrameKind::Text, TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length()));
}

FVaRestFakeSocketAttempt& FVaRestFakeSocketAttempt::Pong(double RttSeconds)
{
	FVaRestSocketEvent Event;
	Event.Type = EVaRestSocketEventType::Pong;
	Event.Seconds = RttSeconds;
	Push(MoveTemp(Event));
	return *this;
}

FVaRestFakeSocketAttempt& FVaRestFakeSocketAttempt::ServerClose(int32 Code, const FString& Reason)
{
	FVaRestSocketEnd Ended;
	Ended.Code = Code;
	Ended.Reason = Reason;
	Ended.bWasClean = true;
	Ended.By = EVaRestSocketCloseBy::Server;
	return End(EVaRestSocketEventType::Closed, Ended);
}

FVaRestFakeSocketAttempt& FVaRestFakeSocketAttempt::Drop()
{
	FVaRestSocketEnd Ended;
	Ended.Code = 1006;
	Ended.By = EVaRestSocketCloseBy::Transport;
	return End(EVaRestSocketEventType::Closed, Ended);
}

FVaRestFakeSocketAttempt& FVaRestFakeSocketAttempt::Fail(EVaRestSocketFailure Failure, int32 HttpStatus, const FString& Detail)
{
	FVaRestSocketEnd Ended;
	Ended.By = Failure == EVaRestSocketFailure::ConnectTimeout ? EVaRestSocketCloseBy::Timeout : EVaRestSocketCloseBy::Transport;
	Ended.Failure = Failure;
	Ended.HttpStatus = HttpStatus;
	Ended.Detail = Detail;
	return End(EVaRestSocketEventType::Error, Ended);
}

FVaRestFakeSocketAttempt& FVaRestFakeSocketAttempt::End(EVaRestSocketEventType Type, const FVaRestSocketEnd& InEnd)
{
	if (Type != EVaRestSocketEventType::Closed && Type != EVaRestSocketEventType::Error)
	{
		UE_LOG(LogVaRestSocket, Error, TEXT("FVaRestFakeSocketAttempt::End takes Closed or Error, not %s"), LexToString(Type));
		return *this;
	}
	check(IsInGameThread());
	if (!HasEnded())
	{
		// Discard before terminal publication/notification. Sent remains historical evidence; discard is not Drained.
		Link->BufferedBytes.store(0, std::memory_order_relaxed);
	}
	FVaRestSocketEvent Event;
	Event.Type = Type;
	Event.End = InEnd;
	Push(MoveTemp(Event));
	return *this;
}

FVaRestFakeSocketAttempt& FVaRestFakeSocketAttempt::Flush(int64 Bytes)
{
	check(IsInGameThread());
	// Task 3 residual: nothing reaches the socket before the open -- the real transport writes a send made before it only
	// after it (plan decision 18) -- so a pending attempt keeps its bytes, and no Drained can come ahead of the Open
	if (!bOpen && !HasEnded())
	{
		return *this;
	}
	// Relaxed: a count (VaRestSocketLink.h); the fake's sends and flushes are both on this thread
	const int64 Take = FMath::Min(Bytes, Link->BufferedBytes.load(std::memory_order_relaxed));
	if (Take > 0)
	{
		Link->ReleaseBuffered(Take);
	}
	return *this;
}

bool FVaRestFakeSocketAttempt::OnSend(FVaRestSocketFrame&& Frame)
{
	check(IsInGameThread());
	// As the real handle: refused after the end and after the consumer's Close (review conc M-6)
	if (!Frame.IsValid() || HasEnded() || bCloseRequested)
	{
		return false;
	}
	++ConsumerDepth;
	// The mark is decided on what this add produced (review I-1), as the real handle passes it on to the service thread
	const int64 After = Link->BufferedBytes.fetch_add(Frame.Length, std::memory_order_relaxed) + Frame.Length;
	if (Frame.Length > 0)
	{
		Link->NoteBuffered(After, Request.SendHighWater);
	}
	Sent.Add(MoveTemp(Frame));
	--ConsumerDepth;
	return true;
}

void FVaRestFakeSocketAttempt::OnClose(int32 Code, FStringView Reason)
{
	check(IsInGameThread());
	// The real handle's spec 3.5 rule and its Warnings, from the same function (review I-2), at every call as the real
	// handle logs them -- before the early return
	const VaRest::WebSocket::FOutboundClose Outbound = VaRest::WebSocket::MakeOutboundClose(Link->GetLabel(), Code, Reason);
	if (HasEnded() || bCloseRequested)
	{
		return;
	}
	++ConsumerDepth;
	CloseCode = Outbound.Code;
	CloseReason = Outbound.Reason.Num() > 0
		? FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Outbound.Reason.GetData()), Outbound.Reason.Num())
		: FString();
	bCloseRequested = true;
	if (!bOpen)
	{
		FVaRestSocketEnd Ended;
		Ended.By = EVaRestSocketCloseBy::Client;
		Ended.Failure = EVaRestSocketFailure::Canceled;
		Ended.Detail = TEXT("closed before the open");
		End(EVaRestSocketEventType::Error, Ended);
	}
	else if (bEchoClose)
	{
		FVaRestSocketEnd Ended;
		Ended.Code = CloseCode;
		Ended.Reason = CloseReason;
		Ended.bWasClean = true;
		Ended.By = EVaRestSocketCloseBy::Client;
		End(EVaRestSocketEventType::Closed, Ended);
	}
	--ConsumerDepth;
}

void FVaRestFakeSocketAttempt::OnConsumed(int64 Bytes)
{
	// The consumer's thread; relaxed: counts (VaRestSocketLink.h)
	ConsumedBytes.fetch_add(Bytes, std::memory_order_relaxed);
	Link->PendingInboundBytes.fetch_sub(Bytes, std::memory_order_relaxed);
}

void FVaRestFakeSocketAttempt::OnReleased()
{
	// Release: a thread that reads WasReleased (acquire) true also sees what the consumer did before letting go
	bReleased.store(true, std::memory_order_release);
	// From here the link discards (plan decision 5); release pairs with Push's acquire (VaRestSocketLink.h)
	Link->bReleased.store(true, std::memory_order_release);
}

void FVaRestFakeSocketAttempt::CloseReleased()
{
	check(IsInGameThread());
	// Plan decision 5 and spec 1.4, as the real handle's release: an open attempt is closed with 1001 "released" (answered at
	// once when bEchoClose), a pending one cancelled; OnClose skips one that has ended or that the consumer closed already.
	// What it queues is discarded, never notified: the link was released first
	OnClose(1001, TEXT("released"));
}

TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> FVaRestFakeSocketTransport::Connect(FVaRestSocketConnectRequest&& Request, TFunction<void()> OnEvents)
{
	check(IsInGameThread());
	const TSharedRef<FVaRestFakeSocketAttempt, ESPMode::ThreadSafe> Attempt = MakeShared<FVaRestFakeSocketAttempt, ESPMode::ThreadSafe>(Attempts.Num(), MoveTemp(Request), MoveTemp(OnEvents));
	Attempts.Add(Attempt);
	if (OnConnect)
	{
		// Inside Connect: what OnConnect scripts is notified later (E7)
		++Attempt->ConsumerDepth;
		OnConnect(*Attempt);
		--Attempt->ConsumerDepth;
	}
	return MakeShared<FVaRestFakeSocketConnection, ESPMode::ThreadSafe>(Attempt);
}

#endif // WITH_DEV_AUTOMATION_TESTS
