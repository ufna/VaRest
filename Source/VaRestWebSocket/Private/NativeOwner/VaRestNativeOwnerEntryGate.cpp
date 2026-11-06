// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestNativeOwnerEntryGate.h"

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)

#include "CoreGlobals.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTLS.h"
#include "Misc/OutputDevice.h"

namespace VaRest::WebSocket::Lws
{
	namespace
	{
		constexpr int32 EntrySchemaCtor1 = 0x56524701;
		constexpr TCHAR EntryGateName1[] = TEXT("VaRest.NativeOwner.EntryCreditGate");
		constexpr TCHAR EntryGateHelp1[] = TEXT("VaRest native-owner caller entry gate; schema 1; READY=0; IN_FLIGHT=1; TERMINAL=2.");
		constexpr TCHAR EntryCtorText1[] = TEXT("1448232705");
		constexpr uint32 EntryInspectionLimit1 = 4;
		constexpr SIZE_T EntryHistoryBytes1 = 512;
		constexpr EConsoleVariableFlags EntrySetFlags1 = EConsoleVariableFlags(ECVF_SetByConsole | ECVF_Set_SetOnly_Unsafe);
		constexpr EConsoleVariableFlags EntryWarmFlags1 = EConsoleVariableFlags(ECVF_ReadOnly | ECVF_SetByConsole);

		template <SIZE_T N>
		bool EntryExactLiteral1(const TCHAR* Actual, const TCHAR (&Expected)[N])
		{
			static_assert(N <= 128);
			if (!Actual) return false;
			for (SIZE_T I = 0; I < N; ++I)
			{
				if (Actual[I] != Expected[I]) return false;
				if (Expected[I] == TCHAR(0)) return true;
			}
			return false;
		}

		// Four scalar fields only; incoming strings are consumed synchronously.
		// Count saturates at4 and cannot cancel arbitrary Core history iteration.
		class FEntryHistory1 final : public FOutputDevice
		{
		public:
			explicit FEntryHistory1(uint32 InExpected) : Expected(InExpected) {}
			void Serialize(const TCHAR* Text, ELogVerbosity::Type, const FName&) override
			{
				bool Match = false;
				if (Count == 0) Match = EntryExactLiteral1(Text, TEXT("\nHISTORY"));
				else if (Count == 1) Match = EntryExactLiteral1(Text, TEXT("Constructor: 1448232705"));
				else if (Count == 2 && Expected == 0) Match = EntryExactLiteral1(Text, TEXT("Console: 0"));
				else if (Count == 2 && Expected == 1) Match = EntryExactLiteral1(Text, TEXT("Console: 1"));
				else if (Count == 2 && Expected == 2) Match = EntryExactLiteral1(Text, TEXT("Console: 2"));
				if (!Match) Mismatch = 1;
				if (Count < 4) ++Count;
			}
			bool Matches() const { return Expected <= 2 && Count == 3 && Mismatch == 0 && Reserved == 0; }
		private:
			uint32 Expected;
			uint32 Count = 0;
			uint32 Mismatch = 0;
			uint32 Reserved = 0;
		};
		static_assert(sizeof(FEntryHistory1) <= 64);
		static_assert(sizeof(TCHAR) <= 4);

		VrStatus EntryRequireContext1()
		{
			if (!GIsGameThreadIdInitialized || FPlatformTLS::GetCurrentThreadId() != GGameThreadId || !IsInGameThread())
				return VR_STATUS_WRONG_THREAD;
			// These are consistency checks, not proof of live Core/host provenance.
			if (!GIsRunning || IsEngineExitRequested()) return VR_STATUS_PROVENANCE;
			return VR_STATUS_OK;
		}

		TConsoleVariableData<int32>* EntryOwningInt1(IConsoleVariable* Variable, EConsoleVariableFlags Flags)
		{
			if (!Variable || Variable->GetFlags() != Flags || !Variable->IsVariableInt()
				|| Variable->IsShadowObject() || Variable->GetParentObject() || Variable->GetShadowedObject())
				return nullptr;
			// Public owning-data accessor rejects Ref; Core can emit an ensure for
			// that negative collision. No private layout cast conceals it.
			TConsoleVariableData<int32>* Data = Variable->AsVariableInt();
			if (!Data) return nullptr;
#if UE_WITH_CVAR_HELP_TEXT
			if (!EntryExactLiteral1(Variable->GetHelp(), EntryGateHelp1)) return nullptr;
#endif
			return Data;
		}

		bool EntryConstructorMatches1(IConsoleVariable* Variable)
		{
			const FString Value = Variable->GetDefaultValue();
			return Value.Len() == UE_ARRAY_COUNT(EntryCtorText1) - 1 && EntryExactLiteral1(*Value, EntryCtorText1);
		}
	}

	VrStatus FNativeOwnerEntryGate1::Inspect(uint32 Expected, uint32& Inspections) const
	{
		const VrStatus Context = EntryRequireContext1();
		if (Context != VR_STATUS_OK) return Context;
		if (Expected > 2 || !Variable || !Data) return VR_STATUS_PROVENANCE;
		IConsoleManager& Manager = IConsoleManager::Get();
		if (!Manager.IsNameRegistered(EntryGateName1) || Manager.FindConsoleObject(EntryGateName1, false) != Variable
			|| EntryOwningInt1(Variable, EntryWarmFlags1) != Data
			|| Variable->GetInt() != int32(Expected) || Data->GetValueOnGameThread() != int32(Expected))
			return VR_STATUS_PROVENANCE;
		const SIZE_T Bytes = Variable->GetHistorySize();
		if (Bytes > EntryHistoryBytes1 || Inspections >= EntryInspectionLimit1) return VR_STATUS_SCAN_LIMIT;
		if (Bytes == 0) return VR_STATUS_PROVENANCE;
		++Inspections;
		{
			FEntryHistory1 History(Expected);
			Variable->LogHistory(History);
			if (!History.Matches()) return VR_STATUS_PROVENANCE;
		}
		// Only now may GetDefaultValue index the verified constructor history.
		if (!EntryConstructorMatches1(Variable) || Variable->GetFlags() != EntryWarmFlags1
			|| Variable->GetInt() != int32(Expected) || Data->GetValueOnGameThread() != int32(Expected))
			return VR_STATUS_PROVENANCE;
		return VR_STATUS_OK;
	}

	VrStatus FNativeOwnerEntryGate1::Acquire(uint32& Inspections)
	{
		const VrStatus Context = EntryRequireContext1();
		if (Context != VR_STATUS_OK) return Context;
		if (Variable || Data || Inspections != 0) return VR_STATUS_PROVENANCE;
		IConsoleManager& Manager = IConsoleManager::Get();
		const bool Registered = Manager.IsNameRegistered(EntryGateName1);
		IConsoleObject* Object = Manager.FindConsoleObject(EntryGateName1, false);
		if (!Registered)
		{
			if (Object) return VR_STATUS_PROVENANCE;
			IConsoleVariable* Fresh = Manager.RegisterConsoleVariable(EntryGateName1, EntrySchemaCtor1, EntryGateHelp1, ECVF_ReadOnly);
			TConsoleVariableData<int32>* FreshData = EntryOwningInt1(Fresh, EConsoleVariableFlags(ECVF_ReadOnly | ECVF_SetByConstructor));
			if (!FreshData || !Manager.IsNameRegistered(EntryGateName1) || Manager.FindConsoleObject(EntryGateName1, false) != Fresh
				|| Fresh->GetInt() != EntrySchemaCtor1 || FreshData->GetValueOnGameThread() != EntrySchemaCtor1
				|| Fresh->GetHistorySize() != 0 || !EntryConstructorMatches1(Fresh))
				return VR_STATUS_PROVENANCE;
			Variable = Fresh;
			Data = FreshData;
			// ONLY this fresh registering stack may prewarm. Reentry observes the
			// sentinel and refuses below without a Set or a second registration.
			Variable->Set(TEXT("0"), EntrySetFlags1, NAME_None);
		}
		else
		{
			if (!Object || Object->IsShadowObject() || Object->GetParentObject() || Object->GetShadowedObject())
				return VR_STATUS_PROVENANCE;
			Variable = Object->AsVariable();
			Data = EntryOwningInt1(Variable, EntryWarmFlags1);
			if (!Data) return VR_STATUS_PROVENANCE;
		}
		const int32 Current = Variable->GetInt();
		if (Current < 0 || Current > 2)
		{
			Variable = nullptr;
			Data = nullptr;
			return VR_STATUS_PROVENANCE;
		}
		VrStatus Status = Inspect(uint32(Current), Inspections);
		if (Status != VR_STATUS_OK || Current != 0)
		{
			// Observing another transaction/terminal value never gives this stack
			// the borrowed pair used by either final-disposition method.
			Variable = nullptr;
			Data = nullptr;
			return Status != VR_STATUS_OK ? Status : Current == 1 ? VR_STATUS_BUSY : VR_STATUS_POISONED;
		}
		Variable->Set(TEXT("1"), EntrySetFlags1, NAME_None);
		Status = Inspect(1, Inspections);
		// A failed Set/readback is not repaired. No caller opens on this refusal.
		return Status;
	}

	VrStatus FNativeOwnerEntryGate1::CompleteKnownDischarge(VrStatus NativeStatus, uint32& Inspections)
	{
		if (NativeStatus == VR_STATUS_RELEASE_UNCERTAIN)
		{
			MakePermanent(Inspections);
			return VR_STATUS_RELEASE_UNCERTAIN;
		}
		// Reserve BOTH remaining inspections before changing the Core state.
		if (Inspections > 2) return VR_STATUS_SCAN_LIMIT;
		if (Inspections != 2) return VR_STATUS_PROVENANCE;
		VrStatus Status = Inspect(1, Inspections);
		if (Status != VR_STATUS_OK) return Status;
		Variable->Set(TEXT("0"), EntrySetFlags1, NAME_None);
		Status = Inspect(0, Inspections);
		if (Status == VR_STATUS_OK)
		{
			Variable = nullptr;
			Data = nullptr;
		}
		return Status;
	}

	VrStatus FNativeOwnerEntryGate1::MakePermanent(uint32& Inspections)
	{
		if (Inspections > 2) return VR_STATUS_SCAN_LIMIT;
		if (Inspections != 2) return VR_STATUS_PROVENANCE;
		VrStatus Status = Inspect(1, Inspections);
		if (Status != VR_STATUS_OK) return Status;
		Variable->Set(TEXT("2"), EntrySetFlags1, NAME_None);
		Status = Inspect(2, Inspections);
		if (Status == VR_STATUS_OK)
		{
			Variable = nullptr;
			Data = nullptr;
		}
		return Status;
	}
}

#endif
