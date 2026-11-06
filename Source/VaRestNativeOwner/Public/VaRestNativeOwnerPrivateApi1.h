// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestNativeOwnerPrivateService1.h"
#include "VaRestNativeOwnerPrivateResource1.h"

// Declaration-only composed object for the selected GNU Linux x86_64 profile.
// No instance, accessor, callable body, export or permission/ready state lives
// here. A genuine producer may emit the COMPLETE object only with all14 real
// same-image entries, including substantive admission. F1 emits no such object.
// Establish the actual original/initialized/read-only WHOLE216 range and genuine
// normal-owner/source/code lifetime BEFORE reading the tail of its actual API.
// A copied public prefix, record/magic or selected E does not establish that.
enum
{
	VR_PRIVATE_OWNER_PUBLIC_BYTES = 72,
	VR_PRIVATE_OWNER_TAIL_BYTES = 144,
	VR_PRIVATE_OWNER_HEADER_BYTES = 32,
	VR_PRIVATE_OWNER_OBJECT_BYTES = 216,
	VR_PRIVATE_OWNER_VERSION = 1,
	VR_PRIVATE_OWNER_SIGNATURE_ABI = 1,
	VR_PRIVATE_OWNER_COUNT = 14,
	VR_PRIVATE_OWNER_MASK = 0x3fff,
	VR_PRIVATE_OWNER_MAGIC_0 = 'V', VR_PRIVATE_OWNER_MAGIC_1 = 'R',
	VR_PRIVATE_OWNER_MAGIC_2 = 'P', VR_PRIVATE_OWNER_MAGIC_3 = 'R',
	VR_PRIVATE_OWNER_MAGIC_4 = 'I', VR_PRIVATE_OWNER_MAGIC_5 = 'V',
	VR_PRIVATE_OWNER_MAGIC_6 = '1', VR_PRIVATE_OWNER_MAGIC_7 = 0
};

typedef struct VrPrivateOwnerTail1
{
	uint8_t Magic[8];
	uint32_t Size, Version, SignatureAbi;
	uint16_t Count, Mask;
	uint32_t ResourceProfile, ResourceCells;
	VrPrivateAdmitControl1Fn VrPrivateAdmitCurrentControl1;
	VrPrivateSessionStatus1Fn VrPrivateServiceNeverCreated1;
	VrPrivateSessionStatus1Fn VrPrivateRegisterCurrentServiceThread1;
	VrPrivateSessionStatus1Fn VrPrivateServiceJoinObserved1;
	VrPrivateSessionVoid1Fn VrPrivateRequestStop1;
	VrPrivatePumpRegistered1Fn VrPrivatePumpRegistered1;
	VrPrivateSessionStatus1Fn VrPrivatePumpEnter1;
	VrPrivateSessionVoid1Fn VrPrivatePumpLeave1;
	VrPrivateSessionVoid1Fn VrPrivateRequestDetachAfterJoin1;
	VrPrivateResourceReserve1Fn VrPrivateResourceReserve1;
	VrPrivateResourceObtained1Fn VrPrivateResourceObtained1;
	VrPrivateResourceNotObtained1Fn VrPrivateResourceNotObtained1;
	VrPrivateResourceClosed1Fn VrPrivateResourceClosed1;
	VrPrivateResourceUncertain1Fn VrPrivateResourceUncertain1;
} VrPrivateOwnerTail1;

typedef struct VrPrivateOwnerApiObject1
{
	VrOwnerApi2 Public;
	VrPrivateOwnerTail1 Private;
} VrPrivateOwnerApiObject1;

#ifdef __cplusplus
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
static_assert(sizeof(VrOwnerApi2) == VR_PRIVATE_OWNER_PUBLIC_BYTES);
static_assert(sizeof(VrPrivateOwnerTail1) == VR_PRIVATE_OWNER_TAIL_BYTES && alignof(VrPrivateOwnerTail1) == 8);
static_assert(sizeof(VrPrivateOwnerApiObject1) == VR_PRIVATE_OWNER_OBJECT_BYTES && alignof(VrPrivateOwnerApiObject1) == 8);
static_assert(offsetof(VrPrivateOwnerApiObject1, Public) == 0 && offsetof(VrPrivateOwnerApiObject1, Private) == 72);
static_assert(std::is_same_v<decltype(VrPrivateOwnerApiObject1::Public), VrOwnerApi2>);
static_assert(std::is_same_v<decltype(VrPrivateOwnerApiObject1::Private), VrPrivateOwnerTail1>);
static_assert(offsetof(VrPrivateOwnerTail1, Magic) == 0 && sizeof(VrPrivateOwnerTail1::Magic) == 8);
static_assert(offsetof(VrPrivateOwnerTail1, Size) == 8 && offsetof(VrPrivateOwnerTail1, Version) == 12);
static_assert(offsetof(VrPrivateOwnerTail1, SignatureAbi) == 16 && offsetof(VrPrivateOwnerTail1, Count) == 20);
static_assert(offsetof(VrPrivateOwnerTail1, Mask) == 22 && offsetof(VrPrivateOwnerTail1, ResourceProfile) == 24);
static_assert(offsetof(VrPrivateOwnerTail1, ResourceCells) == 28);
static_assert(VR_PRIVATE_RESOURCE_PROFILE == 2 && VR_PRIVATE_RESOURCE_CELLS == 140);
#define VR_PRIVATE_OWNER_POD(Type) \
	static_assert(std::is_standard_layout_v<Type> && std::is_aggregate_v<Type> && std::is_trivial_v<Type>); \
	static_assert(std::is_trivially_copyable_v<Type> && std::is_trivially_destructible_v<Type>)
VR_PRIVATE_OWNER_POD(VrPrivateOwnerTail1);
VR_PRIVATE_OWNER_POD(VrPrivateOwnerApiObject1);
#undef VR_PRIVATE_OWNER_POD
#define VR_PRIVATE_OWNER_FIELD(Name, Type, Offset) \
	static_assert(std::is_same_v<decltype(VrPrivateOwnerTail1::Name), Type>); \
	static_assert(sizeof(Type) == 8 && alignof(Type) == 8); \
	static_assert(offsetof(VrPrivateOwnerTail1, Name) == Offset); \
	static_assert(offsetof(VrPrivateOwnerApiObject1, Private) + offsetof(VrPrivateOwnerTail1, Name) == 72 + Offset)
VR_PRIVATE_OWNER_FIELD(VrPrivateAdmitCurrentControl1, VrPrivateAdmitControl1Fn, 32);
VR_PRIVATE_OWNER_FIELD(VrPrivateServiceNeverCreated1, VrPrivateSessionStatus1Fn, 40);
VR_PRIVATE_OWNER_FIELD(VrPrivateRegisterCurrentServiceThread1, VrPrivateSessionStatus1Fn, 48);
VR_PRIVATE_OWNER_FIELD(VrPrivateServiceJoinObserved1, VrPrivateSessionStatus1Fn, 56);
VR_PRIVATE_OWNER_FIELD(VrPrivateRequestStop1, VrPrivateSessionVoid1Fn, 64);
VR_PRIVATE_OWNER_FIELD(VrPrivatePumpRegistered1, VrPrivatePumpRegistered1Fn, 72);
VR_PRIVATE_OWNER_FIELD(VrPrivatePumpEnter1, VrPrivateSessionStatus1Fn, 80);
VR_PRIVATE_OWNER_FIELD(VrPrivatePumpLeave1, VrPrivateSessionVoid1Fn, 88);
VR_PRIVATE_OWNER_FIELD(VrPrivateRequestDetachAfterJoin1, VrPrivateSessionVoid1Fn, 96);
VR_PRIVATE_OWNER_FIELD(VrPrivateResourceReserve1, VrPrivateResourceReserve1Fn, 104);
VR_PRIVATE_OWNER_FIELD(VrPrivateResourceObtained1, VrPrivateResourceObtained1Fn, 112);
VR_PRIVATE_OWNER_FIELD(VrPrivateResourceNotObtained1, VrPrivateResourceNotObtained1Fn, 120);
VR_PRIVATE_OWNER_FIELD(VrPrivateResourceClosed1, VrPrivateResourceClosed1Fn, 128);
VR_PRIVATE_OWNER_FIELD(VrPrivateResourceUncertain1, VrPrivateResourceUncertain1Fn, 136);
#undef VR_PRIVATE_OWNER_FIELD
#endif
#endif
