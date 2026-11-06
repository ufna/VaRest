// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once
#include "VaRestNativeOwnerAbi.h"

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
// Current receiver is Profile2/140. Historical Profile1 whole216 is an
// O-protected ordinary-only blocker before S; raw14/216 never permits reuse.
namespace VaRestNativeOwnerPrivate
{
	// Same-image implementation only. No authority argument, binder or callback.
	// Actual physical GT/H/immutable-origin/E-or-O/return lifetime are genuine
	// initiating-source preconditions, not facts supplied by a Boolean or record.
	// ZERO production callers. The actual final closure remains unqualified.
	[[gnu::visibility("hidden")]] VrStatus NativeAdmitCurrentControl1(uint32_t OutputBytes, VrBootstrap1* Output);
}
#endif
