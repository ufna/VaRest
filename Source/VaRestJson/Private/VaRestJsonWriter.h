// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestJsonNode.h"

namespace VaRest::Json
{
	/** Appends Node as UTF-8 JSON to Out. Non-finite doubles are written as null with one warning. */
	void Write(const FNode& Node, EVaRestJsonFormat Format, TArray<uint8>& Out);
}
