// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestHttpStats.h"

#include "Stats/Stats.h"
#include "VaRestHttpStatsInternal.h"

// Both compile away where LLM is off -- Shipping, and Test when ALLOW_LOW_LEVEL_MEM_TRACKER_IN_TEST is 0 (it is 1 by
// default): LowLevelMemTrackerDefines.h:10-12, :25-30; the empty macros, LowLevelMemTracker.h:949, :964-967
LLM_DEFINE_TAG(VaRest);
LLM_DEFINE_TAG(VaRest_Trace, TEXT("Trace"), TEXT("VaRest"));

// Accumulators, never cleared per frame (EStatFlags::None, Stats.h:145-147): each total is the running sum of its
// increments. An increment always queues a message while STATS is on, watched or not (Stats.h:250-257) -- a thread-local
// append without a lock (StatsSystemTypes.h:1333-1340) that is sent once per frame (StatsSystemTypes.cpp:1110-1120): a
// few per request, nothing while idle. Without STATS (Shipping: Build.h:342) every macro below is empty (Stats.h:646-685).
DECLARE_STATS_GROUP(TEXT("VaRest"), STATGROUP_VaRest, STATCAT_Advanced);
DECLARE_DWORD_ACCUMULATOR_STAT(TEXT("Requests"), STAT_VaRestRequests, STATGROUP_VaRest);
DECLARE_DWORD_ACCUMULATOR_STAT(TEXT("In flight"), STAT_VaRestInFlight, STATGROUP_VaRest);
DECLARE_DWORD_ACCUMULATOR_STAT(TEXT("Bytes up"), STAT_VaRestBytesUp, STATGROUP_VaRest);
DECLARE_DWORD_ACCUMULATOR_STAT(TEXT("Bytes down"), STAT_VaRestBytesDown, STATGROUP_VaRest);
DECLARE_DWORD_ACCUMULATOR_STAT(TEXT("Retries"), STAT_VaRestRetries, STATGROUP_VaRest);
DECLARE_DWORD_ACCUMULATOR_STAT(TEXT("Cache lookups"), STAT_VaRestCacheLookups, STATGROUP_VaRest);
DECLARE_DWORD_ACCUMULATOR_STAT(TEXT("Cache hits"), STAT_VaRestCacheHits, STATGROUP_VaRest);

VaRest::Http::FHttpCounters& VaRest::Http::FHttpStats::Global()
{
	// Atomics only: trivially destructible, but leaked like every singleton here (review I-4)
	static FHttpCounters* Instance = new FHttpCounters();
	return *Instance;
}

void VaRest::Http::FHttpStats::RequestStarted(FHttpCounters& Client)
{
	for (FHttpCounters* Set : { &Global(), &Client })
	{
		Set->Requests.fetch_add(1, std::memory_order_relaxed);
		Set->InFlight.fetch_add(1, std::memory_order_relaxed);
	}
	INC_DWORD_STAT(STAT_VaRestRequests);
	INC_DWORD_STAT(STAT_VaRestInFlight);
}

void VaRest::Http::FHttpStats::RequestFinished(FHttpCounters& Client)
{
	for (FHttpCounters* Set : { &Global(), &Client })
	{
		Set->InFlight.fetch_sub(1, std::memory_order_relaxed);
	}
	DEC_DWORD_STAT(STAT_VaRestInFlight);
}

void VaRest::Http::FHttpStats::Transferred(FHttpCounters& Client, int64 Up, int64 Down)
{
	for (FHttpCounters* Set : { &Global(), &Client })
	{
		Set->BytesUp.fetch_add(Up, std::memory_order_relaxed);
		Set->BytesDown.fetch_add(Down, std::memory_order_relaxed);
	}
	INC_DWORD_STAT_BY(STAT_VaRestBytesUp, Up);
	INC_DWORD_STAT_BY(STAT_VaRestBytesDown, Down);
}

void VaRest::Http::FHttpStats::Retried(FHttpCounters& Client)
{
	for (FHttpCounters* Set : { &Global(), &Client })
	{
		Set->Retries.fetch_add(1, std::memory_order_relaxed);
	}
	INC_DWORD_STAT(STAT_VaRestRetries);
}

void VaRest::Http::FHttpStats::CacheLookup(FHttpCounters& Client)
{
    for (FHttpCounters* Set : {&Global(), &Client}) Set->CacheLookups.fetch_add(1, std::memory_order_relaxed);
    INC_DWORD_STAT(STAT_VaRestCacheLookups);
}
void VaRest::Http::FHttpStats::CacheHit(FHttpCounters& Client)
{
    for (FHttpCounters* Set : {&Global(), &Client}) Set->CacheHits.fetch_add(1, std::memory_order_relaxed);
    INC_DWORD_STAT(STAT_VaRestCacheHits);
}
