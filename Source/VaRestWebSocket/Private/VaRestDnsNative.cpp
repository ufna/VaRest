// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include "VaRestDnsNative.h"

#if VAREST_HAS_PRIVATE_DNS

// Deliberately no Core/UE types, callbacks, allocators or TLS in the worker's reachable source.
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace VaRest::WebSocket::Dns
{
	namespace
	{
		constexpr uint32_t Capacity = 32, NodesPerTurn = 32;
		enum : uint32_t { Free, Running, Poisoned };
		struct FPool;
		struct FSlot
		{
			uint64_t Generation;
			alignas(8) uint64_t Interest;
			alignas(4) uint32_t Published;
			uint32_t State, Created, Joined;
			FPool* Pool;
			pthread_t Thread;
			char Host[256];
			uint16_t Port;
			int ResolverError, SystemError, WorkerError, JoinError;
			addrinfo* List;
			const addrinfo* Cursor;
#if defined(WITH_DEV_AUTOMATION_TESTS) && WITH_DEV_AUTOMATION_TESTS
			int BeforeGate, AfterGate;
			alignas(4) uint32_t GateStage;
#endif
		};
		struct FPool
		{
			alignas(4) uint32_t Lock, Closed, MaySpawn;
			uint32_t Process;
			void* ImageReference;
			alignas(8) uint64_t Created, JoinCalls, Joined, ResolverCalls, FreedLists;
			FSlot Slots[Capacity];
		};

		FPool* GPool = nullptr;
		uint32_t GInitializing = 0;
		void* Worker(void*);

		FPool* Current()
		{
			FPool* Pool = __atomic_load_n(&GPool, __ATOMIC_ACQUIRE);
			return Pool && Pool->Process == static_cast<uint32_t>(getpid()) ? Pool : nullptr;
		}
		struct FTryLock
		{
			FPool& Pool;
			bool bHeld;
			explicit FTryLock(FPool& In) : Pool(In), bHeld(false)
			{
				uint32_t Expected = 0;
				bHeld = __atomic_compare_exchange_n(&Pool.Lock, &Expected, 1u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
			}
			~FTryLock() { if (bHeld) __atomic_store_n(&Pool.Lock, 0u, __ATOMIC_RELEASE); }
		};
		bool Matches(const FPool& Pool, const FTicket& Ticket)
		{
			return Ticket.Generation && Ticket.Pool == reinterpret_cast<uintptr_t>(&Pool)
				&& Ticket.Process == Pool.Process && Ticket.Slot < Capacity;
		}
		bool Interested(const FSlot& Slot, uint64_t Generation)
		{
			return __atomic_load_n(&Slot.Interest, __ATOMIC_ACQUIRE) == Generation;
		}
		void ReleaseList(FPool& Pool, FSlot& Slot)
		{
			if (Slot.List)
			{
				freeaddrinfo(Slot.List);
				Slot.List = nullptr;
				__atomic_add_fetch(&Pool.FreedLists, uint64_t(1), __ATOMIC_RELAXED);
			}
			Slot.Cursor = nullptr;
		}
		/** Control lock held. Native storage is never reinitialized just because Published became1. */
		void Retire(FPool& Pool, FSlot& Slot)
		{
			if (Slot.State != Running || !Slot.Created) return;
			if (!Slot.Joined)
			{
				__atomic_add_fetch(&Pool.JoinCalls, uint64_t(1), __ATOMIC_RELAXED);
				const int Result = pthread_tryjoin_np(Slot.Thread, nullptr);
				if (Result == EBUSY) return;
				if (Result != 0) { Slot.JoinError = Result; Slot.State = Poisoned; return; }
				Slot.Joined = 1;
				__atomic_add_fetch(&Pool.Joined, uint64_t(1), __ATOMIC_RELAXED);
				if (__atomic_load_n(&Slot.Published, __ATOMIC_ACQUIRE) != 1) { Slot.State = Poisoned; return; }
				Slot.Cursor = Slot.List;
			}
			if (__atomic_load_n(&Pool.Closed, __ATOMIC_ACQUIRE) || !Interested(Slot, Slot.Generation))
			{
				ReleaseList(Pool, Slot);
				Slot.Created = Slot.Joined = 0;
				Slot.State = Free;
				// Generation and the atomic object's lifetime remain intact for stale-ticket CAS callers.
				__atomic_store_n(&Slot.Published, 0u, __ATOMIC_RELEASE);
			}
		}

#if defined(WITH_DEV_AUTOMATION_TESTS) && WITH_DEV_AUTOMATION_TESTS
		int Gate(int Fd)
		{
			if (Fd < 0) return 0;
			uint64_t Signal = 0;
			ssize_t Count;
			do { Count = read(Fd, &Signal, sizeof(Signal)); } while (Count < 0 && errno == EINTR);
			return Count == sizeof(Signal) ? 0 : Count < 0 ? errno : EIO;
		}
#endif
		void* Worker(void* Input)
		{
			FSlot& Slot = *static_cast<FSlot*>(Input);
#if defined(WITH_DEV_AUTOMATION_TESTS) && WITH_DEV_AUTOMATION_TESTS
			__atomic_store_n(&Slot.GateStage, Slot.BeforeGate >= 0 ? 1u : 0u, __ATOMIC_RELEASE);
			Slot.WorkerError = Gate(Slot.BeforeGate);
			__atomic_store_n(&Slot.GateStage, 0u, __ATOMIC_RELEASE);
#endif
			if (!Slot.WorkerError)
			{
				char Service[6]{};
				uint32_t Port = Slot.Port, Digits = 1;
				for (uint32_t Rest = Port; Rest >= 10; Rest /= 10) ++Digits;
				for (uint32_t Index = Digits; Index; --Index) { Service[Index - 1] = static_cast<char>('0' + Port % 10); Port /= 10; }
				addrinfo Hints{};
				Hints.ai_family = AF_UNSPEC;
				Hints.ai_socktype = SOCK_STREAM;
				Hints.ai_flags = AI_NUMERICSERV;
				addrinfo* List = nullptr;
				__atomic_add_fetch(&Slot.Pool->ResolverCalls, uint64_t(1), __ATOMIC_RELAXED);
				Slot.ResolverError = getaddrinfo(Slot.Host, Service, &Hints, &List);
				Slot.SystemError = Slot.ResolverError == EAI_SYSTEM ? errno : 0;
				// Only success obtains list ownership. No reading/freeing an unspecified failed out-value.
				Slot.List = Slot.ResolverError == 0 ? List : nullptr;
				if (Slot.List && !Interested(Slot, Slot.Generation)) ReleaseList(*Slot.Pool, Slot);
			}
			__atomic_store_n(&Slot.Published, 1u, __ATOMIC_RELEASE);
#if defined(WITH_DEV_AUTOMATION_TESTS) && WITH_DEV_AUTOMATION_TESTS
			__atomic_store_n(&Slot.GateStage, Slot.AfterGate >= 0 ? 2u : 0u, __ATOMIC_RELEASE);
			const int GateError = Gate(Slot.AfterGate);
			__atomic_store_n(&Slot.GateStage, 0u, __ATOMIC_RELEASE);
			if (GateError) Slot.WorkerError = GateError; // Controls read this only after actual join, not publication.
#endif
			return nullptr;
		}

		bool Normalize(const addrinfo& Node, uint16_t Port, FAddress& Out)
		{
			if (!Node.ai_addr) return false;
			Out = {};
			const uint16_t Expected = htons(Port);
			if (Node.ai_family == AF_INET && Node.ai_addrlen >= sizeof(sockaddr_in))
			{
				sockaddr_in Native{}; memcpy(&Native, Node.ai_addr, sizeof(Native));
				if (Native.sin_family != AF_INET || Native.sin_port != Expected) return false;
				Out.Family = 4; Out.PortBe = Native.sin_port;
				memcpy(Out.Bytes, &Native.sin_addr, 4);
				return true;
			}
			if (Node.ai_family == AF_INET6 && Node.ai_addrlen >= sizeof(sockaddr_in6))
			{
				sockaddr_in6 Native{}; memcpy(&Native, Node.ai_addr, sizeof(Native));
				if (Native.sin6_family != AF_INET6 || Native.sin6_port != Expected) return false;
				Out.Family = 6; Out.PortBe = Native.sin6_port; Out.Scope = Native.sin6_scope_id;
				memcpy(Out.Bytes, &Native.sin6_addr, sizeof(Out.Bytes));
				return true;
			}
			return false;
		}
	}

	EStatus InitializeOnGT(bool bMayCreateNativeThreads)
	{
		if (FPool* Pool = Current())
		{
			__atomic_store_n(&Pool->MaySpawn, bMayCreateNativeThreads ? 1u : 0u, __ATOMIC_RELEASE);
			return __atomic_load_n(&Pool->Closed, __ATOMIC_ACQUIRE) ? EStatus::Stopped : bMayCreateNativeThreads ? EStatus::Ready : EStatus::Deferred;
		}
		if (!bMayCreateNativeThreads) return EStatus::Deferred;
		uint32_t Expected = 0;
		if (!__atomic_compare_exchange_n(&GInitializing, &Expected, 1u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return EStatus::Busy;
		FPool* Pool = static_cast<FPool*>(calloc(1, sizeof(FPool)));
		if (!Pool) { __atomic_store_n(&GInitializing, 0u, __ATOMIC_RELEASE); return EStatus::NoMemory; }
		Pool->Process = static_cast<uint32_t>(getpid());
		Pool->MaySpawn = 1;
#if !IS_MONOLITHIC
		Dl_info Image{};
		if (dladdr(reinterpret_cast<const void*>(&Worker), &Image) == 0 || !Image.dli_fname
			|| !(Pool->ImageReference = dlopen(Image.dli_fname, RTLD_NOLOAD | RTLD_NOW | RTLD_LOCAL)))
		{
			free(Pool); __atomic_store_n(&GInitializing, 0u, __ATOMIC_RELEASE); return EStatus::NativeError;
		}
#endif
		// Exactly one own-image reference/native pool per process+loaded image. Intentionally no destructor/dlclose.
		// A post-fork child never joins/frees copied parent handles; its fresh pool has the actual child PID.
		__atomic_store_n(&GPool, Pool, __ATOMIC_RELEASE);
		__atomic_store_n(&GInitializing, 0u, __ATOMIC_RELEASE);
		return EStatus::Ready;
	}

	bool ParseNumeric(const char* Host, uint32_t Bytes, uint16_t Port, FAddress& Out)
	{
		Out = {};
		if (!Host || !Bytes || Bytes > 255 || !Port) return false;
		char Text[256]{};
		for (uint32_t I = 0; I < Bytes; ++I) { if (!Host[I]) return false; Text[I] = Host[I]; }
		if (inet_pton(AF_INET, Text, Out.Bytes) == 1) Out.Family = 4;
		else if (inet_pton(AF_INET6, Text, Out.Bytes) == 1) Out.Family = 6;
		else { Out = {}; return false; }
		Out.PortBe = htons(Port);
		return true;
	}

	EStatus Submit(const char* Host, uint32_t Bytes, uint16_t Port, FTicket& Out
#if defined(WITH_DEV_AUTOMATION_TESTS) && WITH_DEV_AUTOMATION_TESTS
		, int BeforeResolverGate, int AfterPublishGate
#endif
	)
	{
		Out = {};
		if (!Host || !Bytes || Bytes > 255 || !Port) return EStatus::Invalid;
		for (uint32_t Index = 0; Index < Bytes; ++Index) if (static_cast<uint8_t>(Host[Index]) <= 32 || static_cast<uint8_t>(Host[Index]) >= 127) return EStatus::Invalid;
		FPool* Pool = Current();
		if (!Pool) return EStatus::Deferred;
		if (__atomic_load_n(&Pool->Closed, __ATOMIC_ACQUIRE)) return EStatus::Stopped;
		if (!__atomic_load_n(&Pool->MaySpawn, __ATOMIC_ACQUIRE)) return EStatus::Deferred;
		FTryLock Lock(*Pool);
		if (!Lock.bHeld) return EStatus::Busy;
		for (FSlot& Slot : Pool->Slots) Retire(*Pool, Slot);
		for (uint32_t Index = 0; Index < Capacity; ++Index)
		{
			FSlot& Slot = Pool->Slots[Index];
			if (Slot.State != Free) continue;
			if (Slot.Generation == UINT64_MAX) { Slot.State = Poisoned; continue; }
			if (__atomic_load_n(&Pool->Closed, __ATOMIC_ACQUIRE)) return EStatus::Stopped;
			++Slot.Generation;
			Slot.Pool = Pool; Slot.State = Running; Slot.Created = Slot.Joined = 0;
			Slot.Port = Port; Slot.List = nullptr; Slot.Cursor = nullptr;
			Slot.ResolverError = Slot.SystemError = Slot.WorkerError = Slot.JoinError = 0;
			memcpy(Slot.Host, Host, Bytes); Slot.Host[Bytes] = 0;
#if defined(WITH_DEV_AUTOMATION_TESTS) && WITH_DEV_AUTOMATION_TESTS
			Slot.BeforeGate = BeforeResolverGate; Slot.AfterGate = AfterPublishGate;
			__atomic_store_n(&Slot.GateStage, 0u, __ATOMIC_RELEASE);
#endif
			__atomic_store_n(&Slot.Published, 0u, __ATOMIC_RELEASE);
			__atomic_store_n(&Slot.Interest, Slot.Generation, __ATOMIC_RELEASE);
			if (__atomic_load_n(&Pool->Closed, __ATOMIC_ACQUIRE))
			{
				__atomic_store_n(&Slot.Interest, uint64_t(0), __ATOMIC_RELEASE); Slot.State = Free; return EStatus::Stopped;
			}
			pthread_t Thread{};
			const int Result = pthread_create(&Thread, nullptr, &Worker, &Slot);
			if (Result != 0)
			{
				Slot.WorkerError = Result; Slot.State = Free;
				__atomic_store_n(&Slot.Interest, uint64_t(0), __ATOMIC_RELEASE);
				return EStatus::NativeError; // Failed out-handle is never inspected.
			}
			Slot.Thread = Thread; Slot.Created = 1;
			__atomic_add_fetch(&Pool->Created, uint64_t(1), __ATOMIC_RELAXED);
			if (__atomic_load_n(&Pool->Closed, __ATOMIC_ACQUIRE))
			{
				__atomic_store_n(&Slot.Interest, uint64_t(0), __ATOMIC_RELEASE);
				return EStatus::Stopped; // Actual in-flight child remains owned; stop is not a no-child proof.
			}
			Out = {Slot.Generation, reinterpret_cast<uintptr_t>(Pool), Index, Pool->Process};
			return EStatus::Queued;
		}
		return EStatus::Capacity;
	}

	EStatus Next(const FTicket& Ticket, FAddress& Out, FError& Error)
	{
		Out = {}; Error = {};
		FPool* Pool = Current();
		if (!Pool || !Matches(*Pool, Ticket)) return EStatus::Invalid;
		FTryLock Lock(*Pool); if (!Lock.bHeld) return EStatus::Busy;
		FSlot& Slot = Pool->Slots[Ticket.Slot];
		if (Slot.Generation != Ticket.Generation) return EStatus::Invalid;
		if (__atomic_load_n(&Pool->Closed, __ATOMIC_ACQUIRE) || !Interested(Slot, Ticket.Generation)) return EStatus::Stopped;
		Retire(*Pool, Slot);
		if (Slot.State == Poisoned) { Error.Join = Slot.JoinError; return EStatus::Poisoned; }
		if (!Slot.Joined) return EStatus::Pending;
		Error = {Slot.ResolverError, Slot.SystemError, Slot.WorkerError, Slot.JoinError};
		if (Slot.ResolverError || Slot.WorkerError) return EStatus::NativeError;
		for (uint32_t Visited = 0; Slot.Cursor && Visited < NodesPerTurn; ++Visited)
		{
			const addrinfo& Node = *Slot.Cursor; Slot.Cursor = Node.ai_next;
			if (Normalize(Node, Slot.Port, Out)) return EStatus::Ready;
		}
		return Slot.Cursor ? EStatus::Pending : EStatus::Exhausted;
	}

	void Discard(const FTicket& Ticket)
	{
		FPool* Pool = Current();
		if (!Pool || !Matches(*Pool, Ticket)) return;
		uint64_t Expected = Ticket.Generation;
		(void)__atomic_compare_exchange_n(&Pool->Slots[Ticket.Slot].Interest, &Expected, uint64_t(0), false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
	}
	EStatus PollRetirement()
	{
		FPool* Pool = Current(); if (!Pool) return EStatus::Deferred;
		FTryLock Lock(*Pool); if (!Lock.bHeld) return EStatus::Busy;
		for (FSlot& Slot : Pool->Slots) Retire(*Pool, Slot);
		return EStatus::Ready;
	}
	void StopForModule()
	{
		FPool* Pool = Current(); if (!Pool) return;
		__atomic_store_n(&Pool->Closed, 1u, __ATOMIC_RELEASE);
		for (FSlot& Slot : Pool->Slots) __atomic_exchange_n(&Slot.Interest, uint64_t(0), __ATOMIC_ACQ_REL);
		(void)PollRetirement();
	}

#if defined(WITH_DEV_AUTOMATION_TESTS) && WITH_DEV_AUTOMATION_TESTS
	EStatus SnapshotForTests(FStats& Out)
	{
		Out = {};
		FPool* Pool = Current(); if (!Pool) return EStatus::Deferred;
		FTryLock Lock(*Pool); if (!Lock.bHeld) return EStatus::Busy;
		Out.Capacity = Capacity; Out.PoolBytes = sizeof(FPool);
		Out.bClosed = __atomic_load_n(&Pool->Closed, __ATOMIC_ACQUIRE) != 0;
		Out.Created = __atomic_load_n(&Pool->Created, __ATOMIC_RELAXED);
		Out.JoinCalls = __atomic_load_n(&Pool->JoinCalls, __ATOMIC_RELAXED);
		Out.Joined = __atomic_load_n(&Pool->Joined, __ATOMIC_RELAXED);
		Out.ResolverCalls = __atomic_load_n(&Pool->ResolverCalls, __ATOMIC_RELAXED);
		Out.FreedLists = __atomic_load_n(&Pool->FreedLists, __ATOMIC_RELAXED);
		for (const FSlot& Slot : Pool->Slots)
		{
			Out.Occupied += Slot.State != Free;
			Out.Poisoned += Slot.State == Poisoned;
			Out.BeforeResolverWaiting += Slot.Created && !Slot.Joined && __atomic_load_n(&Slot.GateStage, __ATOMIC_ACQUIRE) == 1;
			Out.AfterPublishedWaiting += Slot.Created && !Slot.Joined && __atomic_load_n(&Slot.GateStage, __ATOMIC_ACQUIRE) == 2;
			Out.Unjoined += Slot.Created && !Slot.Joined;
			Out.PublishedUnjoined += Slot.Created && !Slot.Joined && __atomic_load_n(&Slot.Published, __ATOMIC_ACQUIRE) == 1;
			Out.JoinedLists += Slot.Joined && Slot.List != nullptr; // Never read worker-owned List before join.
		}
		return EStatus::Ready;
	}
#endif

}

#endif // VAREST_HAS_PRIVATE_DNS
