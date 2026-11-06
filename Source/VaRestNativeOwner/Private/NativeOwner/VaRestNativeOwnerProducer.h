// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include <stdint.h>

namespace VaRestNativeOwnerPrivate
{
	// SHA256 of native-source-v1: Source288 archive + adopted contract + parent
	// clarification + sorted path/SHA256 of the ten other changed native files.
	// The Producer header is excluded to avoid a recursive/self-referential hash.
	// Full exact identity inputs are frozen in this source delivery's identity-inputs.json.
	// Source variant identity ONLY; no authentication or final-artifact certificate.
	// Independently built A/B variants still require their own full qualification.
	static constexpr uint8_t ProducerIdentity[32] = {
		0xcf, 0x0f, 0x7c, 0xa3, 0xb5, 0x5b, 0xb3, 0x4b,
		0x1e, 0x57, 0xc5, 0xe1, 0xc7, 0x2a, 0x7d, 0xb8,
		0x19, 0xfe, 0xf8, 0x8d, 0xb7, 0x99, 0x5c, 0x9a,
		0x0e, 0x89, 0x35, 0x76, 0x7c, 0x28, 0xef, 0xe3
	};
}
