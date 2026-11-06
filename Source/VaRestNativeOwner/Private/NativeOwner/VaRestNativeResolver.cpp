// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include "VaRestNativeOwnerState.h"

#if VR_NATIVE_OWNER_GNU_X64

#include <arpa/inet.h>
#include <errno.h>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace VaRestNativeOwnerPrivate
{
	using namespace Detail;
	static_assert(sizeof(int) == sizeof(int32_t) && static_cast<int>(-1) < 0);
	static_assert(sizeof(in_port_t) == sizeof(uint16_t) && sizeof(in_addr_t) == 4);
	static_assert(sizeof(decltype(sockaddr_in6::sin6_scope_id)) == sizeof(uint32_t));
	static_assert(std::is_unsigned_v<decltype(sockaddr_in6::sin6_scope_id)>);

	namespace
	{
		static bool ResolveInputValid(const VrResolve& Input)
		{
			if (Input.size != sizeof(Input) || Input.version != VR_OWNER_VERSION || Input.flags != 0
				|| Input.reserved16 != 0 || Input.reserved != 0 || Input.port_be == 0 || Input.deadline_ns == 0
				|| Input.hostname_bytes == 0 || Input.hostname_bytes > VR_MAX_HOSTNAME_BYTES)
			{
				return false;
			}
			for (uint32_t I = 0; I < sizeof(Input.hostname); ++I)
			{
				if (I < Input.hostname_bytes ? Input.hostname[I] == '\0' : Input.hostname[I] != '\0')
				{
					return false;
				}
			}
			return true;
		}

		static bool SameNormalizedAddress(const VrAddress& A, const VrAddress& B)
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

		static bool NormalizeAddress(const addrinfo& Node, uint16_t PortBe, VrAddress& Address)
		{
			if (Node.ai_addr == nullptr)
			{
				return false;
			}
			if (Node.ai_family == AF_INET && Node.ai_addrlen >= sizeof(sockaddr_in))
			{
				sockaddr_in Native {};
				std::memcpy(&Native, Node.ai_addr, sizeof(Native));
				if (Native.sin_family != AF_INET || Native.sin_port != PortBe)
				{
					return false;
				}
				Address.family = VR_ADDRESS_IPV4;
				Address.port_be = Native.sin_port;
				CopyBytes(&Native.sin_addr.s_addr, Address.address, 4);
			}
			else if (Node.ai_family == AF_INET6 && Node.ai_addrlen >= sizeof(sockaddr_in6))
			{
				sockaddr_in6 Native {};
				std::memcpy(&Native, Node.ai_addr, sizeof(Native));
				if (Native.sin6_family != AF_INET6 || Native.sin6_port != PortBe)
				{
					return false;
				}
				Address.family = VR_ADDRESS_IPV6;
				Address.scope = Native.sin6_scope_id;
				Address.port_be = Native.sin6_port;
				CopyBytes(Native.sin6_addr.s6_addr, Address.address, sizeof(Address.address));
			}
			else
			{
				return false;
			}
			Address.size = sizeof(Address);
			Address.version = VR_OWNER_VERSION;
			return true;
		}

		static VrTicket SlotTicket(const VrOwner& Owner, const FNativeSlot2& Slot, uint32_t Index)
		{
			return { Owner.OwnerEpoch, Slot.ServiceEpoch, Slot.Generation, Index, 0 };
		}
	}

	[[gnu::used, gnu::visibility("hidden")]] void* NativeResolverWorker(void* Input)
	{
		// The only caller is pthread_create with its fully reserved native slot.
		// No service/owner/caller pointer, descriptor lock or occupancy mutation.
		FNativeSlot2& Slot = *static_cast<FNativeSlot2*>(Input);
		char Service[6] {};
		uint32_t Port = ntohs(Slot.PortBe);
		uint32_t Digits = 1;
		for (uint32_t Remaining = Port; Remaining >= 10; Remaining /= 10)
		{
			++Digits;
		}
		Service[Digits] = '\0';
		for (uint32_t I = Digits; I != 0; --I)
		{
			Service[I - 1] = static_cast<char>('0' + Port % 10);
			Port /= 10;
		}
		addrinfo Hints {};
		Hints.ai_family = AF_UNSPEC;
		Hints.ai_socktype = SOCK_STREAM;
		Hints.ai_flags = AI_NUMERICSERV;
		addrinfo* List = nullptr;
		const int ResolverError = getaddrinfo(Slot.Hostname, Service, &Hints, &List);
		const int SystemError = ResolverError == EAI_SYSTEM ? errno : 0;
		Slot.NativeError = SystemError;
		Slot.Result.count = 0;
		if (ResolverError != 0)
		{
			Slot.Result.resolver_error = static_cast<uint32_t>(ResolverError);
			Slot.ResultStatus = VR_STATUS_NATIVE_ERROR;
			// No successful list ownership was obtained on this error path.
			__atomic_store_n(&Slot.ResultPublished, 1u, __ATOMIC_RELEASE);
			return nullptr;
		}
		Slot.Result.resolver_error = 0;
		for (const addrinfo* Node = List; Node != nullptr; Node = Node->ai_next)
		{
			VrAddress Address {};
			if (!NormalizeAddress(*Node, Slot.PortBe, Address))
			{
				continue;
			}
			bool Duplicate = false;
			for (uint32_t I = 0; I < Slot.Result.count; ++I)
			{
				if (SameNormalizedAddress(Address, Slot.Result.address[I]))
				{
					Duplicate = true;
					break;
				}
			}
			if (!Duplicate)
			{
				CopyBytes(&Address, &Slot.Result.address[Slot.Result.count], sizeof(Address));
				++Slot.Result.count;
				if (Slot.Result.count == VR_MAX_ADDRESSES)
				{
					break;
				}
			}
		}
		Slot.ResultStatus = Slot.Result.count != 0 ? VR_STATUS_OK : VR_STATUS_NO_RESULT;
		if (List != nullptr)
		{
			freeaddrinfo(List);
		}
		__atomic_store_n(&Slot.ResultPublished, 1u, __ATOMIC_RELEASE);
		return nullptr;
	}

	[[gnu::used, gnu::visibility("hidden")]] VrStatus VR_CALL NativeSubmit2(VrOwner* Token, const VrResolve* Input, VrTicket* Output)
	{
		if (!IsAligned(Output))
		{
			return VR_STATUS_INVALID_INPUT;
		}
		ZeroBytes(Output, sizeof(*Output));
		if (!IsAligned(Input) || !ResolveInputValid(*Input))
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
		if (!SameSession(*Owner, Input->session))
		{
			return VR_STATUS_STALE;
		}
		if (Owner->Phase == VR_PHASE_POISONED || Owner->Connector.Poisoned != 0)
		{
			return VR_STATUS_POISONED;
		}
		if (Owner->Phase != VR_PHASE_ACTIVE)
		{
			return VR_STATUS_OLD_OWNER_BUSY;
		}
		if (IsStopped(*Owner, Input->session.service_epoch) || Load32(Owner->Connector.PumpRegistered) != 1
			|| Load32(Owner->Connector.DeferredDetachAfterJoin) != 0)
		{
			return VR_STATUS_STALE;
		}
		FNativeWork2 Work(Descriptor);
		uint64_t Now = 0;
		if (!MonotonicNowNs(Now))
		{
			PoisonLiveOwner(Descriptor, *Owner, VR_POISON_CLOCK);
			return VR_STATUS_POISONED;
		}
		if (Now >= Input->deadline_ns)
		{
			return VR_STATUS_EXPIRED;
		}
		if (IsStopped(*Owner, Input->session.service_epoch) || Load32(Owner->Connector.PumpRegistered) != 1
			|| Load32(Owner->Connector.DeferredDetachAfterJoin) != 0)
		{
			return VR_STATUS_STALE;
		}
		uint32_t Index = 0;
		while (Index < VR_MAX_SLOTS && Owner->Slots[Index].State != NativeSlotFree)
		{
			++Index;
		}
		if (Index == VR_MAX_SLOTS)
		{
			return VR_STATUS_CAPACITY;
		}
		FNativeSlot2& Slot = Owner->Slots[Index];
		if (Slot.Generation == UINT64_MAX)
		{
			PoisonLiveOwner(Descriptor, *Owner, VR_POISON_GENERATION);
			return VR_STATUS_POISONED;
		}
		++Slot.Generation;
		// Keep the newly consumed generation while initializing only known FREE storage.
		ZeroBytes(reinterpret_cast<uint8_t*>(&Slot) + sizeof(Slot.Generation), sizeof(Slot) - sizeof(Slot.Generation));
		Slot.State = NativeSlotReserved;
		Slot.ServiceEpoch = Input->session.service_epoch;
		Slot.DeadlineNs = Input->deadline_ns;
		Slot.HostnameBytes = Input->hostname_bytes;
		Slot.PortBe = Input->port_be;
		CopyBytes(Input->hostname, Slot.Hostname, sizeof(Slot.Hostname));
		Slot.Result.size = sizeof(Slot.Result);
		Slot.Result.version = VR_OWNER_VERSION;
		const VrTicket Ticket = SlotTicket(*Owner, Slot, Index);
		CopyBytes(&Ticket, &Slot.Result.ticket, sizeof(Ticket));
		SetInterest(Slot, 1);
		pthread_t Thread {};
		const int Created = pthread_create(&Thread, nullptr, &NativeResolverWorker, &Slot);
		if (Created != 0)
		{
			// Exact no-created-thread outcome; do not inspect or use the failed out-handle.
			Slot.State = NativeSlotCleanup;
			Slot.CloseDone = 1;
			ReleaseNeverCreatedSlot(Slot);
			return VR_STATUS_NATIVE_ERROR;
		}
		Slot.Thread = Thread;
		Slot.ThreadCreated = 1;
		Slot.State = NativeSlotRunning;
		CopyBytes(&Ticket, Output, sizeof(Ticket));
		return VR_STATUS_OK;
	}

	[[gnu::used, gnu::visibility("hidden")]] VrStatus VR_CALL NativePollRetirement2(VrOwner* Token, VrSnapshot* Output)
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
		FNativeWork2 Work(Descriptor);
		const uint64_t EntryStop = Load64(Owner->StopEpoch);
		const uint32_t EntryJoin = Load32(Owner->Connector.DeferredDetachAfterJoin);
		if (EntryJoin > 1)
		{
			return VR_STATUS_ABI_INCOMPATIBLE;
		}
		for (uint32_t Index = 0; Index < VR_MAX_SLOTS; ++Index)
		{
			if (Load64(Owner->StopEpoch) != EntryStop
				|| Load32(Owner->Connector.DeferredDetachAfterJoin) != EntryJoin)
			{
				break;
			}
			FNativeSlot2& Slot = Owner->Slots[Index];
			if (Slot.State == NativeSlotFree || Slot.RetirementUncertain != 0 || Slot.State == NativeSlotPoisoned)
			{
				continue;
			}
			if (Slot.ThreadCreated != 1
				|| (Slot.State != NativeSlotRunning && Slot.State != NativeSlotReturnedUnjoined && Slot.State != NativeSlotRetiredResult))
			{
				Slot.State = NativeSlotPoisoned;
				PoisonLiveOwner(Descriptor, *Owner, VR_POISON_NATIVE_INVARIANT);
				continue;
			}
			if (Slot.State == NativeSlotRunning && Load32(Slot.ResultPublished) == 1)
			{
				Slot.State = NativeSlotReturnedUnjoined;
			}
			if (Load64(Owner->StopEpoch) != EntryStop
				|| Load32(Owner->Connector.DeferredDetachAfterJoin) != EntryJoin)
			{
				break;
			}
			if (Slot.ThreadJoined == 0)
			{
				const int Joined = pthread_tryjoin_np(Slot.Thread, nullptr);
				if (Joined == 0)
				{
					// Record the consumed actual join before any stop check or next observation.
					Slot.ThreadJoined = 1;
					Slot.CloseDone = 1; // GNU has no separate CloseHandle/WSA credit.
				}
				else if (Joined != EBUSY)
				{
					Slot.RetirementUncertain = 1;
					Slot.State = NativeSlotPoisoned;
					PoisonLiveOwner(Descriptor, *Owner, VR_POISON_THREAD_RETIRE);
				}
				if (Load64(Owner->StopEpoch) != EntryStop
				|| Load32(Owner->Connector.DeferredDetachAfterJoin) != EntryJoin)
				{
					break;
				}
				if (Joined != 0)
				{
					continue;
				}
			}
			const VrTicket Ticket = SlotTicket(*Owner, Slot, Index);
			if (Load32(Slot.ResultPublished) != 1 || !CompletedResultValid(Slot, Ticket))
			{
				Slot.State = NativeSlotPoisoned;
				PoisonLiveOwner(Descriptor, *Owner, VR_POISON_NATIVE_INVARIANT);
				continue;
			}
			if (Load64(Owner->StopEpoch) != EntryStop
				|| Load32(Owner->Connector.DeferredDetachAfterJoin) != EntryJoin)
			{
				break;
			}
			bool Interested = Load32(Slot.Interest) == 1
				&& SameSession(*Owner, { Owner->OwnerEpoch, Slot.ServiceEpoch }) && !IsStopped(*Owner, Slot.ServiceEpoch);
			if (Interested)
			{
				uint64_t Now = 0;
				if (!MonotonicNowNs(Now))
				{
					PoisonLiveOwner(Descriptor, *Owner, VR_POISON_CLOCK);
					break;
				}
				if (Load64(Owner->StopEpoch) != EntryStop
				|| Load32(Owner->Connector.DeferredDetachAfterJoin) != EntryJoin)
				{
					break;
				}
				Interested = Now < Slot.DeadlineNs;
			}
			Slot.State = NativeSlotRetiredResult;
			if (!Interested)
			{
				SetInterest(Slot, 0);
				ReleaseRetiredSlot(Slot);
			}
			if (Load64(Owner->StopEpoch) != EntryStop
				|| Load32(Owner->Connector.DeferredDetachAfterJoin) != EntryJoin)
			{
				break;
			}
		}
		if (Load32(Owner->Connector.DeferredDetachAfterJoin) > 1)
		{
			return VR_STATUS_ABI_INCOMPATIBLE;
		}
		BuildSnapshot(*Owner, *Output);
		if (Owner->Phase == VR_PHASE_POISONED)
		{
			return VR_STATUS_POISONED;
		}
		for (const FNativeSlot2& Slot : Owner->Slots)
		{
			if (Slot.State != NativeSlotFree && !RetiredAndClean(Slot))
			{
				return VR_STATUS_PENDING;
			}
		}
		return VR_STATUS_OK;
	}
}

#endif // VR_NATIVE_OWNER_GNU_X64
