// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSioStats.h"
#include "VaRestSioStatsInternal.h"
#include "VaRestSioState.h"
#include "VaRestSioLog.h"
#include "Containers/Ticker.h"
#include "HAL/IConsoleManager.h"
#include "Stats/Stats.h"
#include "UObject/StrongObjectPtr.h"

DECLARE_STATS_GROUP(TEXT("VaRest"), STATGROUP_VaRest, STATCAT_Advanced);
DECLARE_DWORD_ACCUMULATOR_STAT(TEXT("Socket.IO namespaces connected"), STAT_VaRestSioNamespaces, STATGROUP_VaRest);
DECLARE_DWORD_ACCUMULATOR_STAT(TEXT("Socket.IO events in"), STAT_VaRestSioEventsIn, STATGROUP_VaRest);
DECLARE_DWORD_ACCUMULATOR_STAT(TEXT("Socket.IO events out"), STAT_VaRestSioEventsOut, STATGROUP_VaRest);
DECLARE_DWORD_ACCUMULATOR_STAT(TEXT("Socket.IO acks pending"), STAT_VaRestSioAcks, STATGROUP_VaRest);

namespace VaRest::Sio::Stats
{
	namespace
	{
		TArray<TWeakObjectPtr<UVaRestSioState>> States;
		FTSTicker::FDelegateHandle Tick;
#if !UE_BUILD_SHIPPING
		IConsoleObject* ListCommand = nullptr;
#endif
	}
	void EventIn() { INC_DWORD_STAT(STAT_VaRestSioEventsIn); }
	void EventOut() { INC_DWORD_STAT(STAT_VaRestSioEventsOut); }
	void RegisterOnGT(UVaRestSioState* State)
	{
		check(IsInGameThread()); States.RemoveAll([](const auto& Weak) { return !Weak.IsValid() || Weak->bRetired; });
		States.AddUnique(State);
	}
	void StartOnGT()
	{
		check(IsInGameThread()); if (Tick.IsValid()) return;
		Tick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
		{
			States.RemoveAll([](const auto& Weak) { return !Weak.IsValid() || Weak->bRetired; });
#if STATS
			int64 Namespaces = 0, Acks = 0;
			for (const auto& Weak : States) if (auto* State = Weak.Get())
			{ const auto Snapshot = State->GetStatsOnGT(); Namespaces += Snapshot.ConnectedNamespaces; Acks += Snapshot.AcksPending; }
			SET_DWORD_STAT(STAT_VaRestSioNamespaces, Namespaces); SET_DWORD_STAT(STAT_VaRestSioAcks, Acks);
#endif
			return true;
		}));
#if !UE_BUILD_SHIPPING
		ListCommand = IConsoleManager::Get().RegisterConsoleCommand(TEXT("VaRest.Sio.List"), TEXT("List Socket.IO sockets and public namespace ids, recovery and ack gauges; no private Engine.IO ids."),
			FConsoleCommandDelegate::CreateLambda([]
			{
				check(IsInGameThread()); const auto Snapshot = States;
				for (const auto& Weak : Snapshot) if (auto* State = Weak.Get(); State && !State->bRetired)
				{
					const TStrongObjectPtr<UVaRestSioState> Pin(State); const auto Native = State->Socket.Pin(); const auto Stage = State->Stage.Pin();
					if (!Native.IsValid() || !Stage) continue;
					const auto Total = State->GetStatsOnGT();
					UE_LOG(LogVaRestSio, Display, TEXT("%s state=%d namespaces=%d acks=%d url=%s"), *Native.ScrubTextOnGT(Native.GetName().ToString()), static_cast<int32>(Native.GetState()), Total.ConnectedNamespaces, Total.AcksPending, *Native.ScrubTextOnGT(Native.GetRedactedUrl()));
					for (const auto& Namespace : Stage->GetNamespaces())
					{
						const auto Info = Stage->GetNamespaceInfo(Namespace); const auto Stats = State->GetStatsOnGT(Namespace);
						const FString Line = FString::Printf(TEXT("  %s state=%d sid=%s recovered=%d acks=%d"), *Namespace, static_cast<int32>(Stage->GetNamespaceState(Namespace)), *Info.Sid, Info.bRecovered, Stats.AcksPending);
						UE_LOG(LogVaRestSio, Display, TEXT("%s"), *Native.ScrubTextOnGT(Line));
					}
				}
			}), ECVF_Default);
#endif
	}
	void StopOnGT()
	{
		check(IsInGameThread()); FTSTicker::GetCoreTicker().RemoveTicker(Tick); Tick.Reset(); States.Reset();
#if !UE_BUILD_SHIPPING
		if (ListCommand) { IConsoleManager::Get().UnregisterConsoleObject(ListCommand, false); ListCommand = nullptr; }
#endif
	}
}
