// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include <stdint.h>

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
#define VAREST_HAS_PRIVATE_DNS 1

namespace VaRest::WebSocket::Dns
{

	/** Private process-owned native storage; never an old native-owner ABI/authority object. */
	enum class EStatus : uint8_t
	{
		Ready, Queued, Pending, Exhausted, NativeError, Capacity, Stopped, Invalid, Busy, Deferred, Poisoned, NoMemory
	};

	struct FTicket
	{
		uint64_t Generation = 0;
		uintptr_t Pool = 0;
		uint32_t Slot = 0;
		uint32_t Process = 0;
	};

	struct FAddress
	{
		uint32_t Family = 0; // 4 or 6, not a platform sockaddr tag.
		uint32_t Scope = 0;
		uint16_t PortBe = 0;
		uint8_t Bytes[16]{};
	};

	struct FError
	{
		int32_t Resolver = 0;
		int32_t System = 0;
		int32_t Worker = 0;
		int32_t Join = 0;
	};

	/** GT only before publishing a service. False uses actual pre/post-fork threading state, not a fake readiness token. */
	EStatus InitializeOnGT(bool bMayCreateNativeThreads);
	/** Native numeric syntax only; never a resolver, interface lookup or thread. */
	bool ParseNumeric(const char* Host, uint32_t Bytes, uint16_t Port, FAddress& Out);
	/** Any control actor; no blocking control lock. Every successful ticket owns one generation until Discard. */
	EStatus Submit(const char* Host, uint32_t Bytes, uint16_t Port, FTicket& Out
#if defined(WITH_DEV_AUTOMATION_TESTS) && WITH_DEV_AUTOMATION_TESTS
		, int BeforeResolverGate = -1, int AfterPublishGate = -1
#endif
	);
	/** One normalized address at a time, after actual join; at most32 native list nodes inspected per call. */
	EStatus Next(const FTicket&, FAddress& Out, FError& Error);
	/** Logical discard only: generation-CAS, no free/join/callback/UE work. Safe for a late stale ticket. */
	void Discard(const FTicket&);
	/** Nonblocking physical retirement, also collects orphan interests. Does not mean all workers have ended. */
	EStatus PollRetirement();
	/** Module stop only, not per transport. Admission closes and unresolved native storage/code stay process-owned. */
	void StopForModule();

#if defined(WITH_DEV_AUTOMATION_TESTS) && WITH_DEV_AUTOMATION_TESTS
	struct FStats
	{
		uint32_t Capacity = 0, Occupied = 0, Unjoined = 0, PublishedUnjoined = 0, JoinedLists = 0, Poisoned = 0;
		uint32_t BeforeResolverWaiting = 0, AfterPublishedWaiting = 0;
		bool bClosed = false;
		uint64_t Created = 0, JoinCalls = 0, Joined = 0, ResolverCalls = 0, FreedLists = 0, PoolBytes = 0;
	};
	EStatus SnapshotForTests(FStats&);
#endif

}

#else
#define VAREST_HAS_PRIVATE_DNS 0
#endif
