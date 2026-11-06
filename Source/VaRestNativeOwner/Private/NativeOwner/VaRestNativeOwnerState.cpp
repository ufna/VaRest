// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestNativeOwnerState.h"

#if VR_NATIVE_OWNER_GNU_X64

#include "VaRestNativeOwnerPrivateApi1.h"
#include "VaRestNativeOwnerServiceBridge.h"
#include "VaRestNativeOwnerResourceBridge.h"

#include <time.h>
#include <unistd.h>

namespace VaRestNativeOwnerPrivate
{
	namespace Detail
	{
		void ZeroBytes(void* Output, uint32_t Bytes)
		{
			volatile uint8_t* To = static_cast<volatile uint8_t*>(Output);
			for (uint32_t I = 0; I < Bytes; ++I)
			{
				To[I] = 0;
			}
		}

		void CopyBytes(const void* Input, void* Output, uint32_t Bytes)
		{
			const uint8_t* From = static_cast<const uint8_t*>(Input);
			volatile uint8_t* To = static_cast<volatile uint8_t*>(Output);
			for (uint32_t I = 0; I < Bytes; ++I)
			{
				To[I] = From[I];
			}
		}

		bool IsZeroBytes(const void* Input, uint32_t Bytes)
		{
			const uint8_t* From = static_cast<const uint8_t*>(Input);
			for (uint32_t I = 0; I < Bytes; ++I)
			{
				if (From[I] != 0)
				{
					return false;
				}
			}
			return true;
		}

		FDescriptorLock2::FDescriptorLock2(FPassiveDescriptor2& InDescriptor)
			: Descriptor(InDescriptor), Held(false)
		{
		}

		bool FDescriptorLock2::TryAcquire()
		{
			if (Held)
			{
				return false;
			}
			uint32_t Expected = 0;
			Held = __atomic_compare_exchange_n(&Descriptor.Lock, &Expected, 1u, false,
				__ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
			return Held;
		}

		FDescriptorLock2::~FDescriptorLock2()
		{
			if (Held)
			{
				__atomic_store_n(&Descriptor.Lock, 0u, __ATOMIC_RELEASE);
			}
		}

		FNativeWork2::FNativeWork2(FPassiveDescriptor2& InDescriptor)
			: Descriptor(InDescriptor), Saved(__atomic_load_n(&InDescriptor.ActivationDepth, __ATOMIC_ACQUIRE))
		{
			__atomic_store_n(&Descriptor.ActivationDepth, 1u, __ATOMIC_RELEASE);
		}

		FNativeWork2::~FNativeWork2()
		{
			__atomic_store_n(&Descriptor.ActivationDepth, Saved, __ATOMIC_RELEASE);
		}

		uint32_t Load32(const uint32_t& Value)
		{
			return __atomic_load_n(&Value, __ATOMIC_ACQUIRE);
		}

		uint64_t Load64(const uint64_t& Value)
		{
			return __atomic_load_n(&Value, __ATOMIC_ACQUIRE);
		}

		void SetInterest(FNativeSlot2& Slot, uint32_t Value)
		{
			__atomic_store_n(&Slot.Interest, Value, __ATOMIC_RELEASE);
		}

		static bool IsBoolean(uint32_t Value)
		{
			return Value <= 1;
		}

		bool SameImage(VrImageId A, VrImageId B)
		{
			return A.hi == B.hi && A.lo == B.lo;
		}

		bool IsEmptyPin(const VrPin& Pin)
		{
			return Pin.size == sizeof(VrPin) && Pin.version == VR_OWNER_VERSION
				&& Pin.platform == VR_PLATFORM_NONE && Pin.kind == VR_PIN_NONE
				&& Pin.value == 0 && Pin.image.hi == 0 && Pin.image.lo == 0;
		}

		static bool IsNormalOwnerPhase(uint32_t Phase)
		{
			return Phase == VR_PHASE_ACTIVE || Phase == VR_PHASE_DORMANT
				|| Phase == VR_PHASE_RETIRING || Phase == VR_PHASE_POISONED;
		}

		bool IsJournalAbsent(const FPassiveDescriptor2& Descriptor)
		{
			return Descriptor.ArenaJournalState == ArenaJournalAbsent && Descriptor.ArenaJournalPointer == nullptr
				&& Descriptor.ArenaJournalBytes == 0 && Descriptor.ArenaJournalReserved == 0;
		}

		bool IsLeaseMirrorAbsent(const FPassiveDescriptor2& Descriptor)
		{
			const FNativeLeaseIngress3& Mirror = Descriptor.LeaseIngress;
			return __atomic_load_n(&Mirror.ServiceEpoch, __ATOMIC_ACQUIRE) == 0
				&& __atomic_load_n(&Mirror.Owner, __ATOMIC_ACQUIRE) == nullptr
				&& __atomic_load_n(&Mirror.OwnerEpoch, __ATOMIC_ACQUIRE) == 0
				&& __atomic_load_n(&Mirror.Poisoned, __ATOMIC_ACQUIRE) == 0
				&& __atomic_load_n(&Mirror.Reserved, __ATOMIC_ACQUIRE) == 0;
		}

		static bool MirrorIdentityMatches(const FPassiveDescriptor2& Descriptor, const VrOwner& Owner)
		{
			// Caller holds the descriptor lock; arena identity is already known.
			const FNativeLeaseIngress3& Mirror = Descriptor.LeaseIngress;
			return Owner.Connector.Leased == 1 && Owner.Connector.ServiceEpoch != 0 && Owner.OwnerEpoch != 0
				&& __atomic_load_n(&Mirror.ServiceEpoch, __ATOMIC_ACQUIRE) == Owner.Connector.ServiceEpoch
				&& __atomic_load_n(&Mirror.Owner, __ATOMIC_ACQUIRE) == &Owner
				&& __atomic_load_n(&Mirror.OwnerEpoch, __ATOMIC_ACQUIRE) == Owner.OwnerEpoch
				&& __atomic_load_n(&Mirror.Reserved, __ATOMIC_ACQUIRE) == 0;
		}

		static bool LeaseMirrorValidForOwner(const FPassiveDescriptor2& Descriptor, const VrOwner& Owner)
		{
			if (Owner.Connector.Leased == 0)
			{
				return IsLeaseMirrorAbsent(Descriptor);
			}
			const uint32_t Poisoned = __atomic_load_n(&Descriptor.LeaseIngress.Poisoned, __ATOMIC_ACQUIRE);
			return MirrorIdentityMatches(Descriptor, Owner) && IsBoolean(Poisoned)
				&& (Poisoned != 0) == (Owner.Phase == VR_PHASE_POISONED);
		}

		static void PublishLeaseMirror(FPassiveDescriptor2& Descriptor, VrOwner& Owner)
		{
			FNativeLeaseIngress3& Mirror = Descriptor.LeaseIngress;
			__atomic_store_n(&Mirror.Owner, &Owner, __ATOMIC_RELAXED);
			__atomic_store_n(&Mirror.OwnerEpoch, Owner.OwnerEpoch, __ATOMIC_RELAXED);
			__atomic_store_n(&Mirror.Poisoned, 0u, __ATOMIC_RELAXED);
			__atomic_store_n(&Mirror.Reserved, 0u, __ATOMIC_RELAXED);
			__atomic_store_n(&Mirror.ServiceEpoch, Owner.Connector.ServiceEpoch, __ATOMIC_RELEASE);
		}

		static void CloseLeaseMirror(FPassiveDescriptor2& Descriptor)
		{
			// True joined/no-create/resource/pump quiescence established by the caller.
			FNativeLeaseIngress3& Mirror = Descriptor.LeaseIngress;
			__atomic_store_n(&Mirror.ServiceEpoch, UINT64_C(0), __ATOMIC_RELEASE);
			__atomic_store_n(&Mirror.Owner, static_cast<VrOwner*>(nullptr), __ATOMIC_RELAXED);
			__atomic_store_n(&Mirror.OwnerEpoch, UINT64_C(0), __ATOMIC_RELAXED);
			__atomic_store_n(&Mirror.Poisoned, 0u, __ATOMIC_RELAXED);
			__atomic_store_n(&Mirror.Reserved, 0u, __ATOMIC_RELAXED);
		}

		static bool PublishedJournalValid(const FPassiveDescriptor2& Descriptor)
		{
			if (Descriptor.ArenaJournalReserved != 0)
			{
				return false;
			}
			if (Descriptor.ArenaJournalState == ArenaJournalAbsent)
			{
				return IsJournalAbsent(Descriptor) && Descriptor.InlinePoisonReason != VR_POISON_ARENA_RELEASE;
			}
			// OBTAINED is never left observable outside its already-held transaction.
			const VrInlineEscrow2& Escrow = Descriptor.Escrow;
			const VrPin& Pin = Escrow.self_pin;
			const VrImageId Image = NativeImageIdForOwnerOperations();
			return Descriptor.ArenaJournalState == ArenaJournalUncertain && Descriptor.ArenaJournalBytes >= 16384
				&& Descriptor.Owner == nullptr && Descriptor.Api == nullptr
				&& Escrow.phase == VR_PHASE_POISONED && Escrow.self_owned == 1
				&& Escrow.disposition == VR_DISPOSITION_NONE && Escrow.old_state == VR_OLD_NONE
				&& Escrow.close_attempted == 0 && IsEmptyPin(Escrow.old_pin)
				&& Escrow.expected_old.hi == 0 && Escrow.expected_old.lo == 0 && Escrow.expected_old_epoch == 0
				&& Pin.size == sizeof(Pin) && Pin.version == VR_OWNER_VERSION
				&& Pin.platform == VR_PLATFORM_GNU_LINUX_X64 && Pin.kind == VR_PIN_OWNED_GNU_DLOPEN
				&& Pin.value != 0 && SameImage(Pin.image, Image)
				&& Descriptor.InlinePoisonReason == VR_POISON_ARENA_RELEASE;
		}

		// Descriptor-only validation. In particular, no arena/service identity is read here.
		static VrStatus ValidateDescriptorStructure(const FPassiveDescriptor2& Descriptor)
		{
			const VrInlineEscrow2& Escrow = Descriptor.Escrow;
			if (!PublishedJournalValid(Descriptor) || Descriptor.Reserved != 0 || Descriptor.Reserved2 != 0
				|| Descriptor.ControlBound > 1 || Descriptor.ActivationGeneration == 0
				|| Descriptor.InlinePoisonReason > VR_POISON_NATIVE_INVARIANT
				|| Load32(Descriptor.ActivationDepth) > 1 || Escrow.phase > VR_PHASE_RELEASE_UNCERTAIN)
			{
				return VR_STATUS_ABI_INCOMPATIBLE;
			}
			if (Escrow.phase == VR_PHASE_INVALID)
			{
				if (!IsZeroBytes(&Escrow, sizeof(Escrow)) || Descriptor.Owner != nullptr || Descriptor.Api != nullptr
					|| Descriptor.ControlBound != 0 || Descriptor.ControlProcess != 0
					|| !IsZeroBytes(&Descriptor.ControlThread, sizeof(Descriptor.ControlThread))
					|| Descriptor.InlinePoisonReason != 0 || Descriptor.NextOwnerEpoch != 0
					|| !IsJournalAbsent(Descriptor) || !IsLeaseMirrorAbsent(Descriptor))
				{
					return VR_STATUS_ABI_INCOMPATIBLE;
				}
				return VR_STATUS_UNBOUND_CONTROL;
			}
			if (Escrow.size != sizeof(Escrow) || Escrow.version != VR_OWNER_VERSION
				|| Escrow.reserved32 != 0 || Escrow.reserved64[0] != 0 || Escrow.reserved64[1] != 0
				|| Escrow.disposition > VR_DISPOSITION_UNCERTAIN || Escrow.self_owned > 1
				|| Escrow.old_state > VR_OLD_IN_FLIGHT || Escrow.close_attempted > 1)
			{
				return VR_STATUS_ABI_INCOMPATIBLE;
			}
			// Inline poison has no callable owner API, even if binding is absent.
			if (Escrow.phase == VR_PHASE_RELEASE_UNCERTAIN
				|| (Escrow.phase == VR_PHASE_POISONED && Descriptor.Owner == nullptr && Descriptor.Api == nullptr))
			{
				return VR_STATUS_RELEASE_UNCERTAIN;
			}
			if (Escrow.phase == VR_PHASE_PREPARED || Escrow.phase == VR_PHASE_ESCROW
				|| Escrow.phase == VR_PHASE_CLOSE_IN_FLIGHT)
			{
				return VR_STATUS_BUSY;
			}
			if (Descriptor.ControlBound == 0)
			{
				return VR_STATUS_UNBOUND_CONTROL;
			}
			if (Descriptor.ControlProcess == 0)
			{
				return VR_STATUS_ABI_INCOMPATIBLE;
			}
			return VR_STATUS_OK;
		}

		VrStatus ValidateDescriptorControl(const FPassiveDescriptor2& Descriptor)
		{
			const VrStatus Structure = ValidateDescriptorStructure(Descriptor);
			if (Structure != VR_STATUS_OK) return Structure;
			const pid_t Process = getpid();
			if (Process <= 0)
			{
				return VR_STATUS_NATIVE_ERROR;
			}
			if (static_cast<uint64_t>(Process) != Descriptor.ControlProcess)
			{
				return VR_STATUS_FORK_UNESTABLISHED;
			}
			return VR_STATUS_OK;
		}

		static bool LeaseFlagsValid(const FNativeConnectorLease2& Lease)
		{
			return Lease.Reserved == 0 && IsBoolean(Lease.Leased) && IsBoolean(Lease.ServiceStarted)
				&& IsBoolean(Lease.ServiceJoined) && IsBoolean(Lease.Poisoned)
				&& IsBoolean(Lease.ServiceStartReserved) && IsBoolean(Lease.ServiceNeverCreated)
				&& IsBoolean(Lease.ServiceCallExecuting) && IsBoolean(Load32(Lease.PumpRegistered))
				&& IsBoolean(Load32(Lease.PumpExecuting)) && IsBoolean(Load32(Lease.DeferredDetachAfterJoin));
		}

		static uint32_t ResourceKind(uint32_t Cell)
		{
			return Cell == 0 ? 4u : Cell == 1 ? 1u : Cell < 8 ? 2u : Cell < 12 ? 3u : 0u;
		}
		static uint32_t ResourceSubkind(uint32_t Cell) { return Cell < 10 ? 1u : Cell < 12 ? 2u : 0u; }
		static bool ResourceTerminal(uint32_t State) { return State == ResourceClosed1 || State == ResourceNotObtained1; }
		static bool ResourceAcquired(uint32_t State) { return State == ResourceOwned1 || State == ResourceUncertain1; }
		static bool ResourceFd(uint32_t Cell) { return Cell == 1 || (Cell >= 2 && Cell < 8) || (Cell >= 10 && Cell < 12); }
		static bool ResourceIdentityValid(uint32_t Index, uint64_t Value, uint64_t Extent)
		{
			if (ResourceFd(Index)) return Value <= INT32_MAX && Extent == 0;
			if (Index == 0) return Value != UINT64_MAX && Extent >= 94800 && Extent <= 131072 && Extent % 8 == 0;
			return (Index == 8 || Index == 9) && Value != 0 && Extent == 0;
		}
		static int32_t ResourceObtainedResult(uint32_t Index, uint64_t Value)
		{ return ResourceFd(Index) ? static_cast<int32_t>(Value) : 0; }
		static bool ResourceNoResultValid(uint32_t Index, int32_t Result, uint32_t Error)
		{
			// Captured failure errno bits are not normalized or used as authority.
			return (Result == 0 && Error == 0) || (Index != 8 && Index != 9 && Result == -1);
		}

		// Profile2 pool domains are selected by the stored whitelisted label, not
		// by a caller-provided domain or resource count. Prefix0..11 stays separate.
		enum class EPoolDomain2 : uint32_t { Invalid, Allocator, Fd, Pipe, Reference, Bio, Store, Index, Init };
		static EPoolDomain2 PoolDomain2(uint32_t Kind, uint32_t Sub)
		{
			if (Kind == 4 && Sub >= 2 && Sub <= 15) return EPoolDomain2::Allocator;
			if ((Kind == 5 && (Sub == 1 || Sub == 2)) || (Kind == 6 && Sub == 1) || (Kind == 7 && Sub == 1)) return EPoolDomain2::Allocator;
			if ((Kind == 5 && Sub == 3) || (Kind == 8 && Sub == 1)) return EPoolDomain2::Fd;
			if (Kind == 9 && (Sub == 1 || Sub == 2)) return EPoolDomain2::Pipe;
			if (Kind != 8) return EPoolDomain2::Invalid;
			if (Sub == 2 || Sub == 3 || Sub == 6) return EPoolDomain2::Reference;
			if (Sub == 4 || Sub == 5) return EPoolDomain2::Bio;
			if (Sub == 7) return EPoolDomain2::Store;
			if (Sub == 9 || Sub == 10) return EPoolDomain2::Index;
			if (Sub == 8 || Sub == 11 || Sub == 12) return EPoolDomain2::Init;
			return EPoolDomain2::Invalid;
		}
		static uint64_t PoolAllocationCeiling2(uint32_t Kind, uint32_t Sub)
		{
			if (Kind == 7 && Sub == 1) return 65536;
			if (Kind == 6 && Sub == 1) return 16384;
			if (Kind == 5 && (Sub == 1 || Sub == 2)) return 16384;
			if (Kind != 4) return 0;
			switch (Sub)
			{
			case 2: return 32768; case 3: return 24; case 4: return 512; case 5: return 8;
			case 6: return 4096; case 7: return 65556; case 8: return 256; case 9: return 4096;
			case 10: return 4096; case 11: return 65552; case 12: return 32800;
			case 13: return 8192; case 14: return 32768; case 15: return 65536;
			default: return 0;
			}
		}
		static uint32_t PoolGlobalCap2(uint32_t Kind, uint32_t Sub)
		{
			if (PoolDomain2(Kind, Sub) == EPoolDomain2::Invalid) return 0;
			if (Kind == 4) return Sub == 2 || Sub == 3 ? 1u : Sub == 9 ? 16u : Sub == 12 ? 4u : 2u;
			if (Kind == 5) return Sub == 2 ? 1u : 2u;
			if (Kind == 6) return 2;
			if (Kind == 7 || Kind == 9) return 1;
			if (Sub == 2 || Sub == 3 || Sub == 4) return 2;
			if (Sub == 7) return 32;
			if (Sub == 6) return 3;
			return 1;
		}
		static uint32_t PoolParentCap2(uint32_t Kind, uint32_t Sub)
		{
			if (Kind == 4 && Sub == 9) return 8;
			if (Kind == 4 && (Sub == 12 || Sub == 13 || Sub == 14)) return 2;
			if (Kind == 5 && Sub == 1) return 2;
			if (Kind == 6 && Sub == 1) return 2;
			if (Kind == 8 && Sub == 7) return 16;
			return PoolGlobalCap2(Kind, Sub) ? 1u : 0u;
		}
		static uint64_t PoolInitKey2(uint32_t Sub) { return Sub == 8 ? 1u : Sub == 11 ? 2u : Sub == 12 ? 3u : 0u; }
		static uint64_t PoolSigned32Bits2(int32_t Value) { return static_cast<uint64_t>(static_cast<int64_t>(Value)); }
		static bool PoolSigned32Value2(uint64_t Value) { return Value <= INT32_MAX || Value >= UINT64_C(0xffffffff80000000); }
		static bool PoolNoAttempt2(const VrResourceActual1& A)
		{ return A.NativeValue == 0 && A.Extent == 0 && A.Result == 0 && A.SavedError == 0; }
		static bool PoolObtained2(uint32_t Kind, uint32_t Sub, const VrResourceActual1& A)
		{
			if (A.SavedError != 0) return false;
			switch (PoolDomain2(Kind, Sub))
			{
			case EPoolDomain2::Allocator:
				return A.NativeValue != 0 && A.Extent != 0 && A.Extent <= PoolAllocationCeiling2(Kind, Sub) && A.Result == 1;
			case EPoolDomain2::Fd: case EPoolDomain2::Index:
				return A.NativeValue <= INT32_MAX && A.Extent == 0 && A.Result == static_cast<int32_t>(A.NativeValue);
			case EPoolDomain2::Pipe: return A.NativeValue <= INT32_MAX && A.Extent == 0 && A.Result == 0;
			case EPoolDomain2::Reference: case EPoolDomain2::Bio: case EPoolDomain2::Store:
				return A.NativeValue != 0 && A.Extent == 0 && A.Result == 1;
			case EPoolDomain2::Init: return A.NativeValue == PoolInitKey2(Sub) && A.Extent == 0 && A.Result == 1;
			default: return false;
			}
		}
		static bool PoolNotObtained2(uint32_t Kind, uint32_t Sub, const VrResourceActual1& A)
		{
			const EPoolDomain2 Domain = PoolDomain2(Kind, Sub);
			if (Domain == EPoolDomain2::Invalid) return false;
			if (PoolNoAttempt2(A)) return true; // Existing NotObtained entry/state is part of the discriminant.
			switch (Domain)
			{
			case EPoolDomain2::Allocator:
				return A.NativeValue == 0 && A.Extent != 0 && A.Extent <= PoolAllocationCeiling2(Kind, Sub) && A.Result == -1;
			case EPoolDomain2::Fd: case EPoolDomain2::Pipe:
				return A.NativeValue == UINT64_MAX && A.Extent == 0 && A.Result == -1;
			case EPoolDomain2::Reference: case EPoolDomain2::Bio:
				return A.NativeValue == 0 && A.Extent == 0 && A.Result == -1 && A.SavedError == 0;
			case EPoolDomain2::Store: return A.NativeValue != 0 && A.Extent == 0 && A.Result == 0 && A.SavedError == 0;
			case EPoolDomain2::Index:
				return A.Result < 0 && A.NativeValue == PoolSigned32Bits2(A.Result) && A.Extent == 0 && A.SavedError == 0;
			default: return false; // False init may have partial effects, never known absence.
			}
		}
		static bool PoolAcquireFault2(uint32_t Kind, uint32_t Sub, const VrResourceActual1& A)
		{
			if (A.Extent != 0) return false;
			switch (PoolDomain2(Kind, Sub))
			{
			case EPoolDomain2::Fd: return A.Result < -1 && A.NativeValue == PoolSigned32Bits2(A.Result);
			case EPoolDomain2::Pipe:
				// Both actual outputs were captured at the one pipe2 site. A single
				// row cannot prove the other side/equality; the source emits both faults.
				return PoolSigned32Value2(A.NativeValue) && (A.Result != 0 || A.SavedError == 0);
			case EPoolDomain2::Store:
				return A.NativeValue != 0 && A.Result != 0 && A.Result != 1 && A.SavedError == 0;
			case EPoolDomain2::Init:
				return A.NativeValue == PoolInitKey2(Sub) && A.Result != 1 && A.SavedError == 0;
			default: return false;
			}
		}
		static bool PoolDisposition2(uint32_t Kind, uint32_t Sub, const VrResourceActual1& A, bool Closed)
		{
			switch (PoolDomain2(Kind, Sub))
			{
			case EPoolDomain2::Allocator:
				return A.NativeValue != 0 && A.Extent != 0 && A.Extent <= PoolAllocationCeiling2(Kind, Sub) && A.Result == 1 && A.SavedError == 0;
			case EPoolDomain2::Fd: case EPoolDomain2::Pipe:
				return A.NativeValue <= INT32_MAX && A.Extent == 0 && (Closed ? A.Result == 0 && A.SavedError == 0 : A.Result != 0);
			case EPoolDomain2::Reference: return A.NativeValue != 0 && A.Extent == 0 && A.Result == 1 && A.SavedError == 0;
			case EPoolDomain2::Bio:
				return A.NativeValue != 0 && A.SavedError == 0 && (A.Extent == 0 ? (Closed ? A.Result == 1 : A.Result != 1)
					: A.Extent == 1 && Sub == 4 && A.Result == 2);
			case EPoolDomain2::Store: return A.NativeValue != 0 && A.Extent == 1 && A.Result == 2 && A.SavedError == 0;
			default: return false; // No init/index inverse or parent-release fiction.
			}
		}
		static VrResourceActual1 PoolStoredActual2(const FNativeResourceCell1& C)
		{ return {C.NativeValue, C.Extent, C.Result, static_cast<int32_t>(C.SavedError), C.Location, 0}; }
		static bool PoolExactActual2(const FNativeResourceCell1& C, const VrResourceActual1& A)
		{ return C.NativeValue == A.NativeValue && C.Extent == A.Extent && C.Result == A.Result && C.SavedError == static_cast<uint32_t>(A.SavedError) && C.Location == A.Location; }
		static bool PoolOriginalObtained2(const FNativeResourceCell1& C, const VrResourceActual1& A)
		{
			return PoolObtained2(C.Kind, C.Subkind, A) && C.NativeValue == A.NativeValue
				&& (PoolDomain2(C.Kind, C.Subkind) != EPoolDomain2::Allocator || C.Extent == A.Extent);
		}
		static const FNativeResourceCell1* PoolFindSerial2(const VrOwner& Owner, uint64_t Serial)
		{
			if (Serial == 0) return nullptr;
			for (const FNativeResourceCell1& C : Owner.ResourceCells)
				if (C.State != ResourceEmpty1 && C.Serial == Serial) return &C;
			return nullptr;
		}
		static bool PoolParentLabel2(uint32_t Kind, uint32_t Sub, const FNativeResourceCell1& P)
		{
			const bool Context = P.Kind == 7 && P.Subkind == 1;
			const bool Vhost = P.Kind == 6 && P.Subkind == 1;
			const bool Wsi = P.Kind == 5 && P.Subkind == 1;
			const bool SslContext = P.Kind == 8 && P.Subkind == 2;
			const bool Ssl = P.Kind == 8 && P.Subkind == 3;
			if (Kind == 4)
			{
				if (Sub == 2 || Sub == 3 || Sub == 13 || Sub == 14) return Context;
				if (Sub == 4 || Sub == 5) return Vhost;
				return Sub >= 6 && Sub <= 15 && Wsi;
			}
			if (Kind == 5) return Sub == 1 ? Vhost : Sub == 2 ? Context : Sub == 3 && Wsi;
			if (Kind == 6) return Context;
			if (Kind == 8)
			{
				if (Sub == 2) return Vhost;
				if (Sub == 3) return Wsi;
				if (Sub == 4) return Ssl;
				if (Sub == 5 || Sub == 7) return SslContext;
				if (Sub == 6) return SslContext || Ssl;
				return Context;
			}
			return Kind == 9 && P.Kind == 5 && P.Subkind == 2;
		}
		static bool PoolParent2(const VrOwner& Owner, uint32_t Kind, uint32_t Sub, uint64_t ParentSerial)
		{
			if (Kind == 7 && Sub == 1) return ParentSerial == 0;
			const FNativeResourceCell1* P = PoolFindSerial2(Owner, ParentSerial);
			return P && P->State != ResourceEmpty1 && !ResourceTerminal(P->State) && P->State != ResourceAcquireFault2 && PoolParentLabel2(Kind, Sub, *P);
		}
		static bool PoolCapacity2(const VrOwner& Owner, uint32_t Kind, uint32_t Sub, uint64_t ParentSerial, bool Adding)
		{
			uint32_t Global = Adding ? 1u : 0u, PerParent = Adding ? 1u : 0u;
			uint64_t Bytes = Adding ? PoolAllocationCeiling2(Kind, Sub) : 0;
			for (uint32_t I = 12; I < 140; ++I)
			{
				const FNativeResourceCell1& C = Owner.ResourceCells[I];
				if (C.State == ResourceEmpty1 || ResourceTerminal(C.State)) continue;
				if (C.Kind == Kind && C.Subkind == Sub) { ++Global; PerParent += C.ParentSerial == ParentSerial ? 1u : 0u; }
				const uint64_t Charge = C.State == ResourceReserved1 || C.State == ResourceAcquireFault2
					? PoolAllocationCeiling2(C.Kind, C.Subkind) : PoolDomain2(C.Kind, C.Subkind) == EPoolDomain2::Allocator ? C.Extent : 0;
				if (Charge > UINT64_C(1048576) - Bytes) return false;
				Bytes += Charge;
			}
			return Global <= PoolGlobalCap2(Kind, Sub) && PerParent <= PoolParentCap2(Kind, Sub);
		}

		struct FResourceTotals1
		{
			uint32_t Reserved = 0, Owned = 0, Uncertain = 0, Retired = 0;
			uint32_t Listener = 0, Ingress = 0, Pairs = 0, Buffers = 0;
			uint32_t Wsi = 0, Vhosts = 0, Context = 0, Dependencies = 0, Wake = 0;
			uint64_t MappingBytes = 0;
		};
		static bool InspectResourceCells(const VrOwner& Owner, FResourceTotals1& T)
		{
			const FNativeResourceHeader1& H = Owner.Resources;
			if (H.Profile != 2 || H.CellCount != 140 || !IsZeroBytes(H.Reserved, sizeof(H.Reserved))) return false;
			for (uint32_t I = 0; I < 140; ++I)
			{
				const FNativeResourceCell1& C = Owner.ResourceCells[I];
				const bool Prefix = I < 12;
				if (C.Location != I || C.ReservedTail != 0 || C.State > (Prefix ? ResourceNotObtained1 : ResourceAcquireFault2)) return false;
				if (Prefix && (C.Kind != ResourceKind(I) || C.Subkind != ResourceSubkind(I))) return false;
				if (C.State == ResourceEmpty1)
				{
					if (C.Serial || C.ParentSerial || C.NativeValue || C.Extent || C.Result || C.SavedError
						|| (!Prefix && (C.Kind || C.Subkind))) return false;
					continue;
				}
				if (C.Serial == 0 || C.Serial > H.NextSerial) return false;
				for (uint32_t J = 0; J < I; ++J)
					if (Owner.ResourceCells[J].State != ResourceEmpty1 && Owner.ResourceCells[J].Serial == C.Serial) return false;
				if (Prefix)
				{
					if (I < 10) { if (C.ParentSerial != 0) return false; }
					else
					{
						if (C.ParentSerial == 0 || C.ParentSerial >= C.Serial) return false;
						const FNativeResourceCell1& P = Owner.ResourceCells[I - 2];
						if (!ResourceTerminal(C.State) && (P.Serial != C.ParentSerial || P.State == ResourceEmpty1 || ResourceTerminal(P.State))) return false;
					}
					if (C.State == ResourceReserved1)
					{
						if (C.NativeValue || C.Extent || C.Result || C.SavedError) return false;
						++T.Reserved; continue;
					}
					if (C.State == ResourceNotObtained1)
					{
						if (C.NativeValue || C.Extent || !ResourceNoResultValid(I, C.Result, C.SavedError)) return false;
						++T.Retired; continue;
					}
					if (!ResourceIdentityValid(I, C.NativeValue, C.Extent)) return false;
					if (C.State == ResourceOwned1)
					{
						if (C.Result != ResourceObtainedResult(I, C.NativeValue) || C.SavedError) return false;
						++T.Owned;
					}
					else if (C.State == ResourceUncertain1) { if (C.Result == 0) return false; ++T.Uncertain; }
					else { if (C.State != ResourceClosed1 || C.Result != 0 || C.SavedError) return false; ++T.Retired; }
				}
				else
				{
					if (PoolDomain2(C.Kind, C.Subkind) == EPoolDomain2::Invalid) return false;
					if (C.Kind == 7) { if (C.ParentSerial != 0) return false; }
					else if (C.ParentSerial == 0 || C.ParentSerial >= C.Serial) return false;
					// Terminal children retain their historical parent serial even after
					// legitimate physical source retirement/reuse of that parent row.
					if (!ResourceTerminal(C.State)
						&& (!PoolParent2(Owner, C.Kind, C.Subkind, C.ParentSerial)
							|| !PoolCapacity2(Owner, C.Kind, C.Subkind, C.ParentSerial, false))) return false;
					const VrResourceActual1 A = PoolStoredActual2(C);
					if (C.State == ResourceReserved1)
					{
						if (!PoolNoAttempt2(A)) return false;
						++T.Reserved; continue;
					}
					if (C.State == ResourceNotObtained1)
					{
						if (!PoolNotObtained2(C.Kind, C.Subkind, A)) return false;
						++T.Retired; continue;
					}
					if (C.State == ResourceAcquireFault2)
					{
						if (!PoolAcquireFault2(C.Kind, C.Subkind, A)) return false;
						++T.Uncertain; continue; // Unknown is not a fictitious known-live unit.
					}
					if (C.State == ResourceOwned1)
					{
						if (!PoolObtained2(C.Kind, C.Subkind, A)) return false;
						++T.Owned;
					}
					else if (C.State == ResourceUncertain1)
					{
						if (!PoolDisposition2(C.Kind, C.Subkind, A, false)) return false;
						++T.Uncertain;
					}
					else
					{
						if (C.State != ResourceClosed1 || !PoolDisposition2(C.Kind, C.Subkind, A, true)) return false;
						++T.Retired;
					}
				}
				if (ResourceAcquired(C.State))
				{
					T.Listener += C.Kind == 1 ? 1u : 0u; T.Ingress += C.Kind == 2 ? 1u : 0u;
					T.Pairs += C.Kind == 3 && C.Subkind == 1 ? 1u : 0u;
					T.Buffers += C.Kind == 4 ? 1u : 0u;
					T.Wsi += C.Kind == 5 && C.Subkind <= 2 ? 1u : 0u;
					T.Vhosts += C.Kind == 6 ? 1u : 0u; T.Context += C.Kind == 7 ? 1u : 0u;
					T.Dependencies += C.Kind == 8 ? 1u : 0u; T.Wake += C.Kind == 9 ? 1u : 0u;
					if (I == 0) T.MappingBytes = C.Extent;
				}
			}
			return true;
		}

		bool ResourceBookkeepingValid(const VrOwner& Owner)
		{
			const FNativeResourceHeader1& H = Owner.Resources;
			const FNativeConnectorLease2& L = Owner.Connector;
			if (H.Profile == 0)
			{
				return Owner.NextServiceEpoch == 0 && L.Leased == 0 && IsZeroBytes(&H, sizeof(H))
					&& IsZeroBytes(Owner.ResourceCells, sizeof(Owner.ResourceCells))
					&& L.ListenerLive == 0 && L.IngressLive == 0 && L.PairsLive == 0 && L.BuffersLive == 0
					&& L.WsiLive == 0 && L.VhostsLive == 0 && L.ContextLive == 0 && L.DependencyCredits == 0 && L.WakeDescriptorsLive == 0;
			}
			FResourceTotals1 T;
			return Owner.NextServiceEpoch != 0 && InspectResourceCells(Owner, T) && H.ReservedCells == T.Reserved && H.OwnedCells == T.Owned
				&& H.UncertainCells == T.Uncertain && H.RetiredCells == T.Retired && H.MappingBytes == T.MappingBytes
				&& L.ListenerLive == T.Listener && L.IngressLive == T.Ingress && L.PairsLive == T.Pairs && L.BuffersLive == T.Buffers
				&& L.WsiLive == T.Wsi && L.VhostsLive == T.Vhosts && L.ContextLive == T.Context && L.DependencyCredits == T.Dependencies && L.WakeDescriptorsLive == T.Wake
				&& (T.Uncertain == 0 || (Owner.Phase == VR_PHASE_POISONED && L.Poisoned == 1));
		}

		bool ResourceCountsZero(const VrOwner& Owner)
		{
			if (!ResourceBookkeepingValid(Owner)) return false;
			if (Owner.Resources.Profile == 0) return true;
			// Reserved and acquired uncertainty cannot disappear behind zero live totals.
			for (const FNativeResourceCell1& C : Owner.ResourceCells)
				if (C.State != ResourceEmpty1 && !ResourceTerminal(C.State)) return false;
			const FNativeConnectorLease2& L = Owner.Connector;
			return L.ListenerLive == 0 && L.IngressLive == 0 && L.PairsLive == 0 && L.BuffersLive == 0
				&& L.WsiLive == 0 && L.VhostsLive == 0 && L.ContextLive == 0 && L.DependencyCredits == 0 && L.WakeDescriptorsLive == 0;
		}

		static bool PublishResourceTotals(VrOwner& Owner)
		{
			// Only fixed validated state transitions call this; never caller totals.
			FResourceTotals1 T;
			const bool Valid = InspectResourceCells(Owner, T);
			if (!Valid) return false;
			FNativeResourceHeader1& H = Owner.Resources;
			H.ReservedCells = T.Reserved; H.OwnedCells = T.Owned; H.UncertainCells = T.Uncertain; H.RetiredCells = T.Retired;
			H.MappingBytes = T.MappingBytes;
			Owner.Connector.ListenerLive = T.Listener; Owner.Connector.IngressLive = T.Ingress;
			Owner.Connector.PairsLive = T.Pairs; Owner.Connector.BuffersLive = T.Buffers;
			Owner.Connector.WsiLive = T.Wsi; Owner.Connector.VhostsLive = T.Vhosts; Owner.Connector.ContextLive = T.Context;
			Owner.Connector.DependencyCredits = T.Dependencies; Owner.Connector.WakeDescriptorsLive = T.Wake;
			return true;
		}

		static void InitializeResourceProfileOnAttach(VrOwner& Owner)
		{
			if (Owner.Resources.Profile != 0) return; // Preserve serials and terminal history.
			Owner.Resources.Profile = 2; Owner.Resources.CellCount = 140;
			for (uint32_t I = 0; I < 140; ++I)
			{
				Owner.ResourceCells[I].Kind = ResourceKind(I); Owner.ResourceCells[I].Subkind = ResourceSubkind(I);
				Owner.ResourceCells[I].Location = I;
			}
		}

		static bool SlotBookkeepingValid(const FNativeSlot2& Slot)
		{
			// Worker-written result/status/error bytes are deliberately excluded.
			if (Slot.State > NativeSlotPoisoned || Slot.Reserved != 0 || Slot.InputReserved16 != 0
				|| Slot.InputReserved32 != 0 || Slot.HostnameBytes > VR_MAX_HOSTNAME_BYTES
				|| !IsBoolean(Slot.ThreadCreated) || !IsBoolean(Slot.ThreadJoined) || !IsBoolean(Slot.CloseDone)
				|| !IsBoolean(Slot.RetirementUncertain) || Slot.WsaCredit != 0
				|| !IsBoolean(Load32(Slot.Interest)) || !IsBoolean(Load32(Slot.ResultPublished))
				|| (Slot.ThreadJoined != 0 && Slot.ThreadCreated == 0)
				|| (Slot.CloseDone != 0 && Slot.ThreadCreated != 0 && Slot.ThreadJoined == 0))
			{
				return false;
			}
			if (Slot.State == NativeSlotFree)
			{
				return Slot.ServiceEpoch == 0 && Slot.DeadlineNs == 0 && Slot.HostnameBytes == 0 && Slot.PortBe == 0
					&& Slot.ThreadCreated == 0 && Slot.ThreadJoined == 0 && Slot.CloseDone == 0
					&& Slot.RetirementUncertain == 0 && Load32(Slot.Interest) == 0 && Load32(Slot.ResultPublished) == 0;
			}
			if (Slot.State == NativeSlotRetiredResult
				&& (Slot.ThreadCreated != 1 || Slot.ThreadJoined != 1 || Slot.CloseDone != 1 || Slot.RetirementUncertain != 0))
			{
				return false;
			}
			return Slot.Generation != 0 && Slot.ServiceEpoch != 0 && Slot.DeadlineNs != 0
				&& Slot.HostnameBytes != 0 && Slot.PortBe != 0;
		}

		bool KnownOwnerBookkeepingValid(const FPassiveDescriptor2& Descriptor, const VrOwner& Owner)
		{
			const FNativeConnectorLease2& Lease = Owner.Connector;
			const VrImageId Image = NativeImageIdForOwnerOperations();
			if (Owner.Phase != Descriptor.Escrow.phase || !IsNormalOwnerPhase(Owner.Phase)
				|| Owner.OwnerEpoch == 0 || Descriptor.NextOwnerEpoch < Owner.OwnerEpoch
				|| Owner.MappingBytes < 16384 || Owner.MappingBytes > SIZE_MAX
				|| Owner.MappingBytes % alignof(VrOwner) != 0 || !IsJournalAbsent(Descriptor)
				|| Owner.PoisonReason > VR_POISON_NATIVE_INVARIANT
				|| ((Owner.Phase == VR_PHASE_POISONED) != (Owner.PoisonReason != VR_POISON_NONE))
				|| Descriptor.InlinePoisonReason != VR_POISON_NONE
				|| Owner.OwnerPin.size != sizeof(VrPin) || Owner.OwnerPin.version != VR_OWNER_VERSION
				|| Owner.OwnerPin.platform != VR_PLATFORM_GNU_LINUX_X64 || Owner.OwnerPin.kind != VR_PIN_OWNED_GNU_DLOPEN
				|| Owner.OwnerPin.value == 0 || !SameImage(Owner.OwnerPin.image, Image)
				|| !LeaseFlagsValid(Lease) || !LeaseMirrorValidForOwner(Descriptor, Owner) || !ResourceBookkeepingValid(Owner)
				|| !IsBoolean(Owner.Service.Registered) || Owner.Service.Reserved != 0
				|| (Lease.Poisoned != 0 && Owner.Phase != VR_PHASE_POISONED)
				|| (Lease.ServiceJoined != 0 && Lease.ServiceStarted == 0)
				|| (Lease.ServiceNeverCreated != 0 && (Lease.ServiceStarted != 0 || Lease.ServiceJoined != 0))
				|| (Lease.ServiceStartReserved != 0
					&& (Lease.ServiceStarted != 0 || Lease.ServiceJoined != 0 || Lease.ServiceNeverCreated != 0))
				|| (Owner.Service.Registered != 0
					&& (Lease.Leased == 0 || Lease.ServiceStarted == 0 || Lease.ServiceJoined != 0
						|| Lease.ServiceStartReserved != 0 || Lease.ServiceNeverCreated != 0)))
			{
				return false;
			}
			if (Lease.Leased != 0)
			{
				if (Lease.ServiceEpoch == 0 || Lease.ServiceEpoch > Owner.NextServiceEpoch || Owner.Phase == VR_PHASE_DORMANT)
				{
					return false;
				}
			}
			else if (Lease.ServiceEpoch != 0 || Lease.ServiceStarted != 0 || Lease.ServiceJoined != 0
				|| Lease.ServiceStartReserved != 0 || Lease.ServiceNeverCreated != 0 || Lease.ServiceCallExecuting != 0
				|| Owner.Service.Registered != 0 || !ResourceCountsZero(Owner)
				|| Load32(Lease.PumpRegistered) != 0 || Load32(Lease.PumpExecuting) != 0
				|| Load32(Lease.DeferredDetachAfterJoin) != 0)
			{
				return false;
			}
			for (const FNativeSlot2& Slot : Owner.Slots)
			{
				if (!SlotBookkeepingValid(Slot))
				{
					return false;
				}
			}
			return true;
		}

		static VrStatus ValidateRole(const FPassiveDescriptor2& Descriptor, const VrOwner& Owner, ENativeRole2 Role, bool& IsControl)
		{
			IsControl = false;
			const uint32_t Joined = Load32(Owner.Connector.DeferredDetachAfterJoin);
			if (!IsBoolean(Joined))
			{
				return VR_STATUS_ABI_INCOMPATIBLE;
			}
			const pthread_t Current = pthread_self();
			if (Role != ENativeRole2::Service && pthread_equal(Current, Descriptor.ControlThread) != 0)
			{
				IsControl = true;
				return VR_STATUS_OK;
			}
			// Actual join invalidates the identity immediately, even before canonical stores.
			if (Role != ENativeRole2::Control && Joined != 0)
			{
				return VR_STATUS_STALE;
			}
			// Recheck after native control identity calls, before any service identity use.
			const uint32_t LatestJoin = Load32(Owner.Connector.DeferredDetachAfterJoin);
			if (Role != ENativeRole2::Control && LatestJoin != 0)
			{
				return LatestJoin == 1 ? VR_STATUS_STALE : VR_STATUS_ABI_INCOMPATIBLE;
			}
			// This arena identity is consulted only after ValidateOwner has matched the token.
			if (Role != ENativeRole2::Control
				&& (!IsBoolean(Owner.Service.Registered) || Owner.Service.Reserved != 0
					|| !IsBoolean(Owner.Connector.Leased) || !IsBoolean(Owner.Connector.ServiceStarted)
					|| !IsBoolean(Owner.Connector.ServiceJoined) || !IsBoolean(Owner.Connector.ServiceStartReserved)
					|| !IsBoolean(Owner.Connector.ServiceNeverCreated)))
			{
				return VR_STATUS_ABI_INCOMPATIBLE;
			}
			if (Role != ENativeRole2::Control && Owner.Service.Registered == 1
				&& Owner.Connector.Leased != 0 && Owner.Connector.ServiceStarted != 0
				&& Owner.Connector.ServiceJoined == 0 && Owner.Connector.ServiceStartReserved == 0
				&& Owner.Connector.ServiceNeverCreated == 0 && Owner.Connector.ServiceEpoch != 0
				&& pthread_equal(Current, Owner.Service.Thread) != 0)
			{
				return VR_STATUS_OK;
			}
			return VR_STATUS_WRONG_THREAD;
		}

		static VrStatus MatchOwnerAfterDescriptor(FPassiveDescriptor2& Descriptor, VrOwner* Token, VrOwner*& Known)
		{
			Known = nullptr;
			// No dereference of Token, service identity or slot data precedes this gate.
			if ((Descriptor.Owner == nullptr) != (Descriptor.Api == nullptr))
			{
				return VR_STATUS_ABI_INCOMPATIBLE;
			}
			if (Descriptor.Owner == nullptr || Token != Descriptor.Owner)
			{
				return VR_STATUS_STALE;
			}
			if (reinterpret_cast<uintptr_t>(Descriptor.Owner) % alignof(VrOwner) != 0)
			{
				return VR_STATUS_ABI_INCOMPATIBLE;
			}
			if (!IsNormalOwnerPhase(Descriptor.Escrow.phase))
			{
				return VR_STATUS_ABI_INCOMPATIBLE;
			}
			const VrInlineEscrow2& Escrow = Descriptor.Escrow;
			if (Escrow.transfer_generation == 0 || Escrow.self_owned != 0 || Escrow.old_state != VR_OLD_NONE || Escrow.close_attempted != 0
				|| Escrow.disposition != VR_DISPOSITION_NONE || !IsEmptyPin(Escrow.self_pin) || !IsEmptyPin(Escrow.old_pin)
				|| Escrow.expected_old.hi != 0 || Escrow.expected_old.lo != 0 || Escrow.expected_old_epoch != 0)
			{
				return VR_STATUS_ABI_INCOMPATIBLE;
			}
			VrOwner& Owner = *Descriptor.Owner;
			if (Owner.Phase != Descriptor.Escrow.phase)
			{
				return VR_STATUS_ABI_INCOMPATIBLE;
			}
			Known = &Owner;
			return VR_STATUS_OK;
		}

		VrStatus MatchCurrentOwnerForRoleCheck(FPassiveDescriptor2& Descriptor, VrOwner* Token, VrOwner*& Known)
		{
			Known = nullptr;
			const VrStatus Status = ValidateDescriptorControl(Descriptor);
			return Status == VR_STATUS_OK ? MatchOwnerAfterDescriptor(Descriptor, Token, Known) : Status;
		}

		VrStatus ValidateOwner(FPassiveDescriptor2& Descriptor, VrOwner* Token, ENativeRole2 Role, VrOwner*& Known)
		{
			VrOwner* Matched = nullptr;
			Known = nullptr;
			const VrStatus MatchStatus = MatchCurrentOwnerForRoleCheck(Descriptor, Token, Matched);
			if (MatchStatus != VR_STATUS_OK)
			{
				return MatchStatus;
			}
			VrOwner& Owner = *Matched;
			bool IsControl = false;
			const VrStatus RoleStatus = ValidateRole(Descriptor, Owner, Role, IsControl);
			if (RoleStatus != VR_STATUS_OK)
			{
				return RoleStatus;
			}
			if (IsControl)
			{
				const VrStatus Joined = MaterializeGenuineJoinLocked(Descriptor, Owner);
				if (Joined != VR_STATUS_OK && Joined != VR_STATUS_POISONED)
				{
					return Joined;
				}
			}
			if (!KnownOwnerBookkeepingValid(Descriptor, Owner))
			{
				return VR_STATUS_ABI_INCOMPATIBLE;
			}
			Known = &Owner;
			return VR_STATUS_OK;
		}

		bool SameSession(const VrOwner& Owner, VrSession Session)
		{
			return Session.owner_epoch != 0 && Session.owner_epoch == Owner.OwnerEpoch
				&& Session.service_epoch != 0 && Owner.Connector.Leased != 0
				&& Session.service_epoch == Owner.Connector.ServiceEpoch;
		}

		bool IsStopped(const VrOwner& Owner, uint64_t ServiceEpoch)
		{
			return Load64(Owner.StopEpoch) == ServiceEpoch;
		}

		static bool TicketShapeValid(VrTicket Ticket)
		{
			return Ticket.slot < VR_MAX_SLOTS && Ticket.reserved == 0;
		}

		FNativeSlot2* InterestedSlot(VrOwner& Owner, VrTicket Ticket)
		{
			const VrSession Session { Ticket.owner_epoch, Ticket.service_epoch };
			if (!SameSession(Owner, Session) || Ticket.slot_generation == 0)
			{
				return nullptr;
			}
			FNativeSlot2& Slot = Owner.Slots[Ticket.slot];
			if (Slot.State == NativeSlotFree || Slot.Generation != Ticket.slot_generation
				|| Slot.ServiceEpoch != Ticket.service_epoch || Load32(Slot.Interest) != 1)
			{
				return nullptr;
			}
			return &Slot;
		}

		bool RetiredAndClean(const FNativeSlot2& Slot)
		{
			return Slot.State == NativeSlotRetiredResult && Slot.ThreadCreated == 1 && Slot.ThreadJoined == 1
				&& Slot.CloseDone == 1 && Slot.RetirementUncertain == 0 && Slot.WsaCredit == 0;
		}

		void ReleaseRetiredSlot(FNativeSlot2& Slot)
		{
			// Only callers which established actual retirement/cleanup may reach here.
			const uint64_t Generation = Slot.Generation;
			ZeroBytes(&Slot, sizeof(Slot));
			Slot.Generation = Generation;
		}

		void ReleaseNeverCreatedSlot(FNativeSlot2& Slot)
		{
			// Only the creating submit failure path, after pthread_create returned nonzero.
			const uint64_t Generation = Slot.Generation;
			ZeroBytes(&Slot, sizeof(Slot));
			Slot.Generation = Generation;
		}

		void ClearPin(VrPin& Pin)
		{
			ZeroBytes(&Pin, sizeof(Pin));
			Pin.size = sizeof(Pin);
			Pin.version = VR_OWNER_VERSION;
		}

		void ClearCompletedInlineForPhase(FPassiveDescriptor2& Descriptor, uint32_t Phase)
		{
			const uint64_t Transfer = Descriptor.Escrow.transfer_generation;
			ZeroBytes(&Descriptor.Escrow, sizeof(Descriptor.Escrow));
			Descriptor.Escrow.size = sizeof(Descriptor.Escrow);
			Descriptor.Escrow.version = VR_OWNER_VERSION;
			Descriptor.Escrow.phase = Phase;
			Descriptor.Escrow.transfer_generation = Transfer;
			ClearPin(Descriptor.Escrow.self_pin);
			ClearPin(Descriptor.Escrow.old_pin);
		}

		void PoisonInlineRetainedPin(FPassiveDescriptor2& Descriptor, uint32_t Reason)
		{
			// Called only with the actual known creating credit already in self_pin.
			VrPin Pin;
			CopyBytes(&Descriptor.Escrow.self_pin, &Pin, sizeof(Pin));
			ClearCompletedInlineForPhase(Descriptor, VR_PHASE_POISONED);
			CopyBytes(&Pin, &Descriptor.Escrow.self_pin, sizeof(Pin));
			Descriptor.Escrow.self_owned = 1;
			Descriptor.InlinePoisonReason = Reason;
			Descriptor.Owner = nullptr;
			Descriptor.Api = nullptr;
		}

		void SetPhase(FPassiveDescriptor2& Descriptor, VrOwner& Owner, uint32_t Phase)
		{
			Owner.Phase = Phase;
			Descriptor.Escrow.phase = Phase;
		}

		void PoisonLiveOwner(FPassiveDescriptor2& Descriptor, VrOwner& Owner, uint32_t Reason)
		{
			if (MirrorIdentityMatches(Descriptor, Owner)
				&& IsBoolean(__atomic_load_n(&Descriptor.LeaseIngress.Poisoned, __ATOMIC_ACQUIRE)))
			{
				// Preserve malformed mirrors instead of repairing them into a usable lease.
				__atomic_store_n(&Descriptor.LeaseIngress.Poisoned, 1u, __ATOMIC_RELEASE);
			}
			if (Owner.PoisonReason == VR_POISON_NONE)
			{
				Owner.PoisonReason = Reason;
			}
			Owner.Connector.Poisoned = 1;
			SetPhase(Descriptor, Owner, VR_PHASE_POISONED);
		}

		VrStatus MaterializeGenuineJoinLocked(FPassiveDescriptor2& Descriptor, VrOwner& Owner)
		{
			const uint32_t Fact = Load32(Owner.Connector.DeferredDetachAfterJoin);
			if (Fact == 0)
			{
				return VR_STATUS_OK;
			}
			if (Fact != 1)
			{
				return VR_STATUS_ABI_INCOMPATIBLE;
			}
			FNativeConnectorLease2& Lease = Owner.Connector;
			if (!MirrorIdentityMatches(Descriptor, Owner) || Lease.ServiceNeverCreated != 0
				|| Owner.Service.Reserved != 0 || !IsBoolean(Owner.Service.Registered)
				|| !IsBoolean(Lease.ServiceStarted) || !IsBoolean(Lease.ServiceJoined)
				|| !IsBoolean(Lease.ServiceStartReserved))
			{
				PoisonLiveOwner(Descriptor, Owner, VR_POISON_NATIVE_INVARIANT);
				return VR_STATUS_POISONED;
			}
			const bool RegisteredStart = Owner.Service.Registered == 1 && Lease.ServiceStarted == 1
				&& Lease.ServiceJoined == 0 && Lease.ServiceStartReserved == 0;
			const bool UnregisteredStart = Owner.Service.Registered == 0 && Lease.ServiceStarted == 0
				&& Lease.ServiceJoined == 0 && Lease.ServiceStartReserved == 1;
			const bool AlreadyJoined = Owner.Service.Registered == 0 && Lease.ServiceStarted == 1
				&& Lease.ServiceJoined == 1 && Lease.ServiceStartReserved == 0;
			if (!RegisteredStart && !UnregisteredStart && !AlreadyJoined)
			{
				PoisonLiveOwner(Descriptor, Owner, VR_POISON_NATIVE_INVARIANT);
				return VR_STATUS_POISONED;
			}
			Lease.ServiceStarted = 1;
			Lease.ServiceJoined = 1;
			Lease.ServiceStartReserved = 0;
			Owner.Service.Registered = 0;
			// Fact1 remains the durable tombstone/request until true completed detach.
			return VR_STATUS_OK;
		}

		void BuildSnapshot(const VrOwner& Owner, VrSnapshot& Snapshot)
		{
			Snapshot.state = Owner.Phase;
			Snapshot.owner_epoch = Owner.OwnerEpoch;
			Snapshot.service_epoch = Owner.Connector.Leased != 0 ? Owner.Connector.ServiceEpoch : 0;
			Snapshot.image = NativeImageIdForOwnerOperations();
			for (const FNativeSlot2& Slot : Owner.Slots)
			{
				Snapshot.occupied_slots += Slot.State != NativeSlotFree ? 1u : 0u;
			}
			Snapshot.connector_leased = Owner.Connector.Leased;
			Snapshot.poison_reason = Owner.PoisonReason;
			Snapshot.service_executing = (Owner.Connector.ServiceStarted != 0 && Owner.Connector.ServiceJoined == 0
				&& Load32(Owner.Connector.DeferredDetachAfterJoin) == 0)
				|| Owner.Connector.ServiceCallExecuting != 0 ? 1u : 0u;
		}

		static bool ServiceDispositionComplete(const FNativeConnectorLease2& Lease)
		{
			return Lease.ServiceStartReserved == 0
				&& ((Lease.ServiceNeverCreated == 1 && Lease.ServiceStarted == 0 && Lease.ServiceJoined == 0)
					|| (Lease.ServiceNeverCreated == 0 && Lease.ServiceStarted == 1 && Lease.ServiceJoined == 1));
		}

		static bool CanFinishDetach(const VrOwner& Owner)
		{
			const FNativeConnectorLease2& Lease = Owner.Connector;
			return Lease.Leased == 1 && Lease.Poisoned == 0 && ServiceDispositionComplete(Lease)
				&& ResourceCountsZero(Owner) && Lease.ServiceCallExecuting == 0
				&& Owner.Service.Registered == 0 && Load32(Lease.PumpRegistered) == 0 && Load32(Lease.PumpExecuting) == 0;
		}

		static void FinishDetach(FPassiveDescriptor2& Descriptor, VrOwner& Owner)
		{
			// Neither normal nor deferred detach may erase outstanding row custody.
			if (!CanFinishDetach(Owner)) return;
			const uint64_t ServiceEpoch = Owner.Connector.ServiceEpoch;
			CloseLeaseMirror(Descriptor);
			for (FNativeSlot2& Slot : Owner.Slots)
			{
				if (Slot.State != NativeSlotFree && Slot.ServiceEpoch == ServiceEpoch)
				{
					SetInterest(Slot, 0);
					if (RetiredAndClean(Slot))
					{
						ReleaseRetiredSlot(Slot);
					}
				}
			}
			// Actual service join/no-create and pump removal precede these fixed stores.
			ZeroBytes(&Owner.Service, sizeof(Owner.Service));
			ZeroBytes(&Owner.Connector, sizeof(Owner.Connector));
			SetPhase(Descriptor, Owner, VR_PHASE_DORMANT);
		}

		VrStatus ReconcileDeferredDetach(FPassiveDescriptor2& Descriptor, VrOwner& Owner)
		{
			const VrStatus Joined = MaterializeGenuineJoinLocked(Descriptor, Owner);
			if (Joined != VR_STATUS_OK)
			{
				return Joined;
			}
			if (Load32(Owner.Connector.DeferredDetachAfterJoin) == 0)
			{
				return VR_STATUS_OK;
			}
			if (Owner.Connector.ServiceNeverCreated != 0 || Owner.Connector.ServiceJoined != 1
				|| !CanFinishDetach(Owner))
			{
				return VR_STATUS_OLD_OWNER_BUSY;
			}
			FinishDetach(Descriptor, Owner);
			return VR_STATUS_OK;
		}

		bool MonotonicNowNs(uint64_t& Now)
		{
			timespec Stamp {};
			if (clock_gettime(CLOCK_MONOTONIC, &Stamp) != 0 || Stamp.tv_sec < 0
				|| Stamp.tv_nsec < 0 || Stamp.tv_nsec >= 1000000000)
			{
				return false;
			}
			const uint64_t Seconds = static_cast<uint64_t>(Stamp.tv_sec);
			const uint64_t Nanoseconds = static_cast<uint64_t>(Stamp.tv_nsec);
			if (Seconds > (UINT64_MAX - Nanoseconds) / 1000000000u)
			{
				return false;
			}
			Now = Seconds * 1000000000u + Nanoseconds;
			return true;
		}

		static bool SameTicket(VrTicket A, VrTicket B)
		{
			return A.owner_epoch == B.owner_epoch && A.service_epoch == B.service_epoch
				&& A.slot_generation == B.slot_generation && A.slot == B.slot && A.reserved == B.reserved;
		}

		static bool AddressValid(const VrAddress& Address)
		{
			if (Address.size != sizeof(VrAddress) || Address.version != VR_OWNER_VERSION
				|| Address.port_be == 0 || Address.reserved16 != 0
				|| !IsZeroBytes(Address.reserved8, sizeof(Address.reserved8)))
			{
				return false;
			}
			if (Address.family == VR_ADDRESS_IPV4)
			{
				return Address.scope == 0 && IsZeroBytes(Address.address + 4, 12);
			}
			return Address.family == VR_ADDRESS_IPV6;
		}

		static bool SameAddress(const VrAddress& A, const VrAddress& B)
		{
			if (A.family != B.family || A.scope != B.scope || A.port_be != B.port_be)
			{
				return false;
			}
			for (uint32_t I = 0; I < sizeof(A.address); ++I)
			{
				if (A.address[I] != B.address[I])
				{
					return false;
				}
			}
			return true;
		}

		bool CompletedResultValid(const FNativeSlot2& Slot, VrTicket Ticket)
		{
			// Called only after actual retirement, cleanup and acquire publication.
			const VrResult& Result = Slot.Result;
			if (Result.size != sizeof(Result) || Result.version != VR_OWNER_VERSION
				|| !SameTicket(Result.ticket, Ticket) || Result.count > VR_MAX_ADDRESSES)
			{
				return false;
			}
			if (Slot.ResultStatus == VR_STATUS_OK)
			{
				if (Result.count == 0 || Result.resolver_error != 0)
				{
					return false;
				}
			}
			else if (Slot.ResultStatus == VR_STATUS_NATIVE_ERROR)
			{
				if (Result.count != 0 || Result.resolver_error == 0)
				{
					return false;
				}
			}
			else if (Slot.ResultStatus != VR_STATUS_NO_RESULT || Result.count != 0 || Result.resolver_error != 0)
			{
				return false;
			}
			for (uint32_t I = 0; I < VR_MAX_ADDRESSES; ++I)
			{
				if (I >= Result.count)
				{
					if (!IsZeroBytes(&Result.address[I], sizeof(VrAddress)))
					{
						return false;
					}
					continue;
				}
				if (!AddressValid(Result.address[I])
					|| Result.address[I].port_be != Slot.PortBe)
				{
					return false;
				}
				for (uint32_t J = 0; J < I; ++J)
				{
					if (SameAddress(Result.address[I], Result.address[J]))
					{
						return false;
					}
				}
			}
			return true;
		}
		// These guards are NOT a hazard-pointer/admission protocol. The real future
		// caller already owns the live lease/pin and actual GT serialization.
		static VrStatus ValidateNoLockLeaseIngress(FPassiveDescriptor2& Descriptor, VrOwner* Token,
			VrSession Session, VrOwner*& Known)
		{
			Known = nullptr;
			if (!IsAligned(Token) || Session.owner_epoch == 0 || Session.service_epoch == 0)
			{
				return VR_STATUS_INVALID_INPUT;
			}
			const uint32_t Bound = __atomic_load_n(&Descriptor.ControlBound, __ATOMIC_ACQUIRE);
			if (Bound == 0)
			{
				return VR_STATUS_UNBOUND_CONTROL;
			}
			if (Bound != 1 || Descriptor.ControlProcess == 0)
			{
				return VR_STATUS_ABI_INCOMPATIBLE;
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
			// All native identity calls precede this final bounded atomic read set.
			const FNativeLeaseIngress3& Mirror = Descriptor.LeaseIngress;
			const uint64_t First = __atomic_load_n(&Mirror.ServiceEpoch, __ATOMIC_ACQUIRE);
			VrOwner* const Owner = __atomic_load_n(&Mirror.Owner, __ATOMIC_ACQUIRE);
			const uint64_t Epoch = __atomic_load_n(&Mirror.OwnerEpoch, __ATOMIC_ACQUIRE);
			const uint32_t Poisoned = __atomic_load_n(&Mirror.Poisoned, __ATOMIC_ACQUIRE);
			const uint32_t Reserved = __atomic_load_n(&Mirror.Reserved, __ATOMIC_ACQUIRE);
			const uint64_t Last = __atomic_load_n(&Mirror.ServiceEpoch, __ATOMIC_ACQUIRE);
			if (!IsBoolean(Poisoned) || Reserved != 0)
			{
				return VR_STATUS_ABI_INCOMPATIBLE;
			}
			if (First == 0 || First != Last)
			{
				return VR_STATUS_STALE;
			}
			if (!IsAligned(Owner) || Epoch == 0)
			{
				return VR_STATUS_ABI_INCOMPATIBLE;
			}
			if (Owner != Token || Epoch != Session.owner_epoch || First != Session.service_epoch)
			{
				return VR_STATUS_STALE;
			}
			Known = Owner;
			return VR_STATUS_OK;
		}

		static VrStatus PublishGenuineJoinFact(FPassiveDescriptor2& Descriptor, VrOwner* Token, VrSession Session)
		{
			VrOwner* Owner = nullptr;
			const VrStatus Status = ValidateNoLockLeaseIngress(Descriptor, Token, Session, Owner);
			if (Status != VR_STATUS_OK)
			{
				return Status;
			}
			// The actual creating owner ALREADY observed real native service join.
			uint32_t Expected = 0;
			if (__atomic_compare_exchange_n(&Owner->Connector.DeferredDetachAfterJoin, &Expected, 1u,
				false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			{
				return VR_STATUS_OK;
			}
			return Expected == 1 ? VR_STATUS_OK : VR_STATUS_ABI_INCOMPATIBLE;
		}

		static VrStatus PumpPermit(const FPassiveDescriptor2& Descriptor, const VrOwner& Owner, VrSession Session)
		{
			const uint32_t Poisoned = __atomic_load_n(&Descriptor.LeaseIngress.Poisoned, __ATOMIC_ACQUIRE);
			const uint32_t Registered = Load32(Owner.Connector.PumpRegistered);
			const uint32_t Joined = Load32(Owner.Connector.DeferredDetachAfterJoin);
			if (!IsBoolean(Poisoned) || !IsBoolean(Registered) || !IsBoolean(Joined))
			{
				return VR_STATUS_ABI_INCOMPATIBLE;
			}
			if (Poisoned != 0)
			{
				return VR_STATUS_POISONED;
			}
			if (Joined != 0 || IsStopped(Owner, Session.service_epoch))
			{
				return VR_STATUS_STALE;
			}
			return Registered == 1 ? VR_STATUS_OK : VR_STATUS_OLD_OWNER_BUSY;
		}

	}

	using namespace Detail;
	namespace
	{
		enum class EResourceEvent1 : uint32_t { Reserve, Obtained, NotObtained, Closed, Uncertain };
		static bool ResourceSeparate(const void* A, uint32_t N, const void* B, uint32_t M)
		{
			if (A == nullptr || B == nullptr) return true;
			const uintptr_t X = reinterpret_cast<uintptr_t>(A), Y = reinterpret_cast<uintptr_t>(B);
			return N <= UINTPTR_MAX - X && M <= UINTPTR_MAX - Y && (X + N <= Y || Y + M <= X);
		}
		static bool FrameResourceReply(VrOwner* Token, const void* A, uint32_t N,
			const void* B, uint32_t M, uint32_t Bytes, VrResourceReply1* Out)
		{
			// Actual complete accessible spans/code/lease remain caller premises.
			if (Bytes < sizeof(*Out) || !IsAligned(Out)
				|| !ResourceSeparate(Out, sizeof(*Out), A, N) || !ResourceSeparate(Out, sizeof(*Out), B, M)
				|| !ResourceSeparate(A, N, B, M) || !ResourceSeparate(Token, sizeof(VrOwner), Out, sizeof(*Out))
				|| !ResourceSeparate(Token, sizeof(VrOwner), A, N) || !ResourceSeparate(Token, sizeof(VrOwner), B, M)) return false;
			ZeroBytes(Out, sizeof(*Out)); Out->Size = sizeof(*Out); Out->Version = VR_PRIVATE_RESOURCE_VERSION;
			return true;
		}
		static bool ResourceTicketShape(const VrResourceTicket1& T, VrSession S)
		{
			return T.Size == sizeof(T) && T.Version == VR_PRIVATE_RESOURCE_VERSION && T.OwnerEpoch != 0 && T.ServiceEpoch != 0
				&& T.OwnerEpoch == S.owner_epoch && T.ServiceEpoch == S.service_epoch && T.Serial != 0 && T.Cell < 140
				&& (T.Cell < 12 ? T.Kind == ResourceKind(T.Cell) && T.Subkind == ResourceSubkind(T.Cell)
					: PoolDomain2(T.Kind, T.Subkind) != EPoolDomain2::Invalid);
		}
		static void FillResourceTicket(const VrOwner& Owner, VrSession S, uint32_t Index, VrResourceReply1& Out)
		{
			const FNativeResourceCell1& C = Owner.ResourceCells[Index];
			Out.Ticket.Size = sizeof(Out.Ticket); Out.Ticket.Version = VR_PRIVATE_RESOURCE_VERSION;
			Out.Ticket.OwnerEpoch = Owner.OwnerEpoch; Out.Ticket.ServiceEpoch = S.service_epoch;
			Out.Ticket.Serial = C.Serial; Out.Ticket.Cell = Index;
			Out.Ticket.Kind = static_cast<uint16_t>(C.Kind); Out.Ticket.Subkind = static_cast<uint16_t>(C.Subkind);
		}
		struct FPreparedResourceIdentity1 { pid_t Process; pthread_t Thread; };
		static VrStatus SampleResourceIdentity(VrResourceReply1& Out, FPreparedResourceIdentity1& Identity)
		{
			Out.NativeCalls = 1; Identity.Process = getpid();
			if (Identity.Process <= 0) return VR_STATUS_NATIVE_ERROR;
			Out.NativeCalls = 2; Identity.Thread = pthread_self();
			// pthread_self is const to the compiler. Consume opaque prepared values
			// before the try-CAS so BUSY also performs/reports both observations.
			asm volatile("" : "+r"(Identity.Process), "+r"(Identity.Thread) : : "memory");
			return VR_STATUS_OK;
		}
		static VrStatus CheckResourceRoleLocked(FPassiveDescriptor2& D, VrOwner* Token, VrSession Session,
			const FPreparedResourceIdentity1& Identity, VrOwner*& Owner)
		{
			Owner = nullptr;
			const VrStatus Structure = ValidateDescriptorStructure(D);
			if (Structure != VR_STATUS_OK) return Structure;
			if (static_cast<uint64_t>(Identity.Process) != D.ControlProcess) return VR_STATUS_FORK_UNESTABLISHED;
			const VrStatus Matched = MatchOwnerAfterDescriptor(D, Token, Owner);
			if (Matched != VR_STATUS_OK) return Matched;
			// No arena/lease access precedes the exact descriptor owner match.
			if (!SameSession(*Owner, Session) || Owner->Connector.Leased != 1) return VR_STATUS_STALE;
			const uint32_t Fact = Load32(Owner->Connector.DeferredDetachAfterJoin);
			if (Fact > 1) return VR_STATUS_ABI_INCOMPATIBLE;
			const bool Control = pthread_equal(Identity.Thread, D.ControlThread) != 0;
			if (Control && Fact == 1)
			{
				const VrStatus Materialized = MaterializeGenuineJoinLocked(D, *Owner);
				if (Materialized != VR_STATUS_OK && Materialized != VR_STATUS_POISONED) return Materialized;
			}
			if (!KnownOwnerBookkeepingValid(D, *Owner)) return VR_STATUS_ABI_INCOMPATIBLE;
			const FNativeConnectorLease2& L = Owner->Connector;
			if (Control)
			{
				// Actual pre-create/no-child/join at its creating site is indispensable;
				// these immutable native facts/phase checks do not appoint a caller.
				const bool BeforeCreate = Fact == 0 && L.ServiceStartReserved == 1 && L.ServiceStarted == 0
					&& L.ServiceJoined == 0 && L.ServiceNeverCreated == 0 && Owner->Service.Registered == 0;
				const bool NoChild = Fact == 0 && L.ServiceNeverCreated == 1 && L.ServiceStartReserved == 0
					&& L.ServiceStarted == 0 && L.ServiceJoined == 0 && Owner->Service.Registered == 0;
				const bool Joined = Fact == 1 && L.ServiceJoined == 1 && L.ServiceStarted == 1 && Owner->Service.Registered == 0;
				return BeforeCreate || NoChild || Joined ? VR_STATUS_OK : VR_STATUS_WRONG_THREAD;
			}
			// Genuine join excludes ALL service identity reads immediately, including
			// the BUSY canonical-materialization interval. Never compare retired IDs.
			if (Fact != 0 || Load32(L.DeferredDetachAfterJoin) != 0) return VR_STATUS_STALE;
			if (L.ServiceStarted != 1 || L.ServiceJoined != 0 || L.ServiceStartReserved != 0
				|| L.ServiceNeverCreated != 0 || Owner->Service.Registered != 1) return VR_STATUS_WRONG_THREAD;
			if (Load32(L.DeferredDetachAfterJoin) != 0) return VR_STATUS_STALE;
			return pthread_equal(Identity.Thread, Owner->Service.Thread) != 0 ? VR_STATUS_OK : VR_STATUS_WRONG_THREAD;
		}
		static bool ResourceChildOutstanding(const VrOwner& Owner, uint32_t Parent)
		{
			const uint64_t Serial = Owner.ResourceCells[Parent].Serial;
			if (Serial == 0) return false;
			for (const FNativeResourceCell1& C : Owner.ResourceCells)
				if (C.State != ResourceEmpty1 && !ResourceTerminal(C.State) && C.ParentSerial == Serial) return true;
			return false;
		}
		static VrStatus ResourceContradiction(FPassiveDescriptor2& D, VrOwner& Owner)
		{
			PoisonLiveOwner(D, Owner, VR_POISON_CONNECTOR_ACCOUNTING);
			return VR_STATUS_POISONED;
		}
		static bool ResourceActualIdentity(const FNativeResourceCell1& C, const VrResourceActual1& A)
		{ return C.NativeValue == A.NativeValue && C.Extent == A.Extent && C.Location == A.Location; }

		static void StorePoolActual2(FNativeResourceCell1& C, const VrResourceActual1& A, uint32_t State)
		{
			C.NativeValue = A.NativeValue; C.Extent = A.Extent; C.Result = A.Result;
			C.SavedError = static_cast<uint32_t>(A.SavedError); C.State = State;
		}
		static VrStatus CommitPoolResourceEvent2(EResourceEvent1 Event, FPassiveDescriptor2& D,
			VrOwner& Owner, VrSession Session, uint32_t Index, const VrResourceActual1& A, VrResourceReply1& Out)
		{
			FNativeResourceCell1& C = Owner.ResourceCells[Index];
			if (C.State == ResourceAcquireFault2)
			{
				if (Event != EResourceEvent1::Uncertain || !PoolExactActual2(C, A)) return ResourceContradiction(D, Owner);
				FillResourceTicket(Owner, Session, Index, Out); return VR_STATUS_OK;
			}
			if (Event == EResourceEvent1::Obtained)
			{
				if (!PoolObtained2(C.Kind, C.Subkind, A)) return ResourceContradiction(D, Owner);
				if (C.State != ResourceReserved1)
				{
					// Restore the original acquisition domain, NOT the latest reference
					// disposition's Extent1. ACK never rewrites terminal facts/charges.
					if ((C.State != ResourceOwned1 && C.State != ResourceUncertain1 && C.State != ResourceClosed1)
						|| !PoolOriginalObtained2(C, A)) return ResourceContradiction(D, Owner);
					FillResourceTicket(Owner, Session, Index, Out); return VR_STATUS_OK;
				}
				StorePoolActual2(C, A, ResourceOwned1);
			}
			else if (Event == EResourceEvent1::NotObtained)
			{
				if (!PoolNotObtained2(C.Kind, C.Subkind, A)) return ResourceContradiction(D, Owner);
				if (C.State == ResourceNotObtained1)
				{
					if (!PoolExactActual2(C, A)) return ResourceContradiction(D, Owner);
					FillResourceTicket(Owner, Session, Index, Out); return VR_STATUS_OK;
				}
				if (C.State != ResourceReserved1) return ResourceContradiction(D, Owner);
				if (ResourceChildOutstanding(Owner, Index)) return VR_STATUS_BUSY;
				StorePoolActual2(C, A, ResourceNotObtained1);
			}
			else if (Event == EResourceEvent1::Uncertain && C.State == ResourceReserved1)
			{
				if (!PoolAcquireFault2(C.Kind, C.Subkind, A)) return ResourceContradiction(D, Owner);
				StorePoolActual2(C, A, ResourceAcquireFault2);
			}
			else
			{
				const bool Close = Event == EResourceEvent1::Closed;
				const uint32_t State = Close ? ResourceClosed1 : ResourceUncertain1;
				if (!PoolDisposition2(C.Kind, C.Subkind, A, Close) || C.NativeValue != A.NativeValue
					|| (PoolDomain2(C.Kind, C.Subkind) == EPoolDomain2::Allocator && C.Extent != A.Extent))
					return ResourceContradiction(D, Owner);
				if (C.State == State)
				{
					if (!PoolExactActual2(C, A)) return ResourceContradiction(D, Owner);
					FillResourceTicket(Owner, Session, Index, Out); return VR_STATUS_OK;
				}
				if (C.State != ResourceOwned1) return ResourceContradiction(D, Owner);
				if (Close && ResourceChildOutstanding(Owner, Index)) return VR_STATUS_BUSY;
				StorePoolActual2(C, A, State);
			}
			if (!PublishResourceTotals(Owner)) return ResourceContradiction(D, Owner);
			if (C.State == ResourceUncertain1 || C.State == ResourceAcquireFault2)
				PoisonLiveOwner(D, Owner, VR_POISON_CONNECTOR_ACCOUNTING);
			FillResourceTicket(Owner, Session, Index, Out); return VR_STATUS_OK;
		}

		static VrStatus ResourceEntry(EResourceEvent1 Event, VrOwner* Token, VrSession Session,
			const VrResourceSpec1* Spec, const VrResourceTicket1* Ticket, const VrResourceActual1* Actual,
			uint32_t Bytes, VrResourceReply1* Out)
		{
			const bool Reserve = Event == EResourceEvent1::Reserve;
			const void* First = Reserve ? static_cast<const void*>(Spec) : static_cast<const void*>(Ticket);
			if (!FrameResourceReply(Token, First, Reserve ? sizeof(VrResourceSpec1) : sizeof(VrResourceTicket1),
				Actual, Reserve ? 0 : sizeof(VrResourceActual1), Bytes, Out)) return VR_STATUS_INVALID_INPUT;
			if (!IsAligned(Token) || Session.owner_epoch == 0 || Session.service_epoch == 0) return VR_STATUS_INVALID_INPUT;
			uint32_t Index = 0;
			if (Reserve)
			{
				if (!IsAligned(Spec) || Spec->Cell >= 140 || Spec->Location != Spec->Cell) return VR_STATUS_INVALID_INPUT;
				if (Spec->Cell < 12)
				{
					if (Spec->Kind != ResourceKind(Spec->Cell) || Spec->Subkind != ResourceSubkind(Spec->Cell)
						|| (Spec->Cell < 10 ? Spec->ParentSerial != 0 : Spec->ParentSerial == 0)) return VR_STATUS_INVALID_INPUT;
				}
				else if (PoolDomain2(Spec->Kind, Spec->Subkind) == EPoolDomain2::Invalid
					|| (Spec->Kind == 7 ? Spec->ParentSerial != 0 : Spec->ParentSerial == 0)) return VR_STATUS_INVALID_INPUT;
				Index = Spec->Cell;
			}
			else
			{
				if (!IsAligned(Ticket) || !ResourceTicketShape(*Ticket, Session) || !IsAligned(Actual)
					|| Actual->Reserved != 0 || Actual->Location != Ticket->Cell) return VR_STATUS_INVALID_INPUT;
				Index = Ticket->Cell;
			}
			FPreparedResourceIdentity1 Identity {};
			const VrStatus Sampled = SampleResourceIdentity(*Out, Identity);
			if (Sampled != VR_STATUS_OK) return Sampled;
			FPassiveDescriptor2& D = NativeDescriptorForOwnerOperations();
			FDescriptorLock2 Lock(D);
			if (!Lock.TryAcquire()) return VR_STATUS_BUSY;
			// No getpid/pthread_self/clock/acquire/close/imported OS operation below.
			VrOwner* Owner = nullptr;
			const VrStatus Role = CheckResourceRoleLocked(D, Token, Session, Identity, Owner);
			if (Role != VR_STATUS_OK) return Role;
			if (Owner->Resources.Profile != 2 || Owner->Resources.CellCount != 140) return VR_STATUS_ABI_INCOMPATIBLE;
			FNativeResourceCell1& C = Owner->ResourceCells[Index];
			if (Reserve)
			{
				if (Owner->Phase == VR_PHASE_POISONED || Owner->Connector.Poisoned) return VR_STATUS_POISONED;
				if (Owner->Phase != VR_PHASE_ACTIVE || IsStopped(*Owner, Session.service_epoch)) return VR_STATUS_STALE;
				if (C.State != ResourceEmpty1 && !ResourceTerminal(C.State)) return VR_STATUS_OLD_OWNER_BUSY;
				if (ResourceChildOutstanding(*Owner, Index)) return VR_STATUS_OLD_OWNER_BUSY;
				if (Index >= 10 && Index < 12)
				{
					const FNativeResourceCell1& P = Owner->ResourceCells[Index - 2];
					if (P.Serial != Spec->ParentSerial || P.State == ResourceEmpty1 || ResourceTerminal(P.State)) return VR_STATUS_STALE;
				}
				if (Index >= 12)
				{
					if (!PoolParent2(*Owner, Spec->Kind, Spec->Subkind, Spec->ParentSerial)) return VR_STATUS_STALE;
					if (!PoolCapacity2(*Owner, Spec->Kind, Spec->Subkind, Spec->ParentSerial, true)) return VR_STATUS_CAPACITY;
				}
				if (Owner->Resources.NextSerial == UINT64_MAX)
				{ PoisonLiveOwner(D, *Owner, VR_POISON_GENERATION); return VR_STATUS_POISONED; }
				const uint64_t Serial = ++Owner->Resources.NextSerial;
				ZeroBytes(&C, sizeof(C)); C.Serial = Serial; C.ParentSerial = Spec->ParentSerial;
				C.Kind = Spec->Kind; C.Subkind = Spec->Subkind; C.Location = Index; C.State = ResourceReserved1;
				if (!PublishResourceTotals(*Owner)) return ResourceContradiction(D, *Owner);
				FillResourceTicket(*Owner, Session, Index, *Out); return VR_STATUS_OK;
			}
			if (C.State == ResourceEmpty1 || Ticket->Serial != C.Serial || Ticket->Kind != C.Kind || Ticket->Subkind != C.Subkind)
				return VR_STATUS_STALE;
			// Already-issued real results may commit after Stop/poison in this same lease.
			const VrResourceActual1& A = *Actual;
			if (Index >= 12) return CommitPoolResourceEvent2(Event, D, *Owner, Session, Index, A, *Out);
			// Prefix0..11 retains its exact original C1 result/replay normalization.
			const uint32_t ErrorBits = static_cast<uint32_t>(A.SavedError);
			if (Event == EResourceEvent1::Obtained)
			{
				if (!ResourceIdentityValid(Index, A.NativeValue, A.Extent)
					|| A.Result != ResourceObtainedResult(Index, A.NativeValue) || A.SavedError != 0)
					return ResourceContradiction(D, *Owner);
				if (C.State != ResourceReserved1)
				{
					// Prior acquisition replay ACKNOWLEDGES ONLY: terminal state/result/error,
					// identity, parent and charges MUST remain byte-identical.
					if ((C.State != ResourceOwned1 && C.State != ResourceClosed1 && C.State != ResourceUncertain1)
						|| !ResourceActualIdentity(C, A)) return ResourceContradiction(D, *Owner);
					FillResourceTicket(*Owner, Session, Index, *Out); return VR_STATUS_OK;
				}
				C.NativeValue = A.NativeValue; C.Extent = A.Extent; C.Result = A.Result; C.SavedError = 0; C.State = ResourceOwned1;
			}
			else if (Event == EResourceEvent1::NotObtained)
			{
				if (A.NativeValue != 0 || A.Extent != 0 || !ResourceNoResultValid(Index, A.Result, ErrorBits))
					return ResourceContradiction(D, *Owner);
				if (C.State == ResourceNotObtained1)
				{
					if (C.Result != A.Result || C.SavedError != ErrorBits) return ResourceContradiction(D, *Owner);
					FillResourceTicket(*Owner, Session, Index, *Out); return VR_STATUS_OK;
				}
				if (C.State != ResourceReserved1) return ResourceContradiction(D, *Owner);
				if (ResourceChildOutstanding(*Owner, Index)) return VR_STATUS_BUSY;
				C.Result = A.Result; C.SavedError = ErrorBits; C.State = ResourceNotObtained1;
			}
			else
			{
				const bool Close = Event == EResourceEvent1::Closed;
				if (!ResourceActualIdentity(C, A) || (Close ? (A.Result != 0 || A.SavedError != 0) : A.Result == 0))
					return ResourceContradiction(D, *Owner);
				const uint32_t Terminal = Close ? ResourceClosed1 : ResourceUncertain1;
				if (C.State == Terminal)
				{
					if (C.Result != A.Result || C.SavedError != ErrorBits) return ResourceContradiction(D, *Owner);
					FillResourceTicket(*Owner, Session, Index, *Out); return VR_STATUS_OK;
				}
				if (C.State != ResourceOwned1) return ResourceContradiction(D, *Owner);
				if (Close && ResourceChildOutstanding(*Owner, Index)) return VR_STATUS_BUSY;
				C.State = Terminal; C.Result = A.Result; C.SavedError = ErrorBits;
			}
			if (!PublishResourceTotals(*Owner)) return ResourceContradiction(D, *Owner);
			if (C.State == ResourceUncertain1) PoisonLiveOwner(D, *Owner, VR_POISON_CONNECTOR_ACCOUNTING);
			FillResourceTicket(*Owner, Session, Index, *Out); return VR_STATUS_OK;
		}
	}

	[[gnu::used, gnu::visibility("hidden")]] VrStatus NativeResourceReserve1(VrOwner* O, VrSession S, const VrResourceSpec1* P, uint32_t B, VrResourceReply1* R)
	{ return ResourceEntry(EResourceEvent1::Reserve, O, S, P, nullptr, nullptr, B, R); }
	[[gnu::used, gnu::visibility("hidden")]] VrStatus NativeResourceObtained1(VrOwner* O, VrSession S, const VrResourceTicket1* T, const VrResourceActual1* A, uint32_t B, VrResourceReply1* R)
	{ return ResourceEntry(EResourceEvent1::Obtained, O, S, nullptr, T, A, B, R); }
	[[gnu::used, gnu::visibility("hidden")]] VrStatus NativeResourceNotObtained1(VrOwner* O, VrSession S, const VrResourceTicket1* T, const VrResourceActual1* A, uint32_t B, VrResourceReply1* R)
	{ return ResourceEntry(EResourceEvent1::NotObtained, O, S, nullptr, T, A, B, R); }
	[[gnu::used, gnu::visibility("hidden")]] VrStatus NativeResourceClosed1(VrOwner* O, VrSession S, const VrResourceTicket1* T, const VrResourceActual1* A, uint32_t B, VrResourceReply1* R)
	{ return ResourceEntry(EResourceEvent1::Closed, O, S, nullptr, T, A, B, R); }
	[[gnu::used, gnu::visibility("hidden")]] VrStatus NativeResourceUncertain1(VrOwner* O, VrSession S, const VrResourceTicket1* T, const VrResourceActual1* A, uint32_t B, VrResourceReply1* R)
	{ return ResourceEntry(EResourceEvent1::Uncertain, O, S, nullptr, T, A, B, R); }

	[[gnu::used, gnu::visibility("hidden")]] VrStatus VR_CALL NativeQuery2(VrOwner* Token, VrSnapshot* Output)
	{
		if (!PrepareOutput(Output))
		{
			return VR_STATUS_INVALID_INPUT;
		}
		FPassiveDescriptor2& Descriptor = NativeDescriptorForOwnerOperations();
		FDescriptorLock2 Lock(Descriptor);
		if (!Lock.TryAcquire())
		{
			return VR_STATUS_BUSY;
		}
		VrOwner* Owner = nullptr;
		const VrStatus Status = ValidateOwner(Descriptor, Token, ENativeRole2::Control, Owner);
		if (Status != VR_STATUS_OK)
		{
			return Status;
		}
		const VrStatus Deferred = ReconcileDeferredDetach(Descriptor, *Owner);
		if (Deferred != VR_STATUS_OK && Deferred != VR_STATUS_OLD_OWNER_BUSY && Deferred != VR_STATUS_POISONED)
		{
			return Deferred;
		}
		BuildSnapshot(*Owner, *Output);
		return VR_STATUS_OK;
	}

	[[gnu::used, gnu::visibility("hidden")]] VrStatus VR_CALL NativeAttachService2(VrOwner* Token, VrSession* Output)
	{
		if (!IsAligned(Output))
		{
			return VR_STATUS_INVALID_INPUT;
		}
		ZeroBytes(Output, sizeof(*Output));
		FPassiveDescriptor2& Descriptor = NativeDescriptorForOwnerOperations();
		FDescriptorLock2 Lock(Descriptor);
		if (!Lock.TryAcquire())
		{
			return VR_STATUS_BUSY;
		}
		VrOwner* Owner = nullptr;
		VrStatus Status = ValidateOwner(Descriptor, Token, ENativeRole2::Control, Owner);
		if (Status != VR_STATUS_OK)
		{
			return Status;
		}
		if (Owner->Phase == VR_PHASE_POISONED || Owner->Connector.Poisoned != 0)
		{
			return VR_STATUS_POISONED;
		}
		if (Owner->Phase == VR_PHASE_RETIRING)
		{
			return VR_STATUS_OLD_OWNER_BUSY;
		}
		Status = ReconcileDeferredDetach(Descriptor, *Owner);
		if (Status != VR_STATUS_OK)
		{
			return Status;
		}
		if (Owner->Connector.Leased != 0)
		{
			return VR_STATUS_OLD_OWNER_BUSY;
		}
		if (Owner->NextServiceEpoch == UINT64_MAX)
		{
			PoisonLiveOwner(Descriptor, *Owner, VR_POISON_GENERATION);
			return VR_STATUS_POISONED;
		}
		const uint64_t ServiceEpoch = Owner->NextServiceEpoch + 1;
		if (!ResourceCountsZero(*Owner)) return VR_STATUS_OLD_OWNER_BUSY;
		InitializeResourceProfileOnAttach(*Owner);
		Owner->NextServiceEpoch = ServiceEpoch;
		// Slots and StopEpoch retain their existing responsibility and epoch history.
		Owner->Connector.ServiceEpoch = ServiceEpoch;
		Owner->Connector.Leased = 1;
		Owner->Connector.ServiceStartReserved = 1;
		Owner->Connector.ServiceNeverCreated = 0;
		SetPhase(Descriptor, *Owner, VR_PHASE_ACTIVE);
		PublishLeaseMirror(Descriptor, *Owner);
		Output->owner_epoch = Owner->OwnerEpoch;
		Output->service_epoch = ServiceEpoch;
		return VR_STATUS_OK;
	}

	[[gnu::used, gnu::visibility("hidden")]] VrStatus VR_CALL NativeDetachService2(VrOwner* Token, VrSession Session)
	{
		FPassiveDescriptor2& Descriptor = NativeDescriptorForOwnerOperations();
		FDescriptorLock2 Lock(Descriptor);
		if (!Lock.TryAcquire())
		{
			return VR_STATUS_BUSY;
		}
		VrOwner* Owner = nullptr;
		const VrStatus Status = ValidateOwner(Descriptor, Token, ENativeRole2::Control, Owner);
		if (Status != VR_STATUS_OK)
		{
			return Status;
		}
		if (!SameSession(*Owner, Session))
		{
			return VR_STATUS_STALE;
		}
		if (Owner->Phase == VR_PHASE_POISONED || Owner->Connector.Poisoned != 0)
		{
			return VR_STATUS_POISONED;
		}
		if (Owner->Phase == VR_PHASE_RETIRING || !CanFinishDetach(*Owner))
		{
			return VR_STATUS_OLD_OWNER_BUSY;
		}
		FinishDetach(Descriptor, *Owner);
		return VR_STATUS_OK;
	}

	[[gnu::used, gnu::visibility("hidden")]] VrStatus VR_CALL NativeCopyResult2(VrOwner* Token, VrTicket Ticket, VrResult* Output)
	{
		if (!PrepareOutput(Output) || !TicketShapeValid(Ticket))
		{
			return VR_STATUS_INVALID_INPUT;
		}
		FPassiveDescriptor2& Descriptor = NativeDescriptorForOwnerOperations();
		FDescriptorLock2 Lock(Descriptor);
		if (!Lock.TryAcquire())
		{
			return VR_STATUS_BUSY;
		}
		VrOwner* Owner = nullptr;
		const VrStatus Status = ValidateOwner(Descriptor, Token, ENativeRole2::Service, Owner);
		if (Status != VR_STATUS_OK)
		{
			return Status;
		}
		FNativeSlot2* Slot = InterestedSlot(*Owner, Ticket);
		if (Slot == nullptr)
		{
			return VR_STATUS_STALE;
		}
		if (Owner->Phase == VR_PHASE_POISONED || Owner->Connector.Poisoned != 0)
		{
			return VR_STATUS_POISONED;
		}
		if (Owner->Phase == VR_PHASE_RETIRING)
		{
			return VR_STATUS_OLD_OWNER_BUSY;
		}
		uint64_t Now = 0;
		if (!IsStopped(*Owner, Ticket.service_epoch) && !MonotonicNowNs(Now))
		{
			PoisonLiveOwner(Descriptor, *Owner, VR_POISON_CLOCK);
			return VR_STATUS_POISONED;
		}
		const uint32_t Joined = Load32(Owner->Connector.DeferredDetachAfterJoin);
		if (Joined != 0)
		{
			return Joined == 1 ? VR_STATUS_STALE : VR_STATUS_ABI_INCOMPATIBLE;
		}
		const bool Stopped = IsStopped(*Owner, Ticket.service_epoch);
		if (Stopped || Now >= Slot->DeadlineNs)
		{
			SetInterest(*Slot, 0);
			if (RetiredAndClean(*Slot))
			{
				ReleaseRetiredSlot(*Slot);
			}
			return Stopped ? VR_STATUS_STALE : VR_STATUS_EXPIRED;
		}
		if (Slot->State == NativeSlotPoisoned || Slot->RetirementUncertain != 0)
		{
			PoisonLiveOwner(Descriptor, *Owner, VR_POISON_THREAD_RETIRE);
			return VR_STATUS_POISONED;
		}
		if (!RetiredAndClean(*Slot))
		{
			return VR_STATUS_PENDING;
		}
		if (Load32(Slot->ResultPublished) != 1 || !CompletedResultValid(*Slot, Ticket))
		{
			PoisonLiveOwner(Descriptor, *Owner, VR_POISON_NATIVE_INVARIANT);
			return VR_STATUS_POISONED;
		}
		const VrStatus ResultStatus = static_cast<VrStatus>(Slot->ResultStatus);
		CopyBytes(&Slot->Result, Output, sizeof(*Output));
		ReleaseRetiredSlot(*Slot);
		return ResultStatus;
	}

	[[gnu::used, gnu::visibility("hidden")]] VrStatus VR_CALL NativeDiscard2(VrOwner* Token, VrTicket Ticket)
	{
		if (!TicketShapeValid(Ticket))
		{
			return VR_STATUS_INVALID_INPUT;
		}
		FPassiveDescriptor2& Descriptor = NativeDescriptorForOwnerOperations();
		FDescriptorLock2 Lock(Descriptor);
		if (!Lock.TryAcquire())
		{
			return VR_STATUS_BUSY;
		}
		VrOwner* Owner = nullptr;
		const VrStatus Status = ValidateOwner(Descriptor, Token, ENativeRole2::Either, Owner);
		if (Status != VR_STATUS_OK)
		{
			return Status;
		}
		FNativeSlot2* Slot = InterestedSlot(*Owner, Ticket);
		if (Slot == nullptr)
		{
			return VR_STATUS_STALE;
		}
		// Interest can be invalidated in poison, but unretired/uncertain work stays charged.
		SetInterest(*Slot, 0);
		if (RetiredAndClean(*Slot))
		{
			ReleaseRetiredSlot(*Slot);
		}
		return VR_STATUS_OK;
	}

	namespace
	{
		// One complete composed object, never dereferenced by the passive getter.
		// Its actual216-byte relocation/RO/initializer/writer closure is a fresh
		// artifact/loader gate; constinit alone proves no runtime protection.
		[[gnu::used]] constinit const VrPrivateOwnerApiObject1 OwnerApiObject1 {
			{
				sizeof(VrOwnerApi2), VR_OWNER_VERSION,
				&NativeQuery2, &NativePollRetirement2, &NativeAttachService2, &NativeDetachService2,
				&NativeSubmit2, &NativeCopyResult2, &NativeDiscard2, &NativeRetire2
			},
			{
				{ VR_PRIVATE_OWNER_MAGIC_0, VR_PRIVATE_OWNER_MAGIC_1, VR_PRIVATE_OWNER_MAGIC_2, VR_PRIVATE_OWNER_MAGIC_3,
				  VR_PRIVATE_OWNER_MAGIC_4, VR_PRIVATE_OWNER_MAGIC_5, VR_PRIVATE_OWNER_MAGIC_6, VR_PRIVATE_OWNER_MAGIC_7 },
				VR_PRIVATE_OWNER_TAIL_BYTES, VR_PRIVATE_OWNER_VERSION, VR_PRIVATE_OWNER_SIGNATURE_ABI,
				VR_PRIVATE_OWNER_COUNT, VR_PRIVATE_OWNER_MASK, VR_PRIVATE_RESOURCE_PROFILE, VR_PRIVATE_RESOURCE_CELLS,
				&VrPrivateAdmitCurrentControl1,
				&VrPrivateServiceNeverCreated1,
				&VrPrivateRegisterCurrentServiceThread1,
				&VrPrivateServiceJoinObserved1,
				&VrPrivateRequestStop1,
				&VrPrivatePumpRegistered1,
				&VrPrivatePumpEnter1,
				&VrPrivatePumpLeave1,
				&VrPrivateRequestDetachAfterJoin1,
				&VrPrivateResourceReserve1,
				&VrPrivateResourceObtained1,
				&VrPrivateResourceNotObtained1,
				&VrPrivateResourceClosed1,
				&VrPrivateResourceUncertain1
			}
		};
		static_assert(std::is_same_v<decltype(&VrPrivateAdmitCurrentControl1), decltype(VrPrivateOwnerTail1::VrPrivateAdmitCurrentControl1)>);
		static_assert(std::is_same_v<decltype(&VrPrivateServiceNeverCreated1), decltype(VrPrivateOwnerTail1::VrPrivateServiceNeverCreated1)>);
		static_assert(std::is_same_v<decltype(&VrPrivateRegisterCurrentServiceThread1), decltype(VrPrivateOwnerTail1::VrPrivateRegisterCurrentServiceThread1)>);
		static_assert(std::is_same_v<decltype(&VrPrivateServiceJoinObserved1), decltype(VrPrivateOwnerTail1::VrPrivateServiceJoinObserved1)>);
		static_assert(std::is_same_v<decltype(&VrPrivateRequestStop1), decltype(VrPrivateOwnerTail1::VrPrivateRequestStop1)>);
		static_assert(std::is_same_v<decltype(&VrPrivatePumpRegistered1), decltype(VrPrivateOwnerTail1::VrPrivatePumpRegistered1)>);
		static_assert(std::is_same_v<decltype(&VrPrivatePumpEnter1), decltype(VrPrivateOwnerTail1::VrPrivatePumpEnter1)>);
		static_assert(std::is_same_v<decltype(&VrPrivatePumpLeave1), decltype(VrPrivateOwnerTail1::VrPrivatePumpLeave1)>);
		static_assert(std::is_same_v<decltype(&VrPrivateRequestDetachAfterJoin1), decltype(VrPrivateOwnerTail1::VrPrivateRequestDetachAfterJoin1)>);
		static_assert(std::is_same_v<decltype(&VrPrivateResourceReserve1), decltype(VrPrivateOwnerTail1::VrPrivateResourceReserve1)>);
		static_assert(std::is_same_v<decltype(&VrPrivateResourceObtained1), decltype(VrPrivateOwnerTail1::VrPrivateResourceObtained1)>);
		static_assert(std::is_same_v<decltype(&VrPrivateResourceNotObtained1), decltype(VrPrivateOwnerTail1::VrPrivateResourceNotObtained1)>);
		static_assert(std::is_same_v<decltype(&VrPrivateResourceClosed1), decltype(VrPrivateOwnerTail1::VrPrivateResourceClosed1)>);
		static_assert(std::is_same_v<decltype(&VrPrivateResourceUncertain1), decltype(VrPrivateOwnerTail1::VrPrivateResourceUncertain1)>);


		static VrStatus ValidateOutcomeControl(FPassiveDescriptor2& Descriptor, VrOwner* Token, VrOwner*& Owner)
		{
			Owner = nullptr;
			VrOwner* Matched = nullptr;
			const VrStatus Match = MatchCurrentOwnerForRoleCheck(Descriptor, Token, Matched);
			if (Match != VR_STATUS_OK)
			{
				return Match;
			}
			bool IsControl = false;
			const VrStatus Role = Detail::ValidateRole(Descriptor, *Matched, ENativeRole2::Control, IsControl);
			if (Role != VR_STATUS_OK)
			{
				return Role;
			}
			const VrStatus Joined = MaterializeGenuineJoinLocked(Descriptor, *Matched);
			if (Joined != VR_STATUS_OK && Joined != VR_STATUS_POISONED)
			{
				return Joined;
			}
			if (!KnownOwnerBookkeepingValid(Descriptor, *Matched))
			{
				PoisonLiveOwner(Descriptor, *Matched, VR_POISON_NATIVE_INVARIANT);
				return VR_STATUS_POISONED;
			}
			Owner = Matched;
			return VR_STATUS_OK;
		}
	}

	[[gnu::used, gnu::visibility("hidden")]] const VrOwnerApi2* NativeApiForOwnerConstruction()
	{
		return &OwnerApiObject1.Public;
	}

	[[gnu::used, gnu::visibility("hidden")]] VrStatus NativeServiceNeverCreated(VrOwner* Token, VrSession Session)
	{
		FPassiveDescriptor2& Descriptor = NativeDescriptorForOwnerOperations();
		FDescriptorLock2 Lock(Descriptor);
		if (!Lock.TryAcquire())
		{
			return VR_STATUS_BUSY;
		}
		VrOwner* Owner = nullptr;
		const VrStatus Status = ValidateOutcomeControl(Descriptor, Token, Owner);
		if (Status != VR_STATUS_OK)
		{
			return Status;
		}
		if (!SameSession(*Owner, Session))
		{
			return VR_STATUS_STALE;
		}
		const uint32_t Fact = Load32(Owner->Connector.DeferredDetachAfterJoin);
		if (Fact != 0)
		{
			if (Fact == 1)
			{
				PoisonLiveOwner(Descriptor, *Owner, VR_POISON_NATIVE_INVARIANT);
				return VR_STATUS_POISONED;
			}
			return VR_STATUS_ABI_INCOMPATIBLE;
		}
		FNativeConnectorLease2& Lease = Owner->Connector;
		if (Lease.ServiceStartReserved != 1 || Lease.ServiceStarted != 0 || Lease.ServiceJoined != 0
			|| Lease.ServiceNeverCreated != 0 || Owner->Service.Registered != 0)
		{
			return VR_STATUS_STALE;
		}
		// External creating code must already have established the actual no-create outcome.
		Lease.ServiceStartReserved = 0;
		Lease.ServiceNeverCreated = 1;
		return VR_STATUS_OK;
	}

	[[gnu::used, gnu::visibility("hidden")]] VrStatus NativeRegisterCurrentServiceThread(VrOwner* Token, VrSession Session)
	{
		FPassiveDescriptor2& Descriptor = NativeDescriptorForOwnerOperations();
		FDescriptorLock2 Lock(Descriptor);
		if (!Lock.TryAcquire())
		{
			return VR_STATUS_BUSY;
		}
		VrOwner* Owner = nullptr;
		const VrStatus Status = MatchCurrentOwnerForRoleCheck(Descriptor, Token, Owner);
		if (Status != VR_STATUS_OK)
		{
			return Status;
		}
		const uint32_t Fact = Load32(Owner->Connector.DeferredDetachAfterJoin);
		if (Fact != 0)
		{
			return Fact == 1 ? VR_STATUS_STALE : VR_STATUS_ABI_INCOMPATIBLE;
		}
		// This unique zero-caller function is for the actual future native service entry.
		// It does not accept an identity/role bypass from any public owner operation.
		if (!KnownOwnerBookkeepingValid(Descriptor, *Owner))
		{
			PoisonLiveOwner(Descriptor, *Owner, VR_POISON_NATIVE_INVARIANT);
			return VR_STATUS_POISONED;
		}
		if (!SameSession(*Owner, Session))
		{
			return VR_STATUS_STALE;
		}
		if (Owner->Phase == VR_PHASE_POISONED || Owner->Connector.Poisoned != 0)
		{
			return VR_STATUS_POISONED;
		}
		if (Owner->Phase != VR_PHASE_ACTIVE || IsStopped(*Owner, Session.service_epoch))
		{
			return VR_STATUS_STALE;
		}
		FNativeConnectorLease2& Lease = Owner->Connector;
		if (Lease.ServiceStartReserved != 1 || Lease.ServiceStarted != 0 || Lease.ServiceJoined != 0
			|| Lease.ServiceNeverCreated != 0 || Owner->Service.Registered != 0)
		{
			return VR_STATUS_STALE;
		}
		const pthread_t Current = pthread_self();
		Owner->Service.Thread = Current;
		Owner->Service.Registered = 1;
		Lease.ServiceStarted = 1;
		Lease.ServiceJoined = 0;
		Lease.ServiceStartReserved = 0;
		Lease.ServiceNeverCreated = 0;
		return VR_STATUS_OK;
	}

	[[gnu::used, gnu::visibility("hidden")]] VrStatus NativeServiceJoinObserved(VrOwner* Token, VrSession Session)
	{
		FPassiveDescriptor2& Descriptor = NativeDescriptorForOwnerOperations();
		const VrStatus Published = PublishGenuineJoinFact(Descriptor, Token, Session);
		if (Published != VR_STATUS_OK)
		{
			return Published;
		}
		FDescriptorLock2 Lock(Descriptor);
		if (!Lock.TryAcquire())
		{
			// Real join is durable; BUSY never instructs a second actual join.
			return VR_STATUS_BUSY;
		}
		VrOwner* Owner = nullptr;
		const VrStatus Status = ValidateOutcomeControl(Descriptor, Token, Owner);
		if (Status != VR_STATUS_OK)
		{
			return Status;
		}
		if (!SameSession(*Owner, Session))
		{
			return VR_STATUS_STALE;
		}
		if (Load32(Owner->Connector.DeferredDetachAfterJoin) != 1)
		{
			return VR_STATUS_ABI_INCOMPATIBLE;
		}
		return MaterializeGenuineJoinLocked(Descriptor, *Owner);
	}


	[[gnu::used, gnu::visibility("hidden")]] void NativeRequestStop(VrOwner* Token, VrSession Session)
	{
		FPassiveDescriptor2& Descriptor = NativeDescriptorForOwnerOperations();
		VrOwner* Owner = nullptr;
		if (ValidateNoLockLeaseIngress(Descriptor, Token, Session, Owner) != VR_STATUS_OK)
		{
			return;
		}
		if (Load32(Owner->Connector.DeferredDetachAfterJoin) > 1)
		{
			return;
		}
		__atomic_store_n(&Owner->StopEpoch, Session.service_epoch, __ATOMIC_RELEASE);
	}

	[[gnu::used, gnu::visibility("hidden")]] VrStatus NativePumpRegistered(VrOwner* Token, VrSession Session, uint32_t Registered)
	{
		if (Registered > 1)
		{
			return VR_STATUS_INVALID_INPUT;
		}
		FPassiveDescriptor2& Descriptor = NativeDescriptorForOwnerOperations();
		VrOwner* Owner = nullptr;
		const VrStatus Status = ValidateNoLockLeaseIngress(Descriptor, Token, Session, Owner);
		if (Status != VR_STATUS_OK)
		{
			return Status;
		}
		const uint32_t Existing = Load32(Owner->Connector.PumpRegistered);
		const uint32_t Executing = Load32(Owner->Connector.PumpExecuting);
		const uint32_t Joined = Load32(Owner->Connector.DeferredDetachAfterJoin);
		if (!IsBoolean(Existing) || !IsBoolean(Executing) || !IsBoolean(Joined))
		{
			return VR_STATUS_ABI_INCOMPATIBLE;
		}
		if (Registered == 0)
		{
			// Actual disable/removal happened; an entered callback still owes Leave.
			__atomic_store_n(&Owner->Connector.PumpRegistered, 0u, __ATOMIC_RELEASE);
			return VR_STATUS_OK;
		}
		const uint32_t Poisoned = __atomic_load_n(&Descriptor.LeaseIngress.Poisoned, __ATOMIC_ACQUIRE);
		if (!IsBoolean(Poisoned))
		{
			return VR_STATUS_ABI_INCOMPATIBLE;
		}
		if (Poisoned != 0)
		{
			return VR_STATUS_POISONED;
		}
		if (Joined != 0 || IsStopped(*Owner, Session.service_epoch))
		{
			return VR_STATUS_STALE;
		}
		if (Executing != 0)
		{
			return VR_STATUS_BUSY;
		}
		// Actual one successful AddSP boundary is a premise; duplicate means the
		// SAME registration, never permission for a second real delegate.
		uint32_t Expected = 0;
		if (__atomic_compare_exchange_n(&Owner->Connector.PumpRegistered, &Expected, 1u,
			false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		{
			return VR_STATUS_OK;
		}
		return Expected == 1 ? VR_STATUS_OK : VR_STATUS_ABI_INCOMPATIBLE;
	}

	[[gnu::used, gnu::visibility("hidden")]] VrStatus NativePumpEnter(VrOwner* Token, VrSession Session)
	{
		FPassiveDescriptor2& Descriptor = NativeDescriptorForOwnerOperations();
		VrOwner* Owner = nullptr;
		const VrStatus Status = ValidateNoLockLeaseIngress(Descriptor, Token, Session, Owner);
		if (Status != VR_STATUS_OK)
		{
			return Status;
		}
		if (Load32(Owner->Connector.PumpExecuting) > 1)
		{
			return VR_STATUS_ABI_INCOMPATIBLE;
		}
		const VrStatus Permit = PumpPermit(Descriptor, *Owner, Session);
		if (Permit != VR_STATUS_OK)
		{
			return Permit;
		}
		uint32_t Expected = 0;
		if (!__atomic_compare_exchange_n(&Owner->Connector.PumpExecuting, &Expected, 1u,
			false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		{
			return Expected == 1 ? VR_STATUS_BUSY : VR_STATUS_ABI_INCOMPATIBLE;
		}
		const VrStatus Rechecked = PumpPermit(Descriptor, *Owner, Session);
		if (Rechecked != VR_STATUS_OK)
		{
			// Only this invocation's newly acquired charge; no arena access afterward.
			__atomic_store_n(&Owner->Connector.PumpExecuting, 0u, __ATOMIC_RELEASE);
			return Rechecked;
		}
		return VR_STATUS_OK;
	}

	[[gnu::used, gnu::visibility("hidden")]] void NativePumpLeave(VrOwner* Token, VrSession Session)
	{
		// ONLY the SAME verified pinned GT invocation after successful Enter.
		// No fork/control replacement; its strong dispatch reference spans this pair.
		if (!IsAligned(Token) || Session.owner_epoch == 0 || Session.service_epoch == 0)
		{
			return;
		}
		FPassiveDescriptor2& Descriptor = NativeDescriptorForOwnerOperations();
		const FNativeLeaseIngress3& Mirror = Descriptor.LeaseIngress;
		const uint64_t First = __atomic_load_n(&Mirror.ServiceEpoch, __ATOMIC_ACQUIRE);
		VrOwner* const Owner = __atomic_load_n(&Mirror.Owner, __ATOMIC_ACQUIRE);
		const uint64_t Epoch = __atomic_load_n(&Mirror.OwnerEpoch, __ATOMIC_ACQUIRE);
		const uint32_t Poisoned = __atomic_load_n(&Mirror.Poisoned, __ATOMIC_ACQUIRE);
		const uint32_t Reserved = __atomic_load_n(&Mirror.Reserved, __ATOMIC_ACQUIRE);
		const uint64_t Last = __atomic_load_n(&Mirror.ServiceEpoch, __ATOMIC_ACQUIRE);
		if (First == 0 || First != Last || Owner != Token || Epoch != Session.owner_epoch
			|| First != Session.service_epoch || Poisoned > 1 || Reserved != 0)
		{
			return;
		}
		// Stop, poison, removal and actual join never reject a valid balanced Leave.
		// This is the FINAL arena access. A duplicate/malformed invocation mutates nothing.
		uint32_t Expected = 1;
		__atomic_compare_exchange_n(&Owner->Connector.PumpExecuting, &Expected, 0u,
			false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
	}

	[[gnu::used, gnu::visibility("hidden")]] void NativeRequestDetachAfterJoin(VrOwner* Token, VrSession Session)
	{
		FPassiveDescriptor2& Descriptor = NativeDescriptorForOwnerOperations();
		(void)PublishGenuineJoinFact(Descriptor, Token, Session);
	}

}

#endif // VR_NATIVE_OWNER_GNU_X64
