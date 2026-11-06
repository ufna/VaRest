// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestNativeOwnerProducer.h"
#include "VaRestNativeOwnerDeployment1.h"

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
namespace
{
	// Exact ordinary84-byte mapped-note candidate. Section/used is not PT_NOTE,
	// read-only mapping or final closure evidence; the fresh artifact must prove it.
	alignas(4) [[gnu::used, gnu::section(".note.varest.owner")]] constinit const uint8_t OwnerImageNote1[84] = {
		0x07, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00,
		0x31, 0x4f, 0x52, 0x56, 0x56, 0x61, 0x52, 0x65,
		0x73, 0x74, 0x00, 0x00, 0x56, 0x52, 0x4f, 0x77,
		0x6e, 0x65, 0x72, 0x31, 0x01, 0x00, 0x00, 0x00,
		0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
		0x01, 0x00, 0x00, 0x00,
		VaRestNativeOwnerPrivate::ProducerIdentity[0], VaRestNativeOwnerPrivate::ProducerIdentity[1], VaRestNativeOwnerPrivate::ProducerIdentity[2], VaRestNativeOwnerPrivate::ProducerIdentity[3],
		VaRestNativeOwnerPrivate::ProducerIdentity[4], VaRestNativeOwnerPrivate::ProducerIdentity[5], VaRestNativeOwnerPrivate::ProducerIdentity[6], VaRestNativeOwnerPrivate::ProducerIdentity[7],
		VaRestNativeOwnerPrivate::ProducerIdentity[8], VaRestNativeOwnerPrivate::ProducerIdentity[9], VaRestNativeOwnerPrivate::ProducerIdentity[10], VaRestNativeOwnerPrivate::ProducerIdentity[11],
		VaRestNativeOwnerPrivate::ProducerIdentity[12], VaRestNativeOwnerPrivate::ProducerIdentity[13], VaRestNativeOwnerPrivate::ProducerIdentity[14], VaRestNativeOwnerPrivate::ProducerIdentity[15],
		VaRestNativeOwnerPrivate::ProducerIdentity[16], VaRestNativeOwnerPrivate::ProducerIdentity[17], VaRestNativeOwnerPrivate::ProducerIdentity[18], VaRestNativeOwnerPrivate::ProducerIdentity[19],
		VaRestNativeOwnerPrivate::ProducerIdentity[20], VaRestNativeOwnerPrivate::ProducerIdentity[21], VaRestNativeOwnerPrivate::ProducerIdentity[22], VaRestNativeOwnerPrivate::ProducerIdentity[23],
		VaRestNativeOwnerPrivate::ProducerIdentity[24], VaRestNativeOwnerPrivate::ProducerIdentity[25], VaRestNativeOwnerPrivate::ProducerIdentity[26], VaRestNativeOwnerPrivate::ProducerIdentity[27],
		VaRestNativeOwnerPrivate::ProducerIdentity[28], VaRestNativeOwnerPrivate::ProducerIdentity[29], VaRestNativeOwnerPrivate::ProducerIdentity[30], VaRestNativeOwnerPrivate::ProducerIdentity[31],
		0, 0, 0, 0, 0, 0, 0, 0
	};
	static_assert(sizeof(OwnerImageNote1) == VR_Q_NOTE_BYTES);
}
#endif
