// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

class UVaRestSioState;

namespace VaRest::Sio::Stats
{
	void StartOnGT();
	void StopOnGT();
	void RegisterOnGT(UVaRestSioState* State);
	void EventIn();
	void EventOut();
}
