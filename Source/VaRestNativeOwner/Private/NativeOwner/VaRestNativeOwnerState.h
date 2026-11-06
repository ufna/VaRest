// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestNativeOwnerInternal.h"
#include "VaRestNativeOwnerPrivateResource1.h"

#if VR_NATIVE_OWNER_GNU_X64

namespace VaRestNativeOwnerPrivate
{
	enum : uint32_t
	{
		NativeSlotFree = 0,
		NativeSlotReserved = 1,
		NativeSlotRunning = 2,
		NativeSlotReturnedUnjoined = 3,
		NativeSlotRetiredResult = 4,
		NativeSlotCleanup = 5,
		NativeSlotPoisoned = 6
	};

	// Private creating-producer records; never cross the public ABI.
	enum : uint32_t { ResourceEmpty1 = 0, ResourceReserved1 = 1, ResourceOwned1 = 2,
		ResourceUncertain1 = 3, ResourceClosed1 = 4, ResourceNotObtained1 = 5, ResourceAcquireFault2 = 6 };
	struct FNativeResourceCell1
	{
		uint64_t Serial, ParentSerial, NativeValue, Extent;
		uint32_t Kind, Subkind, State, SavedError, Location;
		int32_t Result;
		uint64_t ReservedTail;
	};
	struct FNativeResourceHeader1
	{
		uint64_t NextSerial;
		uint32_t Profile, CellCount, ReservedCells, OwnedCells, UncertainCells, RetiredCells;
		uint64_t MappingBytes, Reserved[3];
	};
	static_assert(sizeof(FNativeResourceCell1) == 64 && alignof(FNativeResourceCell1) == 8);
	static_assert(sizeof(FNativeResourceHeader1) == 64 && alignof(FNativeResourceHeader1) == 8);
#define VR_RESOURCE_OFFSET(T, F, N) static_assert(offsetof(T, F) == N)
	VR_RESOURCE_OFFSET(FNativeResourceCell1, Serial, 0); VR_RESOURCE_OFFSET(FNativeResourceCell1, ParentSerial, 8);
	VR_RESOURCE_OFFSET(FNativeResourceCell1, NativeValue, 16); VR_RESOURCE_OFFSET(FNativeResourceCell1, Extent, 24);
	VR_RESOURCE_OFFSET(FNativeResourceCell1, Kind, 32); VR_RESOURCE_OFFSET(FNativeResourceCell1, Subkind, 36);
	VR_RESOURCE_OFFSET(FNativeResourceCell1, State, 40); VR_RESOURCE_OFFSET(FNativeResourceCell1, SavedError, 44);
	VR_RESOURCE_OFFSET(FNativeResourceCell1, Location, 48); VR_RESOURCE_OFFSET(FNativeResourceCell1, Result, 52);
	VR_RESOURCE_OFFSET(FNativeResourceCell1, ReservedTail, 56);
	VR_RESOURCE_OFFSET(FNativeResourceHeader1, NextSerial, 0); VR_RESOURCE_OFFSET(FNativeResourceHeader1, Profile, 8);
	VR_RESOURCE_OFFSET(FNativeResourceHeader1, CellCount, 12); VR_RESOURCE_OFFSET(FNativeResourceHeader1, ReservedCells, 16);
	VR_RESOURCE_OFFSET(FNativeResourceHeader1, OwnedCells, 20); VR_RESOURCE_OFFSET(FNativeResourceHeader1, UncertainCells, 24);
	VR_RESOURCE_OFFSET(FNativeResourceHeader1, RetiredCells, 28); VR_RESOURCE_OFFSET(FNativeResourceHeader1, MappingBytes, 32);
	VR_RESOURCE_OFFSET(FNativeResourceHeader1, Reserved, 40);
	static_assert(std::is_trivial_v<FNativeResourceCell1> && std::is_standard_layout_v<FNativeResourceCell1>);
	static_assert(std::is_trivially_copyable_v<FNativeResourceCell1> && std::is_trivially_destructible_v<FNativeResourceCell1>);
	static_assert(std::is_trivial_v<FNativeResourceHeader1> && std::is_standard_layout_v<FNativeResourceHeader1>);
	static_assert(std::is_trivially_copyable_v<FNativeResourceHeader1> && std::is_trivially_destructible_v<FNativeResourceHeader1>);
	static_assert(sizeof(VrResourceTicket1) == 40 && alignof(VrResourceTicket1) == 8);
	static_assert(sizeof(VrResourceSpec1) == 24 && alignof(VrResourceSpec1) == 8);
	static_assert(sizeof(VrResourceActual1) == 32 && alignof(VrResourceActual1) == 8);
	static_assert(sizeof(VrResourceReply1) == 64 && alignof(VrResourceReply1) == 8);
#define VR_RESOURCE_POD(T) static_assert(std::is_trivial_v<T> && std::is_standard_layout_v<T> \
		&& std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>)
	VR_RESOURCE_POD(VrResourceTicket1); VR_RESOURCE_POD(VrResourceSpec1);
	VR_RESOURCE_POD(VrResourceActual1); VR_RESOURCE_POD(VrResourceReply1);
#undef VR_RESOURCE_POD
	VR_RESOURCE_OFFSET(VrResourceTicket1, Size, 0); VR_RESOURCE_OFFSET(VrResourceTicket1, Version, 4);
	VR_RESOURCE_OFFSET(VrResourceTicket1, OwnerEpoch, 8); VR_RESOURCE_OFFSET(VrResourceTicket1, ServiceEpoch, 16);
	VR_RESOURCE_OFFSET(VrResourceTicket1, Serial, 24); VR_RESOURCE_OFFSET(VrResourceTicket1, Cell, 32);
	VR_RESOURCE_OFFSET(VrResourceTicket1, Kind, 36); VR_RESOURCE_OFFSET(VrResourceTicket1, Subkind, 38);
	VR_RESOURCE_OFFSET(VrResourceSpec1, Kind, 0); VR_RESOURCE_OFFSET(VrResourceSpec1, Subkind, 4);
	VR_RESOURCE_OFFSET(VrResourceSpec1, Cell, 8); VR_RESOURCE_OFFSET(VrResourceSpec1, Location, 12);
	VR_RESOURCE_OFFSET(VrResourceSpec1, ParentSerial, 16);
	VR_RESOURCE_OFFSET(VrResourceActual1, NativeValue, 0); VR_RESOURCE_OFFSET(VrResourceActual1, Extent, 8);
	VR_RESOURCE_OFFSET(VrResourceActual1, Result, 16); VR_RESOURCE_OFFSET(VrResourceActual1, SavedError, 20);
	VR_RESOURCE_OFFSET(VrResourceActual1, Location, 24); VR_RESOURCE_OFFSET(VrResourceActual1, Reserved, 28);
	VR_RESOURCE_OFFSET(VrResourceReply1, Size, 0); VR_RESOURCE_OFFSET(VrResourceReply1, Version, 4);
	VR_RESOURCE_OFFSET(VrResourceReply1, NativeCalls, 8); VR_RESOURCE_OFFSET(VrResourceReply1, Reserved, 12);
	VR_RESOURCE_OFFSET(VrResourceReply1, Ticket, 16); VR_RESOURCE_OFFSET(VrResourceReply1, ReservedTail, 56);
#undef VR_RESOURCE_OFFSET
	struct FNativeSlot2
	{
		uint64_t Generation;
		uint64_t ServiceEpoch;
		uint64_t DeadlineNs;
		uint32_t State;
		uint32_t HostnameBytes;
		char Hostname[256];
		VrResult Result;
		uint32_t ResultStatus;
		uint32_t ResultPublished;
		uint32_t Interest;
		uint32_t ThreadCreated;
		uint32_t ThreadJoined;
		uint32_t CloseDone;
		uint32_t RetirementUncertain;
		uint32_t WsaCredit;
		pthread_t Thread;
		int32_t NativeError;
		uint32_t Reserved;
		uint16_t PortBe;
		uint16_t InputReserved16;
		uint32_t InputReserved32;
	};

	struct FNativeServiceIdentity2
	{
		pthread_t Thread;
		uint32_t Registered;
		uint32_t Reserved;
	};

	struct FNativeConnectorLease2
	{
		uint64_t ServiceEpoch;
		uint32_t Leased;
		uint32_t ServiceStarted;
		uint32_t ServiceJoined;
		uint32_t Poisoned;
		uint32_t ServiceStartReserved;
		uint32_t ServiceNeverCreated;
		uint32_t ServiceCallExecuting;
		uint32_t ListenerLive;
		uint32_t IngressLive;
		uint32_t PairsLive;
		uint32_t BuffersLive;
		uint32_t WsiLive;
		uint32_t VhostsLive;
		uint32_t ContextLive;
		uint32_t DependencyCredits;
		uint32_t WakeDescriptorsLive;
		uint32_t Reserved;
		uint32_t PumpRegistered;
		uint32_t PumpExecuting;
		uint32_t DeferredDetachAfterJoin;
	};

	static_assert(sizeof(FNativeSlot2) == 712 && alignof(FNativeSlot2) == 8);
	static_assert(offsetof(FNativeSlot2, Generation) == 0);
	static_assert(offsetof(FNativeSlot2, ServiceEpoch) == 8);
	static_assert(offsetof(FNativeSlot2, DeadlineNs) == 16);
	static_assert(offsetof(FNativeSlot2, State) == 24);
	static_assert(offsetof(FNativeSlot2, HostnameBytes) == 28);
	static_assert(offsetof(FNativeSlot2, Hostname) == 32);
	static_assert(offsetof(FNativeSlot2, ResultStatus) == 656);
	static_assert(offsetof(FNativeSlot2, ThreadCreated) == 668);
	static_assert(offsetof(FNativeSlot2, ThreadJoined) == 672);
	static_assert(offsetof(FNativeSlot2, CloseDone) == 676);
	static_assert(offsetof(FNativeSlot2, RetirementUncertain) == 680);
	static_assert(offsetof(FNativeSlot2, WsaCredit) == 684);
	static_assert(offsetof(FNativeSlot2, Result) == 288);
	static_assert(offsetof(FNativeSlot2, ResultPublished) == 660);
	static_assert(offsetof(FNativeSlot2, Interest) == 664);
	static_assert(offsetof(FNativeSlot2, Thread) == 688);
	static_assert(offsetof(FNativeSlot2, NativeError) == 696);
	static_assert(offsetof(FNativeSlot2, Reserved) == 700);
	static_assert(offsetof(FNativeSlot2, PortBe) == 704);
	static_assert(offsetof(FNativeSlot2, InputReserved16) == 706);
	static_assert(offsetof(FNativeSlot2, InputReserved32) == 708);
	static_assert(sizeof(FNativeServiceIdentity2) == 16 && alignof(FNativeServiceIdentity2) == 8);
	static_assert(sizeof(FNativeConnectorLease2) == 88 && alignof(FNativeConnectorLease2) == 8);
	static_assert(offsetof(FNativeConnectorLease2, PumpRegistered) == 76);
	static_assert(offsetof(FNativeConnectorLease2, PumpExecuting) == 80);
	static_assert(offsetof(FNativeConnectorLease2, DeferredDetachAfterJoin) == 84);
	static_assert(offsetof(FNativeServiceIdentity2, Thread) == 0);
	static_assert(offsetof(FNativeServiceIdentity2, Registered) == 8);
	static_assert(offsetof(FNativeServiceIdentity2, Reserved) == 12);
	static_assert(offsetof(FNativeConnectorLease2, ServiceEpoch) == 0);
	static_assert(offsetof(FNativeConnectorLease2, Leased) == 8);
	static_assert(offsetof(FNativeConnectorLease2, ServiceStarted) == 12);
	static_assert(offsetof(FNativeConnectorLease2, ServiceJoined) == 16);
	static_assert(offsetof(FNativeConnectorLease2, Poisoned) == 20);
	static_assert(offsetof(FNativeConnectorLease2, ServiceStartReserved) == 24);
	static_assert(offsetof(FNativeConnectorLease2, ServiceNeverCreated) == 28);
	static_assert(offsetof(FNativeConnectorLease2, ServiceCallExecuting) == 32);
	static_assert(offsetof(FNativeConnectorLease2, ListenerLive) == 36);
	static_assert(offsetof(FNativeConnectorLease2, IngressLive) == 40);
	static_assert(offsetof(FNativeConnectorLease2, PairsLive) == 44);
	static_assert(offsetof(FNativeConnectorLease2, BuffersLive) == 48);
	static_assert(offsetof(FNativeConnectorLease2, WsiLive) == 52);
	static_assert(offsetof(FNativeConnectorLease2, VhostsLive) == 56);
	static_assert(offsetof(FNativeConnectorLease2, ContextLive) == 60);
	static_assert(offsetof(FNativeConnectorLease2, DependencyCredits) == 64);
	static_assert(offsetof(FNativeConnectorLease2, WakeDescriptorsLive) == 68);
	static_assert(offsetof(FNativeConnectorLease2, Reserved) == 72);
	static_assert(std::is_standard_layout_v<FNativeSlot2> && std::is_trivially_copyable_v<FNativeSlot2>);
	static_assert(std::is_standard_layout_v<FNativeServiceIdentity2> && std::is_trivially_copyable_v<FNativeServiceIdentity2>);
	static_assert(std::is_standard_layout_v<FNativeConnectorLease2> && std::is_trivially_copyable_v<FNativeConnectorLease2>);
}

// Definition of the existing opaque token, private to its creating producer.
// Only the hidden prepared constructor can publish a real arena; it has no caller.
struct VrOwner
{
	uint32_t Phase;
	uint32_t PoisonReason;
	uint64_t OwnerEpoch;
	uint64_t NextServiceEpoch;
	uint64_t MappingBytes;
	uint64_t StopEpoch;
	VrPin OwnerPin;
	VaRestNativeOwnerPrivate::FNativeServiceIdentity2 Service;
	VaRestNativeOwnerPrivate::FNativeSlot2 Slots[2];
	VaRestNativeOwnerPrivate::FNativeConnectorLease2 Connector;
	VaRestNativeOwnerPrivate::FNativeResourceHeader1 Resources;
	VaRestNativeOwnerPrivate::FNativeResourceCell1 ResourceCells[140];
};

static_assert(sizeof(VrOwner) == 10632 && sizeof(VrOwner) <= 16384 && alignof(VrOwner) == 8);
static_assert(offsetof(VrOwner, Resources) == 1608 && offsetof(VrOwner, ResourceCells) == 1672);
static_assert(sizeof(VaRestNativeOwnerPrivate::FNativeResourceHeader1) + sizeof(VrOwner::ResourceCells) == 9024);
static_assert(offsetof(VrOwner, Phase) == 0);
static_assert(offsetof(VrOwner, PoisonReason) == 4);
static_assert(offsetof(VrOwner, OwnerEpoch) == 8);
static_assert(offsetof(VrOwner, NextServiceEpoch) == 16);
static_assert(offsetof(VrOwner, MappingBytes) == 24);
static_assert(offsetof(VrOwner, StopEpoch) == 32);
static_assert(offsetof(VrOwner, OwnerPin) == 40);
static_assert(offsetof(VrOwner, Service) == 80);
static_assert(offsetof(VrOwner, Slots) == 96);
static_assert(offsetof(VrOwner, Connector) == 1520);
static_assert(std::is_standard_layout_v<VrOwner> && std::is_trivially_copyable_v<VrOwner>);

static_assert(std::is_aggregate_v<VaRestNativeOwnerPrivate::FNativeServiceIdentity2>);
static_assert(std::is_trivial_v<VaRestNativeOwnerPrivate::FNativeServiceIdentity2>);
static_assert(std::is_trivially_destructible_v<VaRestNativeOwnerPrivate::FNativeServiceIdentity2>);
static_assert(std::is_aggregate_v<VaRestNativeOwnerPrivate::FNativeConnectorLease2>);
static_assert(std::is_trivial_v<VaRestNativeOwnerPrivate::FNativeConnectorLease2>);
static_assert(std::is_trivially_destructible_v<VaRestNativeOwnerPrivate::FNativeConnectorLease2>);
static_assert(std::is_aggregate_v<VrOwner> && std::is_trivial_v<VrOwner> && std::is_trivially_destructible_v<VrOwner>);
static_assert(std::is_aggregate_v<VaRestNativeOwnerPrivate::FNativeSlot2>);
static_assert(std::is_trivial_v<VaRestNativeOwnerPrivate::FNativeSlot2>);
static_assert(std::is_trivially_destructible_v<VaRestNativeOwnerPrivate::FNativeSlot2>);

namespace VaRestNativeOwnerPrivate
{
	[[gnu::visibility("hidden")]] VrStatus NativeResourceReserve1(VrOwner*, VrSession, const VrResourceSpec1*, uint32_t, VrResourceReply1*);
	[[gnu::visibility("hidden")]] VrStatus NativeResourceObtained1(VrOwner*, VrSession, const VrResourceTicket1*, const VrResourceActual1*, uint32_t, VrResourceReply1*);
	[[gnu::visibility("hidden")]] VrStatus NativeResourceNotObtained1(VrOwner*, VrSession, const VrResourceTicket1*, const VrResourceActual1*, uint32_t, VrResourceReply1*);
	[[gnu::visibility("hidden")]] VrStatus NativeResourceClosed1(VrOwner*, VrSession, const VrResourceTicket1*, const VrResourceActual1*, uint32_t, VrResourceReply1*);
	[[gnu::visibility("hidden")]] VrStatus NativeResourceUncertain1(VrOwner*, VrSession, const VrResourceTicket1*, const VrResourceActual1*, uint32_t, VrResourceReply1*);
	// Hidden complete production bodies; no public activation or binding surface.
	// Hidden C++ functions are not an entry bridge and never bind/appoint a caller.
	[[gnu::visibility("hidden")]] VrStatus VR_CALL NativeQuery2(VrOwner*, VrSnapshot*);
	[[gnu::visibility("hidden")]] VrStatus VR_CALL NativeAttachService2(VrOwner*, VrSession*);
	[[gnu::visibility("hidden")]] VrStatus VR_CALL NativeDetachService2(VrOwner*, VrSession);
	[[gnu::visibility("hidden")]] VrStatus VR_CALL NativeCopyResult2(VrOwner*, VrTicket, VrResult*);
	[[gnu::visibility("hidden")]] VrStatus VR_CALL NativeDiscard2(VrOwner*, VrTicket);
	[[gnu::visibility("hidden")]] VrStatus VR_CALL NativeSubmit2(VrOwner*, const VrResolve*, VrTicket*);
	[[gnu::visibility("hidden")]] VrStatus VR_CALL NativePollRetirement2(VrOwner*, VrSnapshot*);
	[[gnu::visibility("hidden")]] VrStatus VR_CALL NativeRetire2(VrOwner*, const VrRetire*, VrRetired*);

	// These creating-code functions have ZERO callers in S2. Scalar arguments are
	// consistency checks, never proof of admission, a no-create outcome or real join.
	[[gnu::visibility("hidden")]] VrStatus NativeConstructPreparedOwner();
	[[gnu::visibility("hidden")]] VrStatus NativeServiceNeverCreated(VrOwner*, VrSession);
	[[gnu::visibility("hidden")]] VrStatus NativeRegisterCurrentServiceThread(VrOwner*, VrSession);
	[[gnu::visibility("hidden")]] VrStatus NativeServiceJoinObserved(VrOwner*, VrSession);
	[[gnu::visibility("hidden")]] const VrOwnerApi2* NativeApiForOwnerConstruction();
	[[gnu::visibility("hidden")]] void* NativeResolverWorker(void*);

	// ZERO external callers in S3; real GT/live-lease/pin and creating boundaries
	// remain future integration premises. Leave is only the same verified pair.
	[[gnu::visibility("hidden")]] void NativeRequestStop(VrOwner*, VrSession);
	[[gnu::visibility("hidden")]] VrStatus NativePumpRegistered(VrOwner*, VrSession, uint32_t);
	[[gnu::visibility("hidden")]] VrStatus NativePumpEnter(VrOwner*, VrSession);
	[[gnu::visibility("hidden")]] void NativePumpLeave(VrOwner*, VrSession);
	[[gnu::visibility("hidden")]] void NativeRequestDetachAfterJoin(VrOwner*, VrSession);


	static_assert(std::is_same_v<decltype(&NativeQuery2), decltype(VrOwnerApi2::query)>);
	static_assert(std::is_same_v<decltype(&NativeAttachService2), decltype(VrOwnerApi2::attach_service)>);
	static_assert(std::is_same_v<decltype(&NativeDetachService2), decltype(VrOwnerApi2::detach_service)>);
	static_assert(std::is_same_v<decltype(&NativeCopyResult2), decltype(VrOwnerApi2::copy_result)>);
	static_assert(std::is_same_v<decltype(&NativeDiscard2), decltype(VrOwnerApi2::discard)>);
	static_assert(std::is_same_v<decltype(&NativeSubmit2), decltype(VrOwnerApi2::submit)>);
	static_assert(std::is_same_v<decltype(&NativePollRetirement2), decltype(VrOwnerApi2::poll_retirement)>);
	static_assert(std::is_same_v<decltype(&NativeRetire2), decltype(VrOwnerApi2::retire)>);

	namespace Detail
	{
		enum class ENativeRole2 : uint32_t { Control, Service, Either };
		struct [[gnu::visibility("hidden")]] FDescriptorLock2
		{
			FPassiveDescriptor2& Descriptor;
			bool Held;
			explicit FDescriptorLock2(FPassiveDescriptor2&);
			bool TryAcquire();
			~FDescriptorLock2();
			FDescriptorLock2(const FDescriptorLock2&) = delete;
			FDescriptorLock2& operator=(const FDescriptorLock2&) = delete;
		};
		struct [[gnu::visibility("hidden")]] FNativeWork2
		{
			FPassiveDescriptor2& Descriptor;
			uint32_t Saved;
			explicit FNativeWork2(FPassiveDescriptor2&);
			~FNativeWork2();
			FNativeWork2(const FNativeWork2&) = delete;
			FNativeWork2& operator=(const FNativeWork2&) = delete;
		};
		[[gnu::visibility("hidden")]] void ZeroBytes(void*, uint32_t);
		[[gnu::visibility("hidden")]] void CopyBytes(const void*, void*, uint32_t);
		[[gnu::visibility("hidden")]] bool IsZeroBytes(const void*, uint32_t);
		template <typename T> [[gnu::visibility("hidden")]] bool IsAligned(const T* Pointer)
		{
			return Pointer != nullptr && reinterpret_cast<uintptr_t>(Pointer) % alignof(T) == 0;
		}
		template <typename T> [[gnu::visibility("hidden")]] bool PrepareOutput(T* Output)
		{
			// Complete accessible nonaliasing caller-owned storage is an ABI premise.
			if (!IsAligned(Output) || Output->size != sizeof(T) || Output->version != VR_OWNER_VERSION)
			{
				return false;
			}
			ZeroBytes(Output, sizeof(T));
			Output->size = sizeof(T);
			Output->version = VR_OWNER_VERSION;
			return true;
		}
		[[gnu::visibility("hidden")]] uint32_t Load32(const uint32_t&);
		[[gnu::visibility("hidden")]] uint64_t Load64(const uint64_t&);
		[[gnu::visibility("hidden")]] void SetInterest(FNativeSlot2&, uint32_t);
		[[gnu::visibility("hidden")]] bool SameImage(VrImageId, VrImageId);
		[[gnu::visibility("hidden")]] bool IsEmptyPin(const VrPin&);
		[[gnu::visibility("hidden")]] bool IsJournalAbsent(const FPassiveDescriptor2&);
		[[gnu::visibility("hidden")]] bool IsLeaseMirrorAbsent(const FPassiveDescriptor2&);
		[[gnu::visibility("hidden")]] VrStatus MaterializeGenuineJoinLocked(FPassiveDescriptor2&, VrOwner&);

		[[gnu::visibility("hidden")]] VrStatus ValidateDescriptorControl(const FPassiveDescriptor2&);
		// Returns only the matched current arena address. It does NOT certify a role.
		[[gnu::visibility("hidden")]] VrStatus MatchCurrentOwnerForRoleCheck(FPassiveDescriptor2&, VrOwner*, VrOwner*&);
		[[gnu::visibility("hidden")]] bool KnownOwnerBookkeepingValid(const FPassiveDescriptor2&, const VrOwner&);
		[[gnu::visibility("hidden")]] VrStatus ValidateOwner(FPassiveDescriptor2&, VrOwner*, ENativeRole2, VrOwner*&);
		[[gnu::visibility("hidden")]] bool SameSession(const VrOwner&, VrSession);
		[[gnu::visibility("hidden")]] bool IsStopped(const VrOwner&, uint64_t);
		[[gnu::visibility("hidden")]] FNativeSlot2* InterestedSlot(VrOwner&, VrTicket);
		[[gnu::visibility("hidden")]] bool RetiredAndClean(const FNativeSlot2&);
		[[gnu::visibility("hidden")]] void ReleaseRetiredSlot(FNativeSlot2&);
		[[gnu::visibility("hidden")]] void ReleaseNeverCreatedSlot(FNativeSlot2&);
		[[gnu::visibility("hidden")]] void SetPhase(FPassiveDescriptor2&, VrOwner&, uint32_t);
		[[gnu::visibility("hidden")]] void PoisonLiveOwner(FPassiveDescriptor2&, VrOwner&, uint32_t);
		[[gnu::visibility("hidden")]] void BuildSnapshot(const VrOwner&, VrSnapshot&);
		[[gnu::visibility("hidden")]] bool ResourceCountsZero(const VrOwner&);
		[[gnu::visibility("hidden")]] bool ResourceBookkeepingValid(const VrOwner&);
		[[gnu::visibility("hidden")]] VrStatus ReconcileDeferredDetach(FPassiveDescriptor2&, VrOwner&);
		[[gnu::visibility("hidden")]] bool MonotonicNowNs(uint64_t&);
		[[gnu::visibility("hidden")]] bool CompletedResultValid(const FNativeSlot2&, VrTicket);
		[[gnu::visibility("hidden")]] void ClearPin(VrPin&);
		[[gnu::visibility("hidden")]] void ClearCompletedInlineForPhase(FPassiveDescriptor2&, uint32_t);
		[[gnu::visibility("hidden")]] void PoisonInlineRetainedPin(FPassiveDescriptor2&, uint32_t);
	}

}

#endif // VR_NATIVE_OWNER_GNU_X64
