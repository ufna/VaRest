// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSocketStats.h"
#include "VaRestSocketStatsInternal.h"
#include "Stats/Stats.h"

DECLARE_STATS_GROUP(TEXT("VaRest"), STATGROUP_VaRest, STATCAT_Advanced);
DECLARE_DWORD_ACCUMULATOR_STAT(TEXT("Sockets open"), STAT_VaRestSocketOpen, STATGROUP_VaRest);
DECLARE_DWORD_ACCUMULATOR_STAT(TEXT("Socket attempts"), STAT_VaRestSocketAttempts, STATGROUP_VaRest);
DECLARE_DWORD_ACCUMULATOR_STAT(TEXT("Socket reconnects"), STAT_VaRestSocketReconnects, STATGROUP_VaRest);
DECLARE_DWORD_ACCUMULATOR_STAT(TEXT("Socket frames in"), STAT_VaRestSocketFramesIn, STATGROUP_VaRest);
DECLARE_DWORD_ACCUMULATOR_STAT(TEXT("Socket frames out"), STAT_VaRestSocketFramesOut, STATGROUP_VaRest);
DECLARE_DWORD_ACCUMULATOR_STAT(TEXT("Socket bytes in"), STAT_VaRestSocketBytesIn, STATGROUP_VaRest);
DECLARE_DWORD_ACCUMULATOR_STAT(TEXT("Socket bytes out"), STAT_VaRestSocketBytesOut, STATGROUP_VaRest);
DECLARE_DWORD_ACCUMULATOR_STAT(TEXT("Socket buffered bytes"), STAT_VaRestSocketBuffered, STATGROUP_VaRest);
DECLARE_DWORD_ACCUMULATOR_STAT(TEXT("Socket queued messages"), STAT_VaRestSocketQueued, STATGROUP_VaRest);
DECLARE_FLOAT_ACCUMULATOR_STAT(TEXT("Socket average RTT (seconds)"), STAT_VaRestSocketRtt, STATGROUP_VaRest);

namespace VaRest::WebSocket::Stats
{
	void Attempt() { INC_DWORD_STAT(STAT_VaRestSocketAttempts); }
	void Open(bool bReconnect) { if (bReconnect) INC_DWORD_STAT(STAT_VaRestSocketReconnects); }
	void Received(int64 Bytes) { INC_DWORD_STAT(STAT_VaRestSocketFramesIn); INC_DWORD_STAT_BY(STAT_VaRestSocketBytesIn, Bytes); }
	void Sent(int64 Bytes) { INC_DWORD_STAT(STAT_VaRestSocketFramesOut); INC_DWORD_STAT_BY(STAT_VaRestSocketBytesOut, Bytes); }
	void Gauges(int64 Open, int64 Buffered, int64 Queued, double AverageRtt)
	{
		SET_DWORD_STAT(STAT_VaRestSocketOpen, Open); SET_DWORD_STAT(STAT_VaRestSocketBuffered, Buffered);
		SET_DWORD_STAT(STAT_VaRestSocketQueued, Queued); SET_FLOAT_STAT(STAT_VaRestSocketRtt, AverageRtt);
	}
}
