// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "Templates/Function.h"
#include "VaRestSocketTypes.h"

#include <atomic>

namespace VaRest::WebSocket
{
	/**
	 * What a connection's producer (the service thread; the fake on the game thread) and its consumer share: the one ordered
	 * event queue (WebSocket spec 2.1, SPSC), the buffered amount (spec 2.4) and the undelivered inbound bytes (spec 2.3).
	 * The producer queues exactly one terminal event (Closed or Error, plan decision 2); a later event is dropped with an
	 * Error log line. Error ends an attempt that never opened, Closed one that did, Open comes at most once, and no Message,
	 * Pong or Drained comes before it: a push that breaks this is queued (the end still reaches the consumer) with an Error
	 * log line -- a transport bug, or a mis-scripted fake. Producer functions run on one thread at a time; PopAll on the
	 * consumer's.
	 *
	 * Memory order. The flags are written with release and read with acquire; the two byte counts are relaxed, because they
	 * publish nothing -- each is one number, read and changed by atomic operations only, and no other memory is read on the
	 * strength of its value. Code outside this file that uses the plain operators (`Link->bReleased = true`, `.load()`) gets
	 * seq_cst, which is stronger than each member's stated order, so it is right as well. The link holds no lock, so no lock
	 * is ever held across OnEvents (user code).
	 */
	class FSocketLink final
	{
	public:
		/** InLabel names the connection in the link's log lines (Task 7: "ws#<id>" or the request's LogName; the fake: its own) */
		explicit FSocketLink(TFunction<void()> InOnEvents, FString InLabel = FString());

		/** Any thread: the connection's name in log lines */
		const FString& GetLabel() const { return Label; }

		/**
		 * Producer: queues Event, stamps its Time, counts a Message's bytes; bNotify false leaves the notification to a later
		 * Notify(). The notification is its last step, so OnEvents may pop (when it runs on the consumer's thread, as the
		 * fake's does) or push again without seeing a half-done Push.
		 */
		void Push(FVaRestSocketEvent&& Event, bool bNotify = true);

		/**
		 * The thread that queued: runs the consumer's OnEvents, if any and not released. It changes no link state, and no lock
		 * is held. The released check is exact on one thread and best effort across threads: a release that lands between the
		 * check and the call still gets that one call, which Connect's contract (OnEvents captures weak or thread-safe state)
		 * already makes harmless.
		 */
		void Notify() const;

		/** Consumer: appends the queued events to Out; returns how many */
		int32 PopAll(TArray<FVaRestSocketEvent>& Out);

		/**
		 * Producer, for one Send: BufferedAfterSend is the count that Send's own add produced (the value fetch_add returned,
		 * plus the frame's length), which is what GetBufferedAmount showed the app. The mark is decided on it, never on a
		 * re-read: by the time the producer handles the Send, earlier frames' bytes may have left (review I-1), and a re-read
		 * below HighWater would lose a crossing the app saw, and with it the Drained (spec 2.4, 6.3). The Send's own bytes stay
		 * counted until it is released, so the count cannot reach 0 before this note.
		 */
		void NoteBuffered(int64 BufferedAfterSend, int64 HighWater);

		/** Producer: Bytes left the buffer (on the socket now); queues Drained when it reaches 0 after the high-water mark (spec 2.4) */
		void ReleaseBuffered(int64 Bytes);

		/**
		 * Added by Send (game thread), taken by the producer as bytes reach the socket; read anywhere (GetBufferedAmount).
		 * Relaxed: a count. Every change is an atomic read-modify-write, so ReleaseBuffered's "it reached 0" sees every
		 * Send counted before it in the count's single modification order; no other data is read because of its value.
		 */
		std::atomic<int64> BufferedBytes{ 0 };
		/**
		 * Added by Push (producer) before the Message is queued, taken by Consumed (consumer) after it was popped; read
		 * anywhere. Relaxed: a count. Adding before the enqueue keeps it from going below 0: the queue orders the add before
		 * the pop, so before the consumer's subtraction in the count's modification order.
		 */
		std::atomic<int64> PendingInboundBytes{ 0 };
		/**
		 * Set once the terminal event is in the queue (producer); read anywhere (HasEnded, Send's refusal). Release after the
		 * enqueue, acquire at every read: a reader that sees true then pops the terminal event (the queue's link to its node
		 * was written before the release store, Queue.h:193), which the "HasEnded, so drain once more" pattern relies on.
		 */
		std::atomic<bool> bEnded{ false };
		/**
		 * The consumer dropped its handle (any thread): Push discards from then on. Release by the handle, acquire in Push: the
		 * producer that sees it also sees what the consumer did before letting go (its last Consumed, for W1b's flow control).
		 */
		std::atomic<bool> bReleased{ false };
		/**
		 * The consumer called Close (game thread): its later sends are refused at the call (review conc M-6). Written and read
		 * on the game thread today, where no order is needed; release/acquire as for every flag, so a read moved to another
		 * thread stays right (on x86-64 both are a plain mov, as relaxed would be).
		 */
		std::atomic<bool> bCloseRequested{ false };

	private:
		/**
		 * TQueue's SPSC mode: Enqueue makes the node with the item, a full barrier, then the link to it (Queue.h:167-193);
		 * Dequeue reads that link (Queue.h:82)
		 */
		TQueue<FVaRestSocketEvent, EQueueMode::Spsc> Queue;
		const TFunction<void()> OnEvents;
		const FString Label;
		/**
		 * Producer only: plain, because producer functions run on one thread at a time. A producer that moves to another
		 * thread hands over with a join (the real transport ends connections on the game thread only once its service thread
		 * has stopped).
		 */
		bool bTerminalQueued = false;
		bool bOpenQueued = false;
		bool bHighWaterReached = false;
	};

	/** Our close as it goes on the wire (spec 3.5) */
	struct FOutboundClose
	{
		/** IsValidCloseCode holds */
		int32 Code = 1000;
		/** UTF-8, at most MaxCloseReasonBytes, cut on a code-point boundary */
		TArray<uint8> Reason;
	};

	/**
	 * Any thread (the handles call it on the game thread, at the consumer's Close): spec 3.5 -- SanitizeCloseCode and
	 * CloseReasonBytes, with a Warning in LogVaRestSocket for each change, naming the connection by Label
	 * (IVaRestSocketConnection::Close promises both). The real handle and the fake share it, so the lines cannot drift.
	 */
	FOutboundClose MakeOutboundClose(const FString& Label, int32 Code, FStringView Reason);
}
