// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreTypes.h"

#if WITH_DEV_AUTOMATION_TESTS && PLATFORM_LINUX

#include <atomic>
#include <cstdint>
#include <pthread.h>

namespace VaRestPressureWire
{

enum class EPeerFailure : uint32_t { None, Setup, Accept, Deadline, Poll, Read, Write, Handshake, Frame, Limit, Thread };

/** Worker argument: fixed native storage only. The GT never closes a worker-owned descriptor. */
struct FPeer final
{
	static constexpr uint32_t TxCapacity = 256 * 1024;
	int Listener = -1, WakeRead = -1, WakeWrite = -1;
	uint16_t Port = 0;
	pthread_t Thread{};
	bool bStarted = false, bJoined = false; // GT only
	char Path[96]{}, Host[64]{}, Request[8193]{}, Key[25]{}, Response[512]{};
	uint8_t Tx[TxCapacity]{};
	uint32_t TxLength = 0, ResponseLength = 0; // published by the request/response release stores
	std::atomic<bool> bStop{false}, bDone{false}, bRequestReady{false}, bResponseReady{false};
	std::atomic<bool> bFirstPongSent{false}, bCloseSeen{false}, bCloseEchoed{false};
	std::atomic<EPeerFailure> Failure{EPeerFailure::None};
	std::atomic<int> ErrorNumber{0};
	std::atomic<uint64_t> TxRequest{0}, TxCompleted{0};
	std::atomic<uint32_t> TxSent{0}, PingsSeen{0}, ClientMessages{0}, CloseCode{0};
	// Diagnostic-only, worker-owned monotonic observations; no buffer is read by the GT.
	std::atomic<uint32_t> AcceptedTcp{0}, RequestBytes{0}, ResponseBytes{0};
};

double NativeSeconds();
bool StartPeer(FPeer& Peer, const char* Path);
void WakePeer(FPeer& Peer);
void CancelPeer(FPeer& Peer);
/** GT calls after bDone, or as its cancellation backstop. No detached worker survives this call. */
void JoinPeer(FPeer& Peer);

}

#endif
