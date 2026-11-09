// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreTypes.h"
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)

#include "VaRestNativeOwnerAbi.h"

class IConsoleVariable;
template <typename T> class TConsoleVariableData;

namespace VaRest::WebSocket::Lws
{
	// ZERO production callers. The qualified host must already reserve the name,
	// writers/history and live Core/WS return graph through the whole transaction.
	// These two borrowed Core pointers neither establish that history nor pin code.
	// One instance belongs to one initiating stack; it never closes an fd or E.
	class [[gnu::visibility("hidden")]] FNativeOwnerEntryGate1 final
	{
	public:
		FNativeOwnerEntryGate1() = default;
		FNativeOwnerEntryGate1(const FNativeOwnerEntryGate1&) = delete;
		FNativeOwnerEntryGate1& operator=(const FNativeOwnerEntryGate1&) = delete;

		// Inspections starts at zero in the actual outer caller's scalar storage.
		// Acquire publishes/readbacks 1 BEFORE that caller's first transaction open.
		// Refusal never repairs a collision, a sentinel, abandoned 1 or terminal 2.
		VrStatus Acquire(uint32& Inspections);
		// CALLSITE PRECONDITION: this initiating caller recorded every real native
		// return and has known no E/fd credit or completed each certified sole close.
		// Any real native status19 forbids 0, even after later known clean closes.
		VrStatus CompleteKnownDischarge(VrStatus NativeStatus, uint32& Inspections);
		// Actual retained/unverified E, ambiguous fd/E close or native19 uses this
		// disposition. Failure leaves the prior nonzero state; no retry/reset exists.
		VrStatus MakePermanent(uint32& Inspections);

	private:
		VrStatus Inspect(uint32 Expected, uint32& Inspections) const;
		IConsoleVariable* Variable = nullptr;
		TConsoleVariableData<int32>* Data = nullptr;
	};
	static_assert(sizeof(FNativeOwnerEntryGate1) == 16);
}

#endif
