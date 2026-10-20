// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestJsonNode.h"

namespace VaRest::Json
{
	/**
	 * Appends Node as UTF-8 JSON to Out. Non-finite doubles are written as null with one warning. Returns false, with
	 * Out back at its previous length, when Node nests deeper than MaxDepth (spec 2.2: the operation fails as a whole).
	 */
	bool Write(const FNode& Node, EVaRestJsonFormat Format, TArray<uint8>& Out);
}
