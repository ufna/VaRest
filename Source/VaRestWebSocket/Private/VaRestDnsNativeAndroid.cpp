// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestDnsNative.h"

#if VAREST_HAS_PRIVATE_DNS && defined(__ANDROID__) && defined(__aarch64__) \
	&& defined(VAREST_ENABLE_ANDROID_PRIVATE_DNS) && VAREST_ENABLE_ANDROID_PRIVATE_DNS

// UNQUALIFIED source candidate. No default selector enables this translation unit.
// Native storage/code remain process-owned; there are no UE objects or callbacks here.
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <netdb.h>
#include <new>
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Exact direct address is checked through the NOLOAD handle before any native child.
// Protected visibility is intentional: this private ELF anchor must not be preempted.
extern "C" __attribute__((visibility("protected"), used, noinline))
void varest_private_dns_android_image_anchor_v1() {}

namespace VaRest::WebSocket::Dns
{
	namespace
	{
		constexpr uint32_t Capacity = 32, NodesPerTurn = 32;
		enum : uint32_t { Free, Running, Poisoned };
		enum : uint32_t { SemInitFailure = 1, SemPostFailure, SemWaitFailure, JoinFailure, ProtocolFailure };
		static_assert(sizeof(void*) == 8, "Android DNS candidate requires ARM64 pointers");
		static_assert(__atomic_always_lock_free(4, nullptr) && __atomic_always_lock_free(8, nullptr)
			&& __atomic_always_lock_free(sizeof(void*), nullptr), "No blocking atomic fallback is permitted");
		static_assert(__is_trivially_destructible(pthread_t), "No generation handle destructor after acknowledgement");

		struct FPool;
		struct FSlot
		{
			// Permanent fields: never reconstruct this object, semaphore or atomic storage.
			FPool* Pool;
			sem_t Wake;
			pthread_t JoinerThread;
			uint32_t SemReady, JoinerCreated, LastCreateStage;
			int LastCreateError; // Control-only raw pthread_create rc: stage1 helper, stage2 resolver.
			alignas(8) uint64_t Fatal;
			alignas(8) uint64_t Interest, HandoffGeneration, PayloadGeneration, PhysicalJoinedGeneration;
			// Control lock owns the following generation fields until pthread_create.
			uint64_t Generation;
			uint32_t State, Created, Joined;
			pthread_t Thread;
			char Host[256];
			uint16_t Port;
			// Worker owns results until matching physical-join acknowledgement.
			int ResolverError, SystemError, WorkerError;
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
			uint32_t Process, InitializationFailed;
			void* ImageReference;
			char InitializationDiagnostic[192];
			alignas(8) uint64_t Created, JoinCalls, Joined, ResolverCalls, FreedLists;
			FSlot Slots[Capacity];
		};

		alignas(8) FPool* GPool = nullptr;
		alignas(4) uint32_t GInitializing = 0, GStopped = 0;
		void* Worker(void*);
		void* Joiner(void*);

		uint32_t Load32(const uint32_t& Value) { return __atomic_load_n(&Value, __ATOMIC_ACQUIRE); }
		uint64_t Load64(const uint64_t& Value) { return __atomic_load_n(&Value, __ATOMIC_ACQUIRE); }
		void Store32(uint32_t& Value, uint32_t New) { __atomic_store_n(&Value, New, __ATOMIC_RELEASE); }
		void Store64(uint64_t& Value, uint64_t New) { __atomic_store_n(&Value, New, __ATOMIC_RELEASE); }
		void Increment(uint64_t& Value) { (void)__atomic_add_fetch(&Value, uint64_t(1), __ATOMIC_RELAXED); }
		FPool* PublishedPool() { return __atomic_load_n(&GPool, __ATOMIC_ACQUIRE); }
		bool SameProcess(const FPool& Pool)
		{
			const pid_t Process = getpid();
			return Process > 0 && static_cast<uint64_t>(Process) <= UINT32_MAX
				&& Pool.Process == static_cast<uint32_t>(Process);
		}
		FPool* Current()
		{
			FPool* Pool = PublishedPool();
			return Pool && SameProcess(*Pool) ? Pool : nullptr;
		}
		bool Closed(const FPool& Pool) { return Load32(GStopped) || Load32(Pool.Closed); }
		struct FTryLock
		{
			FPool& Pool;
			bool bHeld;
			explicit FTryLock(FPool& In) : Pool(In), bHeld(false)
			{
				uint32_t Expected = 0;
				bHeld = __atomic_compare_exchange_n(&Pool.Lock, &Expected, 1u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
			}
			~FTryLock() { if (bHeld) Store32(Pool.Lock, 0); }
		};
		bool Matches(const FPool& Pool, const FTicket& Ticket)
		{
			return Ticket.Generation && Ticket.Pool == reinterpret_cast<uintptr_t>(&Pool)
				&& Ticket.Process == Pool.Process && Ticket.Slot < Capacity;
		}
		bool Interested(const FSlot& Slot, uint64_t Generation) { return Load64(Slot.Interest) == Generation; }
		void ClearInterest(FSlot& Slot)
		{
			uint64_t Expected = Load64(Slot.Interest);
			if (Expected) (void)__atomic_compare_exchange_n(&Slot.Interest, &Expected, uint64_t(0), false,
				__ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
		}
		void Fail(FSlot& Slot, uint32_t Stage, int Code)
		{
			uint64_t Expected = 0;
			const uint64_t Packed = (uint64_t(Stage) << 32) | static_cast<uint32_t>(Code);
			(void)__atomic_compare_exchange_n(&Slot.Fatal, &Expected, Packed, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
		}
		void ReadFailure(uint64_t Fatal, FError& Error)
		{
			Error = {};
			const int32_t Code = static_cast<int32_t>(static_cast<uint32_t>(Fatal));
			if (static_cast<uint32_t>(Fatal >> 32) == JoinFailure) Error.Join = Code;
			else Error.Worker = Code; // Internal Fatal stage distinguishes native errno and synthetic EPROTO.
		}
		void CopyDiagnostic(FPool& Pool, const char* Text)
		{
			if (!Text) Text = "native image identity failure";
			uint32_t Index = 0;
			for (; Index + 1 < sizeof(Pool.InitializationDiagnostic) && Text[Index]; ++Index)
				Pool.InitializationDiagnostic[Index] = Text[Index];
			Pool.InitializationDiagnostic[Index] = 0;
		}
		bool PinImage(FPool& Pool)
		{
			Dl_info Work{}, Reap{}, Anchor{};
			const void* DirectAnchor = reinterpret_cast<const void*>(&varest_private_dns_android_image_anchor_v1);
			if (!dladdr(reinterpret_cast<const void*>(&Worker), &Work)
				|| !dladdr(reinterpret_cast<const void*>(&Joiner), &Reap) || !dladdr(DirectAnchor, &Anchor)
				|| !Work.dli_fname || !*Work.dli_fname || !Reap.dli_fname || !*Reap.dli_fname
				|| !Anchor.dli_fname || !*Anchor.dli_fname || !Work.dli_fbase
				|| Work.dli_fbase != Reap.dli_fbase || Work.dli_fbase != Anchor.dli_fbase)
			{
				CopyDiagnostic(Pool, "dladdr worker/joiner/anchor identity failure"); return false;
			}
			Pool.ImageReference = dlopen(Work.dli_fname, RTLD_NOLOAD | RTLD_NODELETE | RTLD_NOW | RTLD_LOCAL);
			if (!Pool.ImageReference) { CopyDiagnostic(Pool, dlerror()); return false; }
			(void)dlerror();
			void* Symbol = dlsym(Pool.ImageReference, "varest_private_dns_android_image_anchor_v1");
			const char* Error = dlerror();
			if (Error) { CopyDiagnostic(Pool, Error); return false; }
			Dl_info Actual{};
			if (Symbol != DirectAnchor || !dladdr(Symbol, &Actual) || Actual.dli_fbase != Work.dli_fbase)
			{
				CopyDiagnostic(Pool, "NOLOAD handle private anchor identity failure"); return false;
			}
			// Intentionally no dlclose, including monolithic libUnreal.so/APK and failed verification.
			return true;
		}
		EStatus ExistingPool(FPool& Pool, bool bMayCreateNativeThreads)
		{
			if (Load32(GStopped)) return EStatus::Stopped;
			if (!SameProcess(Pool)) return Load32(GStopped) ? EStatus::Stopped : EStatus::Poisoned;
			if (Pool.InitializationFailed) return Load32(GStopped) ? EStatus::Stopped : EStatus::NativeError;
			if (Closed(Pool)) return EStatus::Stopped;
			Store32(Pool.MaySpawn, bMayCreateNativeThreads ? 1u : 0u);
			if (Closed(Pool)) { Store32(Pool.MaySpawn, 0); return EStatus::Stopped; }
			return bMayCreateNativeThreads ? EStatus::Ready : EStatus::Deferred;
		}
		EStatus AbsentPoolStatus()
		{
			if (Load32(GStopped)) return EStatus::Stopped;
			// Current() may have seen null just before the GT published this permanent pool.
			// Classify the newly acquired pointer; never change permission or inspect slot state here.
			FPool* const Pool = PublishedPool();
			EStatus Status = EStatus::Deferred;
			if (Pool)
			{
				if (!SameProcess(*Pool)) Status = EStatus::Poisoned;
				else if (Pool->InitializationFailed) Status = EStatus::NativeError;
				else if (Closed(*Pool)) Status = EStatus::Stopped;
				else Status = Load32(Pool->MaySpawn) ? EStatus::Busy : EStatus::Deferred;
			}
			return Load32(GStopped) ? EStatus::Stopped : Status;
		}
		void ReleaseList(FPool& Pool, FSlot& Slot)
		{
			if (Slot.List)
			{
				freeaddrinfo(Slot.List); Slot.List = nullptr; Increment(Pool.FreedLists);
			}
			Slot.Cursor = nullptr;
		}
		// Control lock held. Payload publication is never admission to result access.
		void Retire(FPool& Pool, FSlot& Slot)
		{
			if (Load64(Slot.Fatal)) { Slot.State = Poisoned; return; }
			if (Slot.State != Running || !Slot.Created) return;
			if (!Slot.Joined)
			{
				if (Load64(Slot.PhysicalJoinedGeneration) != Slot.Generation) return;
				if (Load64(Slot.Fatal)) { Slot.State = Poisoned; return; }
				Slot.Joined = 1; Increment(Pool.Joined); Slot.Cursor = Slot.List;
			}
			// Interested terminal tickets still reserve this generation, even after Exhausted/error.
			if (Closed(Pool) || !Interested(Slot, Slot.Generation))
			{
				ReleaseList(Pool, Slot); Slot.Created = Slot.Joined = 0; Slot.State = Free;
				// Keep the stable atomics and generation history; stale CAS cannot remove a successor.
			}
		}

#if defined(WITH_DEV_AUTOMATION_TESTS) && WITH_DEV_AUTOMATION_TESTS
		int Gate(int Fd)
		{
			if (Fd < 0) return 0;
			uint64_t Signal = 0; ssize_t Count;
			do { Count = read(Fd, &Signal, sizeof(Signal)); } while (Count < 0 && errno == EINTR);
			return Count == static_cast<ssize_t>(sizeof(Signal)) ? 0 : Count < 0 ? errno : EIO;
		}
#endif
		void* Worker(void* Input)
		{
			FSlot& Slot = *static_cast<FSlot*>(Input);
			const uint64_t Generation = Slot.Generation;
#if defined(WITH_DEV_AUTOMATION_TESTS) && WITH_DEV_AUTOMATION_TESTS
			Store32(Slot.GateStage, Slot.BeforeGate >= 0 ? 1u : 0u);
			Slot.WorkerError = Gate(Slot.BeforeGate); Store32(Slot.GateStage, 0);
#endif
			if (!Slot.WorkerError)
			{
				char Service[6]{}; uint32_t Port = Slot.Port, Digits = 1;
				for (uint32_t Rest = Port; Rest >= 10; Rest /= 10) ++Digits;
				for (uint32_t Index = Digits; Index; --Index) { Service[Index - 1] = static_cast<char>('0' + Port % 10); Port /= 10; }
				addrinfo Hints{}; Hints.ai_family = AF_UNSPEC; Hints.ai_socktype = SOCK_STREAM; Hints.ai_flags = AI_NUMERICSERV;
				addrinfo* List = nullptr; Increment(Slot.Pool->ResolverCalls);
				const int Result = getaddrinfo(Slot.Host, Service, &Hints, &List);
				const int System = Result == EAI_SYSTEM ? errno : 0;
				Slot.ResolverError = Result; Slot.SystemError = System; Slot.List = Result == 0 ? List : nullptr;
				// No cancellation shortcut or early free: even discarded workers retain payload until join.
			}
			Store64(Slot.PayloadGeneration, Generation);
#if defined(WITH_DEV_AUTOMATION_TESTS) && WITH_DEV_AUTOMATION_TESTS
			Store32(Slot.GateStage, Slot.AfterGate >= 0 ? 2u : 0u);
			const int GateError = Gate(Slot.AfterGate); Store32(Slot.GateStage, 0);
			if (GateError) Slot.WorkerError = GateError;
#endif
			return nullptr;
		}
		void* Joiner(void* Input)
		{
			FSlot& Slot = *static_cast<FSlot*>(Input);
			sem_t* const Wake = &Slot.Wake; FPool* const Pool = Slot.Pool;
			uint64_t LastJoined = 0;
			for (;;)
			{
				int Result, System;
				do { Result = sem_wait(Wake); System = Result < 0 ? errno : 0; } while (Result < 0 && System == EINTR);
				if (Result < 0) { Fail(Slot, SemWaitFailure, System); return nullptr; }
				if (Load64(Slot.Fatal)) return nullptr;
				const uint64_t Generation = Load64(Slot.HandoffGeneration);
				if (Generation <= LastJoined || !Slot.Created) { Fail(Slot, ProtocolFailure, EPROTO); return nullptr; }
				const pthread_t Thread = Slot.Thread;
				if (pthread_equal(Thread, pthread_self())) { Fail(Slot, ProtocolFailure, EPROTO); return nullptr; }
				Increment(Pool->JoinCalls);
				const int Joined = pthread_join(Thread, nullptr);
				if (Joined != 0) { Fail(Slot, JoinFailure, Joined); return nullptr; }
				if (Load64(Slot.PayloadGeneration) != Generation) { Fail(Slot, ProtocolFailure, EPROTO); return nullptr; }
				LastJoined = Generation;
				// LAST per-generation access. Only permanent Wake/Fatal/Pool and native locals follow.
				Store64(Slot.PhysicalJoinedGeneration, Generation);
			}
		}
		EStatus EnsureJoiner(FSlot& Slot)
		{
			if (Load64(Slot.Fatal)) { Slot.State = Poisoned; return EStatus::Poisoned; }
			if (!Slot.SemReady)
			{
				if (sem_init(&Slot.Wake, 0, 0) != 0)
				{
					const int System = errno; Fail(Slot, SemInitFailure, System); Slot.State = Poisoned; return EStatus::NativeError;
				}
				Slot.SemReady = 1;
			}
			if (!Slot.JoinerCreated)
			{
				pthread_t Thread{}; const int Result = pthread_create(&Thread, nullptr, &Joiner, &Slot);
				if (Result != 0)
				{
					Slot.LastCreateStage = 1; Slot.LastCreateError = Result;
					return EStatus::NativeError; // No owned out-handle; valid semaphore may be retried.
				}
				Slot.JoinerThread = Thread; Slot.JoinerCreated = 1;
			}
			if (Load64(Slot.Fatal)) { Slot.State = Poisoned; return EStatus::Poisoned; }
			return EStatus::Ready;
		}
		bool Normalize(const addrinfo& Node, uint16_t Port, FAddress& Out)
		{
			if (!Node.ai_addr) return false;
			Out = {}; const uint16_t Expected = htons(Port);
			if (Node.ai_family == AF_INET && Node.ai_addrlen >= sizeof(sockaddr_in))
			{
				sockaddr_in Native{}; memcpy(&Native, Node.ai_addr, sizeof(Native));
				if (Native.sin_family != AF_INET || Native.sin_port != Expected) return false;
				Out.Family = 4; Out.PortBe = Native.sin_port; memcpy(Out.Bytes, &Native.sin_addr, 4); return true;
			}
			if (Node.ai_family == AF_INET6 && Node.ai_addrlen >= sizeof(sockaddr_in6))
			{
				sockaddr_in6 Native{}; memcpy(&Native, Node.ai_addr, sizeof(Native));
				if (Native.sin6_family != AF_INET6 || Native.sin6_port != Expected) return false;
				Out.Family = 6; Out.PortBe = Native.sin6_port; Out.Scope = Native.sin6_scope_id;
				memcpy(Out.Bytes, &Native.sin6_addr, sizeof(Out.Bytes)); return true;
			}
			return false;
		}
	}

	EStatus InitializeOnGT(bool bMayCreateNativeThreads)
	{
		if (Load32(GStopped)) return EStatus::Stopped;
		if (FPool* Pool = PublishedPool()) return ExistingPool(*Pool, bMayCreateNativeThreads);
		if (!bMayCreateNativeThreads)
		{
			if (Load32(GStopped)) return EStatus::Stopped;
			if (FPool* Pool = PublishedPool()) return ExistingPool(*Pool, false);
			return Load32(GStopped) ? EStatus::Stopped : EStatus::Deferred;
		}
		uint32_t Expected = 0;
		if (!__atomic_compare_exchange_n(&GInitializing, &Expected, 1u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		{
			if (Load32(GStopped)) return EStatus::Stopped;
			if (FPool* Pool = PublishedPool()) return ExistingPool(*Pool, bMayCreateNativeThreads);
			return Load32(GStopped) ? EStatus::Stopped : EStatus::Busy;
		}
		if (Load32(GStopped)) { Store32(GInitializing, 0); return EStatus::Stopped; }
		if (FPool* Pool = PublishedPool())
		{
			Store32(GInitializing, 0); return ExistingPool(*Pool, bMayCreateNativeThreads);
		}
		const pid_t Process = getpid();
		if (Process <= 0 || static_cast<uint64_t>(Process) > UINT32_MAX)
		{
			Store32(GInitializing, 0); return Load32(GStopped) ? EStatus::Stopped : EStatus::NativeError;
		}
		void* Storage = calloc(1, sizeof(FPool));
		if (!Storage) { Store32(GInitializing, 0); return Load32(GStopped) ? EStatus::Stopped : EStatus::NoMemory; }
		FPool* Pool = ::new (Storage) FPool{}; Pool->Process = static_cast<uint32_t>(Process);
		for (FSlot& Slot : Pool->Slots) Slot.Pool = Pool;
		if (!Load32(GStopped) && !PinImage(*Pool)) Pool->InitializationFailed = 1;
		const bool bClosed = Load32(GStopped) || Pool->InitializationFailed;
		Store32(Pool->Closed, bClosed ? 1u : 0u); Store32(Pool->MaySpawn, bClosed ? 0u : 1u);
		// Success AND failed pin publish once. All raw storage/image references are retained forever.
		__atomic_store_n(&GPool, Pool, __ATOMIC_RELEASE); Store32(GInitializing, 0);
		return ExistingPool(*Pool, bMayCreateNativeThreads); // Final global Stop check; never reopen closed storage.
	}
	bool ParseNumeric(const char* Host, uint32_t Bytes, uint16_t Port, FAddress& Out)
	{
		Out = {}; if (!Host || !Bytes || Bytes > 255 || !Port) return false;
		char Text[256]{};
		for (uint32_t Index = 0; Index < Bytes; ++Index) { if (!Host[Index]) return false; Text[Index] = Host[Index]; }
		if (inet_pton(AF_INET, Text, Out.Bytes) == 1) Out.Family = 4;
		else if (inet_pton(AF_INET6, Text, Out.Bytes) == 1) Out.Family = 6;
		else { Out = {}; return false; }
		Out.PortBe = htons(Port); return true;
	}
	EStatus Submit(const char* Host, uint32_t Bytes, uint16_t Port, FTicket& Out
#if defined(WITH_DEV_AUTOMATION_TESTS) && WITH_DEV_AUTOMATION_TESTS
		, int BeforeResolverGate, int AfterPublishGate
#endif
	)
	{
		Out = {}; if (!Host || !Bytes || Bytes > 255 || !Port) return EStatus::Invalid;
		for (uint32_t Index = 0; Index < Bytes; ++Index)
			if (static_cast<uint8_t>(Host[Index]) <= 32 || static_cast<uint8_t>(Host[Index]) >= 127) return EStatus::Invalid;
		if (Load32(GStopped)) return EStatus::Stopped;
		FPool* Pool = Current(); if (!Pool) return AbsentPoolStatus();
		if (Pool->InitializationFailed) return EStatus::NativeError;
		if (Closed(*Pool)) return EStatus::Stopped;
		if (!Load32(Pool->MaySpawn)) return EStatus::Deferred;
		FTryLock Lock(*Pool); if (!Lock.bHeld) return EStatus::Busy;
		for (FSlot& Slot : Pool->Slots) Retire(*Pool, Slot);
		for (uint32_t Index = 0; Index < Capacity; ++Index)
		{
			FSlot& Slot = Pool->Slots[Index]; if (Slot.State != Free) continue;
			if (Load64(Slot.Fatal)) { Slot.State = Poisoned; continue; }
			if (Slot.Generation == UINT64_MAX) { Fail(Slot, ProtocolFailure, EPROTO); Slot.State = Poisoned; continue; }
			if (Closed(*Pool)) return EStatus::Stopped;
			if (!Load32(Pool->MaySpawn)) return EStatus::Deferred;
			const EStatus Helper = EnsureJoiner(Slot); if (Helper != EStatus::Ready) return Closed(*Pool) ? EStatus::Stopped : Helper;
			++Slot.Generation; Slot.State = Running; Slot.Created = Slot.Joined = 0;
			Slot.Port = Port; Slot.List = nullptr; Slot.Cursor = nullptr;
			Slot.ResolverError = Slot.SystemError = Slot.WorkerError = 0;
			memcpy(Slot.Host, Host, Bytes); Slot.Host[Bytes] = 0;
#if defined(WITH_DEV_AUTOMATION_TESTS) && WITH_DEV_AUTOMATION_TESTS
			Slot.BeforeGate = BeforeResolverGate; Slot.AfterGate = AfterPublishGate; Store32(Slot.GateStage, 0);
#endif
			Store64(Slot.Interest, Slot.Generation);
			if (Closed(*Pool) || !Load32(Pool->MaySpawn) || Load64(Slot.Fatal))
			{
				ClearInterest(Slot); Slot.State = Load64(Slot.Fatal) ? Poisoned : Free;
				return Closed(*Pool) ? EStatus::Stopped : Slot.State == Poisoned ? EStatus::Poisoned : EStatus::Deferred;
			}
			pthread_t Thread{}; const int Result = pthread_create(&Thread, nullptr, &Worker, &Slot);
			if (Result != 0)
			{
				Slot.LastCreateStage = 2; Slot.LastCreateError = Result;
				ClearInterest(Slot); Slot.State = Load64(Slot.Fatal) ? Poisoned : Free;
				return Closed(*Pool) ? EStatus::Stopped : EStatus::NativeError;
			}
			Slot.Thread = Thread; Slot.Created = 1; Increment(Pool->Created);
			Store64(Slot.HandoffGeneration, Slot.Generation);
			if (sem_post(&Slot.Wake) != 0) { const int System = errno; Fail(Slot, SemPostFailure, System); }
			// Never skip real child handoff because Stop/permission/fatal raced pthread_create.
			if (Closed(*Pool)) { ClearInterest(Slot); return EStatus::Stopped; }
			if (Load64(Slot.Fatal)) { Slot.State = Poisoned; ClearInterest(Slot); return EStatus::Poisoned; }
			Out = {Slot.Generation, reinterpret_cast<uintptr_t>(Pool), Index, Pool->Process}; return EStatus::Queued;
		}
		return Closed(*Pool) ? EStatus::Stopped : EStatus::Capacity;
	}
	EStatus Next(const FTicket& Ticket, FAddress& Out, FError& Error)
	{
		Out = {}; Error = {}; FPool* Pool = Current(); if (!Pool || !Matches(*Pool, Ticket)) return EStatus::Invalid;
		FTryLock Lock(*Pool); if (!Lock.bHeld) return EStatus::Busy;
		FSlot& Slot = Pool->Slots[Ticket.Slot]; if (Slot.Generation != Ticket.Generation) return EStatus::Invalid;
		if (Closed(*Pool) || !Interested(Slot, Ticket.Generation)) return EStatus::Stopped;
		Retire(*Pool, Slot);
		const uint64_t Fatal = Load64(Slot.Fatal);
		if (Slot.State == Poisoned || Fatal) { Slot.State = Poisoned; ReadFailure(Fatal, Error); return EStatus::Poisoned; }
		if (Closed(*Pool) || !Interested(Slot, Ticket.Generation)) return EStatus::Stopped;
		if (!Slot.Joined) return EStatus::Pending;
		Error = {Slot.ResolverError, Slot.SystemError, Slot.WorkerError, 0};
		if (Slot.ResolverError || Slot.WorkerError) return EStatus::NativeError;
		for (uint32_t Visited = 0; Slot.Cursor && Visited < NodesPerTurn; ++Visited)
		{
			const addrinfo& Node = *Slot.Cursor; Slot.Cursor = Node.ai_next;
			if (Normalize(Node, Slot.Port, Out))
			{
				if (Closed(*Pool) || !Interested(Slot, Ticket.Generation)) { Out = {}; return EStatus::Stopped; }
				return EStatus::Ready;
			}
		}
		// Active Exhausted/NativeError ticket and list remain owned until Discard/Stop.
		return Slot.Cursor ? EStatus::Pending : EStatus::Exhausted;
	}
	void Discard(const FTicket& Ticket)
	{
		FPool* Pool = Current(); if (!Pool || !Matches(*Pool, Ticket)) return;
		uint64_t Expected = Ticket.Generation;
		(void)__atomic_compare_exchange_n(&Pool->Slots[Ticket.Slot].Interest, &Expected, uint64_t(0), false,
			__ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
	}
	EStatus PollRetirement()
	{
		FPool* Pool = Current(); if (!Pool) return AbsentPoolStatus();
		if (Pool->InitializationFailed) return Load32(GStopped) ? EStatus::Stopped : EStatus::NativeError;
		FTryLock Lock(*Pool); if (!Lock.bHeld) return EStatus::Busy;
		bool bPoisoned = false;
		for (FSlot& Slot : Pool->Slots) { Retire(*Pool, Slot); bPoisoned |= Slot.State == Poisoned; }
		return bPoisoned ? EStatus::Poisoned : EStatus::Ready; // Admitted poll, never all workers ended.
	}
	void StopForModule()
	{
		Store32(GStopped, 1); FPool* Pool = Current(); if (!Pool) return;
		Store32(Pool->Closed, 1); Store32(Pool->MaySpawn, 0);
		for (FSlot& Slot : Pool->Slots) ClearInterest(Slot);
		(void)PollRetirement(); // No semaphore post, wait, helper exit, pool destruction or dlclose.
	}
#if defined(WITH_DEV_AUTOMATION_TESTS) && WITH_DEV_AUTOMATION_TESTS
	EStatus SnapshotForTests(FStats& Out)
	{
		Out = {}; FPool* Pool = Current(); if (!Pool) return AbsentPoolStatus();
		FTryLock Lock(*Pool); if (!Lock.bHeld) return EStatus::Busy;
		Out.Capacity = Capacity; Out.PoolBytes = sizeof(FPool); Out.bClosed = Closed(*Pool);
		Out.Created = Load64(Pool->Created); Out.JoinCalls = Load64(Pool->JoinCalls); Out.Joined = Load64(Pool->Joined);
		Out.ResolverCalls = Load64(Pool->ResolverCalls); Out.FreedLists = Load64(Pool->FreedLists);
		for (const FSlot& Slot : Pool->Slots)
		{
			Out.Occupied += Slot.State != Free; Out.Poisoned += Slot.State == Poisoned || Load64(Slot.Fatal) != 0;
			Out.Unjoined += Slot.Created && !Slot.Joined;
			Out.PublishedUnjoined += Slot.Created && !Slot.Joined && Load64(Slot.PayloadGeneration) == Slot.Generation;
			Out.BeforeResolverWaiting += Slot.Created && !Slot.Joined && Load32(Slot.GateStage) == 1;
			Out.AfterPublishedWaiting += Slot.Created && !Slot.Joined && Load32(Slot.GateStage) == 2;
			Out.JoinedLists += Slot.Joined && Slot.List != nullptr; // Never inspect worker-owned result before ack adoption.
		}
		return EStatus::Ready;
	}
#endif
}

#endif // Explicit Android ARM64 private-DNS source qualification opt-in only.
