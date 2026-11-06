// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace VaRest::WebSocket::Stats
{
	void Attempt();
	void Open(bool bReconnect);
	void Received(int64 Bytes);
	void Sent(int64 Bytes);
	void Gauges(int64 Open, int64 Buffered, int64 Queued, double AverageRtt);
}
