// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS && PLATFORM_LINUX

#include "VaRestSocketPressureWirePeer.h"
#include "VaRestSocketPressureTesting.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/SecureHash.h"
#include <cstdio>
#include <cstring>

namespace VaRestPressureWire
{
using namespace VaRest::WebSocket::PressureTesting;

enum class ECase : uint8 { Metadata, PayloadAnd, PackedFrames, PayloadRepauses, PartialCompletion };

/** Sole GT owner. Nothing on the native peer or service thread points back to this object. */
class FCase final : public TSharedFromThis<FCase, ESPMode::ThreadSafe>
{
public:
	FCase(FAutomationTestBase& InTest, ECase InKind) : Test(InTest), Kind(InKind), Probe(MakeProbe()), Peer(MakeUnique<FPeer>()) {}
	~FCase() { CancelPeer(*Peer); JoinPeer(*Peer); }

	void Begin()
	{
		Until = NativeSeconds() + 60.0;
		if (!Require(DefaultLiveConnections() == 0, TEXT("default transport has an empty live baseline"))) return;
		ContextsBefore = DefaultContextCreations();
		if (Kind == ECase::PackedFrames) Probe->ReceiveWitnessStartOrdinal = 6100;
		else if (Kind == ECase::PartialCompletion) Probe->ReceiveWitnessStartOrdinal = 0;
		const FTCHARToUTF8 Path(*OwnedPath(*Probe));
		if (!Require(StartPeer(*Peer, Path.Get()), TEXT("owned finite loopback peer started"))) return;
		FVaRestSocketConnectRequest Request;
		Request.Url = FString::Printf(TEXT("ws://127.0.0.1:%u%s"), static_cast<unsigned>(Peer->Port), *OwnedPath(*Probe));
		Request.LogName = TEXT("pressure-wire");
		Request.ConnectTimeout = 5.0; Request.CloseTimeout = 1.0;
		Request.PingInterval = Kind == ECase::Metadata ? 0.05 : 0.0;
		Request.PingTimeout = 120.0;
		Request.SendHighWater = 1; // NoteBuffered compares the positive send's captured add with >= high.
		Request.MaxPendingInboundBytes = InboundLimit();
		if (Kind == ECase::PartialCompletion) Request.MaxMessageBytes = 65539;
		Handle = ConnectOnDefault(Peer->Port, MoveTemp(Request), Probe);
		Require(Handle.IsValid(), TEXT("exact owned loopback request admitted on the process transport"));
	}

	bool Update()
	{
		if (bCleaning) return Cleanup();
		if (Peer->Failure.load(std::memory_order_acquire) != EPeerFailure::None)
		{
			Fail(TEXT("native peer reported failure")); return false;
		}
		if (NativeSeconds() >= Until) { Fail(TEXT("finite wire case deadline passed")); return false; }
		if (!Require(!Probe->bRecordOverflow.load(std::memory_order_acquire), TEXT("fixed transition ring did not overflow"))) return false;
		if (Kind != ECase::PayloadAnd && !Require(Probe->PeakQueued.load(std::memory_order_acquire) <= 4098,
			TEXT("real packed callback admission never exceeds high crossing plus Flow slot (4098)"))) return false;
		if ((Kind == ECase::PayloadRepauses || Kind == ECase::PartialCompletion)
			&& !Require(Probe->PeakQueued.load(std::memory_order_acquire) <= (Kind == ECase::PartialCompletion ? 5 : 3),
				TEXT("byte-pressure metadata and payload stay within the exact cycle bound"))) return false;
		if (Handle && Handle->HasEnded()) { Fail(TEXT("wire connection ended before assertions completed")); return false; }
		if (!Handshake()) return false;
		if (Phase == 0)
		{
			if (Probe->Count(EVaRestSocketEventType::Open) != 1) return false;
			if (!Require(DefaultContextCreations() == (ContextsBefore ? ContextsBefore : 1), TEXT("reused one process context"))) return false;
			if (Kind == ECase::Metadata)
			{
				const uint8 Byte = 0x5a;
				if (!Require(Handle->Send(FVaRestSocketFrame::Binary(MakeArrayView(&Byte, 1))), TEXT("positive high-water send admitted"))) return false;
			}
			else if (!Pull(false, MAX_int32)) return false; // Open only, before any scripted server data.
			Phase = 1;
			return false;
		}
		if (Kind == ECase::Metadata) Metadata();
		else if (Kind == ECase::PayloadAnd) Payload();
		else if (Kind == ECase::PackedFrames) Packed();
		else Repauses();
		return false;
	}

private:
	FAutomationTestBase& Test;
	const ECase Kind;
	TSharedRef<FProbe, ESPMode::ThreadSafe> Probe;
	TUniquePtr<FPeer> Peer;
	TSharedPtr<IVaRestSocketConnection, ESPMode::ThreadSafe> Handle;
	int32 Phase = 0, ContextsBefore = 0, Fences = 0, PressureCycles = 0;
	int32 ExpectedWakeRun = 0;
	int32 SeenOpen = 0, SeenPong = 0, SeenDrained = 0, SeenFlow = 0, SeenTerminal = 0;
	uint64 Tx = 0, FenceRequest = 0, LastPass = 0;
	FSnapshot Snapshot;
	int64 Messages = 0, Bytes = 0, Empty = 0, DeferredBytes = 0;
	int32 DeferredMessages = 0, CycleMessages = 0;
	int64 CycleBytes = 0;
	double Until = 0, CleanupUntil = 0, AbortAt = 0;
	bool bCleaning = false, bFailed = false, bClosed = false, bAborted = false, bCanceledPeer = false;
	bool bPayloadHeld = false, bReportedCleanupDeadline = false;
	int64 HeldBytes = 0;

	int64 InboundLimit() const
	{
		if (Kind == ECase::PayloadRepauses) return 0;
		if (Kind == ECase::PayloadAnd || Kind == ECase::PartialCompletion) return 8;
		return 1024 * 1024;
	}

	bool Require(bool Condition, const TCHAR* What)
	{
		if (Condition) return true;
		Test.AddError(FString::Printf(TEXT("%s: phase=%d owner=%llu conn=%llu msgs=%lld peak=%d flow=%d/%d peer=%u errno=%d tcp=%u requestBytes=%u responseBytes=%u requestReady=%d responseReady=%d peerDone=%d Open/Error/Closed=%lld/%lld/%lld terminal=%d ended=%d"), What,
			Phase, static_cast<unsigned long long>(Probe->Owner), static_cast<unsigned long long>(Probe->Connection.load()),
			static_cast<long long>(Messages), Probe->PeakQueued.load(), Probe->RxDisables.load(), Probe->RxEnables.load(),
			static_cast<unsigned>(Peer->Failure.load()), Peer->ErrorNumber.load(),
			static_cast<unsigned>(Peer->AcceptedTcp.load(std::memory_order_acquire)),
			static_cast<unsigned>(Peer->RequestBytes.load(std::memory_order_acquire)),
			static_cast<unsigned>(Peer->ResponseBytes.load(std::memory_order_acquire)),
			Peer->bRequestReady.load(std::memory_order_acquire), Peer->bResponseReady.load(std::memory_order_acquire), Peer->bDone.load(std::memory_order_acquire),
			static_cast<long long>(Probe->Count(EVaRestSocketEventType::Open)), static_cast<long long>(Probe->Count(EVaRestSocketEventType::Error)),
			static_cast<long long>(Probe->Count(EVaRestSocketEventType::Closed)), Probe->TerminalQueued.load(std::memory_order_acquire), Handle && Handle->HasEnded()));
		bFailed = true; BeginCleanup(); return false;
	}
	void Fail(const TCHAR* What) { Require(false, What); }
	void BeginCleanup()
	{
		if (bCleaning) return;
		bCleaning = true; CleanupUntil = NativeSeconds() + 10.0; AbortAt = NativeSeconds() + 2.0;
		Probe->bCanceled.store(true, std::memory_order_release);
		Probe->PopLimit.store(4096, std::memory_order_release);
	}

	bool Handshake()
	{
		if (!Peer->bRequestReady.load(std::memory_order_acquire)) return false;
		if (Peer->bResponseReady.load(std::memory_order_acquire)) return true;
		TArray<uint8> Key;
		if (!Require(FBase64::Decode(FString(UTF8_TO_TCHAR(Peer->Key)), Key) && Key.Num() == 16, TEXT("real client handshake key decodes to 16 bytes"))) return false;
		const FString Material = FString(UTF8_TO_TCHAR(Peer->Key)) + TEXT("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
		const FTCHARToUTF8 Utf8(*Material);
		uint8 Hash[20]; FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Hash);
		const FString Accept = FBase64::Encode(Hash, static_cast<uint32>(sizeof(Hash)));
		const FTCHARToUTF8 AcceptUtf8(*Accept);
		const int Written = std::snprintf(Peer->Response, sizeof(Peer->Response),
			"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", AcceptUtf8.Get());
		if (!Require(Written > 0 && static_cast<uint32>(Written) < sizeof(Peer->Response), TEXT("bounded handshake response"))) return false;
		Peer->ResponseLength = static_cast<uint32>(Written);
		Peer->bResponseReady.store(true, std::memory_order_release); WakePeer(*Peer);
		return true;
	}

	bool Fence()
	{
		if (!FenceRequest)
		{
			FenceRequest = RequestPassFence(Handle.ToSharedRef(), Probe);
			Require(FenceRequest != 0, TEXT("owned monotone service-pass request admitted"));
			return false;
		}
		if (Probe->AckRequest.load(std::memory_order_acquire) != FenceRequest) return false;
		if (!Require(Probe->AckStatus.load(std::memory_order_acquire) == EFenceStatus::Complete, TEXT("owned service-pass fence completed"))) return false;
		Snapshot = Probe->Snapshot;
		if (!Require(Snapshot.Owner == Probe->Owner && Snapshot.Connection == Probe->Connection.load()
			&& Snapshot.Request == FenceRequest && Snapshot.Pass > LastPass, TEXT("matching owner/connection/request and a later completed pass"))) return false;
		LastPass = Snapshot.Pass; FenceRequest = 0; return true;
	}

	bool CheckTransition(int32 Index, int32 Queued, int64 PendingBytes, bool bPause)
	{
		if (!Require(Probe->TransitionCount.load(std::memory_order_acquire) > Index, TEXT("actual rx-flow call has a published transition witness"))) return false;
		const FTransition& Value = Probe->Transitions[Index];
		const int64 Limit = InboundLimit();
		const bool High = Value.Queued > 4096 || Value.Bytes > Limit || Value.ReplyDebt >= 4095;
		const bool Low = Value.Queued <= 2048 && Value.Bytes <= Limit / 2 && Value.ReplyDebt < 2048;
		return Require(Value.Queued == Queued && Value.Bytes == PendingBytes && Value.ReplyDebt == 0
			&& Value.bPaused == bPause && Value.bWasPaused != bPause
			&& (bPause ? High : Low), TEXT("pre-Flow transition inputs satisfy the independent pressure predicate"));
	}
	bool CheckAllTransitions()
	{
		const int32 Count = Probe->TransitionCount.load(std::memory_order_acquire);
		if (!Require(Count <= FProbe::TransitionCapacity && !Probe->bRecordOverflow.load(), TEXT("all flow witnesses fit the fixed ring"))) return false;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FTransition& Value = Probe->Transitions[Index];
			if (!CheckTransition(Index, Value.Queued, Value.Bytes, Index % 2 == 0)) return false;
		}
		return Require(Count == SeenFlow && Count == Probe->RxDisables.load() + Probe->RxEnables.load(), TEXT("actual flow calls, FIFO events and witnesses agree"));
	}

	bool Send(uint32 Length)
	{
		if (!Require(Peer->TxCompleted.load(std::memory_order_acquire) == Tx, TEXT("previous native TX command completed before buffer reuse"))) return false;
		Peer->TxLength = Length;
		Peer->TxRequest.store(++Tx, std::memory_order_release); WakePeer(*Peer); return true;
	}
	bool Sent() const { return Peer->TxCompleted.load(std::memory_order_acquire) == Tx; }
	bool SendEmpty(uint32 Count)
	{
		if (!Require(Sent(), TEXT("native empty-frame buffer is parked before writing"))) return false;
		if (!Require(Count <= FPeer::TxCapacity / 2, TEXT("finite empty-frame script fits native storage"))) return false;
		for (uint32 Index = 0; Index < Count; ++Index) { Peer->Tx[Index * 2] = 0x82; Peer->Tx[Index * 2 + 1] = 0; }
		return Send(Count * 2);
	}
	bool SendSentinel()
	{
		if (!Require(Sent(), TEXT("native sentinel buffer is parked before writing"))) return false;
		const uint8 Value[6]{0x82, 4, 0x10, 0x20, 0x30, 0x40};
		std::memcpy(Peer->Tx, Value, sizeof(Value)); return Send(sizeof(Value));
	}

	bool Pull(bool bCleanup, int32 Limit)
	{
		Probe->PopLimit.store(Limit, std::memory_order_release);
		TArray<FVaRestSocketEvent> Events;
		const int32 Popped = Handle->PopEvents(Events);
		if (!bCleanup && !Require(Popped == Events.Num(), TEXT("real dequeue count matches actual additions"))) return false;
		for (int32 EventIndex = 0; EventIndex < Events.Num(); ++EventIndex)
		{
			const auto& Event = Events[EventIndex];
			if (Event.Type == EVaRestSocketEventType::Message)
			{
				if (bCleanup) { Handle->ConsumedMessages(Event.Bytes.Num(), 1); continue; }
				bool Valid = Event.Kind == EVaRestSocketFrameKind::Binary && Event.bFinal && Event.Bytes.Max() > Event.Bytes.Num();
				if (Kind == ECase::PackedFrames)
				{
					if (Messages % 2 == 0) Valid &= Event.Bytes.IsEmpty();
					else Valid &= Event.Bytes.Num() == 4 && ReadSequence(Event.Bytes) == static_cast<uint32>(Messages);
				}
				else if (Kind == ECase::Metadata) Valid &= Messages < 4094 ? Event.Bytes.IsEmpty() : Event.Bytes.Num() == 4 && ReadSequence(Event.Bytes) == 0x10203040;
				else if (Kind == ECase::PayloadRepauses) Valid &= Event.Bytes.Num() == 4 && ReadSequence(Event.Bytes) == static_cast<uint32>(Messages);
				else if (Kind == ECase::PartialCompletion)
				{
					if (Messages == 0)
					{
						Valid &= Event.Bytes.Num() == 65539;
						if (Event.Bytes.Num() == 65539)
						{
							for (int32 Index = 0; Index < 65535; ++Index) Valid &= Event.Bytes[Index] == 0xa5;
							for (int32 Index = 0; Index < 4; ++Index) Valid &= Event.Bytes[65535 + Index] == 0xf0 + Index;
						}
					}
					else Valid &= Event.Bytes.Num() == 4 && ReadSequence(Event.Bytes) == static_cast<uint32>(Messages);
				}
				else if (Messages == 0)
				{
					Valid &= Event.Bytes.Num() == 9;
					for (int32 Index = 0; Index < Event.Bytes.Num(); ++Index) Valid &= Event.Bytes[Index] == Index + 1;
					bPayloadHeld = true; HeldBytes = Event.Bytes.Num();
				}
				else Valid &= Event.Bytes.Num() == 4 && ReadSequence(Event.Bytes) == 0x10203040;
				if (!Valid)
				{
					FString Hex;
					for (int32 Index = 0; Index < FMath::Min(Event.Bytes.Num(), 16); ++Index)
						Hex += FString::Printf(TEXT("%02x"), static_cast<unsigned>(Event.Bytes[Index]));
					Test.AddInfo(FString::Printf(TEXT("first wire mismatch ordinal=%lld expectedEmpty=%d expectedSequence=%u actualKind=%u final=%d num=%d max=%d sequence=%u hex=%s event=%d/%d popBudget=%d messagesBefore=%lld emptyBefore=%lld bytesBefore=%lld tx=%llu/%llu sent=%u txLength=%u published=%lld transitions=%d"),
						static_cast<long long>(Messages), Kind == ECase::PackedFrames && Messages % 2 == 0,
						static_cast<unsigned>(Messages), static_cast<unsigned>(Event.Kind), Event.bFinal, Event.Bytes.Num(), Event.Bytes.Max(),
						ReadSequence(Event.Bytes), *Hex, EventIndex, Popped, Probe->LastPopBudget.load(),
						static_cast<long long>(Messages), static_cast<long long>(Empty), static_cast<long long>(Bytes),
						static_cast<unsigned long long>(Peer->TxRequest.load()), static_cast<unsigned long long>(Peer->TxCompleted.load()),
						Peer->TxSent.load(), Peer->TxLength, static_cast<long long>(Probe->Count(EVaRestSocketEventType::Message)), Probe->TransitionCount.load()));
					int32 FollowingMessages = 0;
					for (int32 Index = EventIndex + 1; Index < Events.Num() && FollowingMessages < 4; ++Index)
					{
						const FVaRestSocketEvent& Following = Events[Index];
						if (Following.Type != EVaRestSocketEventType::Message) continue;
						++FollowingMessages;
						FString FollowingHex;
						for (int32 Byte = 0; Byte < FMath::Min(Following.Bytes.Num(), 16); ++Byte)
							FollowingHex += FString::Printf(TEXT("%02x"), static_cast<unsigned>(Following.Bytes[Byte]));
						Test.AddInfo(FString::Printf(TEXT("following wire message event=%d kind=%u final=%d num=%d max=%d sequence=%u hex=%s"),
							Index, static_cast<unsigned>(Following.Kind), Following.bFinal, Following.Bytes.Num(), Following.Bytes.Max(),
							ReadSequence(Following.Bytes), *FollowingHex));
					}
					const int32 Witnesses = Probe->ReceiveWitnessCount.load(std::memory_order_acquire);
					for (int32 Index = 0; Index < Witnesses; ++Index)
					{
						const FReceiveWitness& Value = Probe->ReceiveWitnesses[Index];
						Test.AddInfo(FString::Printf(TEXT("wire RX witness ordinal=%llu len=%llu remaining=%llu final=%d binary=%d assembling=%d assemblyBytes=%d queuedBefore=%d"),
							static_cast<unsigned long long>(Value.MessageOrdinal), static_cast<unsigned long long>(Value.Length),
							static_cast<unsigned long long>(Value.Remaining), Value.bFinal, Value.bBinary, Value.bAssembling, Value.AssemblyBytes, Value.Queued));
					}
				}
				++Messages; Bytes += Event.Bytes.Num(); if (Event.Bytes.IsEmpty()) ++Empty;
				if (Kind != ECase::PayloadAnd || Messages != 1) { DeferredBytes += Event.Bytes.Num(); ++DeferredMessages; }
				// Retain every popped receipt even when content validation fails; cleanup returns it exactly once.
				if (!Require(Valid, TEXT("actual wire Message FIFO/kind/payload/slack preserved")))
				{
					// Remaining already-dequeued entries also belong to this consumer, including on this failed iteration.
					for (int32 Index = EventIndex + 1; Index < Events.Num(); ++Index)
						if (Events[Index].Type == EVaRestSocketEventType::Message) Handle->ConsumedMessages(Events[Index].Bytes.Num(), 1);
					return false;
				}
			}
			else if (Event.Type == EVaRestSocketEventType::Open) ++SeenOpen;
			else if (Event.Type == EVaRestSocketEventType::Pong) ++SeenPong;
			else if (Event.Type == EVaRestSocketEventType::Drained) ++SeenDrained;
			else if (Event.Type == EVaRestSocketEventType::Flow)
			{
				const bool ExpectedPause = SeenFlow % 2 == 0;
				++SeenFlow;
				if (!bCleanup && !Require(Event.bReadPaused == ExpectedPause, TEXT("real Flow events alternate pause/resume in FIFO order")))
				{
					for (int32 Index = EventIndex + 1; Index < Events.Num(); ++Index)
						if (Events[Index].Type == EVaRestSocketEventType::Message) Handle->ConsumedMessages(Events[Index].Bytes.Num(), 1);
					return false;
				}
			}
			else if (Event.IsTerminal())
			{
				++SeenTerminal;
				if (!bCleanup) { Fail(TEXT("terminal event inside a held wire proof")); return false; }
				if (!bFailed) Require(Event.Type == EVaRestSocketEventType::Closed && Event.End.Code == 1000 && Event.End.bWasClean,
					TEXT("exactly one clean real close terminal"));
			}
		}
		return true;
	}
	static uint32 ReadSequence(const TArray<uint8>& Value)
	{
		if (Value.Num() != 4) return 0;
		return static_cast<uint32>(Value[0]) << 24 | static_cast<uint32>(Value[1]) << 16 | static_cast<uint32>(Value[2]) << 8 | Value[3];
	}
	void ReturnHeld()
	{
		if (DeferredBytes || DeferredMessages) { Handle->ConsumedMessages(DeferredBytes, DeferredMessages); DeferredBytes = 0; DeferredMessages = 0; }
		if (bPayloadHeld) { Handle->ConsumedMessages(HeldBytes, 1); HeldBytes = 0; bPayloadHeld = false; }
	}

	void Metadata()
	{
		switch (Phase)
		{
		case 1:
			if (Probe->Count(EVaRestSocketEventType::Drained) != 1 || Probe->Count(EVaRestSocketEventType::Pong) != 1 || !Peer->bFirstPongSent.load(std::memory_order_acquire)) return;
			if (!Fence()) return;
			if (!Require(Snapshot.Queued == 3 && Snapshot.Bytes == 0 && !Snapshot.bPaused && Peer->ClientMessages.load() == 1, TEXT("real Open/Drained/Pong prefix occupies three slots"))) return;
			if (SendEmpty(4093)) Phase = 2;
			break;
		case 2:
			if (!Sent() || Probe->Count(EVaRestSocketEventType::Message) != 4093 || !Fence()) return;
			if (!Require(Snapshot.Queued == 4096 && !Snapshot.bPaused && Probe->RxDisables.load() == 0, TEXT("4096 all-kind slots are not above high"))) return;
			if (SendEmpty(1)) Phase = 3;
			break;
		case 3:
			if (!Sent() || Probe->RxDisables.load() != 1 || !Fence()) return;
			if (!Require(Snapshot.Queued == 4098 && Snapshot.bPaused && Snapshot.Bytes == 0, TEXT("4097 crossing plus its own pause Flow slot")) || !CheckTransition(0, 4097, 0, true)) return;
			if (SendSentinel()) { Fences = 0; Phase = 4; }
			break;
		case 4:
			if (!Sent() || !Fence()) return;
			if (!Require(Snapshot.Queued == 4098 && Snapshot.bPaused && Probe->Count(EVaRestSocketEventType::Message) == 4094, TEXT("positively sent sentinel remains blocked through an actual service pass"))) return;
			if (++Fences != 2) return;
			ExpectedWakeRun = Probe->WakeRuns.load(std::memory_order_acquire) + 1;
			if (Pull(false, 2049)) Phase = 5;
			break;
		case 5:
			if (Probe->WakeRuns.load(std::memory_order_acquire) < ExpectedWakeRun || !Fence()) return;
			if (!Require(Snapshot.Queued == 2049 && Snapshot.bPaused && Probe->RxEnables.load() == 0 && Probe->AckMessages.load() == 0
				&& Probe->AckBytes.load() == 0 && Probe->WakeRuns.load() > 0, TEXT("2049 slots stay held after real Pop wake without any Consumed0"))) return;
			ExpectedWakeRun = Probe->WakeRuns.load(std::memory_order_acquire) + 1;
			if (Pull(false, 1)) Phase = 6;
			break;
		case 6:
			if (Probe->WakeRuns.load(std::memory_order_acquire) < ExpectedWakeRun || Probe->RxEnables.load() != 1
				|| Probe->Count(EVaRestSocketEventType::Message) != 4095 || !Fence()) return;
			if (!CheckTransition(1, 2048, 0, false) || !Require(!Snapshot.bPaused && Snapshot.Queued == 2050,
				TEXT("inclusive 2048 resumes pre-sent data and accounts for resume Flow plus sentinel"))) return;
			if (Pull(false, MAX_int32)) { ReturnHeld(); Phase = 7; }
			break;
		case 7:
			if (!Fence()) return;
			if (!Require(Snapshot.Queued == 0 && Snapshot.Bytes == 0 && Snapshot.Messages == 0 && Messages == 4095 && Empty == 4094 && Bytes == 4
				&& SeenOpen == 1 && SeenDrained == 1 && SeenPong == 1 && SeenFlow == 2 && Probe->AckMessages.load() == 4095 && Probe->AckBytes.load() == 4,
				TEXT("metadata FIFO and exact empty/nonempty receipts return to zero"))) return;
			if (CheckAllTransitions()) BeginCleanup(); break;
		}
	}

	void Payload()
	{
		switch (Phase)
		{
		case 1:
			if (!Fence()) return;
			if (!Require(Snapshot.Queued == 0, TEXT("payload proof starts after real Open dequeue"))) return;
			if (!Require(Sent(), TEXT("native payload buffer is parked before writing"))) return;
			Peer->Tx[0] = 0x82; Peer->Tx[1] = 9;
			for (uint8 Index = 0; Index < 9; ++Index) Peer->Tx[Index + 2] = Index + 1;
			if (Send(11)) Phase = 2;
			break;
		case 2:
			if (!Sent() || Probe->RxDisables.load() != 1 || !Fence()) return;
			if (!CheckTransition(0, 1, 9, true) || !Require(Snapshot.Queued == 2 && Snapshot.Bytes == 9 && Snapshot.bPaused, TEXT("nine real bytes exceed eight with metadata low"))) return;
			if (Pull(false, MAX_int32) && SendSentinel()) { Fences = 0; Phase = 3; }
			break;
		case 3:
			if (!Sent() || !Fence()) return;
			if (!Require(Snapshot.Queued == 0 && Snapshot.Bytes == 9 && Snapshot.bPaused && Probe->Count(EVaRestSocketEventType::Message) == 1,
				TEXT("low slots alone cannot resume the pre-sent sentinel"))) return;
			if (++Fences != 2) return;
			ExpectedWakeRun = Probe->WakeRuns.load(std::memory_order_acquire) + 1;
			Handle->Consumed(4); HeldBytes -= 4; Phase = 4; break;
		case 4:
			if (Probe->WakeRuns.load(std::memory_order_acquire) < ExpectedWakeRun || !Fence()) return;
			if (!Require(Snapshot.Bytes == 5 && Snapshot.Queued == 0 && Snapshot.bPaused && Probe->RxEnables.load() == 0, TEXT("five bytes stay above inclusive four-byte low"))) return;
			ExpectedWakeRun = Probe->WakeRuns.load(std::memory_order_acquire) + 1;
			Handle->Consumed(1); HeldBytes -= 1; Phase = 5; break;
		case 5:
			if (Probe->WakeRuns.load(std::memory_order_acquire) < ExpectedWakeRun || Probe->RxEnables.load() != 1
				|| Probe->Count(EVaRestSocketEventType::Message) != 2 || !Fence()) return;
			if (!CheckTransition(1, 0, 4, false) || !Require(!Snapshot.bPaused && Snapshot.Bytes == 8, TEXT("four-byte low resumes the already-sent four-byte sentinel"))) return;
			if (Pull(false, MAX_int32)) { ReturnHeld(); Phase = 6; }
			break;
		case 6:
			if (!Fence()) return;
			if (!Require(Snapshot.Queued == 0 && Snapshot.Bytes == 0 && Snapshot.Messages == 0 && Messages == 2 && Bytes == 13
				&& Probe->AckBytes.load() == 13 && Probe->AckMessages.load() == 2 && SeenFlow == 2, TEXT("partial and final payload receipts are exact"))) return;
			if (CheckAllTransitions()) BeginCleanup(); break;
		}
	}

	void Packed()
	{
		switch (Phase)
		{
		case 1:
			if (!Fence()) return;
			if (!Require(Snapshot.Queued == 0, TEXT("packed proof starts after real Open dequeue"))) return;
			if (!Require(Sent(), TEXT("native packed buffer is parked before writing"))) return;
			{
				uint32 Offset = 0;
				for (uint32 Index = 0; Index < 65536; ++Index)
				{
					Peer->Tx[Offset++] = 0x82; Peer->Tx[Offset++] = Index % 2 ? 4 : 0;
					if (Index % 2) for (int Shift = 24; Shift >= 0; Shift -= 8) Peer->Tx[Offset++] = static_cast<uint8>(Index >> Shift);
				}
				if (!Require(Offset == FPeer::TxCapacity, TEXT("finite packed script is exactly 256 KiB"))) return;
				if (Send(Offset)) { Fences = 0; Phase = 2; }
			}
			break;
		case 2:
			if (Probe->RxDisables.load(std::memory_order_acquire) == 0 || Peer->TxSent.load(std::memory_order_acquire) < 20000 || !Fence()) return;
			if (!CheckTransition(0, 4097, 8192, true) || !Require(Snapshot.Queued == 4098 && Snapshot.bPaused
				&& Probe->Count(EVaRestSocketEventType::Message) == 4097, TEXT("packed callbacks stop at first actual high crossing while complete sent frames remain"))) return;
			if (++Fences != 2) return;
			if (Pull(false, 2049)) { ++PressureCycles; Phase = 3; }
			break;
		case 3:
			if (!Fence()) return;
			if (!Require(Snapshot.Queued == 2049 && Snapshot.bPaused && Probe->RxEnables.load() == 0, TEXT("packed first pause retains hysteresis at 2049"))) return;
			if (Pull(false, 1)) Phase = 4;
			break;
		case 4:
			if (Probe->RxEnables.load() < 1) return;
			if (!CheckTransition(1, 2048, 8192, false)) return;
			Phase = 5; break;
		case 5:
			if (!Fence()) return;
			if (!Require(Probe->PeakQueued.load() <= 4098 && PressureCycles < 40, TEXT("packed queue and finite drain cycle bounds"))) return;
			if (Snapshot.Queued)
			{
				if (Snapshot.bPaused) ++PressureCycles;
				// One entry snapshot only. Credit is deferred, so this real Pop is the only source of slot relief.
				if (!Pull(false, MAX_int32)) return;
			}
			if (Probe->Count(EVaRestSocketEventType::Message) == 65536 && Messages == 65536 && Sent()) Phase = 6;
			break;
		case 6:
			if (!Fence()) return;
			if (Snapshot.Queued) { Pull(false, MAX_int32); return; }
			if (!Require(!Snapshot.bPaused && Messages == 65536 && Empty == 32768 && Bytes == 131072
				&& SeenOpen == 1 && SeenPong == 0 && SeenDrained == 0 && SeenFlow == Probe->RxDisables.load() + Probe->RxEnables.load(),
				TEXT("all packed wire frames and ordered flow events survived finite throttle/resume"))) return;
			ReturnHeld(); Phase = 7; break;
		case 7:
			if (!Fence()) return;
			if (!Require(Snapshot.Queued == 0 && Snapshot.Bytes == 0 && Snapshot.Messages == 0 && Probe->AckMessages.load() == 65536
				&& Probe->AckBytes.load() == 131072, TEXT("packed exact deferred payload/message receipts return to zero"))) return;
			if (CheckAllTransitions()) BeginCleanup(); break;
		}
	}


	void Repauses()
	{
		const bool bPartial = Kind == ECase::PartialCompletion;
		const int64 Target = bPartial ? 13 : 48;
		const int64 TargetBytes = bPartial ? 65587 : 192;
		switch (Phase)
		{
		case 1:
			if (!Fence()) return;
			if (!Require(Snapshot.Queued == 0 && Snapshot.Bytes == 0 && Sent(), TEXT("new byte case starts after actual Open dequeue"))) return;
			if (bPartial)
			{
				Peer->Tx[0] = 0x02; Peer->Tx[1] = 126; Peer->Tx[2] = 0xff; Peer->Tx[3] = 0xff;
				for (uint32 Index = 0; Index < 65535; ++Index) Peer->Tx[Index + 4] = 0xa5;
				if (Send(65539)) Phase = 2;
			}
			else
			{
				uint32 Offset = 0;
				for (uint32 Sequence = 0; Sequence < 48; ++Sequence) WriteSequenceFrame(Offset, Sequence);
				if (Send(Offset)) Phase = 4;
			}
			break;
		case 2:
			if (!Sent() || !Fence()) return;
			{
				bool bSawNonfinal = false;
				const int32 Count = Probe->ReceiveWitnessCount.load(std::memory_order_acquire);
				for (int32 Index = 0; Index < Count; ++Index)
				{
					const FReceiveWitness& Value = Probe->ReceiveWitnesses[Index];
					bSawNonfinal |= !Value.bFinal && Value.Length > 0 && Value.Remaining == 0
						&& static_cast<uint64>(Value.AssemblyBytes) + Value.Length == 65535;
				}
				if (!bSawNonfinal) return;
				if (!Require(Snapshot.Queued == 0 && Snapshot.Bytes == 0 && Snapshot.Messages == 0
					&& Probe->Count(EVaRestSocketEventType::Message) == 0, TEXT("actual nonfinal prefix remains in assembly before completion"))) return;
				uint32 Offset = 0;
				Peer->Tx[Offset++] = 0x80; Peer->Tx[Offset++] = 4;
				for (uint32 Index = 0; Index < 4; ++Index) Peer->Tx[Offset++] = 0xf0 + Index;
				for (uint32 Sequence = 1; Sequence <= 12; ++Sequence) WriteSequenceFrame(Offset, Sequence);
				if (Send(Offset)) Phase = 4;
			}
			break;
		case 4:
			if (!Sent() || !Fence()) return;
			// A published Message may precede its actual RefreshReadFlow call. Keep every receipt
			// charged until the service pass positively observes the high transition and pause.
			if (!Snapshot.bPaused) return;
			CycleMessages = bPartial && Messages != 0 ? 3 : 1;
			CycleBytes = bPartial ? (Messages == 0 ? 65539 : 12) : 4;
			if (!Require(Probe->RxDisables.load() == PressureCycles + 1 && Probe->RxEnables.load() == PressureCycles
				&& Snapshot.Queued == CycleMessages + (PressureCycles ? 2 : 1)
				&& Snapshot.Messages == CycleMessages && Snapshot.Bytes == CycleBytes
				&& Probe->Count(EVaRestSocketEventType::Message) == Messages + CycleMessages,
				TEXT("one exact high crossing is held before any receipt is returned"))) return;
			if (!CheckTransition(PressureCycles * 2, CycleMessages + (PressureCycles ? 1 : 0), CycleBytes, true)) return;
			if (bPartial && Messages == 0)
			{
				bool bSawCompletion = false;
				const int32 Count = Probe->ReceiveWitnessCount.load(std::memory_order_acquire);
				for (int32 Index = 0; Index < Count; ++Index)
				{
					const FReceiveWitness& Value = Probe->ReceiveWitnesses[Index];
					bSawCompletion |= Value.MessageOrdinal == 0 && Value.bAssembling && Value.AssemblyBytes == 65535
						&& Value.bFinal && Value.Remaining == 0 && Value.Length == 4;
				}
				if (!Require(bSawCompletion, TEXT("final continuation completes the real retained prefix"))) return;
			}
			if (!Require(DeferredBytes == 0 && DeferredMessages == 0, TEXT("previous cycle returned only its own exact receipts"))) return;
			ExpectedWakeRun = Probe->WakeRuns.load(std::memory_order_acquire) + 1;
			if (!Pull(false, MAX_int32)) return;
			++PressureCycles;
			if (!Require(DeferredBytes == CycleBytes && DeferredMessages == CycleMessages
				&& Messages <= Target && SeenFlow == PressureCycles * 2 - 1,
				TEXT("actual dequeue retains exactly the paused cycle receipts and ordered Flow"))) return;
			Phase = 5;
			break;
		case 5:
			if (Probe->WakeRuns.load(std::memory_order_acquire) < ExpectedWakeRun || !Fence()) return;
			if (!Require(Snapshot.Queued == 0 && Snapshot.Bytes == CycleBytes && Snapshot.Messages == CycleMessages
				&& Snapshot.bPaused && Probe->RxDisables.load() == PressureCycles && Probe->RxEnables.load() == PressureCycles - 1
				&& Probe->AckMessages.load() == Messages - CycleMessages && Probe->AckBytes.load() == Bytes - CycleBytes,
				TEXT("real Pop wake and later service pass retain positive bytes until exact receipt"))) return;
			ReturnHeld(); Phase = 6;
			break;
		case 6:
			if (Probe->RxEnables.load(std::memory_order_acquire) < PressureCycles || !Fence()) return;
			if (!CheckTransition(PressureCycles * 2 - 1, 0, 0, false)
				|| !Require(Probe->RxEnables.load() == PressureCycles
					&& Probe->AckMessages.load() == Messages && Probe->AckBytes.load() == Bytes,
					TEXT("only returned owned receipts produce the actual zero-byte resume"))) return;
			Phase = Messages == Target ? 7 : 4;
			break;
		case 7:
			if (!Fence()) return;
			if (Snapshot.Queued) { if (Pull(false, MAX_int32)) ReturnHeld(); return; }
			if (!Require(Snapshot.Bytes == 0 && Snapshot.Messages == 0 && !Snapshot.bPaused
				&& Messages == Target && Bytes == TargetBytes && Empty == 0
				&& SeenOpen == 1 && SeenPong == 0 && SeenDrained == 0
				&& Probe->AckMessages.load() == Target && Probe->AckBytes.load() == TargetBytes,
				TEXT("new byte cases preserve exact content, FIFO and all receipts"))) return;
			if (!Require(PressureCycles == (bPartial ? 5 : 48) && Probe->RxDisables.load() == PressureCycles
				&& Probe->RxEnables.load() == PressureCycles && SeenFlow == PressureCycles * 2,
				TEXT("all exact held byte-pressure high and low cycles occurred"))) return;
			if (!bPartial)
			{
				const int32 Count = Probe->TransitionCount.load(std::memory_order_acquire);
				for (int32 Index = 0; Index < Count; ++Index)
					if (!Require(Probe->Transitions[Index].Bytes == (Index % 2 ? 0 : 4), TEXT("zero-byte limit resumes only after exact four-byte receipt"))) return;
			}
			if (CheckAllTransitions()) BeginCleanup(); break;
		}
	}
	void WriteSequenceFrame(uint32& Offset, uint32 Sequence)
	{
		check(Offset + 6 <= FPeer::TxCapacity);
		Peer->Tx[Offset++] = 0x82; Peer->Tx[Offset++] = 4;
		for (int Shift = 24; Shift >= 0; Shift -= 8) Peer->Tx[Offset++] = static_cast<uint8>(Sequence >> Shift);
	}

	bool Cleanup()
	{
		if (Handle)
		{
			ReturnHeld();
			if (!bClosed) { Handle->Close(1000, TEXT("wire done")); bClosed = true; }
			Pull(true, 4096);
			if (!Handle->HasEnded() && !bAborted && NativeSeconds() >= AbortAt)
			{
				bAborted = true;
				const bool Admitted = Handle->Abort(TEXT("pressure wire cleanup"));
				if (!Admitted && !Handle->HasEnded()) Fail(TEXT("owned cleanup Abort admission was refused"));
				if (!bFailed) Fail(TEXT("clean wire close required an Abort fallback"));
			}
		}
		const bool Retired = !Handle || (Probe->bRetired.load(std::memory_order_acquire) && DefaultLiveConnections() == 0);
		const bool AcksDone = Probe->InFlightRequests.load(std::memory_order_acquire) == 0;
		if ((!Retired || !AcksDone) && NativeSeconds() >= CleanupUntil && !bCanceledPeer)
		{
			bCanceledPeer = true; CancelPeer(*Peer); CleanupUntil = NativeSeconds() + 5.0;
			Fail(TEXT("owned destruction/fence deadline passed; canceled only this native peer"));
		}
		if ((!Retired || !AcksDone) && bCanceledPeer && NativeSeconds() >= CleanupUntil && !bReportedCleanupDeadline)
		{
			bReportedCleanupDeadline = true;
			Fail(TEXT("owned native connection/fence remains after finite cancellation; failed test releases only its GT owner"));
			Test.AddError(FString::Printf(TEXT("wire cleanup residual owner=%llu conn=%llu retired=%d activeRequest=%llu ackRequest=%llu ackStatus=%u inFlight=%d"),
				static_cast<unsigned long long>(Probe->Owner), static_cast<unsigned long long>(Probe->Connection.load()), Probe->bRetired.load(),
				static_cast<unsigned long long>(Probe->ActiveRequest.load()), static_cast<unsigned long long>(Probe->AckRequest.load()),
				static_cast<unsigned>(Probe->AckStatus.load()), Probe->InFlightRequests.load()));
		}
		if (Retired || bReportedCleanupDeadline) CancelPeer(*Peer);
		if (((!Retired || !AcksDone) && !bReportedCleanupDeadline) || !Peer->bDone.load(std::memory_order_acquire)) return false;
		JoinPeer(*Peer);
		Require(!Peer->bStarted || Peer->bJoined, TEXT("owned peer was actually joined, including failed cases"));
		if (Handle) Pull(true, MAX_int32); // HasEnded is published after terminal enqueue; drain once more after retirement.
		if (!bFailed)
		{
			Require(Retired && Probe->bWsiDestroyed.load() && SeenTerminal == 1 && Probe->TerminalQueued.load() == 1
				&& Peer->bCloseSeen.load() && Peer->bCloseEchoed.load()
				&& Peer->CloseCode.load() == 1000 && Peer->Failure.load() == EPeerFailure::None, TEXT("real close/WSI destruction/peer join completed before releasing the owner"));
		}
		Test.AddInfo(FString::Printf(TEXT("wire kind=%u owner=%llu conn=%llu passes=%llu msgs=%lld empty=%lld bytes=%lld peak=%d pause/resume=%d/%d pop=%d wakes=%d/%d cycles=%d destroyed=%d retired=%d peerJoined=%d failure=%u"),
			static_cast<unsigned>(Kind), static_cast<unsigned long long>(Probe->Owner), static_cast<unsigned long long>(Probe->Connection.load()),
			static_cast<unsigned long long>(LastPass), static_cast<long long>(Messages), static_cast<long long>(Empty), static_cast<long long>(Bytes),
			Probe->PeakQueued.load(), Probe->RxDisables.load(), Probe->RxEnables.load(), Probe->PopCalls.load(), Probe->WakeRuns.load(), Probe->WakePosts.load(),
			PressureCycles, Probe->bWsiDestroyed.load(), Probe->bRetired.load(), Peer->bJoined, static_cast<unsigned>(Peer->Failure.load())));
		// Residual fence commands and the owned connection retain their native shared probe. No acknowledgement is faked,
		// and none of those captures references this GT owner or the joined peer's storage.
		Handle.Reset();
		return true;
	}
};

class FAdvance final : public IAutomationLatentCommand
{
public:
	explicit FAdvance(TSharedRef<FCase, ESPMode::ThreadSafe> InCase) : Case(MoveTemp(InCase)) {}
	bool Update() override { return Case->Update(); }
private:
	TSharedRef<FCase, ESPMode::ThreadSafe> Case;
};

}

#define VAREST_PRESSURE_WIRE_TEST(ClassName, TestName, Kind) \
IMPLEMENT_SIMPLE_AUTOMATION_TEST(ClassName, TestName, EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter) \
bool ClassName::RunTest(const FString& Parameters) \
{ \
	auto Case = MakeShared<VaRestPressureWire::FCase, ESPMode::ThreadSafe>(*this, VaRestPressureWire::ECase::Kind); \
	Case->Begin(); \
	ADD_LATENT_AUTOMATION_COMMAND(VaRestPressureWire::FAdvance(Case)); \
	return true; \
}

VAREST_PRESSURE_WIRE_TEST(FVaRestSocketPressureWireMetadataTest, "VaRest.WebSocket.Pressure.Wire.Metadata", Metadata)
VAREST_PRESSURE_WIRE_TEST(FVaRestSocketPressureWirePayloadTest, "VaRest.WebSocket.Pressure.Wire.PayloadAnd", PayloadAnd)
VAREST_PRESSURE_WIRE_TEST(FVaRestSocketPressureWirePackedTest, "VaRest.WebSocket.Pressure.Wire.PackedFrames", PackedFrames)
VAREST_PRESSURE_WIRE_TEST(FVaRestSocketPressureWireRepausesTest, "VaRest.WebSocket.Pressure.Wire.PayloadRepauses", PayloadRepauses)
VAREST_PRESSURE_WIRE_TEST(FVaRestSocketPressureWirePartialTest, "VaRest.WebSocket.Pressure.Wire.PartialCompletion", PartialCompletion)

#undef VAREST_PRESSURE_WIRE_TEST

#endif
