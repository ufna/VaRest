// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestHttpLibrary.h"
#include "VaRestHttpLog.h"

FString UVaRestHttpLibrary::DescribeError(const FVaRestError& Error)
{
	return VaRest::Http::EscapeForLog(Error.Describe());
}
