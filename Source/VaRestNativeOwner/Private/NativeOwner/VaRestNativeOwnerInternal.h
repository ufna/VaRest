// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestNativeOwnerAbi.h"

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
#define VR_NATIVE_OWNER_GNU_X64 1
#include <pthread.h>
#else
#define VR_NATIVE_OWNER_GNU_X64 0
#endif

namespace VaRestNativeOwnerPrivate
{
#if VR_NATIVE_OWNER_GNU_X64
	using FNativeControlThread2 = pthread_t;
	static_assert(sizeof(pthread_t) == 8 && alignof(pthread_t) <= 8);
	static_assert(std::is_trivially_copyable_v<pthread_t>);
#else
	// An unbound passive placeholder only; no non-GNU owner operations are declared.
	using FNativeControlThread2 = uint64_t;
#endif

	// Atomic bookkeeping mirror only. It supplies neither a pin nor lifetime authority.
	struct FNativeLeaseIngress3
	{
		VrOwner* Owner;
		uint64_t OwnerEpoch;
		uint64_t ServiceEpoch;
		uint32_t Poisoned;
		uint32_t Reserved;
	};
	static_assert(sizeof(FNativeLeaseIngress3) == 32 && alignof(FNativeLeaseIngress3) == 8);
	static_assert(offsetof(FNativeLeaseIngress3, Owner) == 0);
	static_assert(offsetof(FNativeLeaseIngress3, OwnerEpoch) == 8);
	static_assert(offsetof(FNativeLeaseIngress3, ServiceEpoch) == 16);
	static_assert(offsetof(FNativeLeaseIngress3, Poisoned) == 24);
	static_assert(offsetof(FNativeLeaseIngress3, Reserved) == 28);
	static_assert(std::is_standard_layout_v<FNativeLeaseIngress3> && std::is_trivial_v<FNativeLeaseIngress3>);
	static_assert(std::is_trivially_copyable_v<FNativeLeaseIngress3> && std::is_trivially_destructible_v<FNativeLeaseIngress3>);

	// Loader-provided image bookkeeping, never an owner arena or resolver pool.
	// ControlBound makes ControlThread valid; zero bytes alone never identify a thread.
	struct FPassiveDescriptor2
	{
		uint32_t Lock;
		uint32_t ActivationDepth;
		uint64_t ActivationGeneration;
		uint64_t ControlProcess;
		FNativeControlThread2 ControlThread;
		uint32_t ControlBound;
		uint32_t Reserved;
		VrOwner* Owner;
		const VrOwnerApi2* Api;
		VrInlineEscrow2 Escrow;
		uint32_t InlinePoisonReason;
		uint32_t Reserved2;
		uint64_t NextOwnerEpoch;
		void* ArenaJournalPointer;
		uint64_t ArenaJournalBytes;
		uint32_t ArenaJournalState;
		uint32_t ArenaJournalReserved;
		FNativeLeaseIngress3 LeaseIngress;
	};

	static_assert(sizeof(FPassiveDescriptor2) == 288);
	static_assert(alignof(FPassiveDescriptor2) == 8);
	static_assert(offsetof(FPassiveDescriptor2, Lock) == 0);
	static_assert(offsetof(FPassiveDescriptor2, ActivationDepth) == 4);
	static_assert(offsetof(FPassiveDescriptor2, ActivationGeneration) == 8);
	static_assert(offsetof(FPassiveDescriptor2, ControlProcess) == 16);
	static_assert(offsetof(FPassiveDescriptor2, ControlThread) == 24);
	static_assert(offsetof(FPassiveDescriptor2, ControlBound) == 32);
	static_assert(offsetof(FPassiveDescriptor2, Reserved) == 36);
	static_assert(offsetof(FPassiveDescriptor2, Owner) == 40);
	static_assert(offsetof(FPassiveDescriptor2, Api) == 48);
	static_assert(offsetof(FPassiveDescriptor2, Escrow) == 56);
	static_assert(offsetof(FPassiveDescriptor2, InlinePoisonReason) == 216);
	static_assert(offsetof(FPassiveDescriptor2, Reserved2) == 220);
	static_assert(offsetof(FPassiveDescriptor2, NextOwnerEpoch) == 224);
	static_assert(offsetof(FPassiveDescriptor2, ArenaJournalPointer) == 232);
	static_assert(offsetof(FPassiveDescriptor2, ArenaJournalBytes) == 240);
	static_assert(offsetof(FPassiveDescriptor2, ArenaJournalState) == 248);
	static_assert(offsetof(FPassiveDescriptor2, ArenaJournalReserved) == 252);
	static_assert(offsetof(FPassiveDescriptor2, LeaseIngress) == 256);
	static_assert(offsetof(FPassiveDescriptor2, LeaseIngress) + offsetof(FNativeLeaseIngress3, OwnerEpoch) == 264);
	static_assert(offsetof(FPassiveDescriptor2, LeaseIngress) + offsetof(FNativeLeaseIngress3, ServiceEpoch) == 272);
	static_assert(offsetof(FPassiveDescriptor2, LeaseIngress) + offsetof(FNativeLeaseIngress3, Poisoned) == 280);
	static_assert(offsetof(FPassiveDescriptor2, LeaseIngress) + offsetof(FNativeLeaseIngress3, Reserved) == 284);
	static_assert(std::is_standard_layout_v<FPassiveDescriptor2>);
	static_assert(std::is_trivially_copyable_v<FPassiveDescriptor2>);
	static_assert(std::is_aggregate_v<FPassiveDescriptor2> && std::is_trivial_v<FPassiveDescriptor2>);
	static_assert(std::is_trivially_destructible_v<FPassiveDescriptor2>);

	enum : uint32_t { ArenaJournalAbsent = 0, ArenaJournalObtained = 1, ArenaJournalUncertain = 2 };

#if VR_NATIVE_OWNER_GNU_X64
	static_assert(__atomic_always_lock_free(sizeof(VrOwner*), nullptr));
	static_assert(__atomic_always_lock_free(sizeof(uint32_t), nullptr));
	static_assert(__atomic_always_lock_free(sizeof(uint64_t), nullptr));

	// Same-image implementation plumbing only. Neither function establishes authority.
	__attribute__((visibility("hidden"))) FPassiveDescriptor2& NativeDescriptorForOwnerOperations();
	// Caller holds the descriptor lock. The getter does not call either accessor.
	__attribute__((visibility("hidden"))) VrImageId NativeImageIdForOwnerOperations();
#endif
}
