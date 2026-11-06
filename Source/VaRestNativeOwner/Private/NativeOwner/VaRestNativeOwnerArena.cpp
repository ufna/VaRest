// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestNativeOwnerState.h"

#if VR_NATIVE_OWNER_GNU_X64

#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace VaRestNativeOwnerPrivate
{
	using namespace Detail;

	namespace
	{
		// Actual immutable C++ object used only to start a trivial object's lifetime.
		// It is never an admitted owner, mutable arena, token or candidate.
		[[gnu::used]] constinit const VrOwner OwnerInitializationImage {};
		static_assert(sizeof(OwnerInitializationImage) == sizeof(VrOwner));
		static_assert(std::is_trivial_v<VrOwner> && std::is_aggregate_v<VrOwner>);
		static_assert(std::is_trivially_copyable_v<VrOwner> && std::is_trivially_destructible_v<VrOwner>);

		static bool CurrentOwnedPin(const VrPin& Pin)
		{
			const VrImageId Image = NativeImageIdForOwnerOperations();
			return Pin.size == sizeof(Pin) && Pin.version == VR_OWNER_VERSION
				&& Pin.platform == VR_PLATFORM_GNU_LINUX_X64 && Pin.kind == VR_PIN_OWNED_GNU_DLOPEN
				&& Pin.value != 0 && Image.hi != 0 && Image.lo != 0 && SameImage(Pin.image, Image);
		}

		static bool CompleteLocalTable(const VrOwnerApi2* Api)
		{
			return Api != nullptr && Api->size == sizeof(VrOwnerApi2) && Api->version == VR_OWNER_VERSION
				&& Api->query != nullptr && Api->poll_retirement != nullptr && Api->attach_service != nullptr
				&& Api->detach_service != nullptr && Api->submit != nullptr && Api->copy_result != nullptr
				&& Api->discard != nullptr && Api->retire != nullptr;
		}

		static VrStatus UnlockRefusal(FPassiveDescriptor2& Descriptor, VrStatus Status)
		{
			__atomic_store_n(&Descriptor.Lock, 0u, __ATOMIC_RELEASE);
			return Status;
		}

		static bool OwnerQuiescent(const FPassiveDescriptor2& Descriptor, const VrOwner& Owner)
		{
			const FNativeConnectorLease2& Lease = Owner.Connector;
			if (!IsLeaseMirrorAbsent(Descriptor))
			{
				return false;
			}
			if (!((Owner.Phase == VR_PHASE_ACTIVE && Owner.NextServiceEpoch == 0)
				|| (Owner.Phase == VR_PHASE_DORMANT && Owner.NextServiceEpoch != 0)))
			{
				return false;
			}
			if (Lease.Leased != 0 || Lease.ServiceEpoch != 0 || Lease.Poisoned != 0
				|| Lease.ServiceStartReserved != 0 || Lease.ServiceStarted != 0 || Lease.ServiceJoined != 0
				|| Lease.ServiceNeverCreated != 0 || Lease.ServiceCallExecuting != 0
				|| Owner.Service.Registered != 0 || !ResourceCountsZero(Owner)
				|| Load32(Lease.PumpRegistered) != 0 || Load32(Lease.PumpExecuting) != 0
				|| Load32(Lease.DeferredDetachAfterJoin) != 0)
			{
				return false;
			}
			for (const FNativeSlot2& Slot : Owner.Slots)
			{
				if (Slot.State != NativeSlotFree || Slot.ThreadCreated != 0 || Slot.ThreadJoined != 0
					|| Slot.CloseDone != 0 || Slot.RetirementUncertain != 0 || Slot.WsaCredit != 0
					|| Load32(Slot.Interest) != 0 || Load32(Slot.ResultPublished) != 0)
				{
					return false;
				}
			}
			return true;
		}
	}

	[[gnu::used, gnu::visibility("hidden")]] VrStatus NativeConstructPreparedOwner()
	{
		// ZERO callers in S2. Genuine lock/GT/admission/S/entry lifetime ownership
		// must be supplied by a separately accepted creating-image transaction.
		FPassiveDescriptor2& Descriptor = NativeDescriptorForOwnerOperations();
		if (__atomic_load_n(&Descriptor.Lock, __ATOMIC_ACQUIRE) != 1
			|| Load32(Descriptor.ActivationDepth) != 1)
		{
			return VR_STATUS_BUSY;
		}
		const VrInlineEscrow2& Escrow = Descriptor.Escrow;
		if (Descriptor.Reserved != 0 || Descriptor.Reserved2 != 0 || Descriptor.ArenaJournalReserved != 0
			|| Descriptor.ControlBound > 1 || Descriptor.ActivationGeneration == 0
			|| Descriptor.InlinePoisonReason > VR_POISON_NATIVE_INVARIANT || Descriptor.ArenaJournalState > ArenaJournalUncertain
			|| Escrow.phase > VR_PHASE_RELEASE_UNCERTAIN || Escrow.disposition > VR_DISPOSITION_UNCERTAIN
			|| Escrow.self_owned > 1 || Escrow.old_state > VR_OLD_IN_FLIGHT || Escrow.close_attempted > 1
			|| Escrow.reserved32 != 0 || Escrow.reserved64[0] != 0 || Escrow.reserved64[1] != 0)
		{
			return VR_STATUS_ABI_INCOMPATIBLE;
		}
		if (Escrow.phase == VR_PHASE_POISONED || Escrow.phase == VR_PHASE_RELEASE_UNCERTAIN
			|| Descriptor.InlinePoisonReason != VR_POISON_NONE || Descriptor.ArenaJournalState == ArenaJournalUncertain)
		{
			return VR_STATUS_RELEASE_UNCERTAIN;
		}
		if (Descriptor.ControlBound == 0)
		{
			return VR_STATUS_UNBOUND_CONTROL;
		}
		if (Descriptor.ControlBound != 1 || Descriptor.Reserved != 0 || Descriptor.Reserved2 != 0
			|| Descriptor.ActivationGeneration == 0 || Descriptor.ControlProcess == 0)
		{
			return VR_STATUS_ABI_INCOMPATIBLE;
		}
		if (Descriptor.Owner != nullptr || Descriptor.Api != nullptr || !IsJournalAbsent(Descriptor) || !IsLeaseMirrorAbsent(Descriptor)
			|| Escrow.size != sizeof(Escrow) || Escrow.version != VR_OWNER_VERSION
			|| Escrow.reserved32 != 0 || Escrow.reserved64[0] != 0 || Escrow.reserved64[1] != 0
			|| Escrow.self_owned != 1 || !CurrentOwnedPin(Escrow.self_pin)
			|| Escrow.old_state != VR_OLD_NONE || !IsEmptyPin(Escrow.old_pin) || Escrow.transfer_generation == 0)
		{
			return VR_STATUS_ABI_INCOMPATIBLE;
		}
		const bool Cold = Escrow.phase == VR_PHASE_PREPARED && Escrow.disposition == VR_DISPOSITION_PREPARED
			&& Escrow.close_attempted == 0 && Escrow.expected_old.hi == 0 && Escrow.expected_old.lo == 0
			&& Escrow.expected_old_epoch == 0;
		const bool Consumed = Escrow.phase == VR_PHASE_ESCROW && Escrow.disposition == VR_DISPOSITION_CONSUMED
			&& Escrow.close_attempted == 1 && Escrow.expected_old.hi != 0 && Escrow.expected_old.lo != 0
			&& Escrow.expected_old_epoch != 0 && Escrow.expected_old.hi != Escrow.self_pin.image.hi;
		if (!Cold && !Consumed)
		{
			return VR_STATUS_BUSY;
		}
		const pid_t Process = getpid();
		if (Process <= 0)
		{
			return VR_STATUS_NATIVE_ERROR;
		}
		if (static_cast<uint64_t>(Process) != Descriptor.ControlProcess)
		{
			return VR_STATUS_FORK_UNESTABLISHED;
		}
		if (pthread_equal(pthread_self(), Descriptor.ControlThread) == 0)
		{
			return VR_STATUS_WRONG_THREAD;
		}
		const VrOwnerApi2* Api = NativeApiForOwnerConstruction();
		if (!CompleteLocalTable(Api) || sizeof(VrOwner) > 16384)
		{
			return VR_STATUS_ABI_INCOMPATIBLE;
		}
		const long NativePage = sysconf(_SC_PAGESIZE);
		if (NativePage <= 0)
		{
			return VR_STATUS_NATIVE_ERROR;
		}
		const uint64_t Page = static_cast<uint64_t>(NativePage);
		if (Page > SIZE_MAX || Page % alignof(VrOwner) != 0)
		{
			return VR_STATUS_NATIVE_ERROR;
		}
		uint64_t Pages = UINT64_C(16384) / Page;
		if (UINT64_C(16384) % Page != 0)
		{
			if (Pages == UINT64_MAX)
			{
				return VR_STATUS_NATIVE_ERROR;
			}
			++Pages;
		}
		if (Pages == 0 || Pages > SIZE_MAX / Page)
		{
			return VR_STATUS_NATIVE_ERROR;
		}
		const uint64_t Rounded = Pages * Page;
		if (Rounded < 16384 || Rounded > SIZE_MAX)
		{
			return VR_STATUS_NATIVE_ERROR;
		}
		if (Descriptor.NextOwnerEpoch == UINT64_MAX)
		{
			PoisonInlineRetainedPin(Descriptor, VR_POISON_GENERATION);
			return VR_STATUS_POISONED;
		}
		const uint64_t Epoch = Descriptor.NextOwnerEpoch + 1;
		Descriptor.NextOwnerEpoch = Epoch;
		VrPin Pin;
		CopyBytes(&Escrow.self_pin, &Pin, sizeof(Pin));
		const size_t MappingBytes = static_cast<size_t>(Rounded);
		void* Raw = mmap(nullptr, MappingBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (Raw == MAP_FAILED)
		{
			return VR_STATUS_NATIVE_ERROR;
		}
		// First stores after every obtained mapping, including a successful address0.
		Descriptor.ArenaJournalPointer = Raw;
		Descriptor.ArenaJournalBytes = Rounded;
		Descriptor.ArenaJournalState = ArenaJournalObtained;
		if (Raw == nullptr)
		{
			const int Released = munmap(Raw, MappingBytes);
			if (Released == 0)
			{
				Descriptor.ArenaJournalPointer = nullptr;
				Descriptor.ArenaJournalBytes = 0;
				Descriptor.ArenaJournalState = ArenaJournalAbsent;
				return VR_STATUS_NATIVE_ERROR;
			}
			Descriptor.ArenaJournalState = ArenaJournalUncertain;
			PoisonInlineRetainedPin(Descriptor, VR_POISON_ARENA_RELEASE);
			return VR_STATUS_POISONED;
		}
		// C++20 memcpy creates the implicit-lifetime object; its returned destination
		// identifies that object. No further fallible initialization follows mapping.
		void* Created = std::memcpy(Raw, &OwnerInitializationImage, sizeof(OwnerInitializationImage));
		VrOwner* Owner = static_cast<VrOwner*>(Created);
		// The copied zero image supplies FREE slots, no lease/identity/poison,
		// service allocator0 and stop0; only these prepared live fields differ.
		Owner->MappingBytes = Rounded;
		Owner->OwnerEpoch = Epoch;
		Owner->Phase = VR_PHASE_ACTIVE;
		CopyBytes(&Pin, &Owner->OwnerPin, sizeof(Pin));
		ClearCompletedInlineForPhase(Descriptor, VR_PHASE_ACTIVE);
		Descriptor.Owner = Owner;
		Descriptor.Api = Api;
		Descriptor.ArenaJournalPointer = nullptr;
		Descriptor.ArenaJournalBytes = 0;
		Descriptor.ArenaJournalState = ArenaJournalAbsent;
		return VR_STATUS_OK;
	}

	[[gnu::used, gnu::visibility("hidden")]] VrStatus VR_CALL NativeRetire2(VrOwner* Token, const VrRetire* Input, VrRetired* Output)
	{
		if (!PrepareOutput(Output) || !IsAligned(Input) || Input->size != sizeof(*Input)
			|| Input->version != VR_OWNER_VERSION || Input->successor.hi == 0 || Input->successor.lo == 0
			|| Input->expected_epoch == 0)
		{
			return VR_STATUS_INVALID_INPUT;
		}
		VrRetire Request;
		CopyBytes(Input, &Request, sizeof(Request));
		FPassiveDescriptor2& Descriptor = NativeDescriptorForOwnerOperations();
		uint32_t Expected = 0;
		if (!__atomic_compare_exchange_n(&Descriptor.Lock, &Expected, 1u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		{
			return VR_STATUS_BUSY;
		}
		VrOwner* Owner = nullptr;
		const VrStatus Status = ValidateOwner(Descriptor, Token, ENativeRole2::Control, Owner);
		if (Status != VR_STATUS_OK)
		{
			return UnlockRefusal(Descriptor, Status);
		}
		if (Owner->Phase == VR_PHASE_POISONED || Owner->Connector.Poisoned != 0)
		{
			return UnlockRefusal(Descriptor, VR_STATUS_POISONED);
		}
		if (Request.expected_epoch != Owner->OwnerEpoch || Request.successor.hi == Owner->OwnerPin.image.hi)
		{
			return UnlockRefusal(Descriptor, VR_STATUS_STALE);
		}
		const VrStatus Deferred = ReconcileDeferredDetach(Descriptor, *Owner);
		if (Deferred != VR_STATUS_OK)
		{
			return UnlockRefusal(Descriptor, Deferred);
		}
		if (!OwnerQuiescent(Descriptor, *Owner))
		{
			return UnlockRefusal(Descriptor, VR_STATUS_OLD_OWNER_BUSY);
		}
		if (!CurrentOwnedPin(Owner->OwnerPin) || !IsJournalAbsent(Descriptor)
			|| Owner->MappingBytes < 16384 || Owner->MappingBytes > SIZE_MAX)
		{
			return UnlockRefusal(Descriptor, VR_STATUS_ABI_INCOMPATIBLE);
		}
		// The complete external successor/output/lifetime certificate is a caller
		// premise. All native checks and prepared return stores precede Free.
		VrRetired Success {};
		Success.size = sizeof(Success);
		Success.version = VR_OWNER_VERSION;
		CopyBytes(&Owner->OwnerPin, &Success.released_owner_pin, sizeof(VrPin));
		Success.retired_epoch = Owner->OwnerEpoch;
		void* const Raw = static_cast<void*>(Owner);
		const uint64_t Rounded = Owner->MappingBytes;
		const size_t MappingBytes = static_cast<size_t>(Rounded);
		const uint32_t SavedActivation = Load32(Descriptor.ActivationDepth);
		__atomic_store_n(&Descriptor.ActivationDepth, 1u, __ATOMIC_RELEASE);
		CopyBytes(&Success.released_owner_pin, &Descriptor.Escrow.self_pin, sizeof(VrPin));
		Descriptor.Escrow.self_owned = 1;
		ClearPin(Owner->OwnerPin);
		SetPhase(Descriptor, *Owner, VR_PHASE_RETIRING);
		Descriptor.ArenaJournalPointer = Raw;
		Descriptor.ArenaJournalBytes = Rounded;
		Descriptor.ArenaJournalState = ArenaJournalObtained;
		const int Released = munmap(Raw, MappingBytes);
		if (Released != 0)
		{
			// Possibly invalid arena: passive stores only, no probe or retry.
			Descriptor.Owner = nullptr;
			Descriptor.Api = nullptr;
			Descriptor.ArenaJournalState = ArenaJournalUncertain;
			Descriptor.InlinePoisonReason = VR_POISON_ARENA_RELEASE;
			Descriptor.Escrow.phase = VR_PHASE_POISONED;
			__atomic_store_n(&Descriptor.ActivationDepth, SavedActivation, __ATOMIC_RELEASE);
			__atomic_store_n(&Descriptor.Lock, 0u, __ATOMIC_RELEASE);
			return VR_STATUS_POISONED;
		}
		// IRREVERSIBLE SUCCESS: no calls, arena reads, validation, relock, RAII
		// destructor or ordinary error path follows. O protects this final return.
		Descriptor.Owner = nullptr;
		Descriptor.Api = nullptr;
		Descriptor.ArenaJournalPointer = nullptr;
		Descriptor.ArenaJournalBytes = 0;
		Descriptor.ArenaJournalState = ArenaJournalAbsent;
		Descriptor.InlinePoisonReason = VR_POISON_NONE;
		Descriptor.Escrow.phase = VR_PHASE_RETIRED;
		volatile uint8_t* To = reinterpret_cast<volatile uint8_t*>(Output);
		const uint8_t* From = reinterpret_cast<const uint8_t*>(&Success);
		for (uint32_t I = 0; I < sizeof(Success); ++I)
		{
			To[I] = From[I];
		}
		Descriptor.Escrow.self_owned = 0;
		Descriptor.Escrow.self_pin.size = sizeof(VrPin);
		Descriptor.Escrow.self_pin.version = VR_OWNER_VERSION;
		Descriptor.Escrow.self_pin.platform = VR_PLATFORM_NONE;
		Descriptor.Escrow.self_pin.kind = VR_PIN_NONE;
		Descriptor.Escrow.self_pin.value = 0;
		Descriptor.Escrow.self_pin.image.hi = 0;
		Descriptor.Escrow.self_pin.image.lo = 0;
		__atomic_store_n(&Descriptor.ActivationDepth, SavedActivation, __ATOMIC_RELEASE);
		__atomic_store_n(&Descriptor.Lock, 0u, __ATOMIC_RELEASE);
		return VR_STATUS_OK;
	}
}

#endif // VR_NATIVE_OWNER_GNU_X64
