// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestAuth.h"
#include "VaRestClientCredentials.h"

namespace VaRest::Http
{
FVaRestAuth::FRef MakeClientCredentialsAuth(const FVaRestClientCredentialsConfig& Config);
}
