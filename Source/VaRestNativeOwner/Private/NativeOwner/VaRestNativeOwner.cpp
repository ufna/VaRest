// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestNativeOwnerAbi.h"
#include "VaRestNativeOwnerInternal.h"
#include "VaRestNativeOwnerProducer.h"

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
#define VR_NATIVE_BOOTSTRAP_GNU_X64 1
// The final artifact must verify offset-zero PT_LOAD/EHDR correspondence before native-owner entry.
// This linker-provided local anchor does not discover a loader handle or original E/P provenance.
extern "C" const unsigned char __ehdr_start[] __attribute__((visibility("hidden")));
#else
#define VR_NATIVE_BOOTSTRAP_GNU_X64 0
#endif

#if defined(_WIN32)
#define VR_NATIVE_EXPORT __declspec(dllexport)
#else
#define VR_NATIVE_EXPORT __attribute__((visibility("default")))
#endif

namespace VaRestNativeOwnerPrivate
{
	// The hidden S2 constructor has no caller; this cold getter never calls it or its table.
	static constinit FPassiveDescriptor2 Descriptor { 0, 0, 1, 0, {}, 0, 0, nullptr, nullptr, {}, 0, 0, 0, nullptr, 0, 0, 0, {} };

	static void ClearSnapshot(VrBootstrap1& Output)
	{
		// Volatile byte stores keep the passive closure independent of libc/UE memset helpers.
		volatile uint8_t* Bytes = reinterpret_cast<volatile uint8_t*>(&Output);
		for (uint32_t I = 0; I < sizeof(Output); ++I)
		{
			Bytes[I] = 0;
		}
		Output.size = sizeof(Output);
		Output.version = VR_BOOTSTRAP_VERSION;
	}

	static void CopySnapshot(const VrBootstrap1& Input, VrBootstrap1& Output)
	{
		const uint8_t* From = reinterpret_cast<const uint8_t*>(&Input);
		volatile uint8_t* To = reinterpret_cast<volatile uint8_t*>(&Output);
		for (uint32_t I = 0; I < sizeof(Output); ++I)
		{
			To[I] = From[I];
		}
	}

#if VR_NATIVE_BOOTSTRAP_GNU_X64
	static_assert(__atomic_always_lock_free(sizeof(uint32_t), nullptr), "passive descriptor needs native lock-free32 atomics");

	static bool TryLockDescriptor()
	{
		uint32_t Expected = 0;
		return __atomic_compare_exchange_n(&Descriptor.Lock, &Expected, 1u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
	}

	static void UnlockDescriptor()
	{
		__atomic_store_n(&Descriptor.Lock, 0u, __ATOMIC_RELEASE);
	}

	static bool IsZeroEscrow(const VrInlineEscrow2& Escrow)
	{
		const uint8_t* Bytes = reinterpret_cast<const uint8_t*>(&Escrow);
		for (uint32_t I = 0; I < sizeof(Escrow); ++I)
		{
			if (Bytes[I] != 0)
			{
				return false;
			}
		}
		return true;
	}

	static bool IsPristineControlThread(const FNativeControlThread2& Thread)
	{
		const uint8_t* Bytes = reinterpret_cast<const uint8_t*>(&Thread);
		for (uint32_t I = 0; I < sizeof(Thread); ++I)
		{
			if (Bytes[I] != 0)
			{
				return false;
			}
		}
		return true;
	}

	static bool ObservableJournalValid()
	{
		if (Descriptor.ArenaJournalReserved != 0)
		{
			return false;
		}
		if (Descriptor.ArenaJournalState == ArenaJournalAbsent)
		{
			return Descriptor.ArenaJournalPointer == nullptr && Descriptor.ArenaJournalBytes == 0
				&& Descriptor.InlinePoisonReason != VR_POISON_ARENA_RELEASE;
		}
		// OBTAINED is an internal interval while the existing lock is held.
		// UNCERTAIN records historical extent only, including a possible address0.
		const VrInlineEscrow2& Escrow = Descriptor.Escrow;
		const VrPin& Pin = Escrow.self_pin;
		return Descriptor.ArenaJournalState == ArenaJournalUncertain && Descriptor.ArenaJournalBytes >= 16384
			&& Descriptor.Owner == nullptr && Descriptor.Api == nullptr
			&& Descriptor.InlinePoisonReason == VR_POISON_ARENA_RELEASE
			&& Escrow.phase == VR_PHASE_POISONED && Escrow.self_owned == 1
			&& Escrow.disposition == VR_DISPOSITION_NONE && Escrow.old_state == VR_OLD_NONE
			&& Escrow.close_attempted == 0 && Escrow.expected_old.hi == 0 && Escrow.expected_old.lo == 0
			&& Escrow.expected_old_epoch == 0 && Escrow.old_pin.size == sizeof(VrPin)
			&& Escrow.old_pin.version == VR_OWNER_VERSION && Escrow.old_pin.platform == VR_PLATFORM_NONE
			&& Escrow.old_pin.kind == VR_PIN_NONE && Escrow.old_pin.value == 0
			&& Escrow.old_pin.image.hi == 0 && Escrow.old_pin.image.lo == 0 && Escrow.reserved32 == 0
			&& Escrow.reserved64[0] == 0 && Escrow.reserved64[1] == 0
			&& Pin.size == sizeof(Pin) && Pin.version == VR_OWNER_VERSION
			&& Pin.platform == VR_PLATFORM_GNU_LINUX_X64 && Pin.kind == VR_PIN_OWNED_GNU_DLOPEN && Pin.value != 0
			&& Pin.image.hi == static_cast<uint64_t>(reinterpret_cast<uintptr_t>(__ehdr_start))
			&& Pin.image.lo == Descriptor.ActivationGeneration;
	}

	static bool ObservableLeaseMirrorValid()
	{
		const FNativeLeaseIngress3& Mirror = Descriptor.LeaseIngress;
		const uint64_t ServiceEpoch = __atomic_load_n(&Mirror.ServiceEpoch, __ATOMIC_ACQUIRE);
		VrOwner* const Owner = __atomic_load_n(&Mirror.Owner, __ATOMIC_ACQUIRE);
		const uint64_t OwnerEpoch = __atomic_load_n(&Mirror.OwnerEpoch, __ATOMIC_ACQUIRE);
		const uint32_t Poisoned = __atomic_load_n(&Mirror.Poisoned, __ATOMIC_ACQUIRE);
		const uint32_t Reserved = __atomic_load_n(&Mirror.Reserved, __ATOMIC_ACQUIRE);
		if (Reserved != 0 || Poisoned > 1)
		{
			return false;
		}
		if (ServiceEpoch == 0)
		{
			return Owner == nullptr && OwnerEpoch == 0 && Poisoned == 0;
		}
		const uint32_t Phase = Descriptor.Escrow.phase;
		return Owner != nullptr && Owner == Descriptor.Owner && Descriptor.Api != nullptr && OwnerEpoch != 0
			&& (Phase == VR_PHASE_ACTIVE || Phase == VR_PHASE_RETIRING || Phase == VR_PHASE_POISONED)
			&& (Poisoned != 0) == (Phase == VR_PHASE_POISONED);
	}

	static bool SnapshotLocked(VrBootstrap1& Output)
	{
		const uint32_t Phase = Descriptor.Escrow.phase;
		if (!ObservableJournalValid() || !ObservableLeaseMirrorValid() || Descriptor.ControlBound > 1 || Descriptor.Reserved != 0 || Descriptor.Reserved2 != 0
			|| Descriptor.InlinePoisonReason > VR_POISON_NATIVE_INVARIANT || Descriptor.ActivationGeneration == 0
			|| Phase > VR_PHASE_RELEASE_UNCERTAIN)
		{
			return false;
		}
		if (Phase == VR_PHASE_INVALID)
		{
			if (!IsZeroEscrow(Descriptor.Escrow) || Descriptor.Owner != nullptr || Descriptor.Api != nullptr
				|| Descriptor.ControlBound != 0 || Descriptor.ControlProcess != 0
				|| !IsPristineControlThread(Descriptor.ControlThread)
				|| Descriptor.InlinePoisonReason != 0 || Descriptor.NextOwnerEpoch != 0
				|| Descriptor.ArenaJournalState != ArenaJournalAbsent)
			{
				return false;
			}
		}
		else if (Descriptor.Escrow.size != sizeof(VrInlineEscrow2) || Descriptor.Escrow.version != VR_OWNER_VERSION)
		{
			return false;
		}

		if (Descriptor.InlinePoisonReason != VR_POISON_NONE
			&& (Phase != VR_PHASE_POISONED || Descriptor.Owner != nullptr || Descriptor.Api != nullptr
				|| Descriptor.Escrow.self_owned != 1))
		{
			return false;
		}

		Output.magic = VR_BOOTSTRAP_MAGIC;
		Output.pointer_bits = VR_POINTER_BITS;
		Output.endian = VR_ENDIAN_LITTLE;
		Output.owner_abi = VR_OWNER_VERSION;
		Output.flags = Descriptor.ControlBound ? VR_BOUND_CONTROL : 0u;
		if (__atomic_load_n(&Descriptor.ActivationDepth, __ATOMIC_ACQUIRE) != 0
			|| Phase == VR_PHASE_PREPARED || Phase == VR_PHASE_ESCROW || Phase == VR_PHASE_CLOSE_IN_FLIGHT)
		{
			Output.flags |= VR_CONTROL_BUSY;
		}
		if (Phase == VR_PHASE_RETIRED)
		{
			Output.flags |= VR_RETIRED;
		}
		if (Phase == VR_PHASE_POISONED || Phase == VR_PHASE_RELEASE_UNCERTAIN)
		{
			Output.flags |= VR_POISONED;
		}
		Output.image.hi = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(__ehdr_start));
		Output.image.lo = Descriptor.ActivationGeneration;
		for (uint32_t I = 0; I < sizeof(Output.build_id); ++I)
		{
			Output.build_id[I] = ProducerIdentity[I];
		}
		// Opaque pointers are copied under the descriptor lock; the getter never dereferences an arena/API.
		Output.owner = Descriptor.Owner;
		Output.api = Descriptor.Api;
		return true;
	}

	FPassiveDescriptor2& NativeDescriptorForOwnerOperations()
	{
		return Descriptor;
	}

	VrImageId NativeImageIdForOwnerOperations()
	{
		return { static_cast<uint64_t>(reinterpret_cast<uintptr_t>(__ehdr_start)), Descriptor.ActivationGeneration };
	}
#endif
}

extern "C" VR_NATIVE_EXPORT VrStatus VR_CALL VaRestNativeOwnerBootstrap1(uint32_t OutputBytes, VrBootstrap1* Output)
{
	if (Output == nullptr || OutputBytes < sizeof(VrBootstrap1)
		|| reinterpret_cast<uintptr_t>(Output) % alignof(VrBootstrap1) != 0)
	{
		return VR_STATUS_INVALID_INPUT;
	}
	VaRestNativeOwnerPrivate::ClearSnapshot(*Output);
#if VR_NATIVE_BOOTSTRAP_GNU_X64
	if (!VaRestNativeOwnerPrivate::TryLockDescriptor())
	{
		return VR_STATUS_BUSY;
	}
	VrBootstrap1 Snapshot;
	VaRestNativeOwnerPrivate::ClearSnapshot(Snapshot);
	const bool Valid = VaRestNativeOwnerPrivate::SnapshotLocked(Snapshot);
	VaRestNativeOwnerPrivate::UnlockDescriptor();
	if (!Valid)
	{
		return VR_STATUS_ABI_INCOMPATIBLE;
	}
	VaRestNativeOwnerPrivate::CopySnapshot(Snapshot, *Output);
	return VR_STATUS_OK;
#else
	return VR_STATUS_DISCOVERY_UNESTABLISHED;
#endif
}

#undef VR_NATIVE_EXPORT
#undef VR_NATIVE_BOOTSTRAP_GNU_X64
